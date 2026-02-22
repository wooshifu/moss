// MOSS Kernel Module - Main Partition
// Kernel class, subsystem states, boot phases, and kernel statistics.

export module moss.kernel:main;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.platform;
import moss.hal.uart;
import moss.hal.intc;
import moss.hal.timer;
import moss.containers;
import moss.mm;
import moss.interrupts;
import moss.drivers;
import moss.fdt;
import moss.initramfs;
import moss.ipc;
import moss.process;
import moss.timer;
import moss.logging;
import moss.boot;
import moss.vfs;
import moss.abi;

import :elf;
import :syscall_table;
import :syscall_arch;

// ABI symbols used by kernel boot/init
using moss::abi::syscall_entry_point;
using moss::abi::entry::early_debug_print;

export namespace moss::kernel {

namespace log = moss::kernel::logging;

// Kernel subsystem state
enum class SubsystemState : u8 { Uninitialized = 0, Initializing = 1, Active = 2, Error = 3 };

// Kernel boot phase
enum class BootPhase : u8 {
  EarlyInit = 0,     // Early initialization (after assembly)
  MemoryInit = 1,    // Memory management initialization
  SchedulerInit = 2, // Scheduler initialization
  IpcInit = 3,       // IPC system initialization
  DeviceInit = 4,    // Device management initialization
  ServiceInit = 5,   // System service startup
  UserInit = 6,      // User-space initialization
  Completed = 7      // Boot completed
};

// Kernel statistics
struct KernelStats {
  u64 boot_time;          // Boot time
  u64 uptime;             // Uptime
  u64 total_memory;       // Total memory
  u64 free_memory;        // Free memory
  u32 active_processes;   // Active process count
  u32 total_threads;      // Total thread count
  u64 context_switches;   // Context switch count
  u64 interrupts_handled; // Interrupts handled
  u64 ipc_messages;       // IPC message count
  u32 registered_devices; // Registered device count
};

// Kernel main class
class Kernel {
private:
  // Boot state
  BootPhase current_phase_;
  [[maybe_unused]] SubsystemState subsystem_states_[8]; // Per-subsystem states

  // Core subsystem instances
  containers::ContainerLibrary *container_lib_;
  mm::PageTableManager *page_table_manager_;
  process::ProcessManager *process_manager_;
  process::CfsScheduler *scheduler_;
  process::LoadBalancer *load_balancer_;
  ipc::SharedMemoryManager *shared_memory_manager_;
  ipc::IpcManager *ipc_manager_;
  interrupts::GenericInterruptController *gic_;
  drivers::DeviceManager *device_manager_;

  // Boot time recording
  u64 boot_start_time_;
  u64 phase_start_times_[8];

  // Kernel configuration
  struct KernelConfig {
    bool enable_smp;             // Enable multi-core support
    bool enable_preemption;      // Enable preemptive scheduling
    u32 max_processes;           // Maximum process count
    u32 max_threads_per_process; // Maximum threads per process
    usize kernel_heap_size;      // Kernel heap size
    bool enable_debug_output;    // Enable debug output
    u32 scheduler_timeslice_ms;  // Scheduling timeslice (milliseconds)
  } config_;

public:
  Kernel() noexcept
      : current_phase_(BootPhase::EarlyInit), subsystem_states_{SubsystemState::Uninitialized}, container_lib_(nullptr),
        page_table_manager_(nullptr), process_manager_(nullptr), scheduler_(nullptr), load_balancer_(nullptr),
        shared_memory_manager_(nullptr), ipc_manager_(nullptr), gic_(nullptr), device_manager_(nullptr),
        boot_start_time_(0), phase_start_times_{0} {
    // Initialize kernel configuration
    config_ = {.enable_smp = true,
               .enable_preemption = true,
               .max_processes = 256,
               .max_threads_per_process = 16,
               .kernel_heap_size = 16ULL * 1024 * 1024, // 16MB
               .enable_debug_output = true,
               .scheduler_timeslice_ms = 10};
  }

  ~Kernel() noexcept { shutdown(); }

  // Non-copyable, non-movable
  Kernel(const Kernel &) = delete;
  Kernel &operator=(const Kernel &) = delete;
  Kernel(Kernel &&) = delete;
  Kernel &operator=(Kernel &&) = delete;

