/*
 * x86_64 architecture-specific boot implementation - module implementation unit
 * Implements the unified boot interface for x86_64
 */

module;

// Architecture detection (global module fragment)
#ifndef MOSS_ARCH_X86_64
#define MOSS_ARCH_X86_64
#endif

// Assembly-defined symbols used by x86_64_setup_tss() below
extern "C" {
extern unsigned char g_tss[];                         // 104-byte TSS in start_x86_64.S .bss.tss
extern unsigned long long gdt_table[];                // GDT in start_x86_64.S .data
extern unsigned char _stack_top_addr[];               // Boot stack top (linker symbol)
void early_debug_print(const char *message) noexcept; // UART output (kernel_main.cpp)
extern unsigned char x86_ap_trampoline_start[], x86_ap_trampoline_end[], x86_ap_cr3[], x86_ap_stack[];
[[noreturn]] void x86_secondary_entry() noexcept;

// Page fault handler in mm module (page_fault.cpp)
void x86_64_page_fault_handler(unsigned long long error_code, unsigned long long cr2, unsigned long long rip) noexcept;
}

module moss.boot;

import moss.abi;
import moss.fdt;

using moss::PhysAddr;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;
using moss::VirtAddr;

// =============================================================================
// TSS setup — called from start_x86_64.S before early_main()
// =============================================================================
// x86_64 Task State Segment (104 bytes, defined in start_x86_64.S .bss.tss)
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
  u64 kernel_rsp;
  u64 user_rsp_scratch;
  TSS64 *tss;
};
static X86CpuRuntime cpu_runtime[16];
static TSS64 ap_tss[16];
static u64 ap_gdt[16][7];
alignas(4096) static u8 ap_stacks[16][32768];

static void set_gs_runtime(u32 cpu, TSS64 *tss) noexcept {
  cpu_runtime[cpu].tss = tss;
  u64 base = reinterpret_cast<u64>(&cpu_runtime[cpu]);
  asm volatile("wrmsr" ::"c"(0xC0000101U), "a"(static_cast<u32>(base)), "d"(static_cast<u32>(base >> 32)) : "memory");
}

extern "C" void x86_64_set_kernel_stack(u64 top) noexcept {
  auto cpu = moss::kernel::arch::get_current_cpu_id();
  cpu_runtime[cpu].kernel_rsp = top;
  cpu_runtime[cpu].tss->rsp0 = top;
}

