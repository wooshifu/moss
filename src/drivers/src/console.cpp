module moss.drivers.console;

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
void receive_irq(u32, void *) noexcept { receive(); }
#endif
#endif
} // namespace

bool is_initialized() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(init_lock);
  return initialized;
}

VoidResult initialize() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(init_lock);
  if (initialized)
    return VoidResult{};
  if (!platform::hardware.uart.valid)
    return VoidResult{ErrorCode::NotFound};
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
  auto *controller = interrupts::g_gic;
  if (!controller)
    return VoidResult{ErrorCode::InvalidState};
  const u32 irq = platform::hardware.uart.irq;
#if defined(MOSS_ARCH_ARM64)
  auto registered = controller->register_interrupt(irq, receive_irq, nullptr, "uart_rx");
  if (!registered)
    return registered;
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
      if (restore_irqs)
        arch::enable_interrupts();
      return ch;
    }
    if (moss::abi::bridge::moss_io_wait_interrupted()) {
      event_lock.unlock();
      if (restore_irqs)
        arch::enable_interrupts();
      return -1;
    }
    void *thread = moss::abi::bridge::moss_prepare_io_wait();
    if (!thread) {
      event_lock.unlock();
      if (restore_irqs)
        arch::enable_interrupts();
#if defined(MOSS_ARCH_ARM64)
      asm volatile("wfi" ::: "memory");
#else
      asm volatile("hlt" ::: "memory");
#endif
      arch::disable_interrupts();
      event_lock.lock();
      continue;
    }
    // The event lock makes checking input and publishing sleep one transaction.
    Waiter waiter{thread, waiters};
    waiters = &waiter;
    event_lock.unlock();
    moss::abi::bridge::moss_commit_io_wait();
    event_lock.lock();
    // Wakeup only borrows nodes. The sleeping owner unlinks its stack node
    // under the same lock before returning, including signal-driven wakeups.
    auto **link = &waiters;
    while (*link != &waiter)
      link = &(*link)->next;
    *link = waiter.next;
  }
#else
  for (;;) {
    if (int ch = uart::getc(); ch >= 0)
      return ch;
    if (moss::abi::bridge::moss_io_wait_interrupted())
      return -1;
    asm volatile("wfi" ::: "memory");
  }
#endif
}
} // namespace moss::kernel::drivers::console

extern "C" void console_rx_init() noexcept { (void)moss::kernel::drivers::console::initialize(); }
extern "C" int console_try_getc() noexcept { return moss::kernel::drivers::console::try_getc(); }
extern "C" int console_getc_blocking() noexcept { return moss::kernel::drivers::console::getc_blocking(); }
