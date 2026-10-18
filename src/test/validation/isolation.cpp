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
      if (platform::hardware.intc.gic_version == 1) {
        // BCM2836 interrupt-pending registers change during this test. Use
        // PL011's immutable, read-only Peripheral ID0 (DDI0183G, offset 0xfe0)
        // instead; neither a read nor a forbidden zero store consumes an IRQ.
        if (!ut::expect(platform::hardware.uart.valid && platform::hardware.uart.kind == platform::UartKind::Pl011)) {
          physical = 0;
          return 0;
        }
        physical = platform::uart_base() + 0xfe0;
      } else {
        // GICD_TYPER is read-only at distributor offset 4; unlike acknowledge
        // registers, reads do not consume an IRQ (Arm GIC-600 TRM).
        if (!ut::expect(platform::hardware.intc.gic_version == 2 || platform::hardware.intc.gic_version == 3)) {
          physical = 0;
          return 0;
        }
        physical += 4;
      }
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

// Validation alone replaces this backing between user calls. No writable user
// alias exists; production code approval is not bypassed by a production ABI.
alignas(u32) u8 instruction_backing[8]{}; // Two ARM64/RV64 instructions; x64 needs six bytes.
PhysAddr instruction_root = 0;
constexpr VirtAddr instruction_address = process::user_layout::MMAP_BASE;

long instruction_control(long op, long marker) {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  auto owner = process::current_process();
  auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
  if (!ut::expect(as && (!instruction_root || instruction_root == as->pgd_phys))) {
    return 0;
  }
  auto transaction = as->lock_vm();
  if (op == INSTRUCTION_RELEASE) {
    Tables::unmap_user_page(as->pgd_phys, instruction_address);
    const bool removed = as->remove_vma(instruction_address, instruction_address + page_size);
    instruction_root = 0;
    return ut::expect(removed);
  }
  // Distinct small return values fit every native immediate encoding and make
  // an old instruction-cache line observable without relying on a crash.
  if (!ut::expect(marker == 37 || marker == 41)) {
    return 0;
  }
#if defined(MOSS_ARCH_ARM64)
  // A64 MOVZ W0 places imm16 in [20:5]; RET names X30 by its standard encoding.
  const u32 code[] = {0x52800000U | (static_cast<u32>(marker) << 5), 0xd65f03c0U}; // MOV W0,#imm; RET.
#elif defined(MOSS_ARCH_RISCV64)
  // RV64 ADDI places the signed immediate in [31:20]; RET is JALR ZERO,RA,0.
  const u32 code[] = {0x00000513U | (static_cast<u32>(marker) << 20), 0x00008067U}; // ADDI A0,ZERO,imm; RET.
#else
  const u8 code[] = {0xb8, static_cast<u8>(marker), 0, 0, 0, 0xc3}; // MOV EAX,imm32; RET.
#endif
  __builtin_memcpy(instruction_backing, code, sizeof(code));
  if (!instruction_root) {
    if (!ut::expect(as->add_vma(instruction_address, instruction_address + page_size,
                                process::vma_flags::READ | process::vma_flags::EXEC, process::VmaType::CODE,
                                instruction_backing, 0, sizeof(instruction_backing)))) {
      return 0;
    }
    instruction_root = as->pgd_phys;
    // The first actual EL0 instruction fetch must populate this VMA normally.
    return static_cast<long>(instruction_address);
  }

  auto *pte = Tables::get_user_pte(instruction_root, instruction_address);
  if (!ut::expect(pte && pte->is_valid() && Pfa::page_ref_get(pte->get_phys_addr()) == 1)) {
    return 0;
  }
  const PhysAddr original = pte->get_phys_addr();
  // Keep the next order-0 allocation live. If demand split a larger block,
  // this holds its buddy; otherwise the buddy was already allocated. The
  // just-freed frame then heads the order-0 list, with no table allocation.
  auto guard = mm::allocate_pages(0);
  if (!ut::expect(guard.has_value())) {
    return 0;
  }
  if (!ut::expect(Pfa::page_ref_get(original ^ page_size) != 0)) {
    (void)mm::free_pages(*guard, 0);
    return 0;
  }
  pte->clear();
  Tables::invalidate_tlb_addr(instruction_address);
  ut::expect(Pfa::page_ref_dec(original) == 0);
  ut::expect(mm::free_pages(original, 0).has_value());
  const mm::UserFaultContext context{.root = instruction_root,
                                     .flags = process::vma_flags::READ | process::vma_flags::EXEC,
                                     .start = instruction_address,
                                     .backing = instruction_backing,
                                     .backing_offset = 0,
                                     .backing_size = sizeof(instruction_backing),
                                     .shared_pages = nullptr,
                                     .shared_page_count = 0};
  const bool populated = mm::resolve_user_demand_fault(context, instruction_address, mm::UserFaultAccess::Execute);
  pte = Tables::get_user_pte(instruction_root, instruction_address);
  const bool reused = ut::expect(populated && pte && pte->is_valid() && pte->get_phys_addr() == original);
  (void)mm::free_pages(*guard, 0);
  return reused ? static_cast<long>(instruction_address) : 0;
}

} // namespace

long isolation_control(long op, long arg1, long arg2) {
  if (ut::same_id(selection, "users.vm") && ut::same_id(active_case, "instruction_publication") && affinity_valid() &&
      (op == INSTRUCTION_PREPARE || op == INSTRUCTION_RELEASE)) {
    return instruction_control(op, arg1);
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
  invalid_control();
}
} // namespace moss::test::validation
