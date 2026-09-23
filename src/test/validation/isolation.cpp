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

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/core_cases.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/smp_cases.hpp"
#include "validation_internal.hpp"

#include "validation/isolation.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
using moss::test::validation::KernelPermissions;
using moss::test::validation::page_table_hash;

namespace moss::test::validation {
namespace {
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

} // namespace

long isolation_control(long op, long arg1, long arg2) {
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
  invalid_control();
}
} // namespace moss::test::validation
