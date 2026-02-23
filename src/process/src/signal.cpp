// Signal implementation — module implementation unit for moss.process

module moss.process;

namespace moss::kernel::process {

namespace log = moss::kernel::logging;

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

bool do_signal_checkpoint(Thread *thread) noexcept {
  if (thread == nullptr || !signal_pending(thread)) {
    return false;
  }

  Process *proc = g_process_manager ? g_process_manager->find_process(thread->owner_pid) : nullptr;
  if (proc == nullptr) {
    return false;
  }

  SignalState *sigstate = get_signal_state(proc);

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
        // User handler will be delivered when sigframe is implemented
        log::klog::warn("signal {}: user handler {:#x} not yet delivered", signo, sa->handler);
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
      // User-space signal handler — for now, log and use default action.
      // Full user-space signal delivery (sigframe + sigreturn) requires
      // manipulating the user-space stack and registers, which will be
      // implemented as a follow-up when we have the sigreturn syscall.
      log::klog::warn("signal {}: user handler {:#x} not yet delivered, using default", signo, sa->handler);
      if (do_signal_default(thread, signo)) {
        return true;
      }
    }
  }

  return false;
}

} // namespace moss::kernel::process
