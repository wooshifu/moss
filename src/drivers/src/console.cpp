module moss.drivers.console;

// On ARM64/x64 the debugger can stop after an empty-buffer check while
// event_lock is held. Production and validation images use the same IRQ path.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_console_before_register() noexcept {}
// ARM64/x64 validation observes RX before the handler competes for that lock.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_console_irq_before_lock() noexcept {}

namespace moss::kernel::drivers::console {
namespace {
containers::IrqSpinLock init_lock;
bool initialized = false;
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
containers::IrqSpinLock event_lock;
RxRing ring;
struct Waiter {
  void *thread;
  Waiter *next;
};
Waiter *waiters = nullptr;
void receive() noexcept {
  moss_validation_console_irq_before_lock();
  containers::LockGuard<containers::IrqSpinLock> guard(event_lock);
  // Clear before draining: clearing after a refill can erase its notification.
  uart::ack_rx_interrupt();
  for (int ch = uart::getc(); ch >= 0; ch = uart::getc()) {
    (void)ring.put(static_cast<u8>(ch));
  }
  if (!ring.empty()) {
    for (auto *waiter = waiters; waiter; waiter = waiter->next) {
      moss::abi::bridge::moss_wake_io_waiter(waiter->thread);
    }
  }
}
#if defined(MOSS_ARCH_ARM64)
void receive_irq([[maybe_unused]] u32 irq, [[maybe_unused]] void *context) noexcept { receive(); }
#endif
#else
// RISC-V polls the 16550: two readers must not both observe LSR.DR before
// either consumes RBR, or the second can return an empty-register byte.
containers::IrqSpinLock poll_lock;
#endif
} // namespace

bool is_initialized() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(init_lock);
  return initialized;
}

VoidResult initialize() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(init_lock);
  if (initialized) {
    return VoidResult{};
  }
  if (!platform::hardware.uart.valid) {
    return VoidResult{ErrorCode::NotFound};
  }
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
  auto *controller = interrupts::g_gic;
  if (!controller) {
    return VoidResult{ErrorCode::InvalidState};
  }
  const u32 irq = platform::hardware.uart.irq;
#if defined(MOSS_ARCH_ARM64)
  auto registered = controller->register_interrupt(irq, receive_irq, nullptr, "uart_rx");
  if (!registered) {
    return registered;
  }
#else
  moss::abi::bridge::g_x64_uart_rx_handler = receive;
#endif
  auto enabled = controller->enable_interrupt(irq);
  if (!enabled) {
#if defined(MOSS_ARCH_ARM64)
    (void)controller->unregister_interrupt(irq);
#else
    moss::abi::bridge::g_x64_uart_rx_handler = nullptr;
#endif
    return enabled;
  }
  uart::enable_rx();
  uart::enable_rx_interrupt();
#endif
  initialized = true;
  return VoidResult{};
}

int try_getc() noexcept {
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
  containers::LockGuard<containers::IrqSpinLock> guard(event_lock);
  return ring.get();
#else
  containers::LockGuard<containers::IrqSpinLock> guard(poll_lock);
  return uart::getc();
#endif
}

int getc_blocking() noexcept {
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  event_lock.lock();
  for (;;) {
    if (int ch = ring.get(); ch >= 0) {
      event_lock.unlock();
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return ch;
    }
    if (moss::abi::bridge::moss_io_wait_interrupted()) {
      event_lock.unlock();
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return -1;
    }
    moss_validation_console_before_register();
    void *thread = moss::abi::bridge::moss_prepare_io_wait();
    if (!thread) {
      event_lock.unlock();
      // With no schedulable waiter, leave IRQ delivery live during the wait:
      // a masked HLT/WFI must not strand RX or a remote TLB acknowledgement.
      // cpu_idle_once returns with IRQs masked before event_lock is reacquired.
      arch::cpu_idle_once();
      event_lock.lock();
      continue;
    }
    // The event lock makes checking input and publishing sleep one transaction.
    Waiter waiter{.thread = thread, .next = waiters};
    waiters = &waiter;
    event_lock.unlock();
    moss::abi::bridge::moss_commit_io_wait();
    event_lock.lock();
    // Wakeup only borrows nodes. The sleeping owner unlinks its stack node
    // under the same lock before returning, including signal-driven wakeups.
    auto **link = &waiters;
    while (*link != &waiter) {
      link = &(*link)->next;
    }
    *link = waiter.next;
  }
#else
  for (;;) {
    int ch;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(poll_lock);
      ch = uart::getc();
    }
    if (ch >= 0) {
      return ch;
    }
    if (moss::abi::bridge::moss_io_wait_interrupted()) {
      return -1;
    }
    // A syscall can enter with SIE clear. WFI may wake for SSIP without
    // dispatching its handler, leaving a remote VM owner waiting forever for
    // our TLB ACK before it can send the signal this read is waiting for.
    // No console/VM lock is held here: allow IRQ delivery while waiting and
    // preserve the caller's interrupt state after the idle sequence.
    const bool restore_irqs = arch::interrupts_enabled();
    arch::cpu_idle_once();
    if (restore_irqs) {
      arch::enable_interrupts();
    }
  }
#endif
}
} // namespace moss::kernel::drivers::console

extern "C" void console_rx_init() noexcept { (void)moss::kernel::drivers::console::initialize(); }
extern "C" int console_try_getc() noexcept { return moss::kernel::drivers::console::try_getc(); }
extern "C" int console_getc_blocking() noexcept { return moss::kernel::drivers::console::getc_blocking(); }