  // Kernel initialization main entry
  [[nodiscard]] VoidResult initialize() noexcept {
    boot_start_time_ = get_current_time();

    print_banner();

    // Initialize kernel phase by phase
    auto result = initialize_phase_by_phase();
    if (!result) {
      kernel_panic("Kernel initialization failed", result.error());
    }

    current_phase_ = BootPhase::Completed;

    early_debug_print("[boot] MOSS kernel boot completed\n");

    return VoidResult{};
  }

  // Kernel main loop
  [[nodiscard]] VoidResult run() noexcept {
    early_debug_print("[kernel] MOSS kernel starting...\n");

    // Enable interrupts
    enable_interrupts();

    // Initialize initramfs if bootloader provided one via DTB
    {
      auto &pi = fdt::g_platform_info;
      if (pi.initrd_start != 0 && pi.initrd_end > pi.initrd_start) {
        usize initrd_size = static_cast<usize>(pi.initrd_end - pi.initrd_start);
        log::klog::info("initramfs: found at {:#x}-{:#x} ({} bytes)", pi.initrd_start, pi.initrd_end, initrd_size);
        initramfs::g_initramfs.init(pi.initrd_start, initrd_size);
      } else {
        log::klog::info("initramfs: not present (no -initrd passed to QEMU)");
      }
    }

    // Initialize VFS: mount root (ramfs) + devfs
    vfs::vfs_init();
    early_debug_print("[kernel] VFS initialized\n");

    // Create initial user process
    auto init_result = create_init_process();
    if (!init_result) {
      kernel_panic("Failed to create init process", init_result.error());
    }

    // Enter scheduling loop (start_scheduling is [[noreturn]])
    early_debug_print("[kernel] Entering scheduling loop\n");
    scheduler_->start_scheduling();
  }

  // Kernel shutdown
  void shutdown() noexcept {
    log::klog::info("MOSS kernel shutting down...");

    // Shutdown subsystems in reverse order
    if (device_manager_) {
      (void)device_manager_->suspend_all_devices();
      delete device_manager_;
      device_manager_ = nullptr;
    }

    if (ipc_manager_) {
      delete ipc_manager_;
      ipc_manager_ = nullptr;
    }

    if (shared_memory_manager_) {
      delete shared_memory_manager_;
      shared_memory_manager_ = nullptr;
    }

    if (scheduler_) {
      delete scheduler_;
      scheduler_ = nullptr;
    }

    if (process_manager_) {
      delete process_manager_;
      process_manager_ = nullptr;
    }

    if (page_table_manager_) {
      delete page_table_manager_;
      page_table_manager_ = nullptr;
    }

    if (container_lib_) {
      containers::ContainerLibrary::cleanup();
    }

    // Shutdown memory management system last
    log::klog::info("Shutting down unified memory management system...");
    mm::shutdown_kernel_memory();

    log::klog::info("MOSS kernel shutdown complete");
  }

  // Get kernel statistics
  [[nodiscard]] KernelStats get_statistics() const noexcept {
    KernelStats stats = {};

    stats.boot_time = boot_start_time_;
    stats.uptime = get_current_time() - boot_start_time_;

    if (process_manager_) {
      // stats.active_processes = process_manager_->get_process_count();
    }

    if (gic_) {
      auto gic_stats = gic_->get_statistics();
      stats.interrupts_handled = gic_stats.total_interrupts;
    }

    if (ipc_manager_) {
      auto ipc_stats = ipc_manager_->get_statistics();
      stats.ipc_messages = ipc_stats.messages_processed;
    }

    if (device_manager_) {
      auto dev_stats = device_manager_->get_statistics();
      stats.registered_devices = dev_stats.total_devices;
    }

    return stats;
  }

