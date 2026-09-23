import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.timer;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "validation/resources.hpp"
#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/core_cases.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/smp_cases.hpp"
#include "validation_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;
using moss::test::validation::address_space_control_exhausted;
using moss::test::validation::address_space_control_pressure;
using moss::test::validation::address_space_control_rollback;
using moss::test::validation::address_space_heap_rollback;
using moss::test::validation::asid_leases;
using moss::test::validation::clone_allocation_rollback;
using moss::test::validation::clone_preserves_destination;
using moss::test::validation::HeapPressure;
using moss::test::validation::KernelPermissions;
using moss::test::validation::map_allocation_rollback;
using moss::test::validation::map_preserves_existing;
using moss::test::validation::map_rejects_blocks;
using moss::test::validation::memory_hash;
using moss::test::validation::page_table_hash;
using moss::test::validation::PagePressure;
using moss::test::validation::raw_user_copy_fixup;
using moss::test::validation::timer_contracts;
using moss::test::validation::timer_dispatch;
using moss::test::validation::unmap_reclaims_tables;
using moss::test::validation::user_copy_version_binding;
using moss::test::validation::vma_heap_rollback;

namespace moss::test::validation {
void resources() {
  const auto &info = moss::fdt::get_platform_info();
  // Match the host validation runner's default four vCPUs and 2048 MiB; the
  // boot options override these fixture expectations. Convert MiB to bytes.
  u64 cpus = numeric_boot_option("moss.cpus", 4);
  u64 memory = numeric_boot_option("moss.memory", 2048) * 1024 * 1024;
  u64 mask = (1ULL << cpus) - 1;
  ut::expect(info.cpu_count == cpus);
  ut::expect(__atomic_load_n(&moss::boot::online_cpu_mask, __ATOMIC_ACQUIRE) == mask);
  ut::expect(__atomic_load_n(&moss::boot::cpu_work_mask, __ATOMIC_ACQUIRE) == mask);
  ut::expect(info.memory_map_valid);
  // Allow the same 2 MiB RAM-reporting shortfall as the host protocol parser.
  // This is an acceptance tolerance; its original tuning basis is unrecorded.
  ut::expect(info.total_memory_size <= memory && info.total_memory_size + 0x200000 >= memory);
  auto stats = mm::PageFrameAllocator::get_memory_stats();
  // Probe beyond 256 MiB (half of RAM for smaller fixtures), catching an
  // allocator that only serves the low part of the advertised memory range.
  const u64 probe_offset = memory > 256UL * 1024 * 1024 ? 256UL * 1024 * 1024 : memory / 2;
  ut::expect(stats.total_pages * page_size > probe_offset);
  // Order 9 is 512 * 4 KiB = 2 MiB. Holding up to 129 disjoint blocks exceeds
  // the 128 blocks fitting below the 256 MiB probe, forcing a high allocation
  // in the default fixture. Fragmented or smaller fixtures may fail earlier.
  PhysAddr owned[129]{};
  usize count = 0;
  bool high = false;
  while (count < 129 && !high) {
    auto page = mm::PageFrameAllocator::allocate_pages(9);
    if (!ut::expect(static_cast<bool>(page))) {
      break;
    }
    owned[count++] = *page;
    ut::expect((*page & ((page_size << 9) - 1)) == 0);
    high = *page >= info.total_memory_start + probe_offset;
    auto *values = reinterpret_cast<volatile u64 *>(*page);
    values[0] = *page;
    values[(page_size << 9) / sizeof(u64) - 1] = ~*page;
    ut::expect(values[0] == *page && values[(page_size << 9) / sizeof(u64) - 1] == ~*page);
  }
  ut::expect(high);
  while (count) {
    ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(owned[--count], 9)));
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == stats.free_pages);
}

void *fd_table() {
  auto *thread = process::CfsScheduler::get_current_task();
  auto proc = process::g_process_manager->find_process(thread->owner_pid);
  return proc->fd_table();
}

LifecycleResources LifecycleResources::capture() {
  LifecycleResources result;
  process::g_process_manager->for_each_process([&](auto, process::Process *proc) {
    ++result.processes;
    result.threads += proc->thread_count();
    if (auto as = proc->address_space()) {
      as->vmas.for_each([&](const process::VmaRegion &vma) {
        for (VirtAddr va = vma.start_addr; va < vma.end_addr; va += page_size) {
          auto *pte = mm::PageTableManager::get_user_pte(as->pgd_phys, va);
          if (pte && pte->is_valid()) {
            ++result.user_pages;
            result.stack_pages += vma.type == process::VmaType::STACK;
          }
        }
      });
    }
  });
  auto *table = static_cast<vfs::FdTable *>(fd_table());
  for (u32 fd = 0; table && fd < vfs::MAX_FDS; ++fd) {
    if (auto *file = table->get_file(fd)) {
      ++result.descriptors;
      result.file_refs += file->ref_count;
    }
  }
  // Snapshot iteration owns temporary storage. Account only after it is freed.
  result.heap_bytes = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  result.free_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  result.vfs_pools = vfs::pool_usage();
  return result;
}

void process_heap_rollback() {
  auto &manager = *process::g_process_manager;
  const auto baseline = LifecycleResources::capture();
  const auto forks = manager.total_forks();
  const auto exits = manager.total_exits();
  bool recovered = false;
  unsigned failures = 0;
  {
    HeapPressure pressure;
    if (!ut::expect(pressure.acquire(sizeof(void *)))) {
      return;
    }
    // Gradually restore real capacity through the public creation boundary;
    // intermediate failures must release every partially constructed object.
    for (usize attempt = 0; attempt < sizeof(process::Process) / sizeof(void *) + 16; ++attempt) {
      const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      {
        auto created = manager.create_process();
        if (!created) {
          ++failures;
          ut::expect(created.error() == ErrorCode::OutOfMemory);
          ut::expect(manager.total_forks() == forks && manager.total_exits() == exits);
        } else {
          recovered = true;
          auto &proc = *created;
          ut::expect(manager.total_processes() == baseline.processes + 1);
          ut::expect(manager.find_process(proc->pid()).get() == proc.get());
          ut::expect(manager.terminate_process(proc->pid(), 37).has_value());
          ut::expect(!manager.process_exists(proc->pid()));
        }
      }
      ut::expect(manager.total_processes() == baseline.processes);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == baseline.free_pages);
      if (recovered || !ut::expect(pressure.release_one())) {
        break;
      }
    }
  }
  ut::expect(failures > 0 && recovered);
  ut::expect(manager.total_forks() == forks + 1 && manager.total_exits() == exits + 1);
  ut::expect(LifecycleResources::capture() == baseline);
}

} // namespace moss::test::validation
