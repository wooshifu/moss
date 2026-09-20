"""Run the production console sleep path with deterministic UART interleavings.

The console function, RX ring operations and wake callback come from the real
source. IRQs and the scheduler are host models of the existing sleep handoff;
these checks do not replace the kernel's real serial/QEMU validation.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def braced_definition(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 1
    position = opening + 1
    while depth:
        depth += (text[position] == "{") - (text[position] == "}")
        position += 1
    return text[start:position]


def production_console_source():
    interface = (ROOT / "src/drivers/src/console.cppm").read_text()
    source = (ROOT / "src/drivers/src/console.cpp").read_text()
    ring = braced_definition(interface, "class RxRing") + ";"
    start = source.index("containers::IrqSpinLock event_lock;")
    end = source.index("void receive()", start)
    markers = ("void receive()", "int try_getc()", "int getc_blocking()")
    helpers = "\n".join(braced_definition(source, marker) for marker in markers)
    helpers = helpers.replace('asm volatile("wfi" ::: "memory");', "arch::cpu_idle_once();")
    helpers = helpers.replace('asm volatile("hlt" ::: "memory");', "arch::cpu_idle_once();")
    return "namespace console_rx {\n" + ring + source[start:end] + helpers + "\n}\n"


HOST_MODEL = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using usize = std::size_t;

static const char *scenario;
static bool irqs_enabled;
static bool event_injected;
static bool pending_uart;
static bool pending_remote;
static bool interrupted;
static bool event_locked;
static unsigned fifo_bytes;
static u32 current_cpu;
static u32 switches;
static u32 ready_publications;
static u32 deferred_wakes;

[[noreturn]] static void fail(const char *message) {
  std::fprintf(stderr, "%s\n", message);
  std::exit(1);
}

namespace process {
enum class ProcessState { Running, Sleeping, Ready };
struct CpuContext { bool saved = false; };
struct Thread {
  ProcessState state = ProcessState::Running;
  u32 cpu = 0;
  u32 wake_cpu = 0;
  u32 sleep_handoff = 0;
  CpuContext context;
};
static Thread reader;
static bool is_blocked_state(ProcessState state) { return state == ProcessState::Sleeping; }

class CfsScheduler {
public:
  static Thread *get_current_task() { return &reader; }
  static void switch_to_bootstrap(CpuContext &);
  void dequeue_task(Thread *);
  Thread *prepare_sleep();
  void commit_sleep();
  void task_wakeup(Thread *, u32);
};
static CfsScheduler scheduler;
static CfsScheduler *g_scheduler = &scheduler;
}

static void deliver_uart();
namespace arch {
static bool interrupts_enabled() { return irqs_enabled; }
static void disable_interrupts() {
  if (!event_injected && std::strcmp(scenario, "before-mask") == 0) {
    event_injected = true;
    deliver_uart();
  }
  irqs_enabled = false;
}
static void enable_interrupts() { irqs_enabled = true; }
static void cpu_idle_once() { fail("unexpected platform idle fallback"); }
}
namespace containers {
class IrqSpinLock {
  bool saved_ = false;
public:
  void lock() {
    if (event_locked) fail("recursive event lock");
    saved_ = arch::interrupts_enabled();
    arch::disable_interrupts();
    event_locked = true;
  }
  void unlock() {
    event_locked = false;
    if (pending_remote) { pending_remote = false; deliver_uart(); }
    if (saved_) arch::enable_interrupts();
  }
};
template <typename Lock> struct LockGuard {
  Lock &lock;
  explicit LockGuard(Lock &value) : lock(value) { lock.lock(); }
  ~LockGuard() { lock.unlock(); }
};
}
namespace uart {
static void ack_rx_interrupt() {}
static int getc() { if (!fifo_bytes) return -1; --fifo_bytes; return 'k'; }
}
namespace moss::abi::bridge {
static void *moss_prepare_io_wait() {
  auto *thread = process::g_scheduler->prepare_sleep();
  if (interrupted) process::g_scheduler->task_wakeup(thread, thread->wake_cpu);
  return thread;
}
static void moss_commit_io_wait() { process::g_scheduler->commit_sleep(); }
static void moss_wake_io_waiter(void *thread) {
  process::g_scheduler->task_wakeup(static_cast<process::Thread *>(thread), process::reader.wake_cpu);
}
static bool moss_io_wait_interrupted() { return interrupted; }
}
static void deliver_uart();
static void perform_switch();
"""


