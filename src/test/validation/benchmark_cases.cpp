import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
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
#include "validation/core_cases.hpp"
#include "validation/exec_control.hpp"
#include "validation/isolation.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/process_control.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/signal_control.hpp"
#include "validation/smp_cases.hpp"
#include "validation/timer_control.hpp"
#include "validation/uaccess.hpp"
#include "validation_internal.hpp"

#include "validation/benchmark.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::test::validation {
bench::Clock discover_clock() {
  bench::Clock result;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mrs %0, cntfrq_el0" : "=r"(result.frequency));
  result.source = "cntfrq_el0";
#elif defined(MOSS_ARCH_RISCV64)
  result.frequency = moss::fdt::get_platform_info().timebase_frequency;
  result.source = "dtb.timebase-frequency";
#else
  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
  // CPUID leaf 0x15 supplies crystal Hz (ECX) and the TSC/crystal ratio
  // EBX/EAX. A missing leaf or zero field requires independent calibration.
  if (eax >= 0x15) {
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x15), "c"(0));
    if (eax && ebx && ecx) {
      result.frequency = static_cast<u64>(ecx) * ebx / eax;
      result.source = "cpuid.15";
    }
  }
  if (!result.frequency) {
    auto out = [](u16 port, u8 value) { asm volatile("outb %0, %1" ::"a"(value), "Nd"(port)); };
    auto in = [](u16 port) {
      u8 value;
      asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
      return value;
    };
    auto count = [&] {
      // PC PIT command port 0x43 / channel-0 port 0x40. Command zero latches
      // channel 0, then low/high reads reconstruct one stable 16-bit sample.
      out(0x43, 0);
      u16 low = in(0x40);
      return static_cast<u16>(low | (static_cast<u16>(in(0x40)) << 8));
    };
    u64 frequencies[3]{};
    // Three independent samples let the median reject one outlier. The 5%
    // maximum spread below is a calibration acceptance policy, not precision proof.
    for (unsigned sample = 0; sample < 3; ++sample) {
      // 0x30 selects channel 0, low/high access, binary mode 0. Reload 0xffff
      // provides a finite countdown; an observed wrap invalidates the sample.
      out(0x43, 0x30);
      out(0x40, 0xFF);
      out(0x40, 0xFF);
      u16 first = count();
      u64 start = bench::read_counter();
      u16 last = first;
      // One million port-read attempts bounds a stalled PIT. Require at least
      // 16384 reference ticks (~13.7 ms); the 60000-tick ceiling stays below a
      // full 16-bit countdown. Exact retry/window tuning evidence is not recorded.
      for (u32 retry = 0; retry < 1000000 && first - last < 16384; ++retry) {
        last = count();
      }
      u64 end = bench::read_counter();
      if (last > first || first - last < 16384 || first - last > 60000 || end <= start) {
        return {};
      }
      result.calibration_ticks[sample] = end - start;
      result.reference_ticks[sample] = static_cast<u64>(first - last);
      // Convert using the PC PIT's 1,193,182 Hz reference, also QEMU's PIT_FREQ:
      // https://github.com/qemu/qemu/blob/master/include/hw/timer/i8254.h
      frequencies[sample] = (end - start) * 1193182ULL / result.reference_ticks[sample];
    }
    u64 low = frequencies[0], high = frequencies[0];
    for (u64 frequency : frequencies) {
      if (frequency < low) {
        low = frequency;
      }
      if (frequency > high) {
        high = frequency;
      }
    }
    if (!low || (high - low) * 100 > low * 5) {
      return {};
    }
    result.frequency = frequencies[0] + frequencies[1] + frequencies[2] - low - high;
    // Scale relative spread by 10^6 for ppm and add a policy margin of 1000 ppm
    // (0.1%); the margin has no recorded measurement-based derivation.
    result.uncertainty_ppm = static_cast<unsigned>((high - low) * 1000000 / low) + 1000;
    result.source = "pit.channel0";
  }
