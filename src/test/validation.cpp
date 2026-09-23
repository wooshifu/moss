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
import moss.ipc;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation_internal.hpp"
#include "validation/memory_internal.hpp"
#include "validation/memory_cases.hpp"
#include "validation/core_cases.hpp"
#include "validation/smp_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;
using moss::test::validation::HeapPressure;
using moss::test::validation::timer_contracts;
using moss::test::validation::timer_dispatch;
using moss::test::validation::KernelPermissions;
using moss::test::validation::PagePressure;
using moss::test::validation::address_space_control_exhausted;
using moss::test::validation::address_space_control_pressure;
using moss::test::validation::memory_hash;
using moss::test::validation::page_table_hash;
using moss::test::validation::map_preserves_existing;
using moss::test::validation::map_allocation_rollback;
using moss::test::validation::map_rejects_blocks;
using moss::test::validation::clone_preserves_destination;
using moss::test::validation::clone_allocation_rollback;
using moss::test::validation::address_space_heap_rollback;
using moss::test::validation::address_space_control_rollback;
using moss::test::validation::vma_heap_rollback;
using moss::test::validation::asid_leases;
using moss::test::validation::unmap_reclaims_tables;
using moss::test::validation::user_copy_version_binding;
using moss::test::validation::raw_user_copy_fixup;

namespace {
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
u32 console_irq_probe_armed = 0;
u32 console_reader_at_gap = 0;
u32 console_irq_before_lock = 0;
u32 console_irq_cpu = ~u32{0};
#endif
} // namespace

#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
extern "C" void moss_validation_console_before_register() noexcept {
  if (__atomic_load_n(&console_irq_probe_armed, __ATOMIC_ACQUIRE) == 0 || arch::get_current_cpu_id() != 1) {
    return;
  }
  // Hold the empty-check lock until the remote hardware IRQ has reached the
  // same lock. Registration then races only with a handler already in flight.
  __atomic_store_n(&console_reader_at_gap, 1U, __ATOMIC_RELEASE);
  while (__atomic_load_n(&console_irq_before_lock, __ATOMIC_ACQUIRE) == 0 &&
         __atomic_load_n(&console_irq_probe_armed, __ATOMIC_ACQUIRE) != 0) {
    arch::cpu_yield();
  }
}

extern "C" void moss_validation_console_irq_before_lock() noexcept {
  if (__atomic_load_n(&console_irq_probe_armed, __ATOMIC_ACQUIRE) == 0) {
    return;
  }
  __atomic_store_n(&console_irq_cpu, arch::get_current_cpu_id(), __ATOMIC_RELEASE);
  __atomic_store_n(&console_irq_before_lock, 1U, __ATOMIC_RELEASE);
}
#endif

namespace moss::kernel {
void kernel_uart_puts(const char *str) noexcept { hal::uart::puts(str); }
[[noreturn]] void kernel_test_exit([[maybe_unused]] int code) noexcept {
  // The host observes the completed serial protocol and owns process termination.
  arch::disable_interrupts();
  for (;;) {
    arch::cpu_halt();
  }
}
} // namespace moss::kernel

namespace moss::bench {
Tick read_counter() noexcept {
#if defined(MOSS_ARCH_ARM64)
  Tick value;
  asm volatile("dsb ish; isb; mrs %0, cntvct_el0; isb" : "=r"(value) : : "memory");
  return value;
#elif defined(MOSS_ARCH_X64)
  unsigned lo, hi;
  asm volatile("mfence; lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
  return (static_cast<Tick>(hi) << 32) | lo;
#else
  Tick value;
  asm volatile("fence iorw,iorw; rdtime %0; fence iorw,iorw" : "=r"(value) : : "memory");
  return value;
#endif
}
unsigned current_cpu() noexcept { return arch::get_current_cpu_id(); }
} // namespace moss::bench

namespace {
constexpr usize page_size = moss::kernel::PAGE_SIZE;

// Target ordering is shared with userspace/validation.c and the host catalog.
constexpr const char *kernel_isolation_cases[] = {"kernel_text", "kernel_rodata", "kernel_data", "kernel_page_table",
                                                  "kernel_mmio"};
// Private validation protocol pair, following exec controls 48/49.
constexpr long ISOLATION_PREPARE = 50;
constexpr long ISOLATION_VERIFY = 51;
constexpr u32 isolation_canary = 0x5a39c681; // Nonzero mixed bytes detect an illicit zero store.
const u32 isolation_rodata = isolation_canary;
volatile u32 isolation_data = isolation_canary;

struct KernelIsolation {
  PhysAddr physical{};
  VirtAddr address{};
  PhysAddr user_root{};
  u64 root_hash{};
  u32 contents{};
  long target{-1};
  long alias{-1};

  static bool mapping_matches(PhysAddr root, VirtAddr address, PhysAddr physical) {
    using Tables = mm::PageTableManager;
    // Reuse the native depth selection. A table has 512 entries, each consuming
    // nine VA bits; bit 12 begins the 4 KiB page offset. Walking the active tree
    // prevents an unmapped address from masquerading as a permission rejection.
    unsigned shift = KernelPermissions{}.root_shift;
    for (;;) {
      const auto *table = Tables::get_table_from_physical(root);
      if (!ut::expect(root && table)) {
        return false;
      }
      const auto &entry = table->entries[(address >> shift) & (mm::PageTable::ENTRIES_PER_TABLE - 1)];
      if (!ut::expect(entry.is_valid())) {
        return false;
      }
      if (shift == 12 || !entry.is_table()) {
        return ut::expect((entry.raw & mm::page_attr::USER) == 0 &&
                          (entry.get_phys_addr() | (address & ((1ULL << shift) - 1))) == physical);
      }
      root = entry.get_phys_addr();
      shift -= 9;
    }
  }

  long prepare(long requested_target, long requested_alias) {
    namespace linker = moss::abi::linker;
    auto owner = process::current_process();
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    if (!ut::expect(as && physical == 0 && requested_target >= 0 &&
                    static_cast<usize>(requested_target) < sizeof(kernel_isolation_cases) / sizeof(const char *) &&
                    (requested_alias == 0 || requested_alias == 1))) {
      return 0;
    }
    user_root = as->pgd_phys;
    switch (requested_target) {
    case 0:
      physical = linker::text_start();
      break;
    case 1:
      physical = reinterpret_cast<PhysAddr>(&isolation_rodata);
      break;
    case 2:
      physical = reinterpret_cast<PhysAddr>(&isolation_data);
      break;
    case 3:
      physical = user_root;
      break;
    case 4:
      if (!ut::expect(platform::hardware.intc.valid)) {
        return 0;
      }
      physical = platform::intc_dist_base();
#if defined(MOSS_ARCH_ARM64)
      // GICD_TYPER is the read-only 32-bit register at distributor offset 4;
      // unlike acknowledge registers, reads do not consume an interrupt.
      // Arm GIC-600 TRM, Distributor registers summary (GICv2/v3 contract).
      if (!ut::expect(platform::hardware.intc.gic_version == 2 || platform::hardware.intc.gic_version == 3)) {
        physical = 0;
        return 0;
      }
      physical += 4;
#elif defined(MOSS_ARCH_X64)
      // Intel SDM Vol. 3, Local APIC register map: version is read-only at
      // offset 0x30. The base comes from ACPI, not a fixed QEMU address.
      physical += 0x30;
#else
      // PLIC source zero does not exist: priority slot zero is reserved.
      // QEMU's SiFive PLIC returns zero and ignores stores to this slot, so a
      // broken U bit cannot acknowledge or reconfigure a live interrupt.
      // https://github.com/qemu/qemu/blob/master/hw/intc/sifive_plic.c
#endif
      break;
    default:
      return 0;
    }
    const bool section_matches =
        (requested_target != 0 || (physical >= linker::text_start() && physical + sizeof(u32) <= linker::text_end())) &&
        (requested_target != 1 ||
         (physical >= linker::rodata_start() && physical + sizeof(u32) <= linker::rodata_end())) &&
        (requested_target != 2 || (physical >= linker::data_start() && physical + sizeof(u32) <= linker::data_end()));
    if (!ut::expect(section_matches)) {
      physical = 0;
      return 0;
    }
    target = requested_target;
    alias = requested_alias;
    address = alias ? phys_to_virt(physical) : physical;
    PhysAddr root = user_root;
#if defined(MOSS_ARCH_ARM64)
    if (alias) {
      root = mm::PageTableManager::get_physical_address(mm::PageTableManager::get_kernel_high_pgd());
    }
#endif
    if (!mapping_matches(root, address, physical)) {
      physical = 0;
      return 0;
    }
    contents = *reinterpret_cast<volatile u32 *>(physical);
    root_hash = page_table_hash(user_root);
    return static_cast<long>(address);
  }