  // Kernel debug interface
  void print_system_info() const noexcept {
    log::klog::info("=== MOSS Kernel System Info ===");
    log::klog::info("Boot phase: {}", static_cast<int>(current_phase_));
    log::klog::info("SMP support: {}", config_.enable_smp ? "enabled" : "disabled");
    log::klog::info("Preemptive scheduling: {}", config_.enable_preemption ? "enabled" : "disabled");

    auto stats = get_statistics();
    log::klog::info("Uptime: {} cycles", stats.uptime);
    log::klog::info("Active processes: {}", stats.active_processes);
    log::klog::info("Interrupts handled: {}", stats.interrupts_handled);
    log::klog::info("IPC messages: {}", stats.ipc_messages);
    log::klog::info("Registered devices: {}", stats.registered_devices);
    log::klog::info("===============================");
  }

private:
  // Phase-by-phase initialization
  [[nodiscard]] VoidResult initialize_phase_by_phase() noexcept {
    const char *phase_names[] = {"Early init",        "Memory management", "Scheduler",  "IPC system",
                                 "Device management", "System services",   "User-space", "Complete"};

    for (int phase = 0; phase < 7; ++phase) {
      current_phase_ = static_cast<BootPhase>(phase);
      phase_start_times_[phase] = get_current_time();

      // Direct UART: avoid klog lock contention after secondary CPUs start
      early_debug_print("[boot] Phase ");
      char ph[2] = {static_cast<char>('0' + phase), '\0'};
      early_debug_print(ph);
      early_debug_print(": ");
      early_debug_print(phase_names[phase]);
      early_debug_print("\n");

      VoidResult result = VoidResult{ErrorCode::NotSupported};

      switch (current_phase_) {
      case BootPhase::EarlyInit:
        result = initialize_early();
        break;
      case BootPhase::MemoryInit:
        result = initialize_memory();
        break;
      case BootPhase::SchedulerInit:
        result = initialize_scheduler();
        break;
      case BootPhase::IpcInit:
        result = initialize_ipc();
        break;
      case BootPhase::DeviceInit:
        result = initialize_devices();
        break;
      case BootPhase::ServiceInit:
        result = initialize_services();
        break;
      case BootPhase::UserInit:
        result = initialize_userspace();
        break;
      case BootPhase::Completed:
        result = VoidResult{};
        break;
      default:
        break;
      }

      if (!result) {
        early_debug_print("[boot] Phase FAILED\n");
        return result;
      }

      early_debug_print("[boot] Phase ");
      early_debug_print(ph);
      early_debug_print(" complete\n");
    }

    return VoidResult{};
  }

  // Early initialization
  [[nodiscard]] VoidResult initialize_early() noexcept {
    // Initialize container library
    if (!containers::ContainerLibrary::initialize()) {
      return VoidResult{ErrorCode::InternalError};
    }
    container_lib_ = nullptr; // ContainerLibrary is a singleton/static, no instance needed

    // Initialize timer subsystem (clocksource + hardware timer)
    log::klog::info("Initializing timer subsystem...");
    auto timer_result = timer::TimerSubsystem::instance().initialize();
    if (!timer_result) {
      log::klog::warn("Timer subsystem init failed (non-fatal)");
      // Non-fatal: kernel can operate without timer, just no preemption
    } else {
      auto freq = timer::TimerSubsystem::instance().clocksource().frequency_hz();
      log::klog::info("Timer subsystem initialized: freq={} Hz", freq);
    }

    return VoidResult{};
  }