#endif
  u64 begin = bench::read_counter();
  u64 previous = begin;
  // Probe monotonicity 4096 times, then accept 1 kHz..100 GHz as a broad policy
  // sanity range. These finite checks do not establish counter accuracy or stability.
  for (unsigned i = 0; i < 4096; ++i) {
    u64 current = bench::read_counter();
    if (current < previous) {
      return {};
    }
    previous = current;
  }
  if (previous <= begin || result.frequency < 1000 || result.frequency > 100000000000ULL) {
    return {};
  }
  return result;
}

void record_batch(u64 ticks, usize operations, bool warmup, u64 overhead) {
  Event("batch")
      .number("index", sample_index++)
      .number("ticks", ticks)
      .number("operations", operations)
      .number("warmup", warmup ? 1 : 0)
      .number("overhead_ticks", overhead)
      .number("cpu", arch::get_current_cpu_id())
      .str("measurement_kind", ut::same_id(selection, "bench.signal") || ut::same_id(selection, "bench.timer") ||
                                       ut::same_id(selection, "bench.wakeup")
                                   ? "event_sum"
                                   : "elapsed_batch")
      .send();
}

struct TimerBenchmark {
  timer::HrTimer pending;
  process::Thread *sleeper = nullptr;
  u64 observed = 0;

  static void callback(void *data) noexcept {
    const auto now = bench::read_counter();
    auto &self = *static_cast<TimerBenchmark *>(data);
    __atomic_store_n(&self.observed, now, __ATOMIC_RELEASE);
    if (self.sleeper) {
      process::g_scheduler->task_wakeup(self.sleeper, 0);
    }
  }

  u64 measure(bool wakeup) {
    auto &subsystem = timer::TimerSubsystem::instance();
    // Arm 1 ms ahead and measure lateness after the expected counter value;
    // the programmed delay itself is not included in the returned latency.
    const auto deadline = subsystem.now_ns() + 1000000ULL;
    const auto expected = subsystem.clocksource().deadline_counter(deadline);
    observed = 0;
    sleeper = wakeup ? process::CfsScheduler::get_current_task() : nullptr;
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    if (sleeper) {
      sleeper->state = process::ProcessState::Sleeping;
    }
    pending.init(timer::TimerMode::OneShot, callback, this);
    if (!pending.start(deadline)) {
      if (sleeper) {
        sleeper->state = process::ProcessState::Running;
      }
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return 0;
    }
    u64 resumed = 0;
    if (sleeper) {
      process::g_scheduler->dequeue_task(sleeper);
      process::CfsScheduler::switch_to_bootstrap(sleeper->context);
      resumed = bench::read_counter();
    }
    if (restore_irqs) {
      arch::enable_interrupts();
    }
    while (!__atomic_load_n(&observed, __ATOMIC_ACQUIRE)) {
      arch::cpu_yield();
    }
    pending.cancel_sync();
    const auto fired = __atomic_load_n(&observed, __ATOMIC_ACQUIRE);
    if (!ut::expect(fired >= expected && affinity_valid() && (!sleeper || resumed > fired))) {
      return 0;
    }
    return sleeper ? resumed - fired : fired - expected;
  }
};

void timer_benchmark(bench::Context &context, bool wakeup) {
  TimerBenchmark fixture;
  const auto before = LifecycleResources::capture();
  auto batch = [&](usize count) {
    u64 ticks = 0;
    for (usize i = 0; i < count; ++i) {
      const auto elapsed = fixture.measure(wakeup);
      if (!elapsed) {
        context.valid = false;
      }
      ticks += elapsed;
    }
    return ticks;
  };
  // Cap event batches at 64 to bound repeated timer/scheduler setup. The pilot
  // uses a 1 ms sum-of-lateness target, matching the batch harness time scale.
  constexpr usize capacity = 64;
  if (context.iterations > capacity) {
    context.valid = false;
    return;
  }
  if (!context.iterations) {
    context.iterations = 1;
    while (context.valid && batch(context.iterations) < context.clock.frequency / 1000 &&
           context.iterations < capacity) {
      context.iterations *= 2;
    }
  }
  for (unsigned i = 0; context.valid && i < context.warmup + context.samples; ++i) {
    const auto ticks = batch(context.iterations);
    const auto begin = bench::read_counter();
    const auto overhead = (bench::read_counter() - begin) * context.iterations;
    context.valid = context.valid && LifecycleResources::capture() == before;
    if (context.valid) {
      record_batch(ticks, context.iterations, i < context.warmup, overhead);
    }
  }
}