extern "C" void x86_64_setup_tss() noexcept {
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

// Early VGA text output (x86_64 specific)
class EarlyVGA {
private:
  static u16 *const VGA_BUFFER;
  static constexpr u8 VGA_WIDTH = 80;
  static constexpr u8 VGA_HEIGHT = 25;
  static constexpr u8 VGA_COLOR = 0x07;

  static u8 cursor_row;
  static u8 cursor_col;

public:
  static void put_char(char c) {
    if (c == '\n') {
      cursor_col = 0;
      cursor_row++;
      if (cursor_row >= VGA_HEIGHT) {
        cursor_row = VGA_HEIGHT - 1;
        for (u8 row = 0; row < VGA_HEIGHT - 1; row++) {
          for (u8 col = 0; col < VGA_WIDTH; col++) {
            VGA_BUFFER[row * VGA_WIDTH + col] = VGA_BUFFER[(row + 1) * VGA_WIDTH + col];
          }
        }
        for (u8 col = 0; col < VGA_WIDTH; col++) {
          VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = (VGA_COLOR << 8) | ' ';
        }
      }
      return;
    }

    if (cursor_col >= VGA_WIDTH) {
      cursor_col = 0;
      cursor_row++;
    }

    if (cursor_row >= VGA_HEIGHT) {
      cursor_row = VGA_HEIGHT - 1;
    }

    VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = (VGA_COLOR << 8) | static_cast<u8>(c);
    cursor_col++;
  }

  static void put_string(const char *str) {
    while (*str) {
      put_char(*str++);
    }
  }

  static void clear() {
    for (u8 row = 0; row < VGA_HEIGHT; row++) {
      for (u8 col = 0; col < VGA_WIDTH; col++) {
        VGA_BUFFER[row * VGA_WIDTH + col] = (VGA_COLOR << 8) | ' ';
      }
    }
    cursor_row = 0;
    cursor_col = 0;
  }
};

// Static member definitions
u16 *const EarlyVGA::VGA_BUFFER = reinterpret_cast<u16 *>(0xB8000);
u8 EarlyVGA::cursor_row = 0;
u8 EarlyVGA::cursor_col = 0;

static void early_print(const char *str) { EarlyVGA::put_string(str); }

static void early_print_hex(u64 value) {
  constexpr char hex_chars[] = "0123456789ABCDEF";
  char buffer[19] = "0x";

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

// x86_64BootImpl member function implementations
::moss::kernel::VoidResult moss::boot::X86BootImpl::hardware_early_init(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

  moss::boot::EarlyVGA::clear();
  moss::boot::EarlyVGA::put_string("=== x86_64 Hardware Early Init ===\n");
  moss::boot::EarlyVGA::put_string("Checking initramfs locations...\n");

  // Try to get initramfs location from PVH boot info first
  // QEMU passes modules via PVH start_info structure
  bool found_pvh_initramfs = false;

  // PVH start_info is typically passed at a fixed location by QEMU
  // Check common PVH start_info locations
  const auto *start_info = static_cast<const HvmStartInfo *>(ctx.device_tree_ptr);
  if (!start_info || start_info->magic != 0x336ec578 || start_info->version < 1) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument};
  }
  {

    if (start_info->magic == 0x336ec578 && start_info->nr_modules > 0) {
      moss::boot::EarlyVGA::put_string("Found PVH start_info with modules!\n");

      // Get first module (should be initramfs)
      const auto *modlist = reinterpret_cast<const HvmModlistEntry *>(start_info->modlist_paddr);
      if (modlist->paddr != 0 && modlist->size > 0) {
        moss::fdt::g_platform_info.initrd_start = modlist->paddr;
        moss::fdt::g_platform_info.initrd_end = modlist->paddr + modlist->size;
        found_pvh_initramfs = true;

        moss::boot::EarlyVGA::put_string("PVH initramfs: addr=0x");
        moss::boot::early_print_hex(modlist->paddr);
        moss::boot::EarlyVGA::put_string(" size=");
        moss::boot::early_print_hex(modlist->size);
        moss::boot::EarlyVGA::put_string("\n");
      }
    }
  }

  // Fallback if PVH info not found
  if (!found_pvh_initramfs) {
    moss::boot::EarlyVGA::put_string("No PVH initramfs found, using fallback\n");
    moss::fdt::g_platform_info.initrd_start = 0;
    moss::fdt::g_platform_info.initrd_end = 0;
  }

  ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
  moss::boot::EarlyVGA::put_string("CPU initialized\n");

  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x00000001) : "memory");
  moss::boot::EarlyVGA::put_string("CPUID detected\n");

  // x86_64 QEMU q35 不提供 DTB，静态填充 PlatformInfo 以统一子系统接口。
  // 后续可扩展为 ACPI/E820 内存映射解析。
  {
    auto &info = moss::fdt::g_platform_info;
    // Preserve initrd addresses discovered from PVH modules above
    PhysAddr saved_initrd_start = info.initrd_start;
    PhysAddr saved_initrd_end = info.initrd_end;
    info = {};
    info.initrd_start = saved_initrd_start;
    info.initrd_end = saved_initrd_end;
    info.dtb_valid = false;
    // Detect logical processor count via CPUID leaf 1, EBX[23:16]
    {
      u32 eax1 = 0;
      u32 ebx1 = 0;
      u32 ecx1 = 0;
      u32 edx1 = 0;
      asm volatile("cpuid" : "=a"(eax1), "=b"(ebx1), "=c"(ecx1), "=d"(edx1) : "a"(1) : "memory");
      // The microvm profile uses one socket and dense APIC IDs. fw_cfg's
      // NB_CPUS describes present CPUs, unlike CPUID's topology capacity.
      asm volatile("outw %0, %1" ::"a"(static_cast<u16>(5)), "Nd"(static_cast<u16>(0x510)));
      u8 low = 0;
      u8 high = 0;
      asm volatile("inb %1, %0" : "=a"(low) : "Nd"(static_cast<u16>(0x511)));
      asm volatile("inb %1, %0" : "=a"(high) : "Nd"(static_cast<u16>(0x511)));
      u32 logical_cpus = low | (static_cast<u32>(high) << 8);
      if (logical_cpus == 0 || logical_cpus > 16) {
        return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument};
      }
      info.cpu_count = (logical_cpus > 0) ? logical_cpus : 1;
    }
    struct PvhMemoryEntry {
      u64 base;
      u64 size;
      u32 type;
      u32 reserved;
    };
    if (!start_info->memmap_paddr || start_info->memmap_entries > 128) {
      return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument};
    }
    const auto *map = reinterpret_cast<const PvhMemoryEntry *>(start_info->memmap_paddr);
    for (u32 i = 0; i < start_info->memmap_entries; ++i) {
      if (map[i].type == 1 && map[i].size) {
        if (info.memory_region_count == moss::fdt::MAX_MEMORY_REGIONS) {
          return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument};
        }
        info.memory_regions[info.memory_region_count++] = {.base = map[i].base, .size = map[i].size};
        info.total_memory_size += map[i].size;
      }
    }
    info.memory_map_valid = info.memory_region_count > 0;
    info.total_memory_start = info.memory_regions[0].base;
    info.bootargs = reinterpret_cast<const char *>(start_info->cmdline_paddr);

    ctx.memory_start = info.total_memory_start;
    ctx.memory_size = info.total_memory_size;
    ctx.kernel_phys_base = info.total_memory_start;
  }

  ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

  moss::boot::EarlyVGA::put_string("Memory mapping configured\n");
  moss::boot::EarlyVGA::put_string("x86_64 hardware init complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::setup_memory_management(BootContext & /*ctx*/) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

  moss::boot::early_print("=== x86_64 Memory Management Setup ===\n");

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

  // Initialize runtime heap allocator with a 256 KB region
  VirtAddr heap_start = moss::abi::linker::heap_start();
  ::moss::kernel::usize initial_heap_size = 256ULL * 1024;
  auto heap_result = ::moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(heap_start, initial_heap_size);
  if (!heap_result) {
    moss::boot::early_print("  WARNING: RuntimeHeapAllocator init failed\n");
  }

  moss::boot::early_print("x86_64 memory management setup complete\n\n");
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

// ISR stub table defined in isr_x86_64.S
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
extern "C" void (*g_x86_64_timer_handler)() noexcept = nullptr;
extern "C" void (*g_x86_64_uart_rx_handler)() noexcept = nullptr;

// C++ interrupt/exception handler called from isr_common (isr_x86_64.S)
extern "C" void x86_64_interrupt_handler(u64 vector, u64 error_code, [[maybe_unused]] void *frame) noexcept {
  if (vector < 32) {
    // frame layout: R15..RAX (15 regs), then vector, error_code, RIP, CS, RFLAGS, RSP, SS
    // frame points to saved R15, so RIP is at frame[17] (15 regs + vector + error_code)
    auto *frame_u64 = reinterpret_cast<u64 *>(frame);

    // Page fault (#PF, vector 14): dispatch to demand paging / COW handler
    if (vector == 14) {
      u64 cr2 = 0;
      asm volatile("mov %%cr2, %0" : "=r"(cr2));
      u64 rip = frame_u64[17];
      x86_64_page_fault_handler(error_code, cr2, rip);
      return; // Handler resolved the fault — iretq retries the instruction
    }

    // Other CPU exceptions — print diagnostics and halt
    early_debug_print("EXCEPTION vec=");
    uart_print_hex(vector);
    early_debug_print(" err=");
    uart_print_hex(error_code);
    early_debug_print(" RIP=");
    uart_print_hex(frame_u64[17]);
    early_debug_print("\nHALTED\n");
    asm volatile("cli; hlt");
    __builtin_unreachable();
  }

  // External IRQ (vector >= 32): send EOI first, then dispatch
  constexpr u64 LAPIC_EOI_ADDR = 0xFEE000B0ULL;
  auto *lapic_eoi = reinterpret_cast<volatile u32 *>(LAPIC_EOI_ADDR);
  *lapic_eoi = 0;

  // LAPIC timer (vector 48)
  if (vector == 48 && g_x86_64_timer_handler != nullptr) {
    g_x86_64_timer_handler();
    return;
  }

  // COM1 UART RX (vector 36 = IRQ4 routed via I/O APIC)
  if (vector == 36 && g_x86_64_uart_rx_handler != nullptr) {
    g_x86_64_uart_rx_handler();
  }
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

  moss::boot::early_print("=== x86_64 Interrupts and Exceptions Setup ===\n");

  // 1. Load IDT with 256 entries pointing to ISR stubs
  setup_idt();
  moss::boot::early_print("  IDT loaded (256 entries)\n");

  // 2. Initialize Local APIC (enable via SVR register)
  auto *gic = new moss::kernel::interrupts::GenericInterruptController();
  if (gic) {
    VirtAddr dist_base = moss::kernel::platform::intc_dist_base(); // Local APIC
    VirtAddr cpu_base = moss::kernel::platform::intc_cpu_base();   // I/O APIC
    (void)gic->initialize(dist_base, cpu_base, 0);
    g_gic_controller = gic;
    g_gic_hardware_available = true;
    moss::boot::early_print("  Local APIC initialized\n");
  }

  // 3. Configure LAPIC timer: divide-by-16, one-shot, vector 48, initially masked
  {
    constexpr u64 LAPIC_BASE = 0xFEE00000ULL;
    auto *div_config = reinterpret_cast<volatile u32 *>(LAPIC_BASE + 0x3E0); // Divide Configuration
    auto *lvt_timer = reinterpret_cast<volatile u32 *>(LAPIC_BASE + 0x320);  // LVT Timer
    *div_config = 0x03;                                                      // divide by 16
    *lvt_timer = 48 | (1U << 16); // vector 48, one-shot (bit17=0), masked (bit16=1)
    moss::boot::early_print("  LAPIC timer configured (vec=48, masked)\n");
  }

  moss::boot::early_print("x86_64 interrupt/exception setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::setup_smp_support(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

  moss::boot::early_print("=== x86_64 SMP Support Setup ===\n");

  // Use the CPU count detected via CPUID during hardware_early_init.
  // AP boot via INIT-SIPI-SIPI requires:
  //   1. A real-mode trampoline page below 1 MB
  //   2. ACPI/MP table parsing to discover APIC IDs
  //   3. Per-AP GDT, page tables, and stack allocation
  // These prerequisites are substantial; for now we report the detected
  // count and boot only the BSP.
  u32 detected = moss::fdt::g_platform_info.cpu_count;
  ctx.total_cpus = detected;
  moss::kernel::g_num_cpus = detected;

  moss::boot::early_print("  Detected CPUs: ");
  moss::boot::early_print_hex(detected);
  moss::boot::early_print(" (BSP only for now)\n");
  moss::boot::early_print("x86_64 SMP setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::finalize_arch_init(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

  moss::boot::early_print("=== x86_64 Architecture Init Complete ===\n");

  // Mark runtime heap as ready so operator new uses RuntimeHeapAllocator
  moss::abi::entry::mark_runtime_heap_ready();
  moss::boot::early_print("Runtime heap marked ready\n");

  moss::boot::early_print("x86_64 architecture-specific init all complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86BootImpl::detect_memory_layout(BootContext &ctx) noexcept {
  (void)ctx;
  return ::moss::kernel::VoidResult{};
}

u32 moss::boot::X86BootImpl::get_current_cpu_id() noexcept { return moss::boot::get_current_cpu_id_impl(); }

[[noreturn]] void moss::boot::X86BootImpl::arch_panic(const char *message) noexcept {
  moss::boot::early_print("\n=== x86_64 PANIC ===\n");
  moss::boot::early_print(message);
  moss::boot::early_print("\n====================\n");

  asm volatile("outw %0, %1" : : "a"(static_cast<u16>(0x2000)), "d"(static_cast<u16>(0x604)) : "memory");

  asm volatile("cli");
  while (true) {
    asm volatile("hlt");
  }
}

// === Boot global variables (x86_64 stubs) ===
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
  auto *copy = reinterpret_cast<u8 *>(0x8000);
  u64 size = static_cast<u64>(x86_ap_trampoline_end - x86_ap_trampoline_start);
  if (size > 4096) {
    return;
  }
  for (u64 i = 0; i < size; ++i) {
    copy[i] = x86_ap_trampoline_start[i];
  }
  u64 cr3;
  asm volatile("mov %%cr3, %0" : "=r"(cr3));
  *reinterpret_cast<u64 *>(copy + (x86_ap_cr3 - x86_ap_trampoline_start)) = cr3;
  auto *icr_lo = reinterpret_cast<volatile u32 *>(0xFEE00300ULL);
  auto *icr_hi = reinterpret_cast<volatile u32 *>(0xFEE00310ULL);
  for (u32 cpu = 1; cpu < moss::kernel::g_num_cpus; ++cpu) {
    *reinterpret_cast<u64 *>(copy + (x86_ap_stack - x86_ap_trampoline_start)) =
        reinterpret_cast<u64>(&ap_stacks[cpu][32768]);
    moss::kernel::arch::memory_barrier();
    *icr_hi = cpu << 24;
    *icr_lo = 0x0000C500; // INIT, level assert.
    ap_delay();
    *icr_hi = cpu << 24;
    *icr_lo = 0x00008500; // INIT deassert.
    ap_delay();
    *icr_hi = cpu << 24;
    *icr_lo = 0x00000608; // SIPI vector 8, physical 0x8000.
    ap_delay();
    if (!(__atomic_load_n(&online_cpu_mask, __ATOMIC_ACQUIRE) & (1ULL << cpu))) {
      *icr_hi = cpu << 24;
      *icr_lo = 0x00000608;
    }
    for (u32 retry = 0; retry < 100000000; ++retry) {
      if (__atomic_load_n(&online_cpu_mask, __ATOMIC_ACQUIRE) & (1ULL << cpu)) {
        break;
      }
      moss::kernel::arch::cpu_yield();
    }
  }
}

u32 wait_for_all_cpus_active([[maybe_unused]] u32 timeout_ms) noexcept {
  return static_cast<u32>(__builtin_popcountll(__atomic_load_n(&online_cpu_mask, __ATOMIC_ACQUIRE)));
}

} // namespace moss::boot

extern "C" [[noreturn]] void x86_secondary_entry() noexcept {
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
  } gdtr{.limit = 55, .base = reinterpret_cast<u64>(ap_gdt[cpu])};
  asm volatile("lgdt %0" ::"m"(gdtr) : "memory");
  asm volatile("pushq $8; leaq 1f(%%rip), %%rax; pushq %%rax; lretq; 1:" ::: "rax", "memory");
  asm volatile("ltr %w0" ::"r"(static_cast<u16>(0x28)));
  set_gs_runtime(cpu, tss);
  asm volatile("lidt %0" ::"m"(g_idtr));
  *reinterpret_cast<volatile u32 *>(0xFEE000F0ULL) = 0x1FF;
  *reinterpret_cast<volatile u32 *>(0xFEE00080ULL) = 0;
  *reinterpret_cast<volatile u32 *>(0xFEE003E0ULL) = 3;
  *reinterpret_cast<volatile u32 *>(0xFEE00320ULL) = 48;
  u64 entry = reinterpret_cast<u64>(&moss::abi::syscall_entry_point);
  asm volatile("wrmsr" ::"c"(0xC0000082U), "a"(static_cast<u32>(entry)), "d"(static_cast<u32>(entry >> 32)));
  asm volatile("wrmsr" ::"c"(0xC0000081U), "a"(0U), "d"(0x00100008U));
  asm volatile("wrmsr" ::"c"(0xC0000084U), "a"(0x700U), "d"(0U));
  u32 lo, hi;
  asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080U));
  asm volatile("wrmsr" ::"c"(0xC0000080U), "a"(lo | 1U), "d"(hi));
  moss::boot::record_cpu_online();
  moss::kernel::hal::timer::set_compare(moss::kernel::hal::timer::read_counter() + 10000000);
  moss::kernel::process::secondary_cpu_schedule_loop(cpu);
}
