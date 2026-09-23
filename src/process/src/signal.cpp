// Signal implementation — module implementation unit for moss.process

module moss.process;

namespace moss::kernel::process {

namespace log = moss::kernel::logging;

bool send_signal(Thread *thread, u32 signo) noexcept {
  if (!thread || signo == 0 || signo >= sig::NSIG) {
    return false;
  }
  const u64 mask = sig::sigmask(signo);
  bool wake = false;
  {
    containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
    // POSIX stop/continue generation discards the opposite pending group.
    // One CAS keeps concurrent senders from publishing both groups.
    const u64 stop_mask = sig::sigmask(sig::SIGSTOP) | sig::sigmask(sig::SIGTSTP) | sig::sigmask(sig::SIGTTIN) |
                          sig::sigmask(sig::SIGTTOU);
    u64 pending = thread->pending_signals.load();
    u64 updated;
    do {
      updated = pending;
      if (signo == sig::SIGCONT) {
        updated &= ~stop_mask;
      } else if ((mask & stop_mask) != 0) {
        updated &= ~sig::sigmask(sig::SIGCONT);
      }
      updated |= mask;
    } while (!thread->pending_signals.compare_exchange_weak(pending, updated));
    const bool deliverable = (mask & sig::UNCATCHABLE_MASK) != 0 || (thread->signal_mask & mask) == 0;
    const auto state = thread->state.load();
    wake = (state == ProcessState::Stopped && (signo == sig::SIGCONT || signo == sig::SIGKILL)) ||
           (state == ProcessState::Sleeping && deliverable);
  }
  if (wake && g_scheduler) {
    g_scheduler->task_wakeup(thread, thread->cpu);
  }
  return true;
}

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
  // Qn occupies 16 bytes; paired loads/stores below use n*16 byte offsets
  // and require the signal ABI's 16-byte-aligned 512-byte FP storage.
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

SignalState *get_signal_state(Process *proc) noexcept { return proc ? &proc->signal_state() : nullptr; }

// Called on the current thread's user-return path. Shared uaccess holds a VM
// lease for each copied page; coordinating an entire delivery with concurrent
// shared exec is still a separate process/thread-lifecycle responsibility.
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
  // The SysV AMD64 ABI reserves the 128 bytes below interrupted RSP for its
  // user function; signal delivery must leave that red zone untouched.
  constexpr u64 red_zone = 128;
#else
  constexpr u64 red_zone = 0;
#endif
  // Budget one extra 16-byte alignment unit before subtraction. Rounding
  // down with mask 15 preserves FP/stack alignment and cannot underflow.
  if (user_sp < SignalFrame::FRAME_SIZE + red_zone + 16) {
    return false;
  }
  const u64 sigframe_sp = (user_sp - red_zone - SignalFrame::FRAME_SIZE) & ~15ULL;
  u64 handler_sp = sigframe_sp;
#if defined(MOSS_ARCH_X64)
  // The return-address slot is part of delivery's write footprint, not handler
  // workspace. Check it together with the frame before writing either region.
  handler_sp -= sizeof(u64);
#endif
  if (use_altstack || thread->on_alt_stack) {
    if (thread->alt_stack_size > ~thread->alt_stack_sp) {
      return false;
    }
    const u64 alt_end = thread->alt_stack_sp + thread->alt_stack_size;
    // A writable VMA below an exhausted alternate stack is not stack capacity.
    // Enforce the registered interval, including alignment/red-zone padding,
    // for both first entry and nested delivery before any user copy is made.
    if (handler_sp < thread->alt_stack_sp || user_sp > alt_end || sigframe_sp > alt_end ||
        SignalFrame::FRAME_SIZE > alt_end - sigframe_sp) {
      return false;
    }
  }
  auto proc = g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
  auto as = proc ? proc->address_space() : shared_ptr<AddressSpace>{};
  if (!as || !as->allows_user_access(sa.handler, 1, vma_flags::EXEC)) {
    return false;
  }
  // The trap reports the instruction after SVC/ECALL/SYSCALL. Restore the
  // original result register before replaying it after the handler returns.
  auto saved_frame = frame;
  if (thread->restart_syscall_pending && (sa.flags & sa_flags::SA_RESTART)) {
#if defined(MOSS_ARCH_X64)
    constexpr u64 syscall_instruction_size = 2; // SYSCALL is two bytes.
    const u64 original_result_register = thread->restart_syscall_number;
#else
    constexpr u64 syscall_instruction_size = 4; // SVC and ECALL are four bytes.
    const u64 original_result_register = thread->restart_syscall_arg0;
#endif
    if (saved_frame.pc >= syscall_instruction_size) {
      saved_frame.pc -= syscall_instruction_size;
      saved_frame.result() = original_result_register;
    }
  }
  SignalFrame sf{};
  sf.magic = SignalFrame::MAGIC;
  for (u32 i = 0; i < moss::abi::TrapFrame::GPR_COUNT; ++i) {
    sf.gp_regs[i] = saved_frame.gpr(i);
  }
  sf.elr = saved_frame.pc;
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
  sf.previous = thread->active_signal_frame;
  if (copy_to_user(sigframe_sp, &sf, sizeof(sf)) != 0) {
    return false;
  }
#if defined(MOSS_ARCH_X64)
  // One 8-byte return address gives a C handler the SysV entry RSP alignment;
  // RET removes it, leaving RSP at the 16-byte-aligned signal frame for sigreturn.
  u64 link = user_layout::SIGRETURN_PAGE;
  if (copy_to_user(handler_sp, &link, sizeof(link)) != 0) {
    return false;
  }
#elif defined(MOSS_ARCH_ARM64)
  frame.x30 = user_layout::SIGRETURN_PAGE;
#elif defined(MOSS_ARCH_RISCV64)
  frame.ra = user_layout::SIGRETURN_PAGE;
#endif
  // Publish handler state only after all user writes succeed. A short copy
  // may leave user bytes changed, but must not activate a half-built frame.
  frame.pc = sa.handler;
  frame.sp = handler_sp;
  frame.argument(0) = signo;
  thread->active_signal_frame = sigframe_sp;
  thread->signal_mask = (thread->signal_mask | sa.mask | sig::sigmask(signo)) & ~sig::UNCATCHABLE_MASK;
  thread->on_alt_stack = use_altstack || thread->on_alt_stack;
  return true;
}