void prepare_clock() {
  clock_info = discover_clock();
  Event("clock")
      .number("frequency", clock_info.frequency)
      .str("source", clock_info.source)
      .number("uncertainty_ppm", clock_info.uncertainty_ppm)
      .send();
  for (unsigned i = 0; i < 3; ++i) {
    if (clock_info.reference_ticks[i]) {
      Event("calibration")
          .number("index", i)
          .number("ticks", clock_info.calibration_ticks[i])
          .number("reference_ticks", clock_info.reference_ticks[i])
          .number("reference_frequency", 1193182)
          .send();
    }
  }
  if (!clock_info.frequency) {
    failed = true;
    finish("invalid_clock");
  }
}

void allocation_benchmark(bench::Context &context, unsigned mode) {
  // Track the Context's default 256 operations without heap bookkeeping.
  // Orders 0..4 bound each block to 1..16 pages, keeping the fixture workload
  // separate from the allocator's larger production MAX_ORDER limit.
  PhysAddr pages_owned[256]{};
  bool valid = true;
  usize order = static_cast<usize>(numeric_boot_option("moss.order", 0));
  if (order > 4) {
    failed = true;
    finish("invalid_order");
  }
  auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  context.measure_batches(
      [&](usize count) {
        valid = true;
        for (usize i = 0; i < count; ++i) {
          pages_owned[i] = 0;
          if (mode == 1) {
            auto page = mm::PageFrameAllocator::allocate_pages(order);
            if (!page) {
              valid = false;
              break;
            }
            pages_owned[i] = *page;
          }
        }
        return valid;
      },
      [&](usize i) {
        if (mode == 1) {
          if (!mm::PageFrameAllocator::free_pages(pages_owned[i], order)) {
            valid = false;
          } else {
            pages_owned[i] = 0;
          }
        } else {
          auto page = mm::PageFrameAllocator::allocate_pages(order);
          if (!page) {
            valid = false;
            return;
          }
          pages_owned[i] = *page;
          asm volatile("" : "+m"(pages_owned[i]) : : "memory");
          if (mode == 2) {
            if (!mm::PageFrameAllocator::free_pages(pages_owned[i], order)) {
              valid = false;
            } else {
              pages_owned[i] = 0;
            }
          }
        }
      },
      [&](usize count) {
        for (usize i = 0; i < count; ++i) {
          if (pages_owned[i]) {
            valid = static_cast<bool>(mm::PageFrameAllocator::free_pages(pages_owned[i], order)) && valid;
            pages_owned[i] = 0;
          }
        }
        return valid && mm::PageFrameAllocator::get_memory_stats().free_pages == before && affinity_valid();
      });
  failed = !context.valid;
}

void read_benchmark(bench::Context &context) {
  void *table = fd_table();
  long fd = vfs::syscall::do_open(table, "/fixture.bin", 0, 0);
  if (fd < 0) {
    failed = true;
    return;
  }
  // 256 operations each read 256 bytes: 65536 bytes cover the largest batch.
  // Static storage keeps this buffer off the 16 KiB kernel stack.
  static u8 bytes[65536];
  bool valid = true;
  // Each timed operation reads a separate 256-byte slice of the fixture.
  context.measure_batches(
      [&](usize) {
        valid = true;
        return vfs::syscall::do_lseek(table, fd, 0, 0) == 0;
      },
      [&](usize i) {
        valid = (vfs::syscall::do_read(table, fd, vfs::OutputBuffer::kernel(bytes + i * 256, 256)) == 256) && valid;
      },
      [&](usize count) {
        for (usize i = 0; i < count * 256; ++i) {
          valid = valid && bytes[i] == i % 256;
        }
        return valid && affinity_valid();
      });
  failed = !context.valid;
  failed = vfs::syscall::do_close(table, fd) != 0 || failed;
}
} // namespace moss::test::validation