HOST_DRIVER = r"""
static bool is_scenario(const char *name) { return std::strcmp(scenario, name) == 0; }

static void deliver_uart() {
  if (event_locked) { pending_remote = true; return; }
  ++fifo_bytes;
  console_rx::receive();
}

void process::CfsScheduler::dequeue_task(Thread *) {
  if (event_injected) return;
  if (is_scenario("dequeue-local")) {
    event_injected = true;
    // Local UART IRQs remain pending while masked; the old body exposes an
    // enabled interval before registering its reader and loses this wake.
    if (irqs_enabled) deliver_uart();
    else pending_uart = true;
  } else if (is_scenario("dequeue-remote")) {
    event_injected = true;
    // A remote IRQ waits for the event lock, then sees the published waiter.
    deliver_uart();
  }
}

process::Thread *process::CfsScheduler::prepare_sleep() {
  if (irqs_enabled) fail("sleep preparation ran with local IRQs enabled");
  reader.context.saved = false;
  reader.sleep_handoff = 1; // Prepared, still executing: identical protocol states.
  reader.wake_cpu = current_cpu;
  reader.state = ProcessState::Sleeping;
  dequeue_task(&reader);
  return &reader;
}

void process::CfsScheduler::task_wakeup(Thread *thread, u32) {
  if (!is_blocked_state(thread->state)) return;
  if (thread->sleep_handoff != 0) {
    thread->sleep_handoff = 2; // Remember an early wake until the save completes.
    ++deferred_wakes;
    return;
  }
  // The running reader has excluded CPU 0 from affinity. The production wake
  // policy consequently chooses CPU 1, exposing an unsaved cross-CPU publish.
  constexpr u32 allowed_cpu = 1;
  if (allowed_cpu != current_cpu && !thread->context.saved)
    fail("Ready continuation published to another CPU before its context save");
  ++ready_publications;
  thread->cpu = allowed_cpu;
  thread->state = ProcessState::Ready;
}

static void perform_switch() {
  using namespace process;
  if (irqs_enabled) fail("context switch began with IRQs enabled");
  ++switches;
  if (is_scenario("signal") && !event_injected) {
    event_injected = true;
    interrupted = true;
    scheduler.task_wakeup(&reader, reader.wake_cpu);
  } else if (is_scenario("before-save") && !event_injected) {
    event_injected = true;
    deliver_uart();
  } else if (is_scenario("spurious-then-uart")) {
    if (switches == 1) scheduler.task_wakeup(&reader, reader.cpu);
    else deliver_uart();
  }

  // A synchronous host stand-in for assembly save, bootstrap acknowledgment,
  // then dispatch on the permitted CPU. The real kernel tests these steps in
  // its own scheduler and low-level context-switch implementation.
  reader.context.saved = true;
  const bool early_wake = reader.sleep_handoff == 2;
  reader.sleep_handoff = 0;
  if (early_wake) scheduler.task_wakeup(&reader, reader.wake_cpu);
  if (pending_uart) {
    pending_uart = false;
    deliver_uart();
  }
  if (reader.state != ProcessState::Ready)
    fail("UART byte buffered but its reader remained asleep without a wake");
  current_cpu = reader.cpu;
  reader.state = ProcessState::Running;
}

void process::CfsScheduler::switch_to_bootstrap(CpuContext &) { perform_switch(); }
void process::CfsScheduler::commit_sleep() { perform_switch(); }

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  scenario = argv[1];
  irqs_enabled = std::strcmp(argv[2], "enabled") == 0;
  const bool original_irqs = irqs_enabled;
  if (is_scenario("fast")) console_rx::ring.put('k');
  if (is_scenario("signal-pending")) interrupted = true;

  const int character = console_rx::getc_blocking();
  const int expected = is_scenario("signal") || is_scenario("signal-pending") ? -1 : 'k';
  if (character != expected) fail("console did not return the expected UART/signal result");
  if (irqs_enabled != original_irqs) fail("console changed its caller's IRQ mask");
  if (!console_rx::ring.empty()) fail("console left the returned byte queued");
  if (console_rx::waiters != nullptr) fail("completed console read left a registered waiter");
  if (process::reader.state != process::ProcessState::Running) fail("resumed reader was not Running");
  if (is_scenario("spurious-then-uart") && switches != 2)
    fail("spurious wake did not recheck RX and block until the UART byte arrived");
  if (is_scenario("before-save") && (deferred_wakes == 0 || ready_publications != 1))
    fail("early UART wake did not publish exactly once after its context save");
}
"""


@pytest.fixture(scope="module", params=["MOSS_ARCH_ARM64", "MOSS_ARCH_X64"])
def console_binary(request, tmp_path_factory):
    compiler = shutil.which("clang++")
    if not compiler:
        pytest.skip("Clang is needed for the production console host regression")
    directory = tmp_path_factory.mktemp(request.param.lower())
    cpp = directory / "console.cpp"
    binary = directory / "console"
    cpp.write_text(f"#define {request.param}\n" + HOST_MODEL + production_console_source() + HOST_DRIVER)
    compiled = subprocess.run(
        [compiler, "-std=c++23", "-O1", str(cpp), "-o", str(binary)],
        capture_output=True,
        text=True,
        timeout=90,  # 90 s is the host harness compiler budget, independent of kernel timing.
    )
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    return binary


@pytest.mark.parametrize("irq_state", ["enabled", "masked"])
@pytest.mark.parametrize(
    "scenario",
    [
        "dequeue-local",
        "dequeue-remote",
        "before-save",
        "spurious-then-uart",
        "fast",
        "before-mask",
        "signal",
        "signal-pending",
    ],
)
def test_console_sleep_preserves_uart_wakeup_and_irq_state(console_binary, irq_state, scenario):
    # This in-memory interleaving should finish immediately; 10 s bounds a
    # stalled regression process without changing any console sleep deadline.
    result = subprocess.run([str(console_binary), scenario, irq_state], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
