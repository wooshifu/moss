// MOSS Process Module - Partition: signal
// POSIX signal constants, sigaction structure, signal delivery and checking.
//
// Design: minimal viable subset — default actions (terminate/ignore) and
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

inline constexpr u32 NSIG = 32; // max signal number (1-31 valid)

// Bit mask for a signal number (1-based)
constexpr u64 sigmask(u32 signo) noexcept { return (signo > 0 && signo < NSIG) ? (1ULL << signo) : 0; }

// Signals that cannot be caught or ignored
inline constexpr u64 UNCATCHABLE_MASK = sigmask(SIGKILL) | sigmask(SIGSTOP);
} // namespace sig

// ============================================================================
// Signal action — per-signal handler configuration
// ============================================================================

// Special handler values (like Linux SIG_DFL / SIG_IGN)
inline constexpr VirtAddr SIG_DFL = 0; // default action
inline constexpr VirtAddr SIG_IGN = 1; // ignore signal

struct Sigaction {
  VirtAddr handler; // SIG_DFL, SIG_IGN, or user function pointer
  u64 mask;         // signals to block during handler execution
  u32 flags;        // SA_RESTART, SA_SIGINFO, etc. (reserved)

  constexpr Sigaction() noexcept : handler(SIG_DFL), mask(0), flags(0) {}
};

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
  case sig::SIGCONT:
  case sig::SIGUSR1: // treat as ignore until handlers are set
  case sig::SIGUSR2:
    return SigDefault::Ignore;
  case sig::SIGSTOP:
  case sig::SIGTSTP:
  case sig::SIGTTIN:
  case sig::SIGTTOU:
    return SigDefault::Stop;
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

// ============================================================================
// Per-process signal state
// ============================================================================
struct SignalState {
  // Per-signal action table (indexed by signal number, 0 unused)
  Sigaction actions[sig::NSIG]{};

  constexpr SignalState() noexcept = default;
};

// ============================================================================
// Signal operations — send, check, deliver
// ============================================================================

// Send a signal to a thread.  Sets the pending bit; actual delivery
// happens at the next signal checkpoint (syscall return / IRQ return).
// Returns true if the signal was successfully pended.
inline bool send_signal(Thread *thread, u32 signo) noexcept {
  if (thread == nullptr || signo == 0 || signo >= sig::NSIG) {
    return false;
  }

  u64 mask = sig::sigmask(signo);

  // Check if the signal is blocked by the thread's signal mask
  // (SIGKILL and SIGSTOP can never be blocked)
  if ((mask & sig::UNCATCHABLE_MASK) == 0 && (thread->signal_mask & mask) != 0) {
    return false; // signal is blocked
  }

  thread->pending_signals |= mask;

  // If the thread is in interruptible sleep, wake it up so it can
  // process the signal at the next checkpoint.
  if (thread->state == ProcessState::Sleeping) {
    // The actual wakeup (enqueue to runqueue) must be done by the
    // scheduler; we just mark need_resched here.  The caller
    // (sys_kill) will call task_wakeup() if needed.
    thread->need_resched = true;
  }

  return true;
}

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

// Process default signal action for the given signal.
// Called when the handler is SIG_DFL.
// Returns true if the process should be terminated.
[[nodiscard]] inline bool do_signal_default(Thread *thread, u32 signo) noexcept {
  SigDefault action = default_action(signo);
  switch (action) {
  case SigDefault::Terminate:
  case SigDefault::CoreDump:
    log::klog::info("signal {}: default action terminate PID={}", signo, static_cast<u32>(thread->owner_pid));
    return true; // caller should terminate the process
  case SigDefault::Ignore:
    return false;
  case SigDefault::Stop:
    // Mark thread as Stopped; scheduler dequeue happens in do_signal_checkpoint()
    // (this inline function cannot access g_scheduler from the :signal partition).
    thread->state = ProcessState::Stopped;
    log::klog::info("signal {}: stopped PID={}", signo, static_cast<u32>(thread->owner_pid));
    return false;
  case SigDefault::Continue:
    if (thread->state == ProcessState::Stopped) {
      // Mark Ready; scheduler enqueue happens in do_signal_checkpoint()
      thread->state = ProcessState::Ready;
      log::klog::info("signal {}: continued PID={}", signo, static_cast<u32>(thread->owner_pid));
    }
    return false;
  default:
    return false;
  }
}

// Get the signal state for a process.
// Returns nullptr if the process has no signal state (kernel process).
[[nodiscard]] SignalState *get_signal_state(Process *proc) noexcept;

// Initialize signal state for a process (called from create_process).
void init_signal_state(Process *proc) noexcept;

// Process pending signals at a checkpoint (syscall return / IRQ return).
// This is the main signal delivery entry point.
// Returns true if the thread should be terminated (SIGKILL or default terminate).
bool do_signal_checkpoint(Thread *thread) noexcept;

} // namespace moss::kernel::process
