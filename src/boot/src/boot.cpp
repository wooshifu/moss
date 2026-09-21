/*
 * Unified boot entry file - module implementation unit
 * Provides unified boot flow control for all architectures
 */

module;

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
#define MOSS_CURRENT_ARCH "ARM64"
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X64)
#define MOSS_CURRENT_ARCH "x64"
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV64)
#define MOSS_CURRENT_ARCH "RISC-V 64"
#endif

// Called only after a real secondary start request, before any readiness wait.
// The validation image may align its first TLB registration with a BSP request.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_cpu_started(unsigned /*cpu*/) noexcept {}

module moss.boot;

import moss.abi;
import moss.hal.uart;

using moss::u32;
using moss::u64;
using moss::VirtAddr;

namespace moss::boot {

static bool notify_tlb_cpu(u32 cpu) noexcept {
  using namespace moss::kernel;
  // The interrupt-controller interface takes a 32-bit logical target mask.
  if (cpu >= sizeof(u32) * 8) {
    return false;
  }
  return hal::intc::send_sgi(platform::intc_dist_base(), platform::intc_cpu_base(), arch::TLB_SHOOTDOWN_SGI,
                             u32{1} << cpu)
      .has_value();
}

void record_cpu_online() noexcept {
  u32 cpu = moss::kernel::arch::get_current_cpu_id();
  if (cpu >= moss::kernel::BOOT_MAX_CPUS) {
    return;
  }
  // Exercise the shared production allocator, owned memory, and cleanup on
  // the calling CPU before publishing its readiness evidence.
  auto page = moss::kernel::mm::PageFrameAllocator::allocate_pages(0);
  if (page) {
    auto *value = reinterpret_cast<volatile u64 *>(*page);
    // ASCII "MOSS" in the high word plus logical CPU ID makes the write/read
    // probe recognizable; the marker is test data, not a persisted memory ABI.
    *value = 0x4d4f535300000000ULL | cpu;
    bool valid = *value == (0x4d4f535300000000ULL | cpu);
    auto released = moss::kernel::mm::PageFrameAllocator::free_pages(*page, 0);
    if (valid && released) {
      __atomic_fetch_or(&cpu_work_mask, 1ULL << cpu, __ATOMIC_RELEASE);
    }
  }
  // Entry/trap state and the local interrupt controller are ready before
  // joining; shared VM operations may target this CPU after registration.
  moss::kernel::arch::register_tlb_cpu(notify_tlb_cpu);
  __atomic_fetch_or(&online_cpu_mask, 1ULL << cpu, __ATOMIC_RELEASE);
}

u32 wait_for_all_cpus_active(u32 timeout_ms) noexcept {
  using namespace moss::kernel;
  const u64 start = hal::timer::read_counter();
  // Counter frequency is Hz; divide by 1000 to convert the millisecond budget
  // to ticks before polling the release/acquire readiness publication.
  const u64 ticks = hal::timer::frequency() / 1000 * timeout_ms;
  const u64 expected = (1ULL << g_num_cpus) - 1;
  for (;;) {
    // Only a CPU that completed its own runtime initialization publishes online.
    // A firmware start request or the BSP's activation flag is not readiness.
    const u64 online = __atomic_load_n(&online_cpu_mask, __ATOMIC_ACQUIRE) & expected;
    if (online == expected || hal::timer::read_counter() - start >= ticks) {
      return static_cast<u32>(__builtin_popcountll(online));
    }
    arch::cpu_yield();
  }
}

// Output is unavailable until firmware discovery configures a console.
static void boot_print(const char *message) { moss::kernel::hal::uart::puts(message); }

/// Unified boot main function
/// All architectures go through this unified entry
[[noreturn]] void unified_boot_main(void *device_tree_ptr) {
  boot_print("\n=== Moss Multi-arch Unified Boot System ===\n");
  boot_print("Target arch: ");
  boot_print(MOSS_CURRENT_ARCH);
  boot_print("\n");

  // Initialize boot context
  BootContext ctx{.device_tree_ptr = device_tree_ptr,
                  .memory_start = 0,
                  .memory_size = 0,
                  .cpu_id = 0,
                  .total_cpus = 1,
                  .kernel_phys_base = 0,
                  .kernel_virt_base = 0};

  boot_print("Boot context initialized\n\n");

  // Execute architecture-specific standardized boot sequence
  boot_print("Starting standardized boot sequence...\n");

  // Stage 1: Hardware early init
  boot_print("Stage 1: Hardware early init\n");
  auto hw_result = ArchBoot::hardware_early_init(ctx);
  if (!hw_result) {
    boot_print("Error: Hardware init failed\n");
    ArchBoot::arch_panic("Hardware initialization failed");
  }

  // Stage 2: Memory management setup
  boot_print("Stage 2: Memory management setup\n");
  auto mem_result = ArchBoot::setup_memory_management(ctx);
  if (!mem_result) {
    boot_print("Error: Memory management setup failed\n");
    ArchBoot::arch_panic("Memory management setup failed");
  }

  // Stage 3: Interrupts and exceptions setup
  boot_print("Stage 3: Interrupts and exceptions setup\n");
  auto int_result = ArchBoot::setup_interrupts_and_exceptions(ctx);
  if (!int_result) {
    boot_print("Error: Interrupt/exception setup failed\n");
    ArchBoot::arch_panic("Interrupt/exception setup failed");
  }

  // Stage 4: SMP support — boot secondary CPUs via PSCI
  boot_print("Stage 4: SMP support setup\n");
  auto smp_result = ArchBoot::setup_smp_support(ctx);
  if (!smp_result) {
    ArchBoot::arch_panic("SMP setup failed; refusing silent single-core fallback");
  }
  boot_print("Stage 4: SMP support setup complete\n");

  // Stage 5: Architecture finalization
  boot_print("Stage 5: Architecture init complete\n");

  auto finalize_result = ArchBoot::finalize_arch_init(ctx);
  if (!finalize_result) {
    boot_print("Error: Architecture finalization failed\n");
    ArchBoot::arch_panic("Architecture finalization failed");
  }

  // Update boot status
  update_boot_stage(BootStage::SystemInit);

  boot_print("=== Architecture-specific boot complete ===\n");
  boot_print("Handing off to architecture-independent system init...\n\n");

  // Hand off to architecture-independent system init
  // Run C++ global constructors (.init_array) before kernel_main.
  // In freestanding environments there is no CRT to do this automatically.
  moss::abi::linker::call_global_constructors();

  record_cpu_online();

  boot_print("Launching MOSS kernel main...\n");

  // Mark boot complete
  update_boot_stage(BootStage::Complete);

  // Call kernel main (never returns)
  moss::abi::entry::kernel_main();
}

} // namespace moss::boot

// C entry point, called by architecture assembly code
extern "C" [[noreturn]] void early_main(void *device_tree_ptr) {
  // Directly call unified boot main
  moss::boot::unified_boot_main(device_tree_ptr);
}
