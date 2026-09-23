// MOSS Process Module - Partition: signal
// POSIX signal constants, sigaction structure, signal delivery and checking.
//
// Design: minimal viable subset — default actions (terminate/ignore/stop/continue) and
// user-space signal handlers with sigreturn.  Signal queueing is bitmask-
// based (standard signals 1-31, no real-time signals).

export module moss.process:signal;

import :types;

import moss.intrinsics;
import moss.std;
import moss.types;
import moss.arch;
import moss.containers;
import moss.logging;

export namespace moss::kernel::process {

namespace log = moss::kernel::logging;

// ============================================================================
// Signal numbers (POSIX standard, Linux/ARM64 values)
// ============================================================================
namespace sig {
inline constexpr u32 SIGHUP = 1;
inline constexpr u32 SIGINT = 2;
inline constexpr u32 SIGQUIT = 3;
inline constexpr u32 SIGILL = 4;
inline constexpr u32 SIGTRAP = 5;
inline constexpr u32 SIGABRT = 6;
inline constexpr u32 SIGBUS = 7;
inline constexpr u32 SIGFPE = 8;
inline constexpr u32 SIGKILL = 9;
inline constexpr u32 SIGUSR1 = 10;
inline constexpr u32 SIGSEGV = 11;
inline constexpr u32 SIGUSR2 = 12;
inline constexpr u32 SIGPIPE = 13;
inline constexpr u32 SIGALRM = 14;
inline constexpr u32 SIGTERM = 15;
inline constexpr u32 SIGCHLD = 17;
inline constexpr u32 SIGCONT = 18;
inline constexpr u32 SIGSTOP = 19;
inline constexpr u32 SIGTSTP = 20;
inline constexpr u32 SIGTTIN = 21;
inline constexpr u32 SIGTTOU = 22;

// Native action storage has 32 slots: zero is unused, leaving signals 1..31.
// The mask uses the same signal-number bit positions, including unused bit 0.
inline constexpr u32 NSIG = 32; // max signal number (1-31 valid)

// Bit mask for a signal number (1-based)
constexpr u64 sigmask(u32 signo) noexcept { return (signo > 0 && signo < NSIG) ? (1ULL << signo) : 0; }

// Signals that cannot be caught or ignored
inline constexpr u64 UNCATCHABLE_MASK = sigmask(SIGKILL) | sigmask(SIGSTOP);
} // namespace sig

// Native sigaction ABI assigns bits 0..4; keep these positions in sync with
// userspace. Bit 2 remains reserved for SIGINFO.
namespace sa_flags {
inline constexpr u32 SA_ONSTACK = 0x1; // use alternate signal stack
inline constexpr u32 SA_RESTART = 0x2; // restart interrupted syscalls
inline constexpr u32 SA_SIGINFO = 0x4; // reserved for siginfo_t
inline constexpr u32 SA_NOCLDSTOP = 0x8; // suppress SIGCHLD for stop/continue
inline constexpr u32 SA_NOCLDWAIT = 0x10; // discard child exit status and zombie
} // namespace sa_flags

// sigaltstack flags
namespace ss_flags {
inline constexpr u32 SS_ONSTACK = 1;
inline constexpr u32 SS_DISABLE = 2;
} // namespace ss_flags

// ============================================================================
// Signal action — per-signal handler configuration
// ============================================================================

// Special handler values (like Linux SIG_DFL / SIG_IGN)
inline constexpr VirtAddr SIG_DFL = 0; // default action
inline constexpr VirtAddr SIG_IGN = 1; // ignore signal

static_assert(sizeof(SignalState::actions) / sizeof(Sigaction) == sig::NSIG);

// ============================================================================
// Default signal actions — what happens when handler is SIG_DFL
// ============================================================================
enum class SigDefault : u8 {
  Terminate = 0, // kill the process
  Ignore = 1,    // discard the signal
  Stop = 2,      // stop (suspend) the process
  Continue = 3,  // resume a stopped process
  CoreDump = 4,  // terminate + core dump (treated as Terminate for now)
};

constexpr SigDefault default_action(u32 signo) noexcept {
  switch (signo) {
  case sig::SIGCHLD:
  case sig::SIGUSR1: // treat as ignore until handlers are set
  case sig::SIGUSR2:
    return SigDefault::Ignore;
  case sig::SIGSTOP:
  case sig::SIGTSTP:
  case sig::SIGTTIN:
  case sig::SIGTTOU:
    return SigDefault::Stop;
  case sig::SIGCONT:
    return SigDefault::Continue;
  case sig::SIGQUIT:
  case sig::SIGILL:
  case sig::SIGABRT:
  case sig::SIGFPE:
  case sig::SIGBUS:
  case sig::SIGSEGV:
    return SigDefault::CoreDump;
  default:
    return SigDefault::Terminate;
  }
}

// Native signal ABI, version 2. GP order follows abi::TrapFrame::gpr().
// Only user state is serialized; no kernel frame pointer or entry metadata.
// FP storage is aligned for FXSAVE64 on x86 and Q0-Q31 on ARM64.
struct alignas(16) SignalFrame {
  // A recognizable/versioned sentinel rejects foreign or obsolete layouts;
  // its exact spelling is an ABI identifier, not a numeric tuning choice.
  static constexpr u64 MAGIC = 0xDEAD'5164'5346'5232ULL;
  // 31 eight-byte GPR slots plus state, 512-byte FP storage and trailing
  // signal/mask/link fields round to 848 bytes at 16-byte alignment. Keep
  // this native ABI size in sync with userspace signal-frame consumers.
  static constexpr usize FRAME_SIZE = 848;
  u64 magic;
  u64 gp_regs[31];
  u64 elr;
  u64 spsr;
  u64 sp;
  u64 fpsr;
  u64 fpcr;
  alignas(16) u64 fp[64]; // 64 * 8 bytes: FXSAVE64 or 32 * 16-byte ARM64 Q registers.
  u64 signo;
  u64 saved_mask;
  u64 saved_on_alt_stack;
  u64 previous; // nested-frame link occupies the native ABI's tail padding
};
static_assert(sizeof(SignalFrame) == SignalFrame::FRAME_SIZE);
static_assert(__builtin_offsetof(SignalFrame, fp) % 16 == 0);

// ============================================================================
// Signal operations — send, check, deliver
// ============================================================================

// Send a signal to a thread. Sets the pending bit; handler delivery happens
// at the next checkpoint. SIGCONT resumes a stopped thread at generation.
// Returns true if the signal was successfully pended.
bool send_signal(Thread *thread, u32 signo) noexcept;
void notify_parent_job_status(Thread *thread) noexcept;

// Check if a thread has any unmasked pending signals.
[[nodiscard]] inline bool signal_pending(const Thread *thread) noexcept {
  if (thread == nullptr) {
    return false;
  }
  // Pending signals that are not blocked
  u64 unblocked = thread->pending_signals & ~thread->signal_mask;
  // SIGKILL/SIGSTOP are always deliverable regardless of mask
  unblocked |= thread->pending_signals & sig::UNCATCHABLE_MASK;
  return unblocked != 0;
}

// Dequeue the next pending signal (lowest-numbered first, matching Linux).
// Returns 0 if no signal is pending.
[[nodiscard]] inline u32 dequeue_signal(Thread *thread) noexcept {
  if (thread == nullptr) {
    return 0;
  }

  u64 pending = thread->pending_signals & ~thread->signal_mask;
  pending |= thread->pending_signals & sig::UNCATCHABLE_MASK;

  if (pending == 0) {
    return 0;
  }

  // Find lowest set bit (lowest signal number)
  u32 signo = static_cast<u32>(intrinsics::bitops::ctzll(pending));
  thread->pending_signals &= ~sig::sigmask(signo);
  return signo;
}

// Process pending signals at a checkpoint (syscall return / IRQ return).
// This is the main signal delivery entry point.
// Zero means continue; otherwise return the terminating signal number.
u32 do_signal_checkpoint(Thread *thread) noexcept;

// Set up a signal frame on the user stack and modify the trap frame
// to dispatch to the signal handler on eret.
// Returns true on success, false if the user stack is invalid.
bool setup_sigframe(Thread *thread, u32 signo, const Sigaction &sa) noexcept;

// Restore interrupted context from sigframe on user stack (sigreturn syscall).
// Returns the restored native syscall result register.
long do_sigreturn(Thread *thread) noexcept;

} // namespace moss::kernel::process
