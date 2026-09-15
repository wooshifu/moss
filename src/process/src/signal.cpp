// Signal implementation — module implementation unit for moss.process

module moss.process;

namespace moss::kernel::process {

namespace log = moss::kernel::logging;

// ============================================================================
// NEON/FP state save helpers (ARM64 only)
//
// The kernel is compiled with -mgeneral-regs-only, so we must use
// .arch_extension fp to temporarily enable FP/NEON instructions within
// inline asm blocks, and use the raw system register encoding for
// fpsr (s3_3_c4_c4_1) and fpcr (s3_3_c4_c4_0).
// ============================================================================
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
static void save_neon_state(u64 *neon_buf) noexcept {
  asm volatile(".arch_extension fp\n"
               "stp q0,  q1,  [%0, #(0  * 16)]\n"
               "stp q2,  q3,  [%0, #(2  * 16)]\n"
               "stp q4,  q5,  [%0, #(4  * 16)]\n"
               "stp q6,  q7,  [%0, #(6  * 16)]\n"
               "stp q8,  q9,  [%0, #(8  * 16)]\n"
               "stp q10, q11, [%0, #(10 * 16)]\n"
               "stp q12, q13, [%0, #(12 * 16)]\n"
               "stp q14, q15, [%0, #(14 * 16)]\n"
               "stp q16, q17, [%0, #(16 * 16)]\n"
               "stp q18, q19, [%0, #(18 * 16)]\n"
               "stp q20, q21, [%0, #(20 * 16)]\n"
               "stp q22, q23, [%0, #(22 * 16)]\n"
               "stp q24, q25, [%0, #(24 * 16)]\n"
               "stp q26, q27, [%0, #(26 * 16)]\n"
               "stp q28, q29, [%0, #(28 * 16)]\n"
               "stp q30, q31, [%0, #(30 * 16)]\n"
               ".arch_extension nofp\n"
               :
               : "r"(neon_buf)
               : "memory");
}

static u64 read_fpsr() noexcept {
  u64 val;
  // fpsr = S3_3_C4_C4_1 — raw encoding works with -mgeneral-regs-only
  asm volatile("mrs %0, s3_3_c4_c4_1" : "=r"(val));
  return val;
}

static u64 read_fpcr() noexcept {
  u64 val;
  // fpcr = S3_3_C4_C4_0 — raw encoding works with -mgeneral-regs-only
  asm volatile("mrs %0, s3_3_c4_c4_0" : "=r"(val));
  return val;
}

static void restore_neon_state(const u64 *neon_buf) noexcept {
  asm volatile(".arch_extension fp\n"
               "ldp q0,  q1,  [%0, #(0  * 16)]\n"
               "ldp q2,  q3,  [%0, #(2  * 16)]\n"
               "ldp q4,  q5,  [%0, #(4  * 16)]\n"
               "ldp q6,  q7,  [%0, #(6  * 16)]\n"
               "ldp q8,  q9,  [%0, #(8  * 16)]\n"
               "ldp q10, q11, [%0, #(10 * 16)]\n"
               "ldp q12, q13, [%0, #(12 * 16)]\n"
               "ldp q14, q15, [%0, #(14 * 16)]\n"
               "ldp q16, q17, [%0, #(16 * 16)]\n"
               "ldp q18, q19, [%0, #(18 * 16)]\n"
               "ldp q20, q21, [%0, #(20 * 16)]\n"
               "ldp q22, q23, [%0, #(22 * 16)]\n"
               "ldp q24, q25, [%0, #(24 * 16)]\n"
               "ldp q26, q27, [%0, #(26 * 16)]\n"
               "ldp q28, q29, [%0, #(28 * 16)]\n"
               "ldp q30, q31, [%0, #(30 * 16)]\n"
               ".arch_extension nofp\n"
               :
               : "r"(neon_buf)
               : "memory");
}

static void write_fpsr(u64 val) noexcept { asm volatile("msr s3_3_c4_c4_1, %0" : : "r"(val)); }

static void write_fpcr(u64 val) noexcept { asm volatile("msr s3_3_c4_c4_0, %0" : : "r"(val)); }
#endif

// Per-process signal state storage.
// Stored as void* inside Process to avoid circular header dependencies;
// we cast here in the implementation.  Signal state is allocated once
// when the process is created and freed when the process is destroyed.
//
// NOTE: the Process class does not directly embed SignalState because
// the :signal partition is declared after :types.  Instead, we use the
// existing fd_table_-style void* pattern — but for signals we add a
// dedicated slot.  For now, we store it in a simple hash map keyed by PID.

// Global signal state table (simple fixed-size array indexed by PID).
// This avoids heap allocation and keeps the implementation straightforward.
static constexpr usize MAX_SIGNAL_PROCS = 256;
static SignalState g_signal_states[MAX_SIGNAL_PROCS]{};
static bool g_signal_state_used[MAX_SIGNAL_PROCS]{};

SignalState *get_signal_state(Process *proc) noexcept {
  if (proc == nullptr) {
    return nullptr;
  }
  auto pid = static_cast<usize>(proc->pid());
  if (pid >= MAX_SIGNAL_PROCS) {
    return nullptr;
  }
  if (!g_signal_state_used[pid]) {
    return nullptr;
  }
  return &g_signal_states[pid];
}

void init_signal_state(Process *proc) noexcept {
  if (proc == nullptr) {
    return;
  }
  auto pid = static_cast<usize>(proc->pid());
  if (pid >= MAX_SIGNAL_PROCS) {
    log::klog::warn("init_signal_state: PID {} exceeds MAX_SIGNAL_PROCS", pid);
    return;
  }
  g_signal_states[pid] = SignalState{};
  g_signal_state_used[pid] = true;
}

// Called on the current thread's user-return path. Shared uaccess handles
// admission and recoverable faults; concurrent VMA/PTE lifetime synchronization
// remains a separate responsibility.
bool setup_sigframe(Thread *thread, u32 signo, const Sigaction &sa) noexcept {
  if (!thread || !thread->trap_frame || !thread->trap_frame->from_user()) {
    return false;
  }
  auto &frame = *thread->trap_frame;
  u64 user_sp = frame.sp;
  bool use_altstack =
      (sa.flags & sa_flags::SA_ONSTACK) && thread->alt_stack_flags != ss_flags::SS_DISABLE && !thread->on_alt_stack;
  if (use_altstack) {
    if (thread->alt_stack_size > ~thread->alt_stack_sp) {
      return false;
    }
    user_sp = thread->alt_stack_sp + thread->alt_stack_size;
  }
#if defined(MOSS_ARCH_X64)
  // SysV's red zone belongs to the interrupted user function.
  constexpr u64 red_zone = 128;
#else
  constexpr u64 red_zone = 0;
#endif
  if (user_sp < SignalFrame::FRAME_SIZE + red_zone + 16) {
    return false;
  }
  const u64 sigframe_sp = (user_sp - red_zone - SignalFrame::FRAME_SIZE) & ~15ULL;
  auto proc = g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
  auto *as = proc ? proc->address_space() : nullptr;
  if (!as || !as->allows_user_access(sa.handler, 1, vma_flags::EXEC)) {
    return false;
  }
  SignalFrame sf{};
  sf.magic = SignalFrame::MAGIC;
  for (u32 i = 0; i < moss::abi::TrapFrame::GPR_COUNT; ++i) {
    sf.gp_regs[i] = frame.gpr(i);
  }
  sf.elr = frame.pc;
  sf.spsr = frame.status;
  sf.sp = frame.sp;
#if defined(MOSS_ARCH_ARM64)
  sf.fpsr = read_fpsr();
  sf.fpcr = read_fpcr();
  save_neon_state(sf.fp);
#elif defined(MOSS_ARCH_X64)
  asm volatile("fxsave64 %0" : "=m"(sf.fp)::"memory");
#endif
  sf.signo = signo;
  sf.saved_mask = thread->signal_mask;
  sf.saved_on_alt_stack = thread->on_alt_stack;
  if (copy_to_user(sigframe_sp, &sf, sizeof(sf)) != 0) {
    return false;
  }
  u64 handler_sp = sigframe_sp;
#if defined(MOSS_ARCH_X64)
  // A normal C handler returns with RET, leaving RSP at the signal frame.
  handler_sp -= 8;
  u64 link = user_layout::SIGRETURN_PAGE;
  if (copy_to_user(handler_sp, &link, sizeof(link)) != 0) {
    return false;
  }
#elif defined(MOSS_ARCH_ARM64)
  frame.x30 = user_layout::SIGRETURN_PAGE;
#elif defined(MOSS_ARCH_RISCV64)
  frame.ra = user_layout::SIGRETURN_PAGE;
#endif
  frame.pc = sa.handler;
  frame.sp = handler_sp;
  frame.argument(0) = signo;
  thread->signal_mask = (thread->signal_mask | sa.mask | sig::sigmask(signo)) & ~sig::UNCATCHABLE_MASK;
  thread->on_alt_stack = use_altstack || thread->on_alt_stack;
  return true;
}

static constexpr long SIGRETURN_EFAULT = 14;

long do_sigreturn(Thread *thread) noexcept {
  if (!thread || !thread->trap_frame) {
    return -SIGRETURN_EFAULT;
  }
  auto &frame = *thread->trap_frame;
  SignalFrame sf{};
  if ((frame.sp & 15) || copy_from_user(&sf, frame.sp, sizeof(sf)) != 0 || sf.magic != SignalFrame::MAGIC ||
      !mm::PageTableManager::is_user_range(sf.elr, 1) || !mm::PageTableManager::is_user_range(sf.sp, 1)) {
    return -SIGRETURN_EFAULT;
  }
#if defined(MOSS_ARCH_ARM64)
  if ((sf.elr & 3) || (sf.sp & 15)) {
    return -SIGRETURN_EFAULT;
  }
#elif defined(MOSS_ARCH_RISCV64)
  if (sf.elr & 1) {
    return -SIGRETURN_EFAULT;
  }
#elif defined(MOSS_ARCH_X64)
  X86FpState current;
  asm volatile("fxsave64 %0" : "=m"(current)::"memory");
  const u32 mask = current.mxcsr_mask ? current.mxcsr_mask : 0xffbfU;
  if (static_cast<u32>(sf.fp[3]) & ~mask) {
    return -SIGRETURN_EFAULT;
  }
#endif
  for (u32 i = 0; i < moss::abi::TrapFrame::GPR_COUNT; ++i) {
    frame.gpr(i) = sf.gp_regs[i];
  }
  frame.pc = sf.elr;
  frame.sp = sf.sp;
  frame.status = sf.spsr;
  frame.status = frame.user_status(); // never restore user-supplied privilege/IRQ controls
#if defined(MOSS_ARCH_ARM64)
  write_fpsr(sf.fpsr);
  write_fpcr(sf.fpcr);
  restore_neon_state(sf.fp);
#elif defined(MOSS_ARCH_X64)
  asm volatile("fxrstor64 %0" ::"m"(sf.fp) : "memory");
#endif
  thread->signal_mask = sf.saved_mask & ~sig::UNCATCHABLE_MASK;
  thread->on_alt_stack = sf.saved_on_alt_stack != 0;
  return static_cast<long>(frame.result());
}

u32 do_signal_checkpoint(Thread *thread) noexcept {
  if (thread == nullptr || !signal_pending(thread)) {
    return 0;
  }

  auto proc = g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
  if (!proc) {
    return 0;
  }

  SignalState *sigstate = get_signal_state(proc.get());

  // Process all pending signals (lowest numbered first)
  while (signal_pending(thread)) {
    u32 signo = dequeue_signal(thread);
    if (signo == 0) {
      break;
    }

    // Determine handler
    Sigaction *sa = nullptr;
    if (sigstate != nullptr && signo < sig::NSIG) {
      sa = &sigstate->actions[signo];
    }

    // SIGKILL/SIGSTOP: always default action, cannot be caught
    if (signo == sig::SIGKILL || signo == sig::SIGSTOP) {
      if (do_signal_default(thread, signo)) {
        return signo; // terminate
      }
      // After do_signal_default sets Stopped state, dequeue from scheduler
      if (thread->state == ProcessState::Stopped && g_scheduler) {
        g_scheduler->dequeue_task(thread);
      }
      continue;
    }

    // SIGCONT is special: it always resumes a stopped task, even if caught/ignored
    if (signo == sig::SIGCONT && thread->state == ProcessState::Stopped) {
      thread->state = ProcessState::Ready;
      if (g_scheduler) {
        g_scheduler->enqueue_task(thread, thread->wake_cpu);
      }
      log::klog::info("signal {}: continued PID={}", signo, static_cast<u32>(thread->owner_pid));
      // If handler is not SIG_DFL, still execute handler below
      if (sa != nullptr && sa->handler != SIG_DFL && sa->handler != SIG_IGN) {
        if (!setup_sigframe(thread, signo, *sa)) {
          log::klog::warn("signal {}: sigframe setup failed on SIGCONT for PID={}", signo,
                          static_cast<u32>(thread->owner_pid));
        }
        return 0;
      }
      continue;
    }

    if (sa == nullptr || sa->handler == SIG_DFL) {
      // Default action
      if (do_signal_default(thread, signo)) {
        return signo; // terminate
      }
      // Handle stop signals (SIGTSTP, SIGTTIN, SIGTTOU) via default action
      if (thread->state == ProcessState::Stopped && g_scheduler) {
        g_scheduler->dequeue_task(thread);
      }
    } else if (sa->handler == SIG_IGN) {
      // Explicitly ignored
      continue;
    } else {
      // User-space signal handler — set up sigframe for delivery
      if (!setup_sigframe(thread, signo, *sa)) {
        log::klog::warn("signal {}: sigframe setup failed for PID={}, terminating", signo,
                        static_cast<u32>(thread->owner_pid));
        return signo; // terminate
      }
      // Deliver only one signal per checkpoint — after handler returns via
      // sigreturn, the next syscall return will check for more pending signals.
      return 0;
    }
  }

  return 0;
}

} // namespace moss::kernel::process
