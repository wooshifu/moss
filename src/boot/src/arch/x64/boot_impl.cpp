/*
 * x64 architecture-specific boot implementation - module implementation unit
 * Implements the unified boot interface for x64
 */

module;

// Architecture detection (global module fragment)
#ifndef MOSS_ARCH_X64
#define MOSS_ARCH_X64
#endif

// Assembly-defined symbols used by x64_setup_tss() below
extern "C" {
extern unsigned char g_tss[];                         // 104-byte TSS in start_x64.S .bss.tss
extern unsigned long long gdt_table[];                // GDT in start_x64.S .data
extern unsigned char _stack_top_addr[];               // Boot stack top (linker symbol)
void early_debug_print(const char *message) noexcept; // UART output (kernel_main.cpp)
extern unsigned char x86_ap_trampoline_start[], x86_ap_trampoline_end[], x86_ap_cr3[], x86_ap_stack[];
extern unsigned char pvh_pml4[];
[[noreturn]] void x86_secondary_entry() noexcept;
void moss_validation_cpu_started(unsigned cpu) noexcept;
}

module moss.boot;

import moss.abi;
import moss.fdt;
import moss.hal.uart;

using moss::PhysAddr;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;
using moss::VirtAddr;

static bool initialize_fpu() noexcept {
  u32 a, b, c, d;
  asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  constexpr u32 required = (1U << 0) | (1U << 24) | (1U << 25) | (1U << 26); // x87, FXSR, SSE, SSE2
  if ((d & required) != required) {
    return false;
  }
  u64 cr0, cr4;
  asm volatile("mov %%cr0, %0; mov %%cr4, %1" : "=r"(cr0), "=r"(cr4));
  cr0 = (cr0 & ~u64{0xc}) | 0x22; // clear EM/TS, enable MP/NE
  // Eager legacy-state switching; do not expose AVX until XSAVE ownership exists.
  cr4 = (cr4 & ~(1ULL << 18)) | 0x600; // OSFXSR + OSXMMEXCPT, no OSXSAVE
  asm volatile("mov %0, %%cr0; mov %1, %%cr4" ::"r"(cr0), "r"(cr4) : "memory");
  const moss::kernel::process::X86FpState initial{};
  asm volatile("fxrstor64 %0" ::"m"(initial) : "memory");
  return true;
}

// =============================================================================
// TSS setup — called from start_x64.S before early_main()
// =============================================================================
// x64 Task State Segment (104 bytes, defined in start_x64.S .bss.tss)
struct [[gnu::packed]] TSS64 {
  u32 reserved0;
  u64 rsp0; // Kernel stack for ring 3 → ring 0 transitions
  u64 rsp1;
  u64 rsp2;
  u64 reserved1;
  u64 ist[7]; // Interrupt Stack Table entries
  u64 reserved2;
  u16 reserved3;
  u16 iomap_base;
};

struct alignas(16) X86CpuRuntime {
  // GS offsets 0/8 are consumed by x64_syscall.S for kernel RSP/user scratch.
  // Reordering these fields requires updating the assembly entry contract.
  u64 kernel_rsp;
  u64 user_rsp_scratch;
  TSS64 *tss;
};
// Sixteen entries match BOOT_MAX_CPUS. Each AP gets seven GDT slots (five
// segments plus a two-slot TSS) and a page-aligned 32 KiB boot stack; stack
// budget is a fixed policy with no recorded maximum-depth measurement.
static X86CpuRuntime cpu_runtime[16];
static TSS64 ap_tss[16];
static u64 ap_gdt[16][7];
alignas(4096) static u8 ap_stacks[16][32768];

static void set_gs_runtime(u32 cpu, TSS64 *tss) noexcept {
  cpu_runtime[cpu].tss = tss;
  u64 base = reinterpret_cast<u64>(&cpu_runtime[cpu]);
  // IA32_GS_BASE MSR=0xc0000101 selects this CPU's SYSCALL scratch storage;
  // WRMSR takes the 64-bit address as EDX:EAX.
  asm volatile("wrmsr" ::"c"(0xC0000101U), "a"(static_cast<u32>(base)), "d"(static_cast<u32>(base >> 32)) : "memory");
}

extern "C" void x64_set_kernel_stack(u64 top) noexcept {
  auto cpu = moss::kernel::arch::get_current_cpu_id();
  cpu_runtime[cpu].kernel_rsp = top;
  cpu_runtime[cpu].tss->rsp0 = top;
}

