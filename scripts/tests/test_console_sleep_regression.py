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
    source = (ROOT / "src/kernel/src/syscall_table.cpp").read_text()
    ring_start = source.index("constexpr usize RX_BUF_SIZE =", source.index("namespace console_rx"))
    ring_end = source.index("static bool buf_empty()", ring_start)
    helpers = "\n".join(
        braced_definition(source, marker)
        for marker in (
            "static bool buf_empty()",
            "static int buf_get()",
            "static bool buf_put(",
            "static void wake_blocked_reader()",
        )
    )
    console = braced_definition(source, 'extern "C" int console_getc_blocking()')
    # Only replace privileged platform idle instructions. The tested queue,
    # waiter registration and sleep control flow remain the production body.
    console = console.replace('asm volatile("wfi" ::: "memory");', "arch::cpu_idle_once();")
    console = console.replace('asm volatile("hlt" ::: "memory");', "arch::cpu_idle_once();")
    return "namespace console_rx {\n" + source[ring_start:ring_end] + helpers + "\n}\n" + console


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

namespace arch {
static bool interrupts_enabled() { return irqs_enabled; }
static void disable_interrupts() { irqs_enabled = false; }
static void enable_interrupts() { irqs_enabled = true; }
static void cpu_idle_once() { fail("unexpected platform idle fallback"); }
}
extern "C" int console_try_getc() noexcept;
static void deliver_uart();
static void perform_switch();
"""


HOST_DRIVER = r"""
static bool is_scenario(const char *name) { return std::strcmp(scenario, name) == 0; }

static void deliver_uart() {
  if (!console_rx::buf_put('k')) fail("host UART ring unexpectedly full");
  console_rx::wake_blocked_reader();
}

extern "C" int console_try_getc() noexcept {
  const int result = console_rx::buf_get();
  // A UART IRQ may fill RX after the initial nonblocking read reports empty.
  if (result < 0 && is_scenario("before-mask") && !event_injected) {
    event_injected = true;
    deliver_uart();
  }
  return result;
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
    // Masking the reader CPU cannot stop a UART producer on another CPU.
    // It can fill RX before blocked_reader is published.
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
  if (is_scenario("before-save") && !event_injected) {
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
  if (is_scenario("fast")) console_rx::buf_put('k');

  const int character = console_getc_blocking();
  if (character != 'k') fail("console did not return the received UART byte");
  if (irqs_enabled != original_irqs) fail("console changed its caller's IRQ mask");
  if (!console_rx::buf_empty()) fail("console left the returned byte queued");
  if (console_rx::blocked_reader_ != nullptr) fail("completed console read left a registered waiter");
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
    "scenario", ["dequeue-local", "dequeue-remote", "before-save", "spurious-then-uart", "fast", "before-mask"]
)
def test_console_sleep_preserves_uart_wakeup_and_irq_state(console_binary, irq_state, scenario):
    # This in-memory interleaving should finish immediately; 10 s bounds a
    # stalled regression process without changing any console sleep deadline.
    result = subprocess.run([str(console_binary), scenario, irq_state], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