  bool verify(long requested_target, long requested_alias) {
    auto owner = process::current_process();
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    if (!ut::expect(physical && target == requested_target && alias == requested_alias && as &&
                    as->pgd_phys == user_root)) {
      return false;
    }
    // Fork may update hardware A/D bits, already excluded by page_table_hash;
    // it must not change kernel mappings or the probed object's contents.
    bool valid = ut::expect(page_table_hash(user_root) == root_hash);
    valid = ut::expect(*reinterpret_cast<volatile u32 *>(physical) == contents && isolation_data == isolation_canary &&
                       *reinterpret_cast<const volatile u32 *>(&isolation_rodata) == isolation_canary) &&
            valid;
    physical = 0;
    return valid;
  }
} kernel_isolation;


PagePressure *uaccess_pressure = nullptr;
usize uaccess_free_before = 0;
constexpr const char *uaccess_cases[] = {"allocation_fault", "write_fault",       "read_fault",       "partial_read",
                                         "partial_write",    "partial_pipe_read", "sigframe_fault",   "sigreturn_fault",
                                         "devices",          "cow_copy_fault",    "cow_partial_read", "cow_user_fault"};

// Match valid_id's 80-character limit and reserve one byte for the terminator.
char selection[81]{};
const char *active_case = nullptr;
bool failed = false;
unsigned completed = 0;
unsigned selected_count = 0;
unsigned sample_index = 0;
unsigned warmup_count = 5;
unsigned sample_count = 30;
usize fixed_iterations = 0;
bool stability = false;
bool is_lifecycle() {
  return ut::same_id(selection, "users.lifecycle") || ut::same_id(selection, "users.applications");
}
bench::Clock clock_info;
bench::Scenario *selected_benchmark = nullptr;
usize syscall_iterations = 1;
bool syscall_pilot = true;
u64 syscall_overhead = 0;
usize syscall_capacity = 65536;
const char *const user_benchmark_names[] = {"bench.fault",     "bench.cow",    "bench.switch",
                                            "bench.lifecycle", "bench.signal", "bench.pipe"};

// Private userspace dispatch modes: 2 is getpid and 11..16 follow this six-name
// catalog's order. Keep these numbers synchronized with userspace/validation.c.
long user_benchmark_mode() {
  if (ut::same_id(selection, "bench.getpid")) {
    return 2;
  }
  for (long i = 0; i < 6; ++i) {
    if (ut::same_id(selection, user_benchmark_names[i])) {
      return 11 + i;
    }
  }
  return 0;
}

// All strings come from the bounded catalog or fixed diagnostic identifiers.
// One complete record per UART write avoids allocating a formatting buffer.
class Event {
  // Fixed 1 KiB serial-record budget avoids heap allocation. Overflow marks the
  // run failed instead of emitting a truncated record that could parse as success.
  char buffer_[1024]{};
  usize used_ = 0;
  bool overflow_ = false;
  void append(char c) {
    if (used_ + 1 >= sizeof(buffer_)) {
      overflow_ = true;
      return;
    }
    buffer_[used_++] = c;
    buffer_[used_] = 0;
  }
  void append(const char *s) {
    while (*s) {
      append(*s++);
    }
  }

public:
  explicit Event(const char *type) {
    append("@@MOSS {\"v\":1");
    str("event", type);
    str("workload", selection);
  }
  Event &str(const char *key, const char *value) {
    append(",\"");
    append(key);
    append("\":\"");
    for (; *value; ++value) {
      if (*value == '\\' || *value == '"') {
        append('\\');
      }
      append(*value);
    }
    append('"');
    return *this;
  }
  Event &number(const char *key, u64 value) {
    append(",\"");
    append(key);
    append("\":");
    // A u64 needs at most 20 decimal digits; append adds them individually, so
    // this reverse-order scratch array needs no null terminator.
    char digits[20];
    unsigned n = 0;
    do {
      digits[n++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    while (n) {
      append(digits[--n]);
    }
    return *this;
  }
  void send() {
    append("}\n");
    if (overflow_) {
      kernel_test_exit(2);
    }
    hal::uart::puts(buffer_);
  }
};

[[noreturn]] void finish(const char *reason = "complete") {
  Event("end")
      .number("completed", completed)
      .number("selected", selected_count)
      .number("failed", failed ? 1 : 0)
      .str("reason", reason)
      .send();
  kernel_test_exit(failed ? 1 : 0);
}

bool option(const char *key, char *out, usize capacity) {
  const char *args = moss::fdt::get_platform_info().bootargs;
  if (!args) {
    return false;
  }
  while (*args) {
    while (*args == ' ') {
      ++args;
    }
    const char *token = args;
    while (*args && *args != ' ') {
      ++args;
    }
    const char *p = token;
    const char *k = key;
    while (*k && p < args && *p == *k) {
      ++k;
      ++p;
    }
    if (*k == 0 && p < args && *p == '=') {
      ++p;
      usize n = static_cast<usize>(args - p);
      if (!n || n >= capacity) {
        return false;
      }
      for (usize i = 0; i < n; ++i) {
        out[i] = p[i];
      }
      out[n] = 0;
      return true;
    }
  }
  return false;
}

u64 numeric_option(const char *key, u64 fallback) {
  // Twenty-four bytes fit a u64's 20 decimal digits and terminator. The parser
  // additionally rejects a pre-digit accumulator above 100,000,000, bounding
  // subsequent multiply/add; selection-specific limits are checked at boot.
  char value[24];
  if (!option(key, value, sizeof(value))) {
    return fallback;
  }
  u64 parsed = 0;
  for (const char *p = value; *p; ++p) {
    if (*p < '0' || *p > '9' || parsed > 100000000) {
      failed = true;
      return 0;
    }
    parsed = parsed * 10 + static_cast<unsigned>(*p - '0');
  }
  return parsed;
}

bool affinity_valid() {
  auto *thread = process::CfsScheduler::get_current_task();
  return thread && thread->cpu_affinity_mask.low_word() == 1 && arch::get_current_cpu_id() == 0;
}

void resources() {
  const auto &info = moss::fdt::get_platform_info();
  // Match the host validation runner's default four vCPUs and 2048 MiB; the
  // boot options override these fixture expectations. Convert MiB to bytes.
  u64 cpus = numeric_option("moss.cpus", 4);
  u64 memory = numeric_option("moss.memory", 2048) * 1024 * 1024;
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

struct LifecycleResources {
  u64 heap_bytes = 0, free_pages = 0, processes = 0, threads = 0, descriptors = 0, file_refs = 0;
  u64 user_pages = 0, stack_pages = 0;
  vfs::PoolUsage vfs_pools;

  static LifecycleResources capture() {
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
  bool operator==(const LifecycleResources &) const = default;
};

// Private controls following the isolation pair 50/51. Keep these operations
// synchronized with the real fork/pressure protocol in userspace/validation.c.
constexpr long COW_BEGIN = 52;
constexpr long COW_ARM = 53;
constexpr long COW_CHECK = 54;
constexpr long COW_RELEASE = 55;
constexpr long COW_SPLIT = 56;
constexpr long COW_FINISH = 57;

struct CowAllocationPressure {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  PagePressure pressure;
  LifecycleResources baseline;
  ProcessId parent = INVALID_PROCESS_ID;
  ProcessId child = INVALID_PROCESS_ID;
  VirtAddr address = 0;
  PhysAddr physical = 0;
  u64 contents = 0, parent_entry = 0, child_entry = 0;
  usize free_before = 0;
  bool armed = false, checked = false, released = false, split = false;

  static u64 stable_entry(u64 raw) {
    // Hardware may update access/dirty state independently of COW ownership.
    // Every software permission, physical-address and COW bit remains checked.
    u64 hardware_bits = mm::page_attr::AF;
#if !defined(MOSS_ARCH_ARM64)
    hardware_bits |= mm::page_attr::DIRTY;
#endif
    return raw & ~hardware_bits;
  }

  mm::PageTableEntry *leaf(process::Process &owner) const {
    // This private protocol keeps both processes in the same image until all
    // control calls finish. The returned PTE is borrowed only within that
    // quiescent interval; it is not a general concurrent-VM lookup API.
    auto as = owner.address_space();
    if (!ut::expect(as && as->allows_user_access(address, page_size, process::vma_flags::WRITE))) {
      return nullptr;
    }
    auto *entry = Tables::get_user_pte(as->pgd_phys, address);
    return ut::expect(entry && entry->is_valid() && (entry->raw & mm::page_attr::USER)) ? entry : nullptr;
  }

  bool begin(process::Process &owner, VirtAddr target) {
    address = target;
    parent = owner.pid();
    auto *entry = leaf(owner);
    if (!entry || !ut::expect((address & (page_size - 1)) == 0 && entry->is_writable() && !entry->is_cow())) {
      return false;
    }
    physical = entry->get_phys_addr();
    if (!ut::expect(Pfa::page_ref_get(physical) == 1)) {
      return false;
    }
    contents = memory_hash(phys_to_virt(physical), page_size);
    baseline = LifecycleResources::capture();
    return true;
  }

  bool arm(process::Process &owner) {
    // No retained Process owner may hide destruction after waitpid. The parent
    // remains alive by protocol; lookup owners only within each control call.
    auto peer = process::g_process_manager->find_process(parent);
    if (!ut::expect(!armed && peer && owner.parent_pid() == parent && owner.pid() != parent)) {
      return false;
    }
    auto *source = leaf(*peer);
    auto *target = leaf(owner);
    if (!source || !target ||
        !ut::expect(source->get_phys_addr() == physical && target->get_phys_addr() == physical && source->is_cow() &&
                    target->is_cow() && !source->is_writable() && !target->is_writable() &&
                    Pfa::page_ref_get(physical) == 2)) { // Exactly the real parent and child mappings.
      return false;
    }
    // The preceding page is the writable prefix of the partial-copy case.
    auto as = owner.address_space();
    auto *prefix = Tables::get_user_pte(as->pgd_phys, address - page_size);
    if (!ut::expect(prefix && prefix->is_valid() && prefix->is_writable() && !prefix->is_cow() &&
                    Pfa::page_ref_get(prefix->get_phys_addr()) == 1)) {
      return false;
    }
    child = owner.pid();
    parent_entry = stable_entry(source->raw);
    child_entry = stable_entry(target->raw);
    free_before = Pfa::get_memory_stats().free_pages;
    armed = ut::expect(pressure.acquire(0));
    return armed;
  }

  bool unchanged(process::Process &owner) {
    auto peer = process::g_process_manager->find_process(parent);
    if (!ut::expect(armed && !released && owner.pid() == child && peer)) {
      return false;
    }
    auto *source = leaf(*peer);
    auto *target = leaf(owner);
    bool valid = ut::expect(source && target && stable_entry(source->raw) == parent_entry &&
                            stable_entry(target->raw) == child_entry);
    valid =
        ut::expect(Pfa::page_ref_get(physical) == 2 && memory_hash(phys_to_virt(physical), page_size) == contents) &&
        valid;
    // Reading the same resident COW page must need no allocation. This exercises
    // the production input-copy path while output-copy allocation cannot succeed.
    u64 input = 0;
    valid = ut::expect(process::copy_from_user(&input, address, sizeof(input)) == 0 &&
                       input == *reinterpret_cast<const u64 *>(phys_to_virt(physical))) &&
            valid;
    valid = ut::expect(Pfa::get_memory_stats().free_pages == 0) && valid;
    checked = valid;
    return valid;
  }

  bool release(process::Process &owner) {
    const bool valid = unchanged(owner);
    pressure.release();
    released = true;
    return ut::expect(Pfa::get_memory_stats().free_pages == free_before) && valid;
  }

  bool verify_split(process::Process &owner) {
    auto peer = process::g_process_manager->find_process(parent);
    if (!ut::expect(checked && released && owner.pid() == child && peer)) {
      return false;
    }
    auto *source = leaf(*peer);
    auto *target = leaf(owner);
    if (!source || !target) {
      return false;
    }
    auto expected = *target;
    expected.raw = child_entry;
    expected.clear_cow();
    expected.make_writable();
    expected.set_page(target->get_phys_addr(), expected.raw & ~hal::mmu::PTE_ADDR_MASK);
    split = ut::expect(stable_entry(source->raw) == parent_entry && target->get_phys_addr() != physical &&
                       stable_entry(target->raw) == stable_entry(expected.raw) &&
                       Pfa::page_ref_get(target->get_phys_addr()) == 1 && Pfa::page_ref_get(physical) == 1 &&
                       memory_hash(phys_to_virt(physical), page_size) == contents);
    // Only the formerly shared data page may allocate during the retry.
    split = ut::expect(Pfa::get_memory_stats().free_pages + 1 == free_before) && split;
    return split;
  }

  bool finish(process::Process &owner, bool direct_fault) {
    bool valid = ut::expect(armed && owner.pid() == parent && !process::g_process_manager->find_process(child));
    // A direct user fault exits without releasing pressure. Recover it only
    // after the parent reaps the real child, then compare the full pre-fork
    // resource baseline. This also safely cleans a failed child-side oracle.
    pressure.release();
    auto *entry = leaf(owner);
    valid = ut::expect(entry && stable_entry(entry->raw) == parent_entry && Pfa::page_ref_get(physical) == 1 &&
                       memory_hash(phys_to_virt(physical), page_size) == contents) &&
            valid;
    valid = ut::expect(direct_fault || (checked && released && split)) && valid;
    return ut::expect(LifecycleResources::capture() == baseline) && valid;
  }
};

CowAllocationPressure *cow_pressure = nullptr;

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

LifecycleResources lifecycle_baseline;
struct ForkMetadataPressure {
  LifecycleResources baseline;
  HeapPressure heap;
  unsigned stage = 0, vmas = 0;
  bool holding = false, exhausted = false;
};
ForkMetadataPressure *fork_metadata_pressure = nullptr;
struct ExecAllocationPressure {
  LifecycleResources baseline;
  HeapPressure heap;
  process::AddressSpace *original = nullptr;
  PhysAddr root = 0;
  u64 root_hash = 0;
  char name[16]{}; // Matches Process's 15-byte comm plus terminating NUL.
  unsigned stage = 0, vmas = 0;
  bool holding = false, exhausted = false;
};
ExecAllocationPressure *exec_allocation_pressure = nullptr;
// Validation controls 48/49 are the next unused pair in this private protocol.
// Six stages cover arguments, both SharedPtr allocations, image bytes, the
// address space and a partially populated VMA list; keep userspace in sync.
inline constexpr long EXEC_HEAP_PRESSURE_ARM = 48;
inline constexpr long EXEC_HEAP_PRESSURE_RELEASE = 49;
inline constexpr long EXEC_REGISTER_DORMANT_PEER = 52;
inline constexpr unsigned EXEC_HEAP_ALLOCATION_STAGES = 6;
inline constexpr unsigned EXEC_VMA_ALLOCATION_STAGE = 5;
HeapPressure *fork_clone_pressure = nullptr;
LifecycleResources fork_clone_baseline;
bool fork_clone_exhausted = false;
HeapPressure *user_heap_pressure = nullptr;
LifecycleResources user_heap_baseline;
PagePressure *exec_pressure = nullptr;
LifecycleResources exec_baseline;
process::AddressSpace *exec_original = nullptr;
PhysAddr exec_root = 0;
u64 exec_root_hash = 0;
char exec_name[16]{};
LifecycleResources benchmark_resources;
bool benchmark_warmed = false;
u64 benchmark_switches = 0;
u64 lifecycle_checkpoint = 0;
u64 lifecycle_started_ns = 0;
bool lifecycle_started = false;
bool lifecycle_complete = false;
bool lifecycle_host_released = false;

struct TimerCapacity {
  // One more than the production timer heap's 256 slots forces exhaustion;
  // the 1000-second delay prevents test timers from expiring during setup.
  timer::HrTimer *timers = new timer::HrTimer[257];
  bool full = false;

  TimerCapacity() {
    if (!ut::expect(timers != nullptr)) {
      return;
    }
    unsigned active = 0;
    for (unsigned i = 0; i < 257; ++i) {
      timers[i].init(timer::TimerMode::OneShot, [](void *) noexcept {});
      auto started = timers[i].start_relative(1000000000000ULL);
      ut::expect(started ? timers[i].is_active()
                         : started.error() == ErrorCode::ResourceExhausted && !timers[i].is_active());
      full = full || (!started && started.error() == ErrorCode::ResourceExhausted);
      active += timers[i].is_active();
    }
    // The production heap has 256 slots, including its scheduler tick.
    ut::expect(full && active < 256);
  }

  ~TimerCapacity() {
    if (!timers) {
      return;
    }
    for (unsigned i = 0; i < 257; ++i) {
      timers[i].cancel_sync();
      ut::expect(!timers[i].is_active());
    }
    delete[] timers;
  }
};
TimerCapacity *sleep_capacity = nullptr;
u64 sleep_capacity_heap_before = 0;

void timer_capacity() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  {
    TimerCapacity fixture;
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void empty_case() {}

struct TimerCancellation {
  timer::HrTimer pending;
  u32 peers_ready = 0, callback_cpu = ~0U;
  u32 entered = 0, release = 0, callback_returned = 0, cancel_returned = 0, observer_returned = 0;
  bool cancel_ok = false, observer_ok = false;

  static void callback(void *data) noexcept {
    auto &self = *static_cast<TimerCancellation *>(data);
    __atomic_store_n(&self.callback_cpu, arch::get_current_cpu_id(), __ATOMIC_RELAXED);
    __atomic_store_n(&self.entered, 1U, __ATOMIC_RELEASE);
    moss::test::validation::wait_for_phase(self.release, 1);
    __atomic_store_n(&self.callback_returned, 1U, __ATOMIC_RELEASE);
  }

  bool peer(long actor) {
    if (actor < 1 || actor > 2 || arch::get_current_cpu_id() != static_cast<u32>(actor)) {
      return false;
    }
    // The timer queue is global: a timer IRQ must not take over either actor
    // whose progress the deliberately held callback needs. Publish readiness
    // only after masking local IRQs, before the owner arms the real timer.
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    __atomic_fetch_or(&peers_ready, 1U << static_cast<u32>(actor), __ATOMIC_RELEASE);
    moss::test::validation::wait_for_phase(entered, 1);
    if (actor == 1) {
      pending.cancel_sync();
      cancel_ok = __atomic_load_n(&callback_returned, __ATOMIC_ACQUIRE) == 1;
      __atomic_store_n(&cancel_returned, 1U, __ATOMIC_RELEASE);
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return cancel_ok;
    }
    // A periodic timer remains queued while its callback runs. Inactive thus
    // proves CPU1 has reached cancellation, not merely its pre-call barrier.
    while (pending.is_active()) {
      arch::cpu_yield();
    }
    const auto restarted = pending.start_relative(1000000000ULL);
    observer_ok = !restarted && restarted.error() == ErrorCode::InvalidState;
    // Hold the callback across a bounded observation window. Cancellation may
    // not return during it; the host deadline still bounds every peer barrier.
    auto &clock = timer::TimerSubsystem::instance();
    const u64 until = clock.now_ns() + 2000000ULL;
    while (!__atomic_load_n(&cancel_returned, __ATOMIC_ACQUIRE) && clock.now_ns() < until) {
      arch::cpu_yield();
    }
    observer_ok = observer_ok && !__atomic_load_n(&cancel_returned, __ATOMIC_ACQUIRE);
    __atomic_store_n(&release, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&observer_returned, 1U, __ATOMIC_RELEASE);
    if (restore_irqs) {
      arch::enable_interrupts();
    }
    return observer_ok;
  }

  bool owner() {
    moss::test::validation::wait_for_phase(peers_ready, 6);
    pending.init(timer::TimerMode::Periodic, callback, this);
    if (!pending.start_relative(1000000ULL)) {
      return false;
    }
    moss::test::validation::wait_for_phase(cancel_returned, 1);
    moss::test::validation::wait_for_phase(observer_returned, 1);
    pending.cancel_sync();
    const u32 cpu = __atomic_load_n(&callback_cpu, __ATOMIC_ACQUIRE);
    return cancel_ok && observer_ok && callback_returned == 1 && !pending.is_active() && cpu != 1 && cpu != 2;
  }
};
TimerCancellation *timer_cancellation = nullptr;
process::Thread *early_sleep_thread = nullptr;
u32 early_sleep_visits = 0;
bool early_sleep_woken = true;
ProcessId wait_exit_parent = INVALID_PROCESS_ID;
ProcessId wait_exit_child = INVALID_PROCESS_ID;
u32 wait_exit_phase = 0;
ProcessId cpu_bound_probe_pid = INVALID_PROCESS_ID;
u64 cpu_bound_probe_start = 0;
u64 cpu_bound_probe_end = 0;
u32 cpu_bound_irq_seen = 0;
// Validation control IDs shared with the CPU-bound userspace case.
constexpr long CPU_BOUND_ARM_PROBE = 60;
constexpr long CPU_BOUND_CHECK_PROBE = 61;
constexpr long STOP_STATE_PROBE = 62;
constexpr long STOP_PENDING_PROBE = 63;
bool dispatch_boundary_checked = false;

void failing_case() { ut::expect(false); }
[[noreturn]] void panic_case() {
  Event("fatal").str("case", active_case).str("kind", "panic").send();
  logging::klog::panic("validation intentional panic");
  // Exercise the emergency path with TX already locked by this CPU. It must
  // still reach the real panic/halt; peers cannot interleave its diagnostic.
  hal::uart::TransmitGuard guard;
  arch::kernel_panic("validation intentional panic");
}
[[noreturn]] void timeout_case() {
  Event("fatal").str("case", active_case).str("kind", "timeout").send();
  for (;;) {
    arch::cpu_yield();
  }
}


void declare_cases() {
  moss::test::validation::register_driver_cases();
  ut::register_suite("resources", [] { ut::register_test("cpu_memory", resources); });
  moss::test::validation::register_mm_cases();
  moss::test::validation::register_pfa_cases();
  moss::test::validation::register_heap_cases();
  moss::test::validation::register_containers_cases();
  moss::test::validation::register_smp_cases();
  moss::test::validation::register_vfs_cases();
  ut::register_suite("timers", [] {
    ut::register_test("clocksource_high_frequency",
                      [] { ut::expect(moss::test::hardware::clocksource_high_frequency_regression()); });
    ut::register_test("contracts", timer_contracts);
    ut::register_test("dispatch", timer_dispatch);
    ut::register_test("capacity", timer_capacity);
  });
  moss::test::validation::register_scheduler_cases();
  ut::register_suite("process", [] { ut::register_test("heap_rollback", process_heap_rollback); });
  ut::register_suite("users", [] {
    ut::register_test("syscall_values", empty_case);
    ut::register_test("user_ranges", empty_case);
    ut::register_test("fork_exec_exit_reap", empty_case);
    ut::register_test("pipe_output_rollback", empty_case);
    ut::register_test("yield_reuse", empty_case);
    ut::register_test("wait_status_rollback", empty_case);
    ut::register_test("pipe_waits_for_writer", empty_case);
    ut::register_test("pipe_cross_cpu_roundtrip", empty_case);
    ut::register_test("pipe_waits_for_reader", empty_case);
    ut::register_test("cross_cpu_exit_reap", empty_case);
    ut::register_test("fork_fd_allocation_rollback", empty_case);
    ut::register_test("mmap_heap_rollback", empty_case);
    ut::register_test("fork_process_allocation_rollback", empty_case);
    ut::register_test("fork_metadata_allocation_rollback", empty_case);
  });
  ut::register_suite("users.frame", [] {
    ut::register_test("native_frame", empty_case);
    ut::register_test("fork_registers", empty_case);
    ut::register_test("signal_return", empty_case);
  });
  ut::register_suite("users.uaccess", [] {
    for (const auto *name : uaccess_cases) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("users.vm", [] {
    ut::register_test("private_cow", empty_case);
    ut::register_test("readonly_cow", empty_case);
    ut::register_test("access_permissions", empty_case);
    ut::register_test("brk_lifecycle", empty_case);
    for (const auto *name : kernel_isolation_cases) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("users.lifecycle", [] { ut::register_test("core_paths_recovery", empty_case); });
  ut::register_suite("users.applications", [] { ut::register_test("core_application_recovery", empty_case); });
  ut::register_suite("users.timers", [] {
    ut::register_test("relative_sleep", empty_case);
    ut::register_test("absolute_sleep", empty_case);
    ut::register_test("invalid_arguments", empty_case);
    ut::register_test("short_reuse", empty_case);
    ut::register_test("cancel_in_flight", empty_case);
    ut::register_test("early_wakeup", empty_case);
    ut::register_test("arm_failure_recovery", empty_case);
    ut::register_test("relative_interrupted", empty_case);
    ut::register_test("clock_relative_interrupted", empty_case);
    ut::register_test("clock_absolute_interrupted", empty_case);
  });
  ut::register_suite("users.ipc", [] {
    ut::register_test("roundtrip", empty_case);
    ut::register_test("deadline", empty_case);
    ut::register_test("peer_death", empty_case);
    ut::register_test("signal_cancel", empty_case);
    ut::register_test("capability_transfer", empty_case);
    ut::register_test("delivery_rollback", empty_case);
  });
  ut::register_suite("users.libc", [] {
    ut::register_test("static_runtime", empty_case);
    ut::register_test("filesystem_permissions", empty_case);
  });
  ut::register_suite("users.exec", [] {
    constexpr const char *names[] = {
        "rejects_invalid_entry",
        "rejects_phentsize",
        "rejects_load_size",
        "rejects_truncated_header",
        "rejects_truncated_phdr",
        "rejects_file_range",
        "rejects_user_range",
        "rejects_address_overflow",
        "rejects_page_offset",
        "rejects_alignment",
        "rejects_reserved_range",
        "rejects_page_overlap",
        "rejects_program_header_limit",
        "rejects_wx",
        "rejects_dynamic",
        "rejects_interp",
        "rejects_orphan_tls_file",
        "bad_env_vector",
        "bad_env_string",
        "argument_count_limit",
        "combined_count_limit",
        "string_byte_limit",
        "exact_combined_count",
        "exact_string_bytes",
        "empty_vectors",
        "allocation_rollback",
        "mutable_snapshot_rollback",
        "boundary_load_plan",
        "source_version",
        "registration_gate",
        "shared_thread_gate",
    };
    for (const auto *name : names) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("users.busybox", [] {
    ut::register_test("ash_exit", empty_case);
    ut::register_test("ash_substitution", empty_case);
    ut::register_test("ash_exec_environment", empty_case);
    ut::register_test("text_pipeline", empty_case);
    ut::register_test("directory_lifecycle", empty_case);
    ut::register_test("file_redirection", empty_case);
    ut::register_test("file_copy", empty_case);
    ut::register_test("file_rename", empty_case);
    ut::register_test("application_workflow", empty_case);
    ut::register_test("head", empty_case);
    ut::register_test("cut", empty_case);
    ut::register_test("sort", empty_case);
    ut::register_test("uniq", empty_case);
    ut::register_test("tr", empty_case);
    ut::register_test("tee", empty_case);
    ut::register_test("cmp", empty_case);
    ut::register_test("basename", empty_case);
    ut::register_test("dirname", empty_case);
    ut::register_test("rmdir", empty_case);
    ut::register_test("uname", empty_case);
    ut::register_test("kill", empty_case);
    ut::register_test("find", empty_case);
    ut::register_test("find_rejects_unsupported", empty_case);
  });
  ut::register_suite("users.signals", [] {
    ut::register_test("basic_handler", empty_case);
    ut::register_test("nested_signals", empty_case);
    ut::register_test("sigchld", empty_case);
    ut::register_test("wait_registration", empty_case);
    ut::register_test("wait_interrupted", empty_case);
    ut::register_test("wait_restarted", empty_case);
    ut::register_test("cpu_bound_irq", empty_case);
    ut::register_test("stop_continue", empty_case);
    ut::register_test("sigprocmask", empty_case);
    ut::register_test("sigaltstack", empty_case);
    ut::register_test("sig_ign", empty_case);
    ut::register_test("invalid_arguments", empty_case);
    ut::register_test("frame_validation", empty_case);
    ut::register_test("altstack_overflow", empty_case);
    ut::register_test("altstack_boundaries", empty_case);
    ut::register_test("inheritance", empty_case);
    ut::register_test("pid_lifecycle", empty_case);
    ut::register_test("pipe_sigpipe", empty_case);
    ut::register_test("pipe_interrupted", empty_case);
    ut::register_test("pipe_restarted", empty_case);
    ut::register_test("pipe_noninterrupting_signals", empty_case);
    ut::register_test("pipe_partial_interrupt", empty_case);
    ut::register_test("signal_wakeup_affinity", empty_case);
    ut::register_test("console_interrupted", empty_case);
    ut::register_test("console_partial_interrupt", empty_case);
    ut::register_test("console_restarted", empty_case);
    ut::register_test("console_multi_reader", empty_case);
  });
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
  ut::register_suite("users.console_irq", [] { ut::register_test("irq_before_registration", empty_case); });
#endif
#if defined(MOSS_ARCH_X64)
  ut::register_suite("users.simd_fault", [] { ut::register_test("isolation", empty_case); });
#endif
  moss::test::validation::register_mm_permissions_cases();
  ut::register_suite("mm.transactions", [] {
    ut::register_test("user_copy_version", user_copy_version_binding);
    ut::register_test("raw_copy_fixup", raw_user_copy_fixup);
    ut::register_test("map_preserves_existing", map_preserves_existing);
    ut::register_test("map_allocation_rollback", map_allocation_rollback);
    ut::register_test("map_rejects_blocks", map_rejects_blocks);
    ut::register_test("clone_preserves_destination", clone_preserves_destination);
    ut::register_test("clone_allocation_rollback", clone_allocation_rollback);
    ut::register_test("address_space_heap_rollback", address_space_heap_rollback);
    ut::register_test("address_space_control_rollback", address_space_control_rollback);
    ut::register_test("vma_heap_rollback", vma_heap_rollback);
    ut::register_test("asid_leases", asid_leases);
    ut::register_test("unmap_reclaims_tables", unmap_reclaims_tables);
  });
  moss::test::validation::register_self_cases();
  ut::register_suite("self.fail", [] {
    ut::register_test("intentional_assertion", failing_case);
    ut::register_test("not_run", empty_case);
  });
  ut::register_suite("self.panic", [] { ut::register_test("intentional_panic", panic_case); });
  ut::register_suite("self.timeout", [] { ut::register_test("intentional_timeout", timeout_case); });
}

void start_case(const char *name) {
  active_case = name;
  ut::test_result::assertions_passed = 0;
  ut::test_result::assertions_failed = 0;
  Event("case_start").str("case", name).send();
}
void end_case() {
  failed = failed || ut::test_result::assertions_failed != 0;
  Event("case_end")
      .str("case", active_case)
      .number("passed", static_cast<u64>(ut::test_result::assertions_passed))
      .number("failed", static_cast<u64>(ut::test_result::assertions_failed))
      .send();
  ++completed;
  active_case = nullptr;
}

bench::Clock discover_clock() {
  bench::Clock result;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mrs %0, cntfrq_el0" : "=r"(result.frequency));
  result.source = "cntfrq_el0";
#elif defined(MOSS_ARCH_RISCV64)
  result.frequency = moss::fdt::get_platform_info().timebase_frequency;
  result.source = "dtb.timebase-frequency";
#else
  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
  // CPUID leaf 0x15 supplies crystal Hz (ECX) and the TSC/crystal ratio
  // EBX/EAX. A missing leaf or zero field requires independent calibration.
  if (eax >= 0x15) {
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x15), "c"(0));
    if (eax && ebx && ecx) {
      result.frequency = static_cast<u64>(ecx) * ebx / eax;
      result.source = "cpuid.15";
    }
  }
  if (!result.frequency) {
    auto out = [](u16 port, u8 value) { asm volatile("outb %0, %1" ::"a"(value), "Nd"(port)); };
    auto in = [](u16 port) {
      u8 value;
      asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
      return value;
    };
    auto count = [&] {
      // PC PIT command port 0x43 / channel-0 port 0x40. Command zero latches
      // channel 0, then low/high reads reconstruct one stable 16-bit sample.
      out(0x43, 0);
      u16 low = in(0x40);
      return static_cast<u16>(low | (static_cast<u16>(in(0x40)) << 8));
    };
    u64 frequencies[3]{};
    // Three independent samples let the median reject one outlier. The 5%
    // maximum spread below is a calibration acceptance policy, not precision proof.
    for (unsigned sample = 0; sample < 3; ++sample) {
      // 0x30 selects channel 0, low/high access, binary mode 0. Reload 0xffff
      // provides a finite countdown; an observed wrap invalidates the sample.
      out(0x43, 0x30);
      out(0x40, 0xFF);
      out(0x40, 0xFF);
      u16 first = count();
      u64 start = bench::read_counter();
      u16 last = first;
      // One million port-read attempts bounds a stalled PIT. Require at least
      // 16384 reference ticks (~13.7 ms); the 60000-tick ceiling stays below a
      // full 16-bit countdown. Exact retry/window tuning evidence is not recorded.
      for (u32 retry = 0; retry < 1000000 && first - last < 16384; ++retry) {
        last = count();
      }
      u64 end = bench::read_counter();
      if (last > first || first - last < 16384 || first - last > 60000 || end <= start) {
        return {};
      }
      result.calibration_ticks[sample] = end - start;
      result.reference_ticks[sample] = static_cast<u64>(first - last);
      // Convert using the PC PIT's 1,193,182 Hz reference, also QEMU's PIT_FREQ:
      // https://github.com/qemu/qemu/blob/master/include/hw/timer/i8254.h
      frequencies[sample] = (end - start) * 1193182ULL / result.reference_ticks[sample];
    }
    u64 low = frequencies[0], high = frequencies[0];
    for (u64 frequency : frequencies) {
      if (frequency < low) {
        low = frequency;
      }
      if (frequency > high) {
        high = frequency;
      }
    }
    if (!low || (high - low) * 100 > low * 5) {
      return {};
    }
    result.frequency = frequencies[0] + frequencies[1] + frequencies[2] - low - high;
    // Scale relative spread by 10^6 for ppm and add a policy margin of 1000 ppm
    // (0.1%); the margin has no recorded measurement-based derivation.
    result.uncertainty_ppm = static_cast<unsigned>((high - low) * 1000000 / low) + 1000;
    result.source = "pit.channel0";
  }
#endif
  u64 begin = bench::read_counter();
  u64 previous = begin;
  // Probe monotonicity 4096 times, then accept 1 kHz..100 GHz as a broad policy
  // sanity range. These finite checks do not establish counter accuracy or stability.
  for (unsigned i = 0; i < 4096; ++i) {
    u64 current = bench::read_counter();
    if (current < previous) {
      return {};
    }
    previous = current;
  }
  if (previous <= begin || result.frequency < 1000 || result.frequency > 100000000000ULL) {
    return {};
  }
  return result;
}

void record_batch(u64 ticks, usize operations, bool warmup, u64 overhead) {
  Event("batch")
      .number("index", sample_index++)
      .number("ticks", ticks)
      .number("operations", operations)
      .number("warmup", warmup ? 1 : 0)
      .number("overhead_ticks", overhead)
      .number("cpu", arch::get_current_cpu_id())
      .str("measurement_kind", ut::same_id(selection, "bench.signal") || ut::same_id(selection, "bench.timer") ||
                                       ut::same_id(selection, "bench.wakeup")
                                   ? "event_sum"
                                   : "elapsed_batch")
      .send();
}

struct TimerBenchmark {
  timer::HrTimer pending;
  process::Thread *sleeper = nullptr;
  u64 observed = 0;

  static void callback(void *data) noexcept {
    const auto now = bench::read_counter();
    auto &self = *static_cast<TimerBenchmark *>(data);
    __atomic_store_n(&self.observed, now, __ATOMIC_RELEASE);
    if (self.sleeper) {
      process::g_scheduler->task_wakeup(self.sleeper, 0);
    }
  }

  u64 measure(bool wakeup) {
    auto &subsystem = timer::TimerSubsystem::instance();
    // Arm 1 ms ahead and measure lateness after the expected counter value;
    // the programmed delay itself is not included in the returned latency.
    const auto deadline = subsystem.now_ns() + 1000000ULL;
    const auto expected = subsystem.clocksource().deadline_counter(deadline);
    observed = 0;
    sleeper = wakeup ? process::CfsScheduler::get_current_task() : nullptr;
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    if (sleeper) {
      sleeper->state = process::ProcessState::Sleeping;
    }
    pending.init(timer::TimerMode::OneShot, callback, this);
    if (!pending.start(deadline)) {
      if (sleeper) {
        sleeper->state = process::ProcessState::Running;
      }
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return 0;
    }
    u64 resumed = 0;
    if (sleeper) {
      process::g_scheduler->dequeue_task(sleeper);
      process::CfsScheduler::switch_to_bootstrap(sleeper->context);
      resumed = bench::read_counter();
    }
    if (restore_irqs) {
      arch::enable_interrupts();
    }
    while (!__atomic_load_n(&observed, __ATOMIC_ACQUIRE)) {
      arch::cpu_yield();
    }
    pending.cancel_sync();
    const auto fired = __atomic_load_n(&observed, __ATOMIC_ACQUIRE);
    if (!ut::expect(fired >= expected && affinity_valid() && (!sleeper || resumed > fired))) {
      return 0;
    }
    return sleeper ? resumed - fired : fired - expected;
  }
};

void timer_benchmark(bench::Context &context, bool wakeup) {
  TimerBenchmark fixture;
  const auto before = LifecycleResources::capture();
  auto batch = [&](usize count) {
    u64 ticks = 0;
    for (usize i = 0; i < count; ++i) {
      const auto elapsed = fixture.measure(wakeup);
      if (!elapsed) {
        context.valid = false;
      }
      ticks += elapsed;
    }
    return ticks;
  };
  // Cap event batches at 64 to bound repeated timer/scheduler setup. The pilot
  // uses a 1 ms sum-of-lateness target, matching the batch harness time scale.
  constexpr usize capacity = 64;
  if (context.iterations > capacity) {
    context.valid = false;
    return;
  }
  if (!context.iterations) {
    context.iterations = 1;
    while (context.valid && batch(context.iterations) < context.clock.frequency / 1000 &&
           context.iterations < capacity) {
      context.iterations *= 2;
    }
  }
  for (unsigned i = 0; context.valid && i < context.warmup + context.samples; ++i) {
    const auto ticks = batch(context.iterations);
    const auto begin = bench::read_counter();
    const auto overhead = (bench::read_counter() - begin) * context.iterations;
    context.valid = context.valid && LifecycleResources::capture() == before;
    if (context.valid) {
      record_batch(ticks, context.iterations, i < context.warmup, overhead);
    }
  }
}

void prepare_clock() {
  clock_info = discover_clock();
  Event("clock")
      .number("frequency", clock_info.frequency)
      .str("source", clock_info.source)
      .number("uncertainty_ppm", clock_info.uncertainty_ppm)
      .send();
  for (unsigned i = 0; i < 3; ++i) {
    if (clock_info.reference_ticks[i]) {
      Event("calibration")
          .number("index", i)
          .number("ticks", clock_info.calibration_ticks[i])
          .number("reference_ticks", clock_info.reference_ticks[i])
          .number("reference_frequency", 1193182)
          .send();
    }
  }
  if (!clock_info.frequency) {
    failed = true;
    finish("invalid_clock");
  }
}

void allocation_benchmark(bench::Context &context, unsigned mode) {
  // Track the Context's default 256 operations without heap bookkeeping.
  // Orders 0..4 bound each block to 1..16 pages, keeping the fixture workload
  // separate from the allocator's larger production MAX_ORDER limit.
  PhysAddr pages_owned[256]{};
  bool valid = true;
  usize order = static_cast<usize>(numeric_option("moss.order", 0));
  if (order > 4) {
    failed = true;
    finish("invalid_order");
  }
  auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  context.measure_batches(
      [&](usize count) {
        valid = true;
        for (usize i = 0; i < count; ++i) {
          pages_owned[i] = 0;
          if (mode == 1) {
            auto page = mm::PageFrameAllocator::allocate_pages(order);
            if (!page) {
              valid = false;
              break;
            }
            pages_owned[i] = *page;
          }
        }
        return valid;
      },
      [&](usize i) {
        if (mode == 1) {
          if (!mm::PageFrameAllocator::free_pages(pages_owned[i], order)) {
            valid = false;
          } else {
            pages_owned[i] = 0;
          }
        } else {
          auto page = mm::PageFrameAllocator::allocate_pages(order);
          if (!page) {
            valid = false;
            return;
          }
          pages_owned[i] = *page;
          asm volatile("" : "+m"(pages_owned[i]) : : "memory");
          if (mode == 2) {
            if (!mm::PageFrameAllocator::free_pages(pages_owned[i], order)) {
              valid = false;
            } else {
              pages_owned[i] = 0;
            }
          }
        }
      },
      [&](usize count) {
        for (usize i = 0; i < count; ++i) {
          if (pages_owned[i]) {
            valid = static_cast<bool>(mm::PageFrameAllocator::free_pages(pages_owned[i], order)) && valid;
            pages_owned[i] = 0;
          }
        }
        return valid && mm::PageFrameAllocator::get_memory_stats().free_pages == before && affinity_valid();
      });
  failed = !context.valid;
}

void read_benchmark(bench::Context &context) {
  void *table = fd_table();
  long fd = vfs::syscall::do_open(table, "/fixture.bin", 0, 0);
  if (fd < 0) {
    failed = true;
    return;
  }
  // 256 operations each read 256 bytes: 65536 bytes cover the largest batch.
  // Static storage keeps this buffer off the 16 KiB kernel stack.
  static u8 bytes[65536];
  bool valid = true;
  // Each timed operation reads a separate 256-byte slice of the fixture.
  context.measure_batches(
      [&](usize) {
        valid = true;
        return vfs::syscall::do_lseek(table, fd, 0, 0) == 0;
      },
      [&](usize i) {
        valid = (vfs::syscall::do_read(table, fd, vfs::OutputBuffer::kernel(bytes + i * 256, 256)) == 256) && valid;
      },
      [&](usize count) {
        for (usize i = 0; i < count * 256; ++i) {
          valid = valid && bytes[i] == i % 256;
        }
        return valid && affinity_valid();
      });
  failed = !context.valid;
  failed = vfs::syscall::do_close(table, fd) != 0 || failed;
}
} // namespace

// Keep validation protocol state here while split suites share its case lifecycle.
namespace moss::test::validation {
const char *selected_suite() { return ::selection; }
const char *running_case() { return ::active_case; }
bool boot_option(const char *key, char *out, usize capacity) { return ::option(key, out, capacity); }
bool affinity_valid() { return ::affinity_valid(); }
void start_case(const char *name) { ::start_case(name); }
void end_case() { ::end_case(); }
[[noreturn]] void finish(const char *reason) { ::finish(reason); }
[[noreturn]] void invalid_control() {
  ::failed = true;
  ::finish("invalid_control");
}
} // namespace moss::test::validation

extern "C" void moss_validation_boot() noexcept {
  if (!option("moss.validation", selection, sizeof(selection)) || !ut::valid_id(selection)) {
    failed = true;
    finish("invalid_selection");
  }
  warmup_count = static_cast<unsigned>(numeric_option("moss.warmup", 5));
  sample_count = static_cast<unsigned>(numeric_option("moss.samples", 30));
  fixed_iterations = static_cast<usize>(numeric_option("moss.iterations", 0));
  const u64 stability_option = numeric_option("moss.stability", 0);
  if (stability_option > 1 || (stability_option && !is_lifecycle())) {
    failed = true;
    finish("invalid_stability_parameters");
  }
  stability = stability_option == 1;
  // Policy ceilings bound serial output and run time; 65536 also matches the
  // largest userspace syscall fixture capacity. They are not measured accuracy limits.
  if (!sample_count || sample_count > 1000 || warmup_count > 100 || fixed_iterations > 65536) {
    failed = true;
    finish("invalid_parameters");
  }
  declare_cases();
  bench::register_benchmark("bench.allocate", [](bench::Context &context) { allocation_benchmark(context, 0); });
  bench::register_benchmark("bench.release", [](bench::Context &context) { allocation_benchmark(context, 1); });
  bench::register_benchmark("bench.combined", [](bench::Context &context) { allocation_benchmark(context, 2); });
  bench::register_benchmark("bench.read", read_benchmark);
  bench::register_benchmark("bench.getpid", [](bench::Context &) {});
  for (const char *name : user_benchmark_names) {
    bench::register_benchmark(name, [](bench::Context &) {});
  }
  bench::register_benchmark("bench.wakeup", [](bench::Context &context) { timer_benchmark(context, true); });
  bench::register_benchmark("bench.timer", [](bench::Context &context) { timer_benchmark(context, false); });
  if (bench::registry.error) {
    failed = true;
    finish(bench::registry.error);
  }
  if (ut::registry.error) {
    failed = true;
    finish(ut::registry.error);
  }
  const auto &info = moss::fdt::get_platform_info();
  Event("ready")
      .number("detected_cpus", info.cpu_count)
      .number("online_mask", __atomic_load_n(&moss::boot::online_cpu_mask, __ATOMIC_ACQUIRE))
      .number("work_mask", __atomic_load_n(&moss::boot::cpu_work_mask, __ATOMIC_ACQUIRE))
      .number("ram_bytes", info.total_memory_size)
      .number("managed_pages", mm::PageFrameAllocator::get_memory_stats().total_pages)
      .send();
  for (unsigned i = 0; i < ut::registry.case_count; ++i) {
    const auto &item = ut::registry.cases[i];
    if (ut::same_id(item.suite_name, selection)) {
      Event("catalog").str("case", item.name).send();
      ++selected_count;
    }
  }
  for (unsigned i = 0; i < bench::registry.count; ++i) {
    if (ut::same_id(selection, bench::registry.scenarios[i].name)) {
      selected_benchmark = &bench::registry.scenarios[i];
      selected_count = 1;
      Event("catalog").str("case", selection).send();
    }
  }
  if (!selected_count) {
    failed = true;
    finish("empty_selection");
  }
}

extern "C" void moss_validation_dispatch_selected() noexcept {
  if (is_lifecycle() && active_case) {
    dispatch_boundary_checked = true;
    // A timer IRQ must not consume this cached selection before it is dispatched.
    ut::expect(!arch::interrupts_enabled());
  }
}

extern "C" void moss_validation_user_return(void *raw_frame) noexcept {
  const auto pid = __atomic_load_n(&cpu_bound_probe_pid, __ATOMIC_ACQUIRE);
  if (pid == INVALID_PROCESS_ID || !ut::same_id(active_case, "cpu_bound_irq")) {
    return;
  }
  auto *thread = process::CfsScheduler::get_current_task();
  auto &frame = *static_cast<moss::abi::TrapFrame *>(raw_frame);
  // The child performs no syscall inside this registered PC interval. Seeing
  // its PC here proves that an IRQ used the common user-return checkpoint.
  if (thread && thread->owner_pid == pid && arch::get_current_cpu_id() == 1 &&
      frame.pc >= cpu_bound_probe_start && frame.pc < cpu_bound_probe_end) {
    __atomic_fetch_add(&cpu_bound_irq_seen, 1U, __ATOMIC_RELEASE);
  }
}

extern "C" void moss_validation_sleep_armed(void *pending) noexcept {
  auto *thread = process::CfsScheduler::get_current_task();
  if (!thread || thread != early_sleep_thread) {
    return;
  }
  ++early_sleep_visits;
  auto &armed = *static_cast<timer::HrTimer *>(pending);
  // Local IRQs are masked by the real sleep syscall. Another CPU must expire
  // its timer before this caller saves its context. The host bounds the wait.
  while (armed.is_active()) {
    arch::cpu_yield();
  }
  armed.cancel_sync();
  early_sleep_woken = early_sleep_woken && !arch::interrupts_enabled() &&
                      (thread->state == process::ProcessState::Ready || thread->sleep_handoff.load() == 2);
}

extern "C" void moss_validation_wait_before_register(u32 parent_pid, long wait_pid) noexcept {
  if (!ut::same_id(active_case, "wait_registration") || parent_pid != wait_exit_parent || wait_pid <= 1 ||
      wait_pid > static_cast<long>(~ProcessId{0}) || __atomic_load_n(&wait_exit_phase, __ATOMIC_ACQUIRE) != 1) {
    return;
  }
  // The child can leave its gate only after the first Zombie scan missed it.
  wait_exit_child = static_cast<ProcessId>(wait_pid);
  __atomic_store_n(&wait_exit_phase, 2U, __ATOMIC_RELEASE);
  moss::test::validation::wait_for_phase(wait_exit_phase, 3);
}

extern "C" void moss_validation_child_exit_notified(u32 child_pid, u32 parent_pid) noexcept {
  if (ut::same_id(active_case, "wait_registration") && parent_pid == wait_exit_parent && child_pid == wait_exit_child &&
      __atomic_load_n(&wait_exit_phase, __ATOMIC_ACQUIRE) == 2) {
    // The real exit path has published Zombie and attempted wakeup while no
    // waiter exists; release/acquire also makes its status visible to wait4.
    __atomic_store_n(&wait_exit_phase, 3U, __ATOMIC_RELEASE);
  }
}

extern "C" void moss_validation_fd_clone(bool entering) noexcept {
  if (!fork_clone_pressure || fork_clone_exhausted) {
    return;
  }
  if (entering) {
    ut::expect(fork_clone_pressure->acquire(sizeof(vfs::FdTable)));
  } else {
    fork_clone_pressure->release();
    fork_clone_exhausted = true;
  }
}

extern "C" void moss_validation_fork_metadata(unsigned stage, bool entering) noexcept {
  auto *probe = fork_metadata_pressure;
  if (!probe || probe->stage != stage || probe->exhausted) {
    return;
  }
  if (entering) {
    // Fail after one successful VMA copy, not only before any work is owned.
    if (stage == 0 && probe->vmas++ == 0) {
      return;
    }
    // Keep the fork hook's stage order: VMA=0, Thread=1, ThreadEntry=2.
    usize size = sizeof(void *);
    if (stage == 0) {
      size = sizeof(process::VmaRegion);
    } else if (stage == 1) {
      size = sizeof(process::Thread);
    } else if (stage == 2) {
      size = sizeof(process::ThreadEntry);
    }
    probe->holding = true;
    ut::expect(probe->heap.acquire(size));
  } else if (probe->holding) {
    probe->heap.release();
    probe->holding = false;
    probe->exhausted = true;
  }
}

extern "C" void moss_validation_address_space_allocation(bool entering, bool control_block, usize size) noexcept {
  if (!address_space_control_pressure || !control_block) {
    return;
  }
  if (entering) {
    // The AddressSpace object already exists. Exhaust the real allocator for
    // the exact control-block request, not a synthetic failure return value.
    address_space_control_exhausted = ut::expect(address_space_control_pressure->acquire(size));
  } else {
    address_space_control_pressure->release();
  }
}

extern "C" void moss_validation_exec_allocation(unsigned stage, bool entering, usize allocation_size) noexcept {
  auto *probe = exec_allocation_pressure;
  if (!probe || probe->stage != stage) {
    return;
  }
  if (entering) {
    if (probe->holding || probe->exhausted) {
      return;
    }
    // The final stage is the VMA-node boundary. Let one node become owned first so the
    // failure proves cleanup of a partially prepared address space.
    if (stage == EXEC_VMA_ALLOCATION_STAGE && probe->vmas++ == 0) {
      return;
    }
    probe->holding = true;
    probe->exhausted = ut::expect(probe->heap.acquire(allocation_size));
  } else if (probe->holding) {
    probe->heap.release();
    probe->holding = false;
  }
}

namespace {
unsigned exec_source_swaps = 0;
bool exec_registration_refused = false;
}

extern "C" void moss_validation_exec_source_snapshot(PhysAddr root, VirtAddr argv) noexcept {
  if (!ut::same_id(selection, "users.exec")) {
    return;
  }
  if (ut::same_id(active_case, "registration_gate")) {
    auto owner = process::current_process();
    auto *peer = owner ? process::Thread::try_create(process::Process::allocate_thread_id(), owner->pid()) : nullptr;
    if (!ut::expect(owner && peer)) {
      return;
    }
    auto registered = owner->register_thread(peer);
    exec_registration_refused = !registered && registered.error() == ErrorCode::InvalidState &&
                                owner->thread_count() == 1;
    if (!registered) {
      delete peer;
    }
    return;
  }
  if (!ut::same_id(active_case, "source_version")) {
    return;
  }
  auto owner = process::current_process();
  auto source = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
  const VirtAddr page = argv & ~(VirtAddr{page_size} - 1);
  if (!ut::expect(source && source->pgd_phys == root && argv - page <= page_size - sizeof(VirtAddr))) {
    return;
  }
  auto replacement = process::user_space::create_user_address_space();
  auto frame = mm::allocate_pages(0);
  if (!ut::expect(replacement && frame)) {
    if (frame) {
      (void)mm::free_pages(*frame, 0);
    }
    return;
  }
  auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(*frame));
  if (!ut::expect(source->copy_from_user(bytes, page, page_size) == 0)) {
    (void)mm::free_pages(*frame, 0);
    return;
  }
  VirtAddr name = 0;
  __builtin_memcpy(&name, bytes + argv - page, sizeof(name));
  if (!ut::expect(name >= page && name - page <= page_size - sizeof("fail"))) {
    (void)mm::free_pages(*frame, 0);
    return;
  }
  __builtin_memcpy(bytes + name - page, "fail", sizeof("fail"));
  if (!ut::expect((*replacement)->add_vma(page, page + page_size, process::vma_flags::READ | process::vma_flags::WRITE,
                                          process::VmaType::MMAP) &&
                  mm::PageTableManager::map_user_page((*replacement)->pgd_phys, page, *frame,
                                                      mm::page_perms::USER_RW))) {
    (void)mm::free_pages(*frame, 0);
    return;
  }
  // The syscall remains on its retained old hardware root until exec commits;
  // no user instruction runs from this deliberately incomplete replacement.
  (void)owner->set_address_space(moss::move(*replacement));
  ++exec_source_swaps;
}

extern "C" long moss_validation_call(long op, long arg1, [[maybe_unused]] long arg2) noexcept {
  // These opcodes and returned mode IDs form the private validation protocol
  // shared with src/userspace validation programs, not Linux syscall numbers.
  // Keep both endpoints in sync: opcode zero performs the startup handshake;
  // later operations drive suite-specific ownership checks and benchmarks.
  // Invoked only after the production scheduler and a real userspace exec.
  if (op == 0) {
    if (arg1 != 0 || !affinity_valid()) {
      failed = true;
      finish("affinity");
    }
    Event("worker").number("cpu", arch::get_current_cpu_id()).number("affinity", 1).send();
    if (ut::same_id(selection, "users")) {
      return 1;
    }
    if (ut::same_id(selection, "users.vm")) {
      return 5;
    }
    if (ut::same_id(selection, "users.frame")) {
      return 6;
    }
    if (ut::same_id(selection, "users.uaccess")) {
      return 7;
    }
    if (ut::same_id(selection, "users.signals")) {
      return 8;
    }
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
    if (ut::same_id(selection, "users.console_irq")) {
      return 24;
    }
#endif
    if (ut::same_id(selection, "users.lifecycle")) {
      return 9;
    }
    if (ut::same_id(selection, "users.applications")) {
      return 23;
    }
    if (ut::same_id(selection, "users.timers")) {
      return 10;
    }
    if (ut::same_id(selection, "users.ipc")) {
      return 25;
    }
    if (ut::same_id(selection, "users.libc")) {
      return 20;
    }
    if (ut::same_id(selection, "users.exec")) {
      return 22;
    }
    if (ut::same_id(selection, "users.busybox")) {
      return 21;
    }
    if (ut::same_id(selection, "users.simd_fault")) {
      start_case("isolation");
      return 4;
    }
    if (const long mode = moss::test::validation::start_smp_suite()) {
      return mode;
    }
    if (selected_benchmark) {
      start_case(selection);
      prepare_clock();
      if (const long mode = user_benchmark_mode()) {
#if defined(MOSS_ARCH_ARM64)
        u64 control;
        asm volatile("mrs %0, cntkctl_el1" : "=r"(control));
        control |= 2; // EL0VCTEN: userspace brackets the real syscall round trip.
        asm volatile("msr cntkctl_el1, %0; isb" ::"r"(control) : "memory");
#endif
        syscall_pilot = fixed_iterations == 0;
        syscall_capacity = mode == 2 || mode == 13 || mode == 15 || mode == 16 ? 65536 : 64;
        if (fixed_iterations > syscall_capacity) {
          failed = true;
          finish("invalid_iterations");
        }
        syscall_iterations = fixed_iterations ? fixed_iterations : 1;
        return mode;
      }
      // Restore ordinary interrupts during the in-kernel workload. The pinned
      // thread still uses the real scheduler, locks, and syscall return path.
      arch::enable_interrupts();
      bench::Context context{.clock = clock_info,
                             .iterations = fixed_iterations,
                             .capacity = 256,
                             .warmup = warmup_count,
                             .samples = sample_count,
                             .cpu = 0,
                             .valid = true,
                             .record = record_batch};
      selected_benchmark->run(context);
      failed = failed || !context.valid;
      if (failed) {
        ++ut::test_result::assertions_failed;
      }
      end_case();
      finish();
    }
    arch::enable_interrupts();
    for (unsigned i = 0; i < ut::registry.case_count && !failed; ++i) {
      auto &item = ut::registry.cases[i];
      if (ut::same_id(item.suite_name, selection)) {
        start_case(item.name);
        item.test_function();
        end_case();
      }
    }
    finish();
  }
  const bool user_suite = ut::same_id(selection, "users") || ut::same_id(selection, "users.vm") ||
                          ut::same_id(selection, "users.frame") || ut::same_id(selection, "users.uaccess") ||
                          ut::same_id(selection, "users.signals") || ut::same_id(selection, "users.console_irq") ||
                          is_lifecycle() || ut::same_id(selection, "users.timers") ||
                          ut::same_id(selection, "users.ipc") ||
                          ut::same_id(selection, "users.libc") || ut::same_id(selection, "users.busybox") ||
                          ut::same_id(selection, "users.exec");
  if (op == 1 && user_suite && !failed && !active_case && arg1 == static_cast<long>(completed) && arg1 >= 0) {
    long index = 0;
    for (unsigned i = 0; i < ut::registry.case_count; ++i) {
      const auto &item = ut::registry.cases[i];
      if (ut::same_id(item.suite_name, selection) && index++ == arg1) {
        start_case(item.name);
        return 0;
      }
    }
  }
  if (op == 2 && active_case && (user_suite || ut::same_id(selection, "users.simd_fault"))) {
    if (arg2 != 0) {
      logging::klog::error("users checks failed: mask={:#x}", static_cast<u64>(arg2));
    }
    ut::expect(arg1 != 0 && affinity_valid());
    if (is_lifecycle()) {
      ut::expect(lifecycle_complete && dispatch_boundary_checked);
    }
    if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "source_version")) {
      ut::expect(exec_source_swaps == 1);
    }
    if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "registration_gate")) {
      ut::expect(exec_registration_refused);
    }
    end_case();
    return failed ? 0 : 1;
  }
  if (op == 3) {
    finish();
  }
  const bool vm_isolation = ut::same_id(selection, "users.vm") && arg1 >= 0 &&
                            static_cast<usize>(arg1) < sizeof(kernel_isolation_cases) / sizeof(const char *) &&
                            ut::same_id(active_case, kernel_isolation_cases[arg1]);
  // Signal attacks reuse the existing mapped kernel-data sentinel (target 2),
  // not a guessed address whose rejection could merely mean "unmapped".
  const bool signal_isolation =
      ut::same_id(selection, "users.signals") && arg1 == 2 &&
      (ut::same_id(active_case, "frame_validation") || ut::same_id(active_case, "altstack_boundaries"));
  if ((vm_isolation || signal_isolation) && active_case && affinity_valid()) {
    if (op == ISOLATION_PREPARE) {
      return kernel_isolation.prepare(arg1, arg2);
    }
    if (op == ISOLATION_VERIFY) {
      return kernel_isolation.verify(arg1, arg2);
    }
  }
  if (ut::same_id(selection, "users") && ut::same_id(active_case, "fork_fd_allocation_rollback") && affinity_valid()) {
    if (op == 40 && !fork_clone_pressure) {
      fork_clone_baseline = LifecycleResources::capture();
      fork_clone_exhausted = false;
      fork_clone_pressure = new HeapPressure{};
      return 1;
    }
    if (op == 41 && fork_clone_pressure) {
      const bool exercised = ut::expect(fork_clone_exhausted);
      delete fork_clone_pressure;
      fork_clone_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == fork_clone_baseline) && exercised;
    }
  }
  if (ut::same_id(selection, "users") && ut::same_id(active_case, "fork_metadata_allocation_rollback") &&
      affinity_valid()) {
    if (op == 46 && arg1 >= 0 && arg1 < 4 && !fork_metadata_pressure) {
      const auto baseline = LifecycleResources::capture();
      fork_metadata_pressure = new ForkMetadataPressure{};
      fork_metadata_pressure->baseline = baseline;
      fork_metadata_pressure->stage = static_cast<unsigned>(arg1);
      return 1;
    }
    if (op == 47 && fork_metadata_pressure) {
      const auto baseline = fork_metadata_pressure->baseline;
      const bool exercised = ut::expect(fork_metadata_pressure->exhausted && !fork_metadata_pressure->holding);
      delete fork_metadata_pressure;
      fork_metadata_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == baseline) && exercised;
    }
  }
  const bool mmap_pressure_case = ut::same_id(active_case, "mmap_heap_rollback");
  if (ut::same_id(selection, "users") &&
      (mmap_pressure_case || ut::same_id(active_case, "fork_process_allocation_rollback")) && affinity_valid()) {
    if (op == (mmap_pressure_case ? 42 : 44) && !user_heap_pressure) {
      user_heap_baseline = LifecycleResources::capture();
      user_heap_pressure = new HeapPressure{};
      if (user_heap_pressure->acquire(mmap_pressure_case ? sizeof(process::VmaRegion) : sizeof(process::Process))) {
        return 1;
      }
      delete user_heap_pressure;
      user_heap_pressure = nullptr;
      return 0;
    }
    if (op == (mmap_pressure_case ? 43 : 45) && user_heap_pressure) {
      delete user_heap_pressure;
      user_heap_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == user_heap_baseline);
    }
  }
  // 52 arms the case, 53 gates the migrated child, and 54 verifies the wake
  // ordering before clearing the fixture for later cases.
  if (ut::same_id(selection, "users.signals") && ut::same_id(active_case, "wait_registration")) {
    if (op == 52 && affinity_valid() && arg1 == 0 && g_num_cpus >= 2 &&
        __atomic_load_n(&wait_exit_phase, __ATOMIC_ACQUIRE) == 0) {
      auto parent = process::current_process();
      if (!parent) {
        return 0;
      }
      wait_exit_parent = parent->pid();
      wait_exit_child = INVALID_PROCESS_ID;
      __atomic_store_n(&wait_exit_phase, 1U, __ATOMIC_RELEASE);
      return 1;
    }
    if (op == 53 && arch::get_current_cpu_id() == 1) {
      moss::test::validation::wait_for_phase(wait_exit_phase, 2);
      auto child = process::current_process();
      if (!child || child->pid() != wait_exit_child || child->parent_pid() != wait_exit_parent) {
        return 0;
      }
      return 1;
    }
    if (op == 54 && affinity_valid()) {
      const bool observed =
          __atomic_load_n(&wait_exit_phase, __ATOMIC_ACQUIRE) == 3 && arg1 == static_cast<long>(wait_exit_child);
      __atomic_store_n(&wait_exit_phase, 0U, __ATOMIC_RELEASE);
      wait_exit_parent = INVALID_PROCESS_ID;
      wait_exit_child = INVALID_PROCESS_ID;
      return observed;
    }
  }
  if (op == 55 && ut::same_id(selection, "users.signals") &&
      (ut::same_id(active_case, "wait_interrupted") || ut::same_id(active_case, "wait_restarted")) &&
      arch::get_current_cpu_id() == 1) {
    auto child = process::current_process();
    if (!child || arg1 != static_cast<long>(child->parent_pid()) || arg2 != static_cast<long>(child->pid())) {
      return -1;
    }
    auto parent = process::g_process_manager->find_process(child->parent_pid());
    auto *thread = parent ? parent->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
    auto *frame = thread->trap_frame;
    // Native syscall 13 is waitpid; inspect the real blocked frame rather
    // than treating the child's readiness or a preceding syscall as proof.
    return thread->state == process::ProcessState::Sleeping && thread->sleep_handoff.load() == 0 && frame &&
           frame->syscall_number() == 13 && frame->argument(0) == static_cast<u64>(arg2);
  }
  if (ut::same_id(selection, "users.signals") && ut::same_id(active_case, "cpu_bound_irq")) {
    if (op == CPU_BOUND_ARM_PROBE && arch::get_current_cpu_id() == 1 && arg1 > 0 && arg2 > arg1) {
      auto child = process::current_process();
      if (!child) {
        return -1;
      }
      cpu_bound_probe_start = static_cast<u64>(arg1);
      cpu_bound_probe_end = static_cast<u64>(arg2);
      __atomic_store_n(&cpu_bound_irq_seen, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&cpu_bound_probe_pid, child->pid(), __ATOMIC_RELEASE);
      return 1;
    }
    if (op == CPU_BOUND_CHECK_PROBE && affinity_valid() && arg1 == static_cast<long>(cpu_bound_probe_pid)) {
      if (__atomic_load_n(&cpu_bound_irq_seen, __ATOMIC_ACQUIRE) < 2) {
        return 0;
      }
      __atomic_store_n(&cpu_bound_probe_pid, INVALID_PROCESS_ID, __ATOMIC_RELEASE);
      return 1;
    }
  }
  if ((op == STOP_STATE_PROBE || op == STOP_PENDING_PROBE) && ut::same_id(selection, "users.signals") &&
      ut::same_id(active_case, "stop_continue")) {
    if (!affinity_valid() || arg1 <= 0 || arg1 > static_cast<long>(~ProcessId{0}) || (arg2 != 0 && arg2 != 1)) {
      return -1;
    }
    auto child = process::g_process_manager->find_process(static_cast<ProcessId>(arg1));
    auto *thread = child ? child->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    if (op == STOP_PENDING_PROBE) {
      const u64 stop_mask = process::sig::sigmask(process::sig::SIGTSTP);
      const u64 cont_mask = process::sig::sigmask(process::sig::SIGCONT);
      const u64 expected = arg2 == 0 ? stop_mask : cont_mask;
      return (thread->signal_mask & (stop_mask | cont_mask)) == (stop_mask | cont_mask) &&
             (thread->pending_signals & (stop_mask | cont_mask)) == expected;
    }
    // A state label alone is insufficient: the old STOP path marked the
    // thread Stopped while continuing to execute its user return frame.
    if (thread->state != process::ProcessState::Stopped || thread->sleep_handoff.load() != 0 ||
        process::CfsScheduler::get_current_task_on_cpu(1) == thread) {
      return 0;
    }
    return arg2 == 0 || (thread->pending_signals & process::sig::sigmask(process::sig::SIGUSR1)) != 0;
  }
  if (op == 56 && ut::same_id(selection, "users.timers") && arch::get_current_cpu_id() == 1) {
    const long mode = ut::same_id(active_case, "relative_interrupted")         ? 0
                      : ut::same_id(active_case, "clock_relative_interrupted") ? 1
                      : ut::same_id(active_case, "clock_absolute_interrupted") ? 2
                                                                               : -1;
    auto child = process::current_process();
    if (!child || mode != arg2 || arg1 != static_cast<long>(child->parent_pid())) {
      return -1;
    }
    auto parent = process::g_process_manager->find_process(child->parent_pid());
    auto *thread = parent ? parent->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
    auto *frame = thread->trap_frame;
    // Native syscall numbers 86/87 are nanosleep/clock_nanosleep. Inspect the
    // actual frame after its sleep handoff completes, not a guessed delay.
    return thread->state == process::ProcessState::Sleeping && thread->sleep_handoff.load() == 0 && frame &&
           frame->syscall_number() == (mode == 0 ? 86U : 87U) &&
           (mode == 0 || frame->argument(1) == static_cast<u64>(mode == 2));
  }
  if (op == 57 && ut::same_id(selection, "users.signals") && ut::same_id(active_case, "console_multi_reader") &&
      affinity_valid()) {
    auto parent = process::current_process();
    if (!parent || arg1 <= 1 || arg2 <= 1 || arg1 > ~ProcessId{0} || arg2 > ~ProcessId{0} || arg1 == arg2) {
      return -1;
    }
    const long pids[] = {arg1, arg2};
    for (long pid : pids) {
      auto child = process::g_process_manager->find_process(static_cast<ProcessId>(pid));
      if (!child || child->parent_pid() != parent->pid()) {
        return -1;
      }
      auto *thread = child->get_main_thread();
      if (!thread) {
        return -1;
      }
      containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
      if (thread->state == process::ProcessState::Zombie) {
        return -1;
      }
#if !defined(MOSS_ARCH_RISCV64)
      if (thread->state != process::ProcessState::Sleeping || thread->sleep_handoff.load() != 0) {
        return 0;
      }
#endif
      auto *frame = thread->trap_frame;
      if (!frame || frame->syscall_number() != 32) { // Native read syscall.
        return 0;
      }
    }
    return 1;
  }
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
  if (op == 58 && ut::same_id(selection, "users.console_irq") && ut::same_id(active_case, "irq_before_registration") &&
      affinity_valid()) {
    if (arg1 == 0 && arg2 == 0) {
      if (g_num_cpus < 2 || !drivers::console::is_initialized()) {
        return -1;
      }
      __atomic_store_n(&console_reader_at_gap, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&console_irq_before_lock, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&console_irq_cpu, ~u32{0}, __ATOMIC_RELEASE);
      __atomic_store_n(&console_irq_probe_armed, 1U, __ATOMIC_RELEASE);
      return 1;
    }
    if (arg1 == -1 && arg2 == 0) {
      __atomic_store_n(&console_irq_probe_armed, 0U, __ATOMIC_RELEASE);
      return __atomic_load_n(&console_reader_at_gap, __ATOMIC_ACQUIRE) == 1 &&
                     __atomic_load_n(&console_irq_before_lock, __ATOMIC_ACQUIRE) == 1 &&
                     __atomic_load_n(&console_irq_cpu, __ATOMIC_ACQUIRE) == 0
                 ? 1
                 : -1;
    }
    auto parent = process::current_process();
    if (!parent || arg1 <= 1 || arg1 > ~ProcessId{0} || arg2 != 0) {
      return -1;
    }
    auto child = process::g_process_manager->find_process(static_cast<ProcessId>(arg1));
    if (!child || child->parent_pid() != parent->pid()) {
      return -1;
    }
    return __atomic_load_n(&console_reader_at_gap, __ATOMIC_ACQUIRE) == 1 ? 1 : 0;
  }
#endif
  if (op == 39 && ut::same_id(selection, "users.signals") &&
      (ut::same_id(active_case, "pipe_interrupted") || ut::same_id(active_case, "pipe_restarted") ||
       ut::same_id(active_case, "pipe_noninterrupting_signals") ||
       ut::same_id(active_case, "console_interrupted") || ut::same_id(active_case, "console_partial_interrupt") ||
       ut::same_id(active_case, "console_restarted"))) {
    if (arg1 == 0 && arg2 == 0) {
      return 1; // The production image's weak hook still returns ENOSYS.
    }
    auto caller = process::current_process();
    if (!caller || arg1 != static_cast<long>(caller->parent_pid()) || arg2 < 0 || arg2 >= vfs::MAX_FDS) {
      return -1;
    }
    auto parent = process::g_process_manager->find_process(caller->parent_pid());
    auto *thread = parent ? parent->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
    bool require_sleep = true;
#if defined(MOSS_ARCH_RISCV64)
    // RV64 still polls console RX; its active read frame is the readiness boundary.
    require_sleep =
        !ut::same_id(active_case, "console_interrupted") && !ut::same_id(active_case, "console_partial_interrupt") &&
        !ut::same_id(active_case, "console_restarted");
#endif
    if (require_sleep && (thread->state != process::ProcessState::Sleeping || thread->sleep_handoff.load() != 0)) {
      return 0;
    }
    // Do not mistake the parent's preparation nanosleep for the intended I/O.
    auto *frame = thread->trap_frame;
    return frame && (frame->syscall_number() == 32 || frame->syscall_number() == 33) &&
           frame->argument(0) == static_cast<u64>(arg2);
  }
  if (op == 38 && arg1 == 0 && arg2 == 0 && ut::same_id(selection, "users.libc") &&
      ut::same_id(active_case, "filesystem_permissions")) {
    auto owner = process::current_process();
    if (!owner || owner->euid() != 0) {
      return -1;
    }
    // Test precondition, not a production identity-management interface.
    owner->set_uid(99);
    owner->set_gid(99);
    return 0;
  }
  if (op == EXEC_REGISTER_DORMANT_PEER && ut::same_id(selection, "users.exec") &&
      ut::same_id(active_case, "shared_thread_gate") &&
      affinity_valid()) {
    auto owner = process::current_process();
    auto *peer = owner ? process::Thread::try_create(process::Process::allocate_thread_id(), owner->pid()) : nullptr;
    if (!peer) {
      return 0;
    }
    if (!owner->register_thread(peer)) {
      delete peer;
      return 0;
    }
    // The peer is registered but never enqueued, so this probes exec admission
    // without running an unsupported shared user-root or second user stack.
    return owner->thread_count() == 2 ? 1 : 0;
  }
  if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "mutable_snapshot_rollback") &&
      affinity_valid()) {
    auto owner = process::current_process();
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    if (op == EXEC_HEAP_PRESSURE_ARM && !exec_allocation_pressure && as && arg1 >= 0 &&
        arg1 < EXEC_HEAP_ALLOCATION_STAGES) {
      // Capture before allocating the probe so release can require an exact
      // return to the caller's pre-injection ownership state.
      const auto baseline = LifecycleResources::capture();
      auto *probe = new ExecAllocationPressure{};
      if (!probe) {
        return 0;
      }
      probe->baseline = baseline;
      probe->original = as.get();
      probe->root = as->pgd_phys;
      probe->root_hash = page_table_hash(probe->root);
      __builtin_memcpy(probe->name, owner->name(), sizeof(probe->name));
      probe->stage = static_cast<unsigned>(arg1);
      exec_allocation_pressure = probe;
      return 1;
    }
    if (op == EXEC_HEAP_PRESSURE_RELEASE && exec_allocation_pressure) {
      auto *probe = exec_allocation_pressure;
      bool valid = ut::expect(arg1 >= 0 && static_cast<unsigned>(arg1) == probe->stage);
      valid = ut::expect(probe->exhausted && !probe->holding) && valid;
      if (probe->stage == EXEC_VMA_ALLOCATION_STAGE) {
        valid = ut::expect(probe->vmas >= 2) && valid;
      }
      valid = ut::expect(as && as.get() == probe->original && as->pgd_phys == probe->root &&
                         page_table_hash(probe->root) == probe->root_hash &&
                         __builtin_memcmp(probe->name, owner->name(), sizeof(probe->name)) == 0) &&
              valid;
      const auto baseline = probe->baseline;
      delete probe;
      exec_allocation_pressure = nullptr;
      valid = ut::expect(LifecycleResources::capture() == baseline) && valid;
      return valid ? 1 : 0;
    }
  }
  if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "allocation_rollback") && affinity_valid()) {
    const long stages = mm::PageTableManager::is_user_range(1ULL << 39, page_size) ? 5 : 4;
    if (op == 35) {
      return stages; // Root, stack leaf, then every missing intermediate table.
    }
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = process::g_process_manager->find_process(thread->owner_pid);
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    if (op == 36 && as && arg1 >= 0 && arg1 < stages) {
      if (!exec_pressure) {
        if (arg1 != 0) {
          return 0;
        }
        exec_baseline = LifecycleResources::capture();
        exec_original = as.get();
        exec_root = as->pgd_phys;
        exec_root_hash = page_table_hash(exec_root);
        __builtin_memcpy(exec_name, owner->name(), sizeof(exec_name));
        exec_pressure = new PagePressure{};
        // Exhaust the PFA once while retaining one allowance page per native
        // page-table stage. Each completed failure releases one more allowance,
        // producing the same 0..N-page budgets without rescanning all RAM.
        const bool ready = ut::expect(exec_pressure && exec_pressure->acquire(static_cast<usize>(stages)));
        if (!ready) {
          delete exec_pressure;
          exec_pressure = nullptr;
          return 0;
        }
      }
      const auto exposed_pages = static_cast<long>(static_cast<usize>(stages) - exec_pressure->count);
      if (!ut::expect(arg1 == exposed_pages)) {
        delete exec_pressure;
        exec_pressure = nullptr;
        return 0;
      }
      return 1;
    }
    if (op == 37 && exec_pressure) {
      bool valid = ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == static_cast<usize>(arg1));
      valid =
          ut::expect(arg1 >= 0 && arg1 < stages && exec_pressure->count == static_cast<usize>(stages - arg1)) && valid;
      valid = ut::expect(as && as.get() == exec_original && as->pgd_phys == exec_root &&
                         page_table_hash(exec_root) == exec_root_hash &&
                         __builtin_memcmp(exec_name, owner->name(), sizeof(exec_name)) == 0) &&
              valid;
      if (valid && arg1 + 1 < stages) {
        exec_pressure->give_one();
        valid = ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == static_cast<usize>(arg1 + 1));
        if (valid) {
          return 1;
        }
      }
      delete exec_pressure;
      exec_pressure = nullptr;
      valid = ut::expect(LifecycleResources::capture() == exec_baseline) && valid;
      return valid && arg1 + 1 == stages ? 1 : 0;
    }
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "arm_failure_recovery") && affinity_valid()) {
    if (op == 28 && !sleep_capacity) {
      sleep_capacity_heap_before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      sleep_capacity = new TimerCapacity();
      return sleep_capacity && sleep_capacity->full;
    }
    if (op == 29 && sleep_capacity) {
      delete sleep_capacity;
      sleep_capacity = nullptr;
      return mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == sleep_capacity_heap_before;
    }
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "early_wakeup")) {
    if (op == 24 && !early_sleep_thread && arch::get_current_cpu_id() == 1) {
      early_sleep_visits = 0;
      early_sleep_woken = true;
      early_sleep_thread = process::CfsScheduler::get_current_task();
      return g_num_cpus >= 2;
    }
    if (op == 25 && early_sleep_thread == process::CfsScheduler::get_current_task()) {
      early_sleep_thread = nullptr;
      return early_sleep_woken && early_sleep_visits == 2;
    }
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "cancel_in_flight")) {
    if (op == 20 && !timer_cancellation && affinity_valid()) {
      timer_cancellation = new TimerCancellation();
      return timer_cancellation && g_num_cpus >= 3;
    }
    if (op == 21 && timer_cancellation) {
      arch::enable_interrupts();
      return timer_cancellation->peer(arg1);
    }
    if (op == 22 && timer_cancellation && affinity_valid()) {
      arch::enable_interrupts();
      return timer_cancellation->owner();
    }
    if (op == 23 && timer_cancellation && affinity_valid()) {
      delete timer_cancellation;
      timer_cancellation = nullptr;
      return 1;
    }
  }
  if (op >= COW_BEGIN && op <= COW_FINISH && ut::same_id(selection, "users.uaccess") && affinity_valid() &&
      (ut::same_id(active_case, "cow_copy_fault") || ut::same_id(active_case, "cow_partial_read") ||
       ut::same_id(active_case, "cow_user_fault"))) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = thread ? process::g_process_manager->find_process(thread->owner_pid) : shared_ptr<process::Process>{};
    if (!ut::expect(static_cast<bool>(owner))) {
      return 0;
    }
    if (op == COW_BEGIN) {
      if (!ut::expect(!cow_pressure)) {
        return 0;
      }
      cow_pressure = new CowAllocationPressure{};
      if (ut::expect(cow_pressure != nullptr) && cow_pressure->begin(*owner, static_cast<VirtAddr>(arg1))) {
        return 1;
      }
      delete cow_pressure;
      cow_pressure = nullptr;
      return 0;
    }
    if (!ut::expect(cow_pressure && cow_pressure->address == static_cast<VirtAddr>(arg1))) {
      return 0;
    }
    if (op == COW_ARM) {
      return cow_pressure->arm(*owner);
    }
    if (op == COW_CHECK) {
      return cow_pressure->unchanged(*owner);
    }
    if (op == COW_RELEASE) {
      return cow_pressure->release(*owner);
    }
    if (op == COW_SPLIT) {
      return cow_pressure->verify_split(*owner);
    }
    const bool valid = cow_pressure->finish(*owner, ut::same_id(active_case, "cow_user_fault"));
    delete cow_pressure;
    cow_pressure = nullptr;
    return valid;
  }
  if ((op == 12 || op == 13) && ut::same_id(selection, "users.uaccess") && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = thread && process::g_process_manager ? process::g_process_manager->find_process(thread->owner_pid)
                                                      : shared_ptr<process::Process>{};
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    const u64 address = static_cast<u64>(arg1);
    if (!ut::expect(as && as->allows_user_access(address, 8, process::vma_flags::WRITE))) {
      return 0;
    }
    auto *leaf = mm::PageTableManager::get_user_pte(as->pgd_phys, address);
    ut::expect(!leaf || !leaf->is_valid());
    usize released_pages = 0;
    if (op == 12) {
      if (!ut::expect(uaccess_pressure == nullptr)) {
        return 0;
      }
      uaccess_pressure = new PagePressure{};
      uaccess_free_before = mm::PageFrameAllocator::get_memory_stats().free_pages;
      if (ut::expect(uaccess_pressure && uaccess_pressure->acquire(0))) {
        return 1;
      }
    } else {
      ut::expect(uaccess_pressure != nullptr);
      released_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
      // The sigframe test reaps its child before releasing the pressure.
      // Account for those newly freed, non-fixture pages without relaxing the
      // zero-free-page invariant of the other fault cases.
      if (arg2 != 1 || !ut::same_id(active_case, "sigframe_fault")) {
        ut::expect(released_pages == 0);
      }
    }
    delete uaccess_pressure;
    uaccess_pressure = nullptr;
    ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == uaccess_free_before + released_pages);
    return 0;
  }
  if (op == 14 && ut::same_id(selection, "users.uaccess") && ut::same_id(active_case, "partial_read") &&
      uaccess_pressure) {
    u8 bytes[8];
    for (auto &byte : bytes) {
      byte = 0xa5;
    }
    bool valid = ut::expect(process::copy_from_user(bytes, static_cast<u64>(arg1), sizeof(bytes)) == 4);
    for (usize i = 0; i < sizeof(bytes); ++i) {
      valid = ut::expect(bytes[i] == (i < 4 ? i : 0)) && valid;
    }
    return valid ? 1 : 0;
  }
  if (op == 10 && ut::same_id(selection, "users.frame") && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    const u64 address = thread ? reinterpret_cast<u64>(thread->trap_frame) : 0;
    // Validate ownership before dereferencing: the old RV entry supplies 511,
    // and the old x86 entry supplies null, neither is a kernel-stack frame.
    const bool owned = thread && address >= thread->kernel_stack_base &&
                       address <= thread->kernel_stack_top() - sizeof(moss::abi::TrapFrame) && (address & 15) == 0;
    if (!owned && thread) {
      logging::klog::error("frame validation: frame={:#x}, stack=[{:#x}, {:#x})", address, thread->kernel_stack_base,
                           thread->kernel_stack_top());
    }
    ut::expect(owned);
    if (owned) {
      auto &frame = *thread->trap_frame;
      ut::expect(frame.from_user() && frame.syscall_number() == 511);
      for (u32 i = 0; i < 6; ++i) {
        ut::expect(frame.argument(i) == (i == 0 ? 10 : i * 11));
      }
      ut::expect(mm::PageTableManager::is_user_range(frame.pc, 1));
      ut::expect(mm::PageTableManager::is_user_range(frame.sp, 1));
    }
    return owned ? 12345 : -1;
  }
  if (op == 4 && user_benchmark_mode()) {
    if (sample_index >= warmup_count + sample_count) {
      end_case();
      return 0;
    }
    return static_cast<long>(syscall_iterations);
  }
  if (op == 5 && user_benchmark_mode()) {
    if (arg1 <= 0 || !arg2 || !affinity_valid()) {
      failed = true;
      finish("invalid_sample");
    }
    if (syscall_pilot) {
      if (static_cast<u64>(arg1) >= clock_info.frequency / 1000 || syscall_iterations == syscall_capacity) {
        syscall_pilot = false;
      } else {
        syscall_iterations *= 2;
      }
    } else {
      record_batch(static_cast<u64>(arg1), syscall_iterations, sample_index < warmup_count, syscall_overhead);
    }
    return 0;
  }
  if (op == 6 && user_benchmark_mode() && arg1 >= 0) {
    syscall_overhead = static_cast<u64>(arg1);
    return 0;
  }
  if (op == 30 && user_benchmark_mode() >= 11 && active_case && affinity_valid()) {
    const auto now = LifecycleResources::capture();
    if (arg1 == 0) {
      benchmark_resources = now;
      benchmark_switches = process::g_scheduler->total_context_switches();
      return 1;
    }
    bool valid = !benchmark_warmed || now == benchmark_resources;
    benchmark_warmed = true;
    if (ut::same_id(selection, "bench.switch")) {
      valid = valid && process::g_scheduler->total_context_switches() >= benchmark_switches + syscall_iterations;
    }
    return ut::expect(valid) ? 1 : 0;
  }
  if (op == 31 && (ut::same_id(selection, "bench.fault") || ut::same_id(selection, "bench.cow")) && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = process::g_process_manager->find_process(thread->owner_pid);
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    bool valid =
        as && arg2 > 0 && arg2 <= 64 &&
        as->allows_user_access(static_cast<u64>(arg1), static_cast<usize>(arg2) * page_size, process::vma_flags::WRITE);
    for (long i = 0; valid && i < arg2; ++i) {
      const auto *pte =
          mm::PageTableManager::get_user_pte(as->pgd_phys, static_cast<u64>(arg1) + static_cast<u64>(i) * page_size);
      valid = ut::same_id(selection, "bench.fault")
                  ? !pte || !pte->is_valid()
                  : pte && pte->is_valid() && pte->is_cow() &&
                        mm::PageFrameAllocator::page_ref_get(pte->get_phys_addr()) >= 2;
    }
    return ut::expect(valid) ? 1 : 0;
  }
  if (op == 10 && is_lifecycle() && active_case) {
    const bool applications = ut::same_id(selection, "users.applications");
    // Five is the midpoint of CTest's ten-cycle application profile, keeping
    // the 30-second no-progress deadline sensitive to a real stall instead of
    // aggregate host throttling. Core runs stay at 100 cycles to avoid making
    // their 1,000/10,000-cycle profiles protocol-bound. Userspace and the host
    // parser must use the same cadence.
    const u64 interval = applications ? 5 : 100;
    if (!ut::expect(arg1 >= 0 && arg2 == (applications ? arg1 : 0))) {
      return 0; // Each declared core cycle must also complete its BusyBox child.
    }
    const auto now = LifecycleResources::capture();
    const u64 time_ns = timer::TimerSubsystem::instance().now_ns();
    if (arg1 == 0 && !lifecycle_started) {
      lifecycle_started_ns = time_ns;
    }
    const u64 elapsed_ns = time_ns - lifecycle_started_ns;
    Event("checkpoint")
        .str("case", active_case)
        .number("cycles", static_cast<u64>(arg1))
        .number("application_cycles", static_cast<u64>(arg2))
        .number("elapsed_ns", elapsed_ns)
        .number("heap_bytes", now.heap_bytes)
        .number("free_pages", now.free_pages)
        .number("processes", now.processes)
        .number("threads", now.threads)
        .number("user_pages", now.user_pages)
        .number("stack_pages", now.stack_pages)
        .number("descriptors", now.descriptors)
        .number("file_refs", now.file_refs)
        .number("vfs_inodes", now.vfs_pools.inodes)
        .number("vfs_dentries", now.vfs_pools.dentries)
        .number("vfs_files", now.vfs_pools.files)
        .send();
    if (arg1 == 0 && !lifecycle_started) {
      lifecycle_baseline = now;
      lifecycle_started = true;
      return 1;
    }
    if (!ut::expect(lifecycle_started && !lifecycle_complete &&
                    arg1 == static_cast<long>(lifecycle_checkpoint + interval) && now == lifecycle_baseline)) {
      return 0;
    }
    lifecycle_checkpoint = static_cast<u64>(arg1);
    if (stability && !lifecycle_host_released) {
      lifecycle_host_released = moss::abi::bridge::console_try_getc() == 'S';
    }
    // Guest clocks may drift relative to the host (e.g. calibrated x86 TSC).
    // Keep doing complete cycles until both clocks and the host agree to stop.
    // Routine callers may lower the cycle count through moss.iterations only
    // when it lands on this workload's checkpoint cadence. Stability remains
    // fixed at 10,000 cycles and 30 minutes (1.8e12 ns), plus a host release.
    const u64 routine_cycles = fixed_iterations ? fixed_iterations : 1000U;
    const u64 target_cycles = stability ? 10000U : routine_cycles;
    if (!ut::expect(target_cycles % interval == 0)) {
      return 0;
    }
    lifecycle_complete = lifecycle_checkpoint >= target_cycles && elapsed_ns >= (stability ? 1800000000000ULL : 0) &&
                         (!stability || lifecycle_host_released);
    return lifecycle_complete ? 2 : 1;
  }
  return moss::test::validation::smp_control(op, arg1, arg2);
}