extern "C" void x64_setup_tss() noexcept {
  auto *tss = reinterpret_cast<TSS64 *>(g_tss);

  // Zero the TSS
  auto *bytes = reinterpret_cast<u8 *>(tss);
  for (u32 i = 0; i < sizeof(TSS64); i++) {
    bytes[i] = 0;
  }

  // Set RSP0 to boot stack top (updated per-task by scheduler later)
  tss->rsp0 = reinterpret_cast<u64>(_stack_top_addr);
  tss->iomap_base = static_cast<u16>(sizeof(TSS64));

  // Build TSS descriptor (16 bytes = 2 GDT slots) at GDT[5-6] (selector 0x28)
  u64 tss_addr = reinterpret_cast<u64>(tss);
  u32 tss_limit = sizeof(TSS64) - 1;

  // Low 8 bytes: limit[15:0], base[23:0], type=0x89(available 64-bit TSS, P=1), limit[19:16], base[31:24]
  u64 desc_lo = 0;
  desc_lo |= static_cast<u64>(tss_limit & 0xFFFF);
  desc_lo |= (tss_addr & 0xFFFF) << 16;
  desc_lo |= ((tss_addr >> 16) & 0xFF) << 32;
  desc_lo |= 0x89ULL << 40; // Type=9(available TSS), P=1
  desc_lo |= static_cast<u64>((tss_limit >> 16) & 0xF) << 48;
  desc_lo |= ((tss_addr >> 24) & 0xFF) << 56;

  // High 8 bytes: base[63:32]
  u64 desc_hi = (tss_addr >> 32) & 0xFFFFFFFF;

  gdt_table[5] = desc_lo;
  gdt_table[6] = desc_hi;

  // Load the Task Register with TSS selector (0x28)
  asm volatile("ltr %w0" ::"r"(static_cast<u16>(0x28)));
  set_gs_runtime(0, tss);
}

