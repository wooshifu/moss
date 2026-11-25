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

void process_edges() {
  using namespace process;

  auto *bare = new Process(500, INVALID_PROCESS_ID);
  if (!ut::expect(bare != nullptr)) {
    return;
  }
  ut::expect(!bare->create_thread(0x1000, 0x2000, 0x1000).has_value());
  ut::expect(!user_space::allocate_user_heap(nullptr, moss::kernel::PAGE_SIZE).has_value());
  ut::expect(!user_space::allocate_user_heap(bare, moss::kernel::PAGE_SIZE).has_value());
  ut::expect(!user_space::allocate_user_heap(bare, 0).has_value());

  auto space_result = user_space::create_user_address_space();
  if (!ut::expect(static_cast<bool>(space_result))) {
    return;
  }
  auto space = *space_result;
  auto *proc = new Process(501, INVALID_PROCESS_ID);
  if (!ut::expect(proc != nullptr)) {
    return;
  }
  if (!ut::expect(proc->set_address_space(space).has_value())) {
    return;
  }

  moss::abi::DomainInfo info{};
  proc->monitor_snapshot(info);
  ut::expect(info.threads == 0);

  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto created = proc->create_thread(0x1000, 0x2000, 0x1000);
  ut::expect(static_cast<bool>(created));
  auto *main = proc->get_main_thread();
  if (main && g_scheduler) {
    g_scheduler->dequeue_task(main);
    main->state = ProcessState::Sleeping;
  }
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  proc->monitor_snapshot(info);
  ut::expect(info.threads == 1);
  ut::expect(proc->try_begin_exec());
  ut::expect(!proc->try_begin_exec());
  proc->finish_exec();

  auto *extra = new Thread(600, proc->pid());
  if (ut::expect(extra != nullptr)) {
    extra->state = ProcessState::Sleeping;
    ut::expect(proc->register_thread(extra).has_value());
    ut::expect(!proc->try_begin_exec());
  }

  // A heap failure while publishing a thread must withdraw the reserved count.
  auto *pending = new Thread(601, proc->pid());
  if (ut::expect(pending != nullptr)) {
    pending->state = ProcessState::Sleeping;
    {
      HeapPressure pressure;
      if (ut::expect(pressure.acquire(sizeof(void *)))) {
        ut::expect(!proc->register_thread(pending).has_value());
      }
    }
    delete pending;
  }

  // Thread construction itself fails when the heap is exhausted.
  {
    HeapPressure pressure;
    if (ut::expect(pressure.acquire(sizeof(void *)))) {
      ut::expect(!proc->create_thread(0x1000, 0x2000, 0x1000).has_value());
    }
  }

  // Heap growth admission and VMA conflict rollback.
  ut::expect(user_space::allocate_user_heap(proc, moss::kernel::PAGE_SIZE).has_value());
  const VirtAddr break_addr = proc->address_space()->brk_current;
  auto conflict = proc->address_space()->add_vma(break_addr, break_addr + moss::kernel::PAGE_SIZE,
                                                 vma_flags::READ | vma_flags::WRITE, VmaType::MMAP);
  ut::expect(conflict);
  ut::expect(!user_space::allocate_user_heap(proc, moss::kernel::PAGE_SIZE).has_value());
  proc->address_space()->remove_vma(break_addr, break_addr + moss::kernel::PAGE_SIZE);
  ut::expect(!user_space::allocate_user_heap(proc, 1ULL << 40).has_value());

  proc->set_state(ProcessState::Zombie);
  proc->monitor_snapshot(info);
  ut::expect(info.state == static_cast<u32>(ProcessState::Zombie));

  ut::expect(!proc->register_thread(nullptr).has_value());
  {
    CfsScheduler scheduler;
    auto *linked = new Thread(603, 0);
    if (ut::expect(linked != nullptr)) {
      linked->ipc_scheduler = &scheduler;
      delete linked;
    }
  }
  ut::expect(!user_space::allocate_user_heap(proc, USER_MAX).has_value());
}

} // namespace moss::test::validation
