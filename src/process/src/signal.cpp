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

bool setup_sigframe(Thread *thread, u32 signo, const Sigaction &sa) noexcept {
  if (thread == nullptr || thread->trap_frame == 0) {
    return false;
  }

  auto *frame = reinterpret_cast<u64 *>(thread->trap_frame);

  // 1. Determine signal stack
  u64 user_sp = frame[33]; // SP_EL0 from trap frame

  bool use_altstack = false;
  if ((sa.flags & sa_flags::SA_ONSTACK) != 0 && thread->alt_stack_flags != ss_flags::SS_DISABLE &&
      !thread->on_alt_stack) {
    user_sp = thread->alt_stack_sp + thread->alt_stack_size;
    use_altstack = true;
  }

  // 2. Allocate sigframe on user stack (grows downward, 16-byte aligned)
  u64 sigframe_sp = (user_sp - SignalFrame::FRAME_SIZE) & ~static_cast<u64>(0xF);

  // 3. Build sigframe in kernel buffer
  SignalFrame sf{};
  sf.magic = SignalFrame::MAGIC;

  // Copy GP regs from trap frame
  for (u32 i = 0; i < 31; ++i) {
    sf.gp_regs[i] = frame[i];
  }
  sf.elr = frame[31];
  sf.spsr = frame[32];
  sf.sp = frame[33];

  // Save NEON/FP state (still live from user-space since kernel doesn't use NEON)
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
  sf.fpsr = read_fpsr();
  sf.fpcr = read_fpcr();
  save_neon_state(sf.neon);
#endif

  sf.signo = signo;
  sf.saved_mask = thread->signal_mask;

  // Sigreturn trampoline: mov x8, #17; svc #0
  sf.trampoline[0] = 0xD2800228U; // mov x8, #0x11 (17 = SYS_SIGRETURN)
  sf.trampoline[1] = 0xD4000001U; // svc #0

  // 4. Write sigframe to user stack
  // We are in EL1 with TTBR0 set to this process's page tables,
  // so we can directly access user-space addresses.
  auto *dst = reinterpret_cast<SignalFrame *>(sigframe_sp);
  moss::memcpy(dst, &sf, sizeof(sf));

  // 5. Modify trap frame for handler dispatch
  frame[31] = sa.handler;  // ELR -> handler address
  frame[33] = sigframe_sp; // SP_EL0 -> sigframe base
  frame[0] = signo;        // x0 -> signal number (first arg to handler)
  // LR (x30) -> sigreturn trampoline page (read+exec, mapped in every process).
  // The trampoline contains: mov x8, #17; svc #0 (triggers sigreturn syscall).
  // Using a fixed executable page avoids needing execute permission on the user stack.
  frame[30] = user_layout::SIGRETURN_PAGE;

  // 6. Block signals during handler execution
  thread->signal_mask |= sa.mask | sig::sigmask(signo);
  thread->signal_mask &= ~sig::UNCATCHABLE_MASK;

  if (use_altstack) {
    thread->on_alt_stack = true;
  }

  log::klog::info("signal {}: delivering to handler {:#x} for PID={}", signo, sa.handler,
                  static_cast<u32>(thread->owner_pid));

  return true;
}

// Linux EFAULT errno value for sigreturn error returns.
// We define it locally because the errc namespace lives in moss.kernel:syscall_table
// which is not imported by this module.
static constexpr long SIGRETURN_EFAULT = 14;

long do_sigreturn(Thread *thread) noexcept {
  if (thread == nullptr || thread->trap_frame == 0) {
    return -SIGRETURN_EFAULT;
  }

  auto *frame = reinterpret_cast<u64 *>(thread->trap_frame);

  // The sigframe is at the current SP_EL0 (which points to our sigframe)
  u64 sigframe_addr = frame[33]; // SP_EL0

  // Read sigframe from user space
  SignalFrame sf{};
  const auto *src = reinterpret_cast<const SignalFrame *>(sigframe_addr);
  moss::memcpy(&sf, src, sizeof(sf));

  // Validate magic
  if (sf.magic != SignalFrame::MAGIC) {
    log::klog::warn("sigreturn: invalid magic {:#x} at {:#x}", sf.magic, sigframe_addr);
    return -SIGRETURN_EFAULT;
  }

  // Restore GP registers to trap frame
  for (u32 i = 0; i < 31; ++i) {
    frame[i] = sf.gp_regs[i];
  }
  frame[31] = sf.elr;  // Restore PC
  frame[32] = sf.spsr; // Restore SPSR
  frame[33] = sf.sp;   // Restore original SP_EL0

  // Restore NEON/FP state
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
  write_fpsr(sf.fpsr);
  write_fpcr(sf.fpcr);
  restore_neon_state(sf.neon);
#endif

  // Restore signal mask
  thread->signal_mask = sf.saved_mask;

  // Clear on_alt_stack flag
  thread->on_alt_stack = false;

  log::klog::info("sigreturn: restored context for PID={}, PC={:#x}", static_cast<u32>(thread->owner_pid), sf.elr);

  // Return the original x0 from the sigframe — this is what the trap frame
  // restore will put in x0 after eret. The syscall dispatch normally writes
  // the return value into frame[0], but since we already set frame[0] = sf.gp_regs[0],
  // we return that same value so the dispatch doesn't overwrite it.
  return static_cast<long>(sf.gp_regs[0]);
}

bool do_signal_checkpoint(Thread *thread) noexcept {
  if (thread == nullptr || !signal_pending(thread)) {
    return false;
  }

  auto proc = g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
  if (!proc) {
    return false;
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
        return true; // terminate
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
        return false;
      }
      continue;
    }

    if (sa == nullptr || sa->handler == SIG_DFL) {
      // Default action
      if (do_signal_default(thread, signo)) {
        return true; // terminate
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
        return true; // terminate
      }
      // Deliver only one signal per checkpoint — after handler returns via
      // sigreturn, the next syscall return will check for more pending signals.
      return false;
    }
  }

  return false;
}

} // namespace moss::kernel::process