namespace moss::boot {

// PVH (Xen PVH) Boot Info Structure
// This structure is passed by QEMU when using PVH boot protocol
struct HvmStartInfo {
  u32 magic;          // HVM_START_MAGIC_VALUE (0x336ec578)
  u32 version;        // version of this structure
  u32 flags;          // SIF_xxx flags
  u32 nr_modules;     // number of modules passed to domain
  u64 modlist_paddr;  // physical address of module info (struct hvm_modlist_entry)
  u64 cmdline_paddr;  // physical address of the command line
  u64 rsdp_paddr;     // physical address of RSDP ACPI data structure
  u64 memmap_paddr;   // physical address of memory map
  u32 memmap_entries; // number of memory map entries
  u32 reserved;       // must be zero
};

// HVM Module List Entry (for initramfs)
struct HvmModlistEntry {
  u64 paddr;         // physical address of module
  u64 size;          // size of module in bytes
  u64 cmdline_paddr; // physical address of command line
  u64 reserved;      // must be zero
};

// Global boot status
BootStatus g_boot_status = {.current_stage = BootStage::PreInit,
                            .completed_stages_mask = 0,
                            .stage_timestamps = {0},
                            .last_error = ::moss::kernel::ErrorCode::Success};

static void early_print(const char *str) { moss::kernel::hal::uart::puts(str); }

static void early_print_hex(u64 value) {
  constexpr char hex_chars[] = "0123456789ABCDEF";
  char buffer[19] = "0x"; // 2 prefix bytes + 16 u64 hex digits + NUL.

  for (int i = 15; i >= 0; i--) {
    buffer[2 + (15 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
  }
  buffer[18] = '\0';

  early_print(buffer);
}

static u64 get_timestamp_counter() noexcept {
  u32 low, high;
  asm volatile("rdtsc" : "=a"(low), "=d"(high) : : "memory");
  return (static_cast<u64>(high) << 32) | low;
}

static u32 get_current_cpu_id_impl() noexcept { return moss::kernel::arch::get_current_cpu_id(); }

// Boot stage status update
void update_boot_stage(BootStage stage, ::moss::kernel::ErrorCode error) noexcept {
  g_boot_status.current_stage = stage;
  g_boot_status.last_error = error;

  u32 stage_index = static_cast<u32>(stage);
  if (stage_index < 8) {
    g_boot_status.stage_timestamps[stage_index] = get_timestamp_counter();

    if (error == ::moss::kernel::ErrorCode::Success) {
      g_boot_status.completed_stages_mask |= (1U << stage_index);
    }
  }
}

} // namespace moss::boot

// x64BootImpl member function implementations
// ACPI tables are firmware data, not emulator configuration. All early physical
// accesses are bounded by the currently supported 4-GiB identity mapping.
// Xen's PVH ABI also requires start-of-day structures below this boundary.
static constexpr u64 PVH_BOOT_ADDRESS_LIMIT = u64{1} << 32;

static bool physical_range(u64 base, u64 size) noexcept {
  // The temporary PVH identity map and the boot ABI both bound boot data below
  // 4 GiB. Requiring a non-empty range also prevents zero-sized modules from
  // being mistaken for a supplied initramfs.
  return base && size && base < PVH_BOOT_ADDRESS_LIMIT && size <= PVH_BOOT_ADDRESS_LIMIT - base;
}

static bool valid_pvh_module_list(const moss::boot::HvmStartInfo &start) noexcept {
  // Validate the complete declared table even though Moss currently consumes
  // its first entry as the initramfs. This prevents a forged count from
  // describing memory outside the early identity map.
  const u64 table_size = static_cast<u64>(start.nr_modules) * sizeof(moss::boot::HvmModlistEntry);
  return start.nr_modules && physical_range(start.modlist_paddr, table_size);
}

static bool valid_pvh_initrd(const moss::boot::HvmModlistEntry &module,
                             const moss::kernel::platform::MemoryRegion *regions, u32 region_count) noexcept {
  if (module.reserved || !physical_range(module.paddr, module.size)) {
    return false;
  }
  const u64 module_end = module.paddr + module.size;
  u64 covered_end = module.paddr;
  while (covered_end < module_end) {
    u64 next_end = covered_end;
    for (u32 i = 0; i < region_count; ++i) {
      const auto &region = regions[i];
      if (region.size && region.base < PVH_BOOT_ADDRESS_LIMIT && region.size <= PVH_BOOT_ADDRESS_LIMIT - region.base &&
          region.base <= covered_end && region.base + region.size > next_end) {
        next_end = region.base + region.size;
      }
    }
    if (next_end == covered_end) {
      // A syntactically valid physical address can still name MMIO or reserved
      // firmware data. Every byte must be covered by usable RAM.
      return false;
    }
    covered_end = next_end;
  }
  return true;
}
static u64 acpi_value(const u8 *p, u32 bytes) noexcept {
  u64 value = 0;
  for (u32 i = 0; i < bytes; ++i) {
    value |= static_cast<u64>(p[i]) << (i * 8);
  }
  return value;
}
static bool signature(const u8 *p, const char *text, u32 length) noexcept {
  for (u32 i = 0; i < length; ++i) {
    if (p[i] != static_cast<u8>(text[i])) {
      return false;
    }
  }
  return true;
}
static bool checksum(const u8 *p, u32 length) noexcept {
  u8 sum = 0;
  for (u32 i = 0; i < length; ++i) {
    sum = static_cast<u8>(sum + p[i]);
  }
  return sum == 0;
}
static const u8 *acpi_table(u64 address) noexcept {
  // ACPI SDT headers are 36 bytes; Length is a little-endian u32 at +4.
  // The 1 MiB maximum bounds malformed-table scans, not ACPI table size by
  // specification; its exact policy threshold has no recorded derivation.
  if (!physical_range(address, 36)) {
    return nullptr;
  }
  const auto *p = reinterpret_cast<const u8 *>(address);
  u64 size = acpi_value(p + 4, 4);
  if (size < 36 || size > 1024ULL * 1024 || !physical_range(address, size) || !checksum(p, static_cast<u32>(size))) {
    return nullptr;
  }
  return p;
}
static const u8 *rsdp_at(u64 address) noexcept {
  // RSDP revision (+15) >=2 adds Length(+20) and XSDT address(+24) to the
  // original 20-byte checksum prefix; the extended minimum is 36 bytes.
  // The 4096-byte scan ceiling is a local rejection policy, not an ACPI limit.
  if (!physical_range(address, 36)) {
    return nullptr;
  }
  const auto *p = reinterpret_cast<const u8 *>(address);
  if (!signature(p, "RSD PTR ", 8) || !checksum(p, 20)) {
    return nullptr;
  }
  if (p[15] >= 2) {
    u64 size = acpi_value(p + 20, 4);
    if (size < 36 || size > 4096 || !physical_range(address, size) || !checksum(p, static_cast<u32>(size))) {
      return nullptr;
    }
  }
  return p;
}
static bool discover_acpi(u64 address) noexcept {
  using namespace moss::kernel::platform;
  const u8 *rsdp = address ? rsdp_at(address) : nullptr;
  if (address && !rsdp) {
    return false;
  }
  if (!address) {
    // IA-PC RSDP discovery checks 16-byte boundaries in EBDA's first KiB,
    // then ROM 0xe0000..0xfffff. BDA 0x40e stores the EBDA segment (*16).
    // The 0x80000..0x9fc00 EBDA admission window is a local safety policy.
    u64 ebda = static_cast<u64>(*reinterpret_cast<volatile u16 *>(0x40E)) << 4;
    if (ebda >= 0x80000 && ebda <= 0x9FC00) {
      for (u64 p = ebda; p < ebda + 1024 && !rsdp; p += 16) {
        rsdp = rsdp_at(p);
      }
    }
    for (u64 p = 0xE0000; p < 0x100000 && !rsdp; p += 16) {
      rsdp = rsdp_at(p);
    }
  }
  if (!rsdp) {
    return false;
  }
  bool extended = rsdp[15] >= 2 && acpi_value(rsdp + 24, 8);
  // XSDT entries are 8-byte physical addresses; legacy RSDT entries are 4-byte.
  const auto *root = acpi_table(acpi_value(rsdp + (extended ? 24 : 16), extended ? 8 : 4));
  if (!root || !signature(root, extended ? "XSDT" : "RSDT", 4)) {
    return false;
  }
  u32 size = static_cast<u32>(acpi_value(root + 4, 4));
  u32 stride = extended ? 8U : 4U;
  if ((size - 36) % stride) {
    return false;
  }
  bool madt_found = false;
  for (u32 offset = 36; offset < size; offset += stride) {
    const auto *table = acpi_table(acpi_value(root + offset, stride));
    if (!table) {
      return false;
    }
    u32 length = static_cast<u32>(acpi_value(table + 4, 4));
    if (signature(table, "APIC", 4)) {
      // MADT follows its 36-byte header with LAPIC address(+36), flags(+40),
      // then records at +44. Each record begins with one-byte Type and Length.
      if (madt_found || length < 44) {
        return false;
      }
      madt_found = true;
      hardware.intc.dist_base = acpi_value(table + 36, 4);
      for (u32 pos = 44; pos < length;) {
        if (length - pos < 2 || table[pos + 1] < 2 || table[pos + 1] > length - pos) {
          return false;
        }
        const u8 *entry = table + pos;
        u32 len = entry[1];
        if (entry[0] == 0) {
          // Type 0: 8-byte Local APIC, APIC ID +3, enabled flag bit 0 at +4.
          if (len != 8) {
            return false;
          }
          if (acpi_value(entry + 4, 4) & 1) {
            if (hardware.cpu_count == 16) {
              return false;
            }
            hardware.cpus[hardware.cpu_count++].hardware_id = entry[3];
          }
        } else if (entry[0] == 1) {
          // Type 1: 12-byte I/O APIC, MMIO address +4 and GSI base +8.
          // This profile accepts one GSI-zero domain; more need routing support.
          if (len != 12 || hardware.intc.cpu_base || acpi_value(entry + 8, 4) != 0) {
            return false;
          }
          hardware.intc.cpu_base = acpi_value(entry + 4, 4);
        } else if (entry[0] == 2) {
          // Type 2: 10-byte ISA override, bus +2, IRQ +3, GSI +4, flags +8.
          if (len != 10 || entry[2] != 0 || entry[3] >= 16) {
            return false;
          }
          hardware.isa_gsi[entry[3]] = static_cast<u32>(acpi_value(entry + 4, 4));
          hardware.isa_flags[entry[3]] = static_cast<u16>(acpi_value(entry + 8, 2));
        } else if (entry[0] == 5) {
          // Type 5: 12-byte LAPIC address override with a 64-bit address at +4.
          if (len != 12) {
            return false;
          }
          hardware.intc.dist_base = acpi_value(entry + 4, 8);
        } else if (entry[0] == 9) {
          // Type 9: 16-byte x2APIC, flags +8; enabled x2APIC needs another driver.
          if (len != 16 || (acpi_value(entry + 8, 4) & 1)) {
            return false;
          }
        }
        pos += len;
      }
    } else if (signature(table, "SPCR", 4) && length >= 80) {
      // SPCR: interface +36, GAS +40 (space/width/offset, address +44),
      // interrupt kind +52, legacy IRQ +53. Require the supported 16550 layout.
      u64 base = acpi_value(table + 44, 8);
      if (table[36] <= 1 && table[40] <= 1 && table[41] == 8 && !table[42] && (table[52] & 1) && table[53] > 0 &&
          table[53] < 16 && base && (table[40] ? base <= 0xFFF8 : physical_range(base, 8))) {
        hardware.uart = {.kind = UartKind::Ns16550,
                         .reg_shift = 0,
                         .reg_width = 1,
                         .port_io = table[40] == 1,
                         .base_addr = base,
                         .size = 8,
                         .clock_freq = 0,
                         .irq = table[53],
                         .valid = true};
      }
    }
  }
  if (!madt_found || !physical_range(hardware.intc.dist_base, 4096) || !physical_range(hardware.intc.cpu_base, 32)) {
    return false;
  }
  u32 lo, hi;
  // IA32_APIC_BASE MSR=0x1b: bit 11 enables APIC, bit 10 selects x2APIC;
  // bits [35:12] locate xAPIC MMIO and must agree with MADT discovery.
  asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1BU));
  u64 apic_base = (static_cast<u64>(hi) << 32) | lo;
  if (!(apic_base & (1U << 11)) || (apic_base & (1U << 10)) ||
      (apic_base & 0xFFFFFF000ULL) != hardware.intc.dist_base) {
    return false;
  }
  hardware.intc.valid = true;
  return true;
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::hardware_early_init(BootContext &ctx) noexcept {
  using namespace moss::kernel::platform;
  update_boot_stage(BootStage::HardwareInit);
  if (!initialize_fpu()) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::NotSupported};
  }
  hardware = {};
  for (u32 i = 0; i < 16; ++i) {
    hardware.isa_gsi[i] = i;
  }
  // BDA 0x400 supplies the first serial I/O base; eight register ports must
  // fit the 16-bit I/O space (base<=0xfff8). IRQ4 is this legacy serial policy.
  u16 serial = *reinterpret_cast<volatile u16 *>(0x400);
  if (serial && serial <= 0xFFF8) {
    hardware.uart = {.kind = UartKind::Ns16550,
                     .reg_shift = 0,
                     .reg_width = 1,
                     .port_io = true,
                     .base_addr = serial,
                     .size = 8,
                     .clock_freq = 0,
                     .irq = 4,
                     .valid = true};
  }
  const auto *start = static_cast<const HvmStartInfo *>(ctx.device_tree_ptr);
  // Xen PVH magic=0x336ec578; version>=1 adds the memory map. The 128-entry
  // admission cap bounds firmware scans and has no recorded sizing derivation;
  // map type 1 denotes usable RAM, other types must not enter the allocator.
  auto invalid = [] { return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument}; };
  if (!start || !physical_range(reinterpret_cast<u64>(start), sizeof(*start)) || start->magic != 0x336ec578 ||
      start->version < 1 || !start->memmap_entries || start->memmap_entries > 128 || start->reserved) {
    early_print("BOOT ERROR: invalid PVH start info\n");
    return invalid();
  }
  struct PvhMemoryEntry {
    u64 base;
    u64 size;
    u32 type;
    u32 reserved;
  };
  if (!physical_range(start->memmap_paddr, start->memmap_entries * sizeof(PvhMemoryEntry))) {
    early_print("BOOT ERROR: invalid PVH memory map\n");
    return invalid();
  }
  const auto *map = reinterpret_cast<const PvhMemoryEntry *>(start->memmap_paddr);
  for (u32 i = 0; i < start->memmap_entries; ++i) {
    if (map[i].reserved) {
      early_print("BOOT ERROR: invalid PVH memory map entry\n");
      return invalid();
    }
    if (map[i].type != 1 || !map[i].size) {
      continue;
    }
    if (hardware.memory_region_count == MAX_MEMORY_REGIONS || map[i].base >= PVH_BOOT_ADDRESS_LIMIT ||
        map[i].size > PVH_BOOT_ADDRESS_LIMIT - map[i].base) {
      early_print("BOOT ERROR: invalid PVH RAM range\n");
      return invalid();
    }
    hardware.memory_regions[hardware.memory_region_count++] = {.base = map[i].base, .size = map[i].size};
    hardware.total_memory_size += map[i].size;
  }
  hardware.memory_map_valid = hardware.memory_region_count > 0;
  hardware.total_memory_start = hardware.memory_regions[0].base;
  if (!start->nr_modules) {
    early_print("BOOT ERROR: PVH initrd module is required\n");
    return invalid();
  }
  if (!valid_pvh_module_list(*start)) {
    early_print("BOOT ERROR: invalid PVH module list\n");
    return invalid();
  }
  const auto *module = reinterpret_cast<const HvmModlistEntry *>(start->modlist_paddr);
  if (!valid_pvh_initrd(*module, hardware.memory_regions, hardware.memory_region_count)) {
    early_print("BOOT ERROR: invalid PVH initrd module\n");
    return invalid();
  }
  hardware.initrd_start = module->paddr;
  hardware.initrd_end = module->paddr + module->size;
  if (!hardware.memory_map_valid || !discover_acpi(start->rsdp_paddr)) {
    early_print("BOOT ERROR: valid PVH memory map and ACPI MADT required\n");
    return invalid();
  }
  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
  if (!order_cpus((ebx >> 24) & 0xFF)) {
    return invalid();
  }
  hardware.bootargs = reinterpret_cast<const char *>(start->cmdline_paddr);
  ctx.cpu_id = 0;
  ctx.total_cpus = hardware.cpu_count;
  ctx.memory_start = hardware.total_memory_start;
  ctx.memory_size = hardware.total_memory_size;
  ctx.kernel_phys_base = reinterpret_cast<PhysAddr>(moss::abi::_start);
  ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;
  early_print("PVH memory and ACPI hardware discovered\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::setup_memory_management(BootContext & /*ctx*/) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

  moss::boot::early_print("=== x64 Memory Management Setup ===\n");

  // Initialize kernel page tables for user space support
  // This is required for creating user address spaces even though PVH already provides identity mapping
  moss::boot::early_print("  Phase 1: kernel page table setup\n");
  auto mmu_result = ::moss::kernel::mm::setup_mmu();
  if (!mmu_result) {
    moss::boot::early_print("  ERROR: MMU setup failed\n");
    return ::moss::kernel::VoidResult{mmu_result.error()};
  }
  moss::boot::early_print("  Phase 1 complete: kernel PGD initialized\n");

  auto pfa_result = ::moss::kernel::mm::PageFrameAllocator::initialize();
  if (!pfa_result) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
  }
  auto high_result = ::moss::kernel::mm::PageTableManager::setup_kernel_high_half_tables();
  if (!high_result) {
    return high_result;
  }
  ::moss::kernel::mm::PageTableManager::enable_dynamic_alloc();

  // 256 KiB initializes the allocator inside the linker's 8 MiB heap reserve;
  // it is an initial policy, not all available RAM. Exact sizing evidence is absent.
  VirtAddr heap_start = moss::abi::linker::heap_start();
  ::moss::kernel::usize initial_heap_size = 256ULL * 1024;
  auto heap_result = ::moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(heap_start, initial_heap_size);
  if (!heap_result) {
    moss::boot::early_print("  RuntimeHeapAllocator init failed\n");
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
  }

  moss::boot::early_print("x64 memory management setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

// IDT entry structure (16 bytes for 64-bit long mode)
struct [[gnu::packed]] IDTEntry {
  u16 offset_low;  // Target offset [15:0]
  u16 selector;    // Code segment selector (0x08 = kernel CS)
  u8 ist;          // Interrupt Stack Table index (0 for most)
  u8 type_attr;    // Type + DPL + Present (0x8E = interrupt gate, DPL=0)
  u16 offset_mid;  // Target offset [31:16]
  u32 offset_high; // Target offset [63:32]
  u32 reserved;    // Must be 0
};

static IDTEntry g_idt[256];
static struct [[gnu::packed]] {
  u16 limit;
  u64 base;
} g_idtr;

// ISR stub table defined in isr_x64.S
extern "C" void *isr_stub_table[256];

static void setup_idt() {
  for (u32 i = 0; i < 256; ++i) {
    u64 addr = reinterpret_cast<u64>(isr_stub_table[i]);
    g_idt[i].offset_low = static_cast<u16>(addr & 0xFFFF);
    g_idt[i].selector = 0x08; // kernel code segment
    g_idt[i].ist = 0;
    g_idt[i].type_attr = 0x8E; // interrupt gate, present, DPL=0
    g_idt[i].offset_mid = static_cast<u16>((addr >> 16) & 0xFFFF);
    g_idt[i].offset_high = static_cast<u32>((addr >> 32) & 0xFFFFFFFF);
    g_idt[i].reserved = 0;
  }
  g_idtr.limit = sizeof(g_idt) - 1;
  g_idtr.base = reinterpret_cast<u64>(&g_idt[0]);
  asm volatile("lidt %0" ::"m"(g_idtr));
}

// Helper: print hex value to UART
static void uart_print_hex(u64 value) noexcept {
  constexpr char hex[] = "0123456789ABCDEF";
  char buf[19] = "0x";
  for (int i = 15; i >= 0; i--) {
    buf[2 + (15 - i)] = hex[(value >> (i * 4)) & 0xF];
  }
  buf[18] = '\0';
  early_debug_print(buf);
}

// Global callbacks for cross-module interrupt dispatch (registered by kernel-main.cppm).
// extern "C" to avoid module-local mangling — accessible from any translation unit.
extern "C" void (*g_x64_timer_handler)() noexcept = nullptr;
extern "C" {
void (*g_x64_uart_rx_handler)() noexcept = nullptr;
}

// C++ interrupt/exception handler called from isr_common (isr_x64.S)
extern "C" void x64_interrupt_handler(u64 vector, u64 error_code, [[maybe_unused]] void *frame) noexcept {
  if (vector < 32) {
    auto &saved = *static_cast<moss::abi::TrapFrame *>(frame);

    // Page fault (#PF, vector 14): dispatch to demand paging / COW handler
    if (vector == 14) {
      u64 cr2 = 0;
      asm volatile("mov %%cr2, %0" : "=r"(cr2));
      u64 rip = saved.pc;
      moss::abi::entry::x64_page_fault_handler(error_code, cr2, rip, &saved);
      return; // Handler resolved the fault — iretq retries the instruction
    }

    // User x87/SIMD arithmetic faults belong to the process, not the kernel.
    if ((vector == 16 || vector == 19) && saved.from_user()) {
      moss::abi::bridge::terminate_current_user_process(-8); // SIGFPE-equivalent termination
    }

    // Other CPU exceptions — print diagnostics and halt
    early_debug_print("EXCEPTION vec=");
    uart_print_hex(vector);
    early_debug_print(" err=");
    uart_print_hex(error_code);
    early_debug_print(" RIP=");
    uart_print_hex(saved.pc);
    early_debug_print("\nHALTED\n");
    asm volatile("cli; hlt");
    __builtin_unreachable();
  }

  // EOI before dispatch because the timer callback may context-switch away
  // before returning; deferring EOI could leave the local interrupt in service.
  moss::kernel::hal::intc::eoi(moss::kernel::platform::intc_dist_base(), 0);

  if (vector == moss::kernel::arch::X64_TLB_SHOOTDOWN_VECTOR) {
    moss::kernel::arch::service_tlb_shootdown();
    return;
  }

  // LAPIC timer (vector 48)
  if (vector == 48 && g_x64_timer_handler != nullptr) {
    g_x64_timer_handler();
    return;
  }

  // Firmware UART IRQ n is routed to IDT 32+n, beyond CPU exception vectors.
  if (vector == 32 + moss::kernel::platform::hardware.uart.irq && g_x64_uart_rx_handler != nullptr) {
    g_x64_uart_rx_handler();
  }
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

  moss::boot::early_print("=== x64 Interrupts and Exceptions Setup ===\n");

  // 1. Load IDT with 256 entries pointing to ISR stubs
  setup_idt();
  moss::boot::early_print("  IDT loaded (256 entries)\n");

  // 2. Initialize Local APIC (enable via SVR register)
  auto *gic = new moss::kernel::interrupts::GenericInterruptController();
  if (!gic) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
  }
  {
    VirtAddr dist_base = moss::kernel::platform::intc_dist_base(); // Local APIC
    VirtAddr cpu_base = moss::kernel::platform::intc_cpu_base();   // I/O APIC
    auto result = gic->initialize(dist_base, cpu_base, 0);
    if (!result) {
      delete gic;
      return result;
    }
    g_gic_controller = gic;
    g_gic_hardware_available = true;
    moss::boot::early_print("  Local APIC initialized\n");
  }

  // 3. Configure LAPIC timer: divide-by-16, one-shot, vector 48, initially masked
  {
    moss::kernel::hal::timer::configure_local_timer(true);
    moss::boot::early_print("  LAPIC timer configured (vec=48, masked)\n");
  }

  if (!moss::kernel::hal::timer::calibrate()) {
    moss::boot::early_print("BOOT ERROR: PIT/TSC/APIC clock calibration failed\n");
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::NotSupported};
  }
  moss::boot::early_print("x64 interrupt/exception setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::setup_smp_support(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

  moss::boot::early_print("=== x64 SMP Support Setup ===\n");

  // ACPI MADT already supplied the CPU count. Defer INIT/SIPI activation until
  // the kernel calls activate_secondary_cpus(), after shared runtime setup.
  u32 detected = moss::fdt::g_platform_info.cpu_count;
  ctx.total_cpus = detected;
  moss::kernel::g_num_cpus = detected;

  moss::boot::early_print("  Detected CPUs: ");
  moss::boot::early_print_hex(detected);
  moss::boot::early_print(" (BSP only for now)\n");
  moss::boot::early_print("x64 SMP setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::finalize_arch_init(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

  moss::boot::early_print("=== x64 Architecture Init Complete ===\n");

  // Mark runtime heap as ready so operator new uses RuntimeHeapAllocator
  moss::abi::entry::mark_runtime_heap_ready();
  moss::boot::early_print("Runtime heap marked ready\n");

  moss::boot::early_print("x64 architecture-specific init all complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::detect_memory_layout(BootContext &ctx) noexcept {
  (void)ctx;
  return ::moss::kernel::VoidResult{};
}

u32 moss::boot::X86BootImpl::get_current_cpu_id() noexcept { return moss::boot::get_current_cpu_id_impl(); }

[[noreturn]] void moss::boot::X86BootImpl::arch_panic(const char *message) noexcept {
  moss::boot::early_print("\n=== x64 PANIC ===\n");
  moss::boot::early_print(message);
  moss::boot::early_print("\n====================\n");

  asm volatile("cli");
  while (true) {
    asm volatile("hlt");
  }
}

// === Boot global variables (x64 stubs) ===
namespace moss::boot {

moss::kernel::interrupts::GenericInterruptController *g_gic_controller = nullptr;
bool g_gic_hardware_available = false;

static void ap_delay() noexcept {
  // INIT must precede SIPI by at least 10 ms. A bounded PIT channel-0
  // countdown is independent of the uncalibrated TSC and leaves IRQ0 masked.
  auto out = [](u16 port, u8 value) { asm volatile("outb %0, %1" ::"a"(value), "Nd"(port)); };
  auto in = [](u16 port) {
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
  };
  out(0x43, 0x30);
  // PIT channel 0 mode 0, low byte 0/high byte 0x40: 16384 reference ticks
  // (~13.7 ms at 1193182 Hz). The 10000000 read-back polls only bound a
  // stalled counter; their exact count is uncalibrated and not a timeout in ms.
  out(0x40, 0x00);
  out(0x40, 0x40);
  for (u32 i = 0; i < 10000000; ++i) {
    out(0x43, 0xE2); // Read-back status only, channel 0.
    if (in(0x40) & 0x80) {
      return;
    }
  }
}

void activate_secondary_cpus() noexcept {
  // SIPI carries a 4 KiB page vector below 1 MiB: vector 8 enters at 0x8000.
  // This fixed trampoline address must match every relocated reference in
  // start_x64.S; the current boot profile assumes firmware leaves it available.
  auto *copy = reinterpret_cast<u8 *>(0x8000);
  u64 size = static_cast<u64>(x86_ap_trampoline_end - x86_ap_trampoline_start);
  if (size > 4096) {
    return;
  }
  for (u64 i = 0; i < size; ++i) {
    copy[i] = x86_ap_trampoline_start[i];
  }
  // The temporary PVH map covers the real-mode trampoline. The AP switches to
  // final W^X tables after leaving that page, before publishing itself online.
  u64 cr3 = reinterpret_cast<u64>(pvh_pml4);
  *reinterpret_cast<u64 *>(copy + (x86_ap_cr3 - x86_ap_trampoline_start)) = cr3;
  for (u32 cpu = 1; cpu < moss::kernel::g_num_cpus; ++cpu) {
    *reinterpret_cast<u64 *>(copy + (x86_ap_stack - x86_ap_trampoline_start)) =
        reinterpret_cast<u64>(&ap_stacks[cpu][32768]);
    moss::kernel::arch::memory_barrier();
    const auto apic_id = static_cast<u32>(moss::kernel::platform::hardware.cpus[cpu].hardware_id);
    moss::kernel::hal::intc::send_startup_ipi(apic_id, 0x0000C500); // INIT, level assert.
    ap_delay();
    moss::kernel::hal::intc::send_startup_ipi(apic_id, 0x00008500); // INIT deassert.
    ap_delay();
    moss::kernel::hal::intc::send_startup_ipi(apic_id, 0x00000608); // SIPI vector 8, physical 0x8000.
    ap_delay();
    if (!(__atomic_load_n(&online_cpu_mask, __ATOMIC_ACQUIRE) & (1ULL << cpu))) {
      moss::kernel::hal::intc::send_startup_ipi(apic_id, 0x00000608);
    }
    moss_validation_cpu_started(cpu);
    // Bound readiness spinning to 100000000 CPU hints; this is a poll budget
    // with no measured wall-clock rationale, separate from the final timed wait.
    for (u32 retry = 0; retry < 100000000; ++retry) {
      if (__atomic_load_n(&online_cpu_mask, __ATOMIC_ACQUIRE) & (1ULL << cpu)) {
        break;
      }
      moss::kernel::arch::cpu_yield();
    }
  }
}

} // namespace moss::boot

extern "C" [[noreturn]] void x86_secondary_entry() noexcept {
  using Tables = moss::kernel::mm::PageTableManager;
  if (!moss::kernel::hal::mmu::enable_mmu(Tables::get_physical_address(Tables::get_kernel_pgd()))) {
    moss::kernel::arch::kernel_panic("secondary CPU MMU initialization failed");
  }
  if (!initialize_fpu()) {
    moss::kernel::arch::kernel_panic("secondary CPU lacks x87/FXSR/SSE2");
  }
  auto cpu = moss::kernel::arch::get_current_cpu_id();
  if (cpu == 0 || cpu >= 16) {
    for (;;) {
      asm volatile("cli; hlt");
    }
  }
  for (u32 i = 0; i < 5; ++i) {
    ap_gdt[cpu][i] = gdt_table[i];
  }
  auto *tss = &ap_tss[cpu];
  tss->rsp0 = reinterpret_cast<u64>(&ap_stacks[cpu][32768]);
  tss->iomap_base = sizeof(TSS64);
  u64 base = reinterpret_cast<u64>(tss);
  ap_gdt[cpu][5] = (sizeof(TSS64) - 1) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) | (((base >> 24) & 0xFF) << 56);
  ap_gdt[cpu][6] = base >> 32;
  struct [[gnu::packed]] {
    u16 limit;
    u64 base;
  } gdtr{.limit = 55, .base = reinterpret_cast<u64>(ap_gdt[cpu])}; // 7 * 8 GDT bytes - 1.
  asm volatile("lgdt %0" ::"m"(gdtr) : "memory");
  asm volatile("pushq $8; leaq 1f(%%rip), %%rax; pushq %%rax; lretq; 1:" ::: "rax", "memory");
  asm volatile("ltr %w0" ::"r"(static_cast<u16>(0x28)));
  set_gs_runtime(cpu, tss);
  asm volatile("lidt %0" ::"m"(g_idtr));
  moss::kernel::hal::intc::enable_secondary_interface();
  moss::kernel::hal::timer::configure_local_timer(false);
  u64 entry = reinterpret_cast<u64>(&moss::abi::syscall_entry_point);
  // SYSCALL MSRs: LSTAR=0xc0000082, STAR=0xc0000081, FMASK=0xc0000084,
  // EFER=0xc0000080. STAR high=0x00100008 selects kernel CS 8 and SYSRET
  // base 0x10; FMASK=0x700 clears TF/IF/DF on entry and EFER.SCE bit 0 enables it.
  asm volatile("wrmsr" ::"c"(0xC0000082U), "a"(static_cast<u32>(entry)), "d"(static_cast<u32>(entry >> 32)));
  asm volatile("wrmsr" ::"c"(0xC0000081U), "a"(0U), "d"(0x00100008U));
  asm volatile("wrmsr" ::"c"(0xC0000084U), "a"(0x700U), "d"(0U));
  u32 lo, hi;
  asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080U));
  asm volatile("wrmsr" ::"c"(0xC0000080U), "a"(lo | 1U), "d"(hi));
  moss::boot::record_cpu_online();
  // 10000000 is TSC ticks, not ns: initial delay=10000000/calibrated_TSC_Hz s.
  // The specific first-compare tick budget has no recorded tuning rationale.
  moss::kernel::hal::timer::set_compare(moss::kernel::hal::timer::read_counter() + 10000000);
  moss::kernel::process::secondary_cpu_schedule_loop(cpu);
}