// Native syscall error convention uses Linux errno EFAULT=14 for bad frames.
static constexpr long SIGRETURN_EFAULT = 14;

long do_sigreturn(Thread *thread) noexcept {
  if (!thread || !thread->trap_frame) {
    return -SIGRETURN_EFAULT;
  }
  auto &frame = *thread->trap_frame;
  SignalFrame sf{};
  // Signal-frame/FP storage requires 16-byte alignment; mask 15 checks it.
  if ((frame.sp & 15) || copy_from_user(&sf, frame.sp, sizeof(sf)) != 0 || sf.magic != SignalFrame::MAGIC ||
      !mm::PageTableManager::is_user_range(sf.elr, 1) || !mm::PageTableManager::is_user_range(sf.sp, 1)) {
    return -SIGRETURN_EFAULT;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
  auto as = proc ? proc->address_space() : shared_ptr<AddressSpace>{};
  if (!as || !thread->active_signal_frame || frame.sp != thread->active_signal_frame ||
      !as->allows_user_access(sf.elr, 1, vma_flags::EXEC) || sf.sp == 0 ||
      // A downward-growing SP may be the VMA's exclusive end; validate the
      // preceding byte rather than rejecting that valid empty-stack boundary.
      !as->allows_user_access(sf.sp - 1, 1, vma_flags::WRITE) ||
      (sf.previous != 0 && !as->allows_user_access(sf.previous, sizeof(sf), vma_flags::READ))) {
    return -SIGRETURN_EFAULT;
  }
#if defined(MOSS_ARCH_ARM64)
  // ARM64 instructions are 4-byte aligned; stacks must be 16-byte aligned.
  if ((sf.elr & 3) || (sf.sp & 15)) {
    return -SIGRETURN_EFAULT;
  }
#elif defined(MOSS_ARCH_RISCV64)
  // Compressed RISC-V instructions allow 2-byte PC alignment; the stack ABI
  // still requires 16 bytes. Reject malformed addresses before resuming.
  if ((sf.elr & 1) || (sf.sp & 15)) {
    return -SIGRETURN_EFAULT;
  }
#elif defined(MOSS_ARCH_X64)
  X86FpState current;
  asm volatile("fxsave64 %0" : "=m"(current)::"memory");
  // FXSAVE's MXCSR_MASK can be zero; 0xffbf is the architectural fallback
  // excluding reserved bits (including unsupported DAZ). MXCSR lives at byte
  // offset 24, hence u64 slot 3. Reject bits that would make FXRSTOR fault.
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
  thread->active_signal_frame = sf.previous;
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

    // Uncatchable signals use their default action regardless of sigaction.
    if (signo == sig::SIGKILL || signo == sig::SIGSTOP || sa == nullptr || sa->handler == SIG_DFL) {
      switch (default_action(signo)) {
      case SigDefault::Terminate:
      case SigDefault::CoreDump:
        return signo;
      case SigDefault::Stop:
        if (!g_scheduler) {
          return signo;
        }
        g_scheduler->stop_current();
        continue;
      case SigDefault::Ignore:
      case SigDefault::Continue:
        continue;
      default:
        return signo;
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
