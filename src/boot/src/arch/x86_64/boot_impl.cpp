/*
 * x86_64 architecture-specific boot implementation - module implementation unit
 * Implements the unified boot interface for x86_64
 */

module;

// Architecture detection (global module fragment)
#ifndef MOSS_ARCH_X86_64
#define MOSS_ARCH_X86_64
#endif

module moss.boot;

import moss.abi;

using moss::PhysAddr;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;
using moss::VirtAddr;

namespace moss::boot {

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

static u32 get_current_cpu_id_impl() noexcept { return 0; }

// Boot stage status update
void update_boot_stage(BootStage stage, ::moss::kernel::ErrorCode error) noexcept {
  g_boot_status.current_stage = stage;
  g_boot_status.last_error = error;

  u32 stage_index = static_cast<u32>(stage);
  if (stage_index < 8) {
    g_boot_status.stage_timestamps[stage_index] = get_timestamp_counter();

    if (error == ::moss::kernel::ErrorCode::Success) {
      g_boot_status.completed_stages_mask |= (1u << stage_index);
    }
  }
}

} // namespace moss::boot

// x86_64BootImpl member function implementations
::moss::kernel::VoidResult moss::boot::X86_64BootImpl::hardware_early_init(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

  moss::boot::EarlyVGA::clear();
  moss::boot::early_print("=== x86_64 Hardware Early Init ===\n");

  ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
  moss::boot::early_print("CPU ID: ");
  moss::boot::early_print_hex(ctx.cpu_id);
  moss::boot::early_print("\n");

  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x00000001) : "memory");
  moss::boot::early_print("CPUID Features: ");
  moss::boot::early_print_hex(edx);
  moss::boot::early_print("\n");

  // x86_64 QEMU q35 不提供 DTB，静态填充 PlatformInfo 以统一子系统接口。
  // 后续可扩展为 ACPI/E820 内存映射解析。
  {
    auto &info = moss::fdt::g_platform_info;
    info = {};
    info.dtb_valid = false;
    // Detect logical processor count via CPUID leaf 1, EBX[23:16]
    {
      u32 eax1 = 0;
      u32 ebx1 = 0;
      u32 ecx1 = 0;
      u32 edx1 = 0;
      asm volatile("cpuid" : "=a"(eax1), "=b"(ebx1), "=c"(ecx1), "=d"(edx1) : "a"(1) : "memory");
      u32 logical_cpus = (ebx1 >> 16) & 0xFF;
      info.cpu_count = (logical_cpus > 0) ? logical_cpus : 1;
    }
    info.memory_regions[0] = {moss::kernel::platform::ram_base(), moss::kernel::platform::ram_size()};
    info.memory_region_count = 1;
    info.total_memory_start = moss::kernel::platform::ram_base();
    info.total_memory_size = moss::kernel::platform::ram_size();

    ctx.memory_start = info.total_memory_start;
    ctx.memory_size = info.total_memory_size;
    ctx.kernel_phys_base = info.total_memory_start;
  }

  ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

  moss::boot::early_print("Memory range: ");
  moss::boot::early_print_hex(ctx.memory_start);
  moss::boot::early_print(" - ");
  moss::boot::early_print_hex(ctx.memory_start + ctx.memory_size);
  moss::boot::early_print("\n");

  moss::boot::early_print("x86_64 hardware init complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_memory_management(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

  moss::boot::early_print("=== x86_64 Memory Management Setup ===\n");

  // In long mode the MMU is always active.  We initialize the unified
  // memory subsystem (page frame allocator, kernel heap, high-half map)
  // using the same interface as ARM64.
  auto mmu_result = ::moss::kernel::mm::setup_mmu();
  if (!mmu_result) {
    moss::boot::early_print("  WARNING: setup_mmu failed\n");
  }

  auto pfa_result = ::moss::kernel::mm::PageFrameAllocator::initialize();
  if (!pfa_result) {
    moss::boot::early_print("  WARNING: PageFrameAllocator init failed\n");
  }

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

// C++ interrupt/exception handler called from isr_common (isr_x86_64.S)
extern "C" void x86_64_interrupt_handler(u64 vector, u64 error_code, [[maybe_unused]] void *frame) noexcept {
  if (vector < 32) {
    // CPU exception — log via VGA and halt
    moss::boot::early_print("x86_64 EXCEPTION: vec=");
    moss::boot::early_print_hex(vector);
    moss::boot::early_print(" err=");
    moss::boot::early_print_hex(error_code);
    moss::boot::early_print("\n");
    // Fatal for now: halt
    asm volatile("cli");
    while (true) {
      asm volatile("hlt");
    }
  }

  // External IRQ (vector >= 32): send EOI to Local APIC
  constexpr u64 LAPIC_EOI_ADDR = 0xFEE000B0ULL;
  auto *lapic_eoi = reinterpret_cast<volatile u32 *>(LAPIC_EOI_ADDR);
  *lapic_eoi = 0;
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
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
    gic->initialize(dist_base, cpu_base, 0);
    g_gic_controller = gic;
    g_gic_hardware_available = true;
    moss::boot::early_print("  Local APIC initialized\n");
  }

  moss::boot::early_print("x86_64 interrupt/exception setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_smp_support(BootContext &ctx) noexcept {
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

  moss::boot::early_print("  Detected CPUs: ");
  moss::boot::early_print_hex(detected);
  moss::boot::early_print(" (BSP only for now)\n");
  moss::boot::early_print("x86_64 SMP setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::finalize_arch_init(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

  moss::boot::early_print("=== x86_64 Architecture Init Complete ===\n");

  // Mark runtime heap as ready so operator new uses RuntimeHeapAllocator
  moss::abi::entry::mark_runtime_heap_ready();
  moss::boot::early_print("Runtime heap marked ready\n");

  moss::boot::early_print("x86_64 architecture-specific init all complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::detect_memory_layout(BootContext &ctx) noexcept {
  (void)ctx;
  return ::moss::kernel::VoidResult{};
}

u32 moss::boot::X86_64BootImpl::get_current_cpu_id() noexcept { return moss::boot::get_current_cpu_id_impl(); }

[[noreturn]] void moss::boot::X86_64BootImpl::arch_panic(const char *message) noexcept {
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

void activate_secondary_cpus() noexcept { early_print("[x86_64] SMP activation not yet implemented\n"); }

u32 wait_for_all_cpus_active([[maybe_unused]] u32 timeout_ms) noexcept { return 1; }

} // namespace moss::boot
