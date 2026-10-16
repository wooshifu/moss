import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
import moss.hal.intc;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.timer;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/memory_internal.hpp"
#include "validation/smp_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::test::validation {
struct InterruptUnbind {
  interrupts::GenericInterruptController *controller = moss::boot::g_gic_controller;
  // The QEMU profiles leave the source adjacent to the UART unused; the test
  // only exercises software admission and never enables this hardware IRQ.
  interrupts::InterruptId irq = platform::hardware.uart.irq + 1;
  shared_ptr<interrupts::InterruptDescriptor> descriptor;
  u32 entered = 0, finished = 0;
#if defined(MOSS_ARCH_ARM64)
  u32 probe_requested = 0;
#endif

  void prepare() {
    smp_require(controller && !controller->get_interrupt_info(irq));
    smp_require(controller->register_interrupt(irq, +[](u32, void *) noexcept {}, this, "unbind-lifetime"));
    descriptor = controller->get_interrupt_info(irq);
    smp_require(descriptor && descriptor->context == this);
  }

  bool peer() {
    const bool right_cpu = arch::get_current_cpu_id() == 1;
    if (!descriptor->begin_callback()) {
      return false;
    }
    __atomic_store_n(&entered, 1U, __ATOMIC_RELEASE);
    // Hold the old callback while the owner starts unbinding. A second
    // admission must fail before the first callback releases its context.
    while (descriptor->begin_callback()) {
      descriptor->end_callback();
      arch::cpu_yield();
    }
    const bool unbinding = controller->enable_interrupt(irq).error() == ErrorCode::ResourceBusy &&
                           controller->unregister_interrupt(irq).error() == ErrorCode::ResourceBusy;
    // Unregister may return as soon as the callback lease reaches zero.
    // Publish the completed checks before releasing that lease.
    __atomic_store_n(&finished, 1U, __ATOMIC_RELEASE);
    descriptor->end_callback();
#if defined(MOSS_ARCH_ARM64)
    wait_for_phase(probe_requested, 1);
    // SGI/mailbox 0 is the scheduler's real reschedule source; target bit zero
    // selects the BSP and exercises the previously missing CPU1 -> CPU0 route.
    const auto &intc = platform::hardware.intc;
    const auto sent = hal::intc::send_sgi(intc.dist_base, intc.cpu_base, 0, 1);
    return right_cpu && unbinding && sent.has_value();
#else
    return right_cpu && unbinding;
#endif
  }

  void owner() {
    wait_for_phase(entered, 1);
    const auto result = controller->unregister_interrupt(irq);
    ut::expect(result.has_value() && __atomic_load_n(&finished, __ATOMIC_ACQUIRE) == 1 &&
               !controller->get_interrupt_info(irq) && affinity_valid());
#if defined(MOSS_ARCH_ARM64)
    auto &timers = timer::TimerSubsystem::instance();
    // Park this CPU's timer one second ahead. Only an IPI can reprogram it
    // during the probe; a periodic tick cannot hide a disabled mailbox.
    // Reuse timer_control.cpp's 200 ms host-scheduling margin, not a latency SLA.
    const auto parked = hal::timer::read_counter() + timers.clocksource().ns_to_cycles(1000000000ULL);
    const auto deadline = hal::timer::read_counter() + timers.clocksource().ns_to_cycles(200000000ULL);
    arch::disable_interrupts();
    hal::timer::set_compare(parked);
    __atomic_store_n(&probe_requested, 1U, __ATOMIC_RELEASE);
    arch::enable_interrupts();
    u64 compare = parked;
    while (compare == parked && hal::timer::read_counter() < deadline) {
      asm volatile("mrs %0, cntv_cval_el0" : "=r"(compare));
      arch::cpu_yield();
    }
    ut::expect(arch::get_current_cpu_id() == 0 && compare != parked);
    timers.reprogram_local();
#endif
  }
};
InterruptUnbind *interrupt_unbind = nullptr;

static void empty_case() {}
void register_interrupt_smp() {
  ut::register_suite("interrupts.smp", [] { ut::register_test("irq_context_retirement", empty_case); });
}

long start_interrupt_smp() {
  const char *selection = selected_suite();
  if (ut::same_id(selection, "interrupts.smp")) {
    start_case("irq_context_retirement");
    smp_require(g_num_cpus >= 2);
    interrupt_unbind = new InterruptUnbind();
    interrupt_unbind->prepare();
    return 3;
  }
  return 0;
}

long control_interrupt_smp(long op, long arg1, [[maybe_unused]] long arg2) {
  const char *selection = selected_suite();
  const char *active_case = running_case();
  if (ut::same_id(selection, "interrupts.smp") && active_case && interrupt_unbind) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return interrupt_unbind->peer() ? 1 : 0;
    }
    if (op == 8) {
      smp_require(arg1 && affinity_valid());
      arch::enable_interrupts();
      interrupt_unbind->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete interrupt_unbind;
      interrupt_unbind = nullptr;
      end_case();
      finish();
    }
  }
  invalid_control();
}
} // namespace moss::test::validation