  // Memory management initialization
  [[nodiscard]] VoidResult initialize_memory() noexcept {
    log::klog::info("Initializing unified memory management system...");

    // Initialize unified memory management system first
    if (!mm::initialize_kernel_memory()) {
      log::klog::error("Unified memory management system initialization failed");
      return VoidResult{ErrorCode::InternalError};
    }
    log::klog::info("Unified memory management system initialized");

    // Create page table manager (now uses new memory management system)
    page_table_manager_ = new mm::PageTableManager();
    if (!page_table_manager_) {
      log::klog::error("PageTableManager creation failed");
      mm::shutdown_kernel_memory();
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Initialize page table manager (use current page table)
    auto init_result = moss::kernel::mm::PageTableManager::initialize_from_current();
    if (!init_result) {
      log::klog::error("PageTableManager initialization failed");
      delete page_table_manager_;
      page_table_manager_ = nullptr;
      mm::shutdown_kernel_memory();
      return init_result;
    }

    // Check memory system health
    if (!mm::is_memory_system_healthy()) {
      log::klog::warn("Memory system status abnormal");
    }

    // Print memory system information
    auto pressure = mm::get_memory_pressure();
    const char *pressure_str = "UNKNOWN";
    switch (pressure) {
    case mm::MemoryPressure::LOW:
      pressure_str = "LOW";
      break;
    case mm::MemoryPressure::MEDIUM:
      pressure_str = "MEDIUM";
      break;
    case mm::MemoryPressure::HIGH:
      pressure_str = "HIGH";
      break;
    case mm::MemoryPressure::CRITICAL:
      pressure_str = "CRITICAL";
      break;
    default:
      pressure_str = "UNKNOWN";
      break;
    }
    log::klog::info("Memory pressure level: {}", pressure_str);

    return VoidResult{};
  }

  // Scheduler initialization
  [[nodiscard]] VoidResult initialize_scheduler() noexcept {
    // Create process manager
    process_manager_ = new process::ProcessManager();
    if (!process_manager_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Set global process manager pointer
    ::moss::kernel::process::g_process_manager = process_manager_;

    // Create CFS scheduler
    scheduler_ = new process::CfsScheduler();
    if (!scheduler_) {
      delete process_manager_;
      process_manager_ = nullptr;
      ::moss::kernel::process::g_process_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Set global scheduler pointer
    ::moss::kernel::process::g_scheduler = scheduler_;

    // Create load balancer
    load_balancer_ = new process::LoadBalancer();
    ::moss::kernel::process::g_load_balancer = load_balancer_;
    if (!load_balancer_) {
      // NOTE: error handling below covers cleanup
      delete scheduler_;
      delete process_manager_;
      scheduler_ = nullptr;
      process_manager_ = nullptr;
      ::moss::kernel::process::g_scheduler = nullptr;
      ::moss::kernel::process::g_load_balancer = nullptr;
      ::moss::kernel::process::g_process_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Wire periodic load balance into scheduler_tick via callback.
    // This avoids circular module partition dependency (scheduler→load_balancer).
    scheduler_->set_balance_callback([](u64 now, process::CfsScheduler *sched) {
      if (::moss::kernel::process::g_load_balancer && sched) {
        ::moss::kernel::process::g_load_balancer->periodic_balance(now, *sched);
      }
    });

    // Linux-style SMP delayed activation: activate secondary CPUs after scheduler is ready
    if (config_.enable_smp) {
      early_debug_print("[sched] activating parked secondary CPUs...\n");

      // Activate all parked secondary CPUs
      moss::boot::activate_secondary_cpus();

      // Wait for secondary CPUs to complete activation
      u32 active_cpus = moss::boot::wait_for_all_cpus_active(5000);

      early_debug_print("[sched] CPU activation complete\n");

      if (active_cpus > 1) {
        early_debug_print("[sched] multi-CPU scheduler started\n");
      } else {
        early_debug_print("[sched] single-core mode\n");
      }
    }

    return VoidResult{};
  }

  // IPC system initialization
  [[nodiscard]] VoidResult initialize_ipc() noexcept {
    // Create shared memory manager
    shared_memory_manager_ = new ipc::SharedMemoryManager();
    if (!shared_memory_manager_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Set global shared memory manager pointer
    ::moss::kernel::ipc::g_shared_memory_manager = shared_memory_manager_;

    // Create IPC manager
    ipc_manager_ = new ipc::IpcManager(shared_memory_manager_);
    if (!ipc_manager_) {
      delete shared_memory_manager_;
      shared_memory_manager_ = nullptr;
      ::moss::kernel::ipc::g_shared_memory_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Set global IPC manager pointer
    ::moss::kernel::ipc::g_ipc_manager = ipc_manager_;

    return VoidResult{};
  }

  // Device management initialization
  [[nodiscard]] VoidResult initialize_devices() noexcept {
    // Create GIC interrupt controller
    gic_ = new interrupts::GenericInterruptController();
    if (!gic_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 从 DTB 解析结果获取 GIC 地址，若 DTB 无效则回退到 QEMU virt 默认值
    const auto &plat = ::moss::fdt::get_platform_info();
    VirtAddr gic_dist_base =
        (plat.dtb_valid && plat.intc.valid) ? static_cast<VirtAddr>(plat.intc.dist_base) : platform::intc_dist_base();
    VirtAddr gic_cpu_base =
        (plat.dtb_valid && plat.intc.valid) ? static_cast<VirtAddr>(plat.intc.cpu_base) : platform::intc_cpu_base();

    auto gic_result = gic_->initialize(gic_dist_base, gic_cpu_base);
    if (!gic_result) {
      delete gic_;
      gic_ = nullptr;
      return gic_result;
    }

    // Set global GIC pointer so other subsystems can access it
    ::moss::kernel::interrupts::g_gic = gic_;

    // Create device manager
    device_manager_ = new drivers::DeviceManager();
    if (!device_manager_) {
      delete gic_;
      gic_ = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Initialize multi-architecture syscall support
    log::klog::info("Initializing multi-architecture syscall support...");
    if (!arch::syscall::initialize_architecture_syscalls()) {
      log::klog::error("Syscall architecture initialization failed");
      return VoidResult{ErrorCode::NotSupported};
    }
    log::klog::info("Syscall architecture initialization succeeded");

    // Print syscall architecture information
    arch::syscall::print_syscall_convention();

    return VoidResult{};
  }

  // System service initialization
  [[nodiscard]] VoidResult initialize_services() noexcept {
    // Kernel service threads can be started here
    // e.g., memory reclamation, timer services, etc.
    return VoidResult{};
  }

  // User-space initialization
  [[nodiscard]] VoidResult initialize_userspace() noexcept {
    // Prepare user-space environment
    // Set up user-mode page tables, load initial programs, etc.
    return VoidResult{};
  }

  // Create initial user-mode process (TID=1000, the "init" process).
  //
  // The scheduler's context_switch_to_task() recognises TID=1000 and calls
  // switch_to_user(), which performs an eret to EL0. The user program is
  // embedded directly in the kernel binary (arm64_user_program.S).
  [[nodiscard]] VoidResult create_init_process() noexcept {
#if defined(MOSS_ARCH_ARM64)
    using namespace process;

    early_debug_print("[init] creating init user process\n");

    // Step 1: Create process
    if (!process_manager_) {
      early_debug_print("[init] ERROR: process manager not initialized\n");
      return VoidResult{ErrorCode::InvalidState};
    }
    auto proc_result = process_manager_->create_process(0);
    if (!proc_result) {
      early_debug_print("[init] ERROR: failed to create init process\n");
      return VoidResult{proc_result.error()};
    }
    Process *init_proc = proc_result.value();
    ProcessId init_pid = init_proc->pid();
    early_debug_print("[init] init process registered\n");

    // Step 2: Create real AddressSpace with buddy-allocated PGD
    auto as_result = user_space::create_user_address_space();
    if (!as_result) {
      early_debug_print("[init] ERROR: failed to create address space\n");
      return VoidResult{as_result.error()};
    }
    auto as = moss::move(*as_result);

    // Step 3: Register VMA regions for demand paging
    //
    // The embedded user program is raw machine code (not ELF).
    // We place it at a fixed user virtual address and register as a code VMA
    // with backing data pointing to the kernel-resident copy.
    const auto *raw_code = moss::abi::arm64::user_program_start();
    usize code_size = moss::abi::arm64::user_program_size();

    // Code VMA: readable + executable, backed by the embedded raw program
    VirtAddr code_end = (user_layout::CODE_BASE + code_size + PAGE_SIZE - 1) & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
    as->add_vma(user_layout::CODE_BASE, code_end, vma_flags::READ | vma_flags::EXEC, VmaType::CODE, raw_code, 0,
                code_size);
    early_debug_print("[init] VMA code registered\n");

    VirtAddr entry_point = user_layout::CODE_BASE; // entry = start of raw code

    // Stack VMA: demand-zero
    constexpr VirtAddr STACK_BOTTOM = user_layout::STACK_TOP - user_layout::STACK_SIZE;
    as->add_vma(STACK_BOTTOM, user_layout::STACK_TOP, vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO,
                VmaType::STACK);
    early_debug_print("[init] VMA stack registered\n");

    // Heap VMA: small initial region, demand-zero
    as->add_vma(user_layout::HEAP_START, user_layout::HEAP_START + user_layout::HEAP_INIT,
                vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO, VmaType::HEAP);
    as->brk_base = user_layout::HEAP_START;
    as->brk_current = user_layout::HEAP_START;
    as->mmap_next = user_layout::MMAP_BASE;

    // Bind AddressSpace to process
    auto set_result = init_proc->set_address_space(moss::move(as));
    if (!set_result) {
      early_debug_print("[init] ERROR: failed to set address space\n");
      return VoidResult{set_result.error()};
    }

    // Step 4: Create thread with user-space entry point
    ThreadId init_tid = Process::allocate_thread_id();
    auto *init_thread = new Thread(init_tid, init_pid);
    if (!init_thread) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Allocate per-thread kernel stack (16KB = order 2, 4 pages).
    // This stack is used as SP_EL1 when handling exceptions from this
    // thread's user-mode execution — prevents all user processes from
    // sharing the single boot stack.
    constexpr usize KERNEL_STACK_ORDER = 2; // 4 pages = 16KB
    constexpr usize KERNEL_STACK_SIZE = PAGE_SIZE << KERNEL_STACK_ORDER;
    auto kstack_result = mm::allocate_pages(KERNEL_STACK_ORDER);
    if (!kstack_result) {
      early_debug_print("[init] ERROR: failed to allocate kernel stack\n");
      delete init_thread;
      return VoidResult{ErrorCode::OutOfMemory};
    }
    // Use physical address directly (identity-mapped region)
    PhysAddr kstack_phys = *kstack_result;
    init_thread->kernel_stack_base = static_cast<VirtAddr>(kstack_phys);
    init_thread->kernel_stack_size = KERNEL_STACK_SIZE;
    early_debug_print("[init] kernel stack allocated\n");

    // User context: entry point and stack pointer are user-space VAs
    // (demand-paged on first access)
    init_thread->stack_base = STACK_BOTTOM;
    init_thread->stack_size = user_layout::STACK_SIZE;
    init_thread->context.pc = entry_point;
    init_thread->context.sp = user_layout::STACK_TOP - 16; // 16-byte aligned
    init_thread->context.pstate = 0x00000000;              // EL0t
    init_thread->needs_initial_eret = true;                // First dispatch uses switch_to_user + eret
    init_thread->is_user_task = true;                      // Permanent: drives TTBR0 switch on re-dispatch

    init_thread->sched_class = SchedClass::Normal;
    init_thread->se.nice = -5;
    init_thread->se.weight = cfs_params::nice_to_weight(-5);
    init_thread->se.vruntime = 1;
    init_thread->state = ProcessState::Ready;

    init_proc->set_state(ProcessState::Running);

    // Step 4b: Allocate FdTable and open stdin/stdout/stderr
    {
      auto *fdt = new vfs::FdTable();
      fdt->init();
      init_proc->set_fd_table(fdt);
      vfs::vfs_init_stdio(fdt);
      early_debug_print("[init] VFS fd table initialized (fd 0/1/2)\n");
    }

    // Step 5: Register thread into process's thread list (for cleanup)
    init_proc->register_thread(init_thread);

    // Step 6: Enqueue into scheduler
    scheduler_->enqueue_task(init_thread, current_cpu());

    early_debug_print("[init] init process enqueued to scheduler\n");
    (void)code_size;
#else
    early_debug_print("[init] user process not supported on this arch\n");
#endif
    return VoidResult{};
  }

  // Enable interrupts
  void enable_interrupts() noexcept { arch::enable_interrupts(); }

  // Kernel panic handler
  [[noreturn]] void kernel_panic(const char *message, ErrorCode error) noexcept {
    arch::disable_interrupts();

    log::klog::panic("KERNEL PANIC");
    log::klog::panic("Error: {}", message);
    log::klog::panic("Error code: {}", static_cast<int>(error));
    log::klog::panic("Current phase: {}", static_cast<int>(current_phase_));

    // Print call stack
    print_stack_trace();

    // Halt
    while (true) {
      arch::cpu_halt();
    }
  }

  // Print boot banner
  void print_banner() const noexcept {
    log::klog::info("");
    log::klog::info("MOSS Hybrid Kernel v1.0");
    log::klog::info("");
  }

  // Print call stack
  void print_stack_trace() const noexcept {
    log::klog::panic("Stack trace:");

    u64 fp = arch::get_frame_pointer();

    for (int i = 0; i < 10 && fp != 0; i++) {
      u64 *frame = reinterpret_cast<u64 *>(fp);
      if (frame != nullptr) {
        u64 lr = frame[1]; // Return address
        fp = frame[0];     // Next frame pointer

        log::klog::panic("  [{}] {:#x}", i, lr);
      } else {
        break;
      }
    }
  }

  // Get current time
  [[nodiscard]] static u64 get_current_time() noexcept { return arch::get_timestamp_counter(); }
};

} // namespace moss::kernel
