// Production-userspace timing probe; build instructions live in the Pi performance report.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "syscall.h"

// Two warmups fault in code/data; seven retained batches give a useful exploratory
// median and spread. These counts are not a calibrated regression acceptance gate.
enum { WARMUPS = 2, SAMPLES = 7 };
// A MiB is large enough to cross the A53's private cache; prefault it before copying.
enum { BUFFER_BYTES = 1024 * 1024, PAGE_BYTES = MOSS_MEM_OBJECT_BYTES };
static unsigned char source[BUFFER_BYTES], destination[BUFFER_BYTES];
static volatile unsigned long sink;
static struct moss_ipc_endpoints endpoint;
static long ipc_child;
static int descriptor;

static void require(int valid, const char *what) {
  if (!valid) {
    printf("RPI_PERF_ERROR %s\n", what);
    exit(1);
  }
}

static unsigned long now(void) {
  unsigned long ns = 0;
  require(syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&ns) == 0, "clock");
  return ns;
}

static void pin(unsigned cpu) {
  unsigned mask = 1U << cpu;
  require(syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(mask), (long)&mask) == 0, "affinity");
  // Yield after changing affinity so subsequent samples execute on the selected core.
  require(syscall0(SYS_SCHED_YIELD) == 0, "yield");
}

static void start_ipc(unsigned server_cpu) {
  require(syscall1(SYS_IPC_CREATE, (long)&endpoint) == 0, "ipc_create");
  require(syscall2(SYS_CAP_SET_INHERIT, endpoint.receive, 1) == 0, "ipc_inherit");
  ipc_child = syscall0(SYS_FORK);
  require(ipc_child >= 0, "ipc_fork");
  if (!ipc_child) {
    pin(server_cpu);
    for (;;) {
      struct moss_ipc_message request = {0};
      unsigned long reply = 0;
      require(syscall3(SYS_IPC_RECEIVE, endpoint.receive, (long)&request, (long)&reply) == 1, "ipc_receive");
      if (!request.payload[0]) {
        syscall1(SYS_EXIT, 0);
        __builtin_unreachable();
      }
      require(syscall2(SYS_IPC_REPLY, reply, (long)&request) == 0, "ipc_reply");
    }
  }
  require(syscall1(SYS_CAP_CLOSE, endpoint.receive) == 0, "parent_receive_close");
}

static void operation(const char *name, unsigned long iterations, unsigned long bytes) {
  for (unsigned long i = 0; i < iterations; ++i) {
    if (!strcmp(name, "empty")) {
      asm volatile("" ::: "memory"); // Calibrate dispatch and timestamp overhead with no workload.
    } else if (!strcmp(name, "instructions")) {
      // Exactly 20,000 retired instructions in the loop, plus setup/dispatch/PMU reads.
      unsigned long count = 10000;
      asm volatile("1: subs %0, %0, #1; b.ne 1b" : "+r"(count)::"cc", "memory");
    } else if (!strcmp(name, "clock")) {
      sink = now();
    } else if (!strcmp(name, "getpid_raw")) {
      long pid = syscall0(SYS_GETPID);
      require(pid > 0, "raw_getpid");
      sink = (unsigned long)pid;
    } else if (!strcmp(name, "getpid_libc")) {
      pid_t pid = getpid();
      require(pid > 0, "libc_getpid");
      sink = (unsigned long)pid;
    } else if (!strcmp(name, "yield")) {
      require(syscall0(SYS_SCHED_YIELD) == 0, "yield");
    } else if (!strcmp(name, "memcpy")) {
      memcpy(destination, source, bytes);
      // Compiler barrier plus observed data keep every real copy in the timed loop.
      asm volatile("" : : "r"(destination) : "memory");
      sink = destination[i % bytes];
    } else if (!strcmp(name, "read_seek")) {
      require(lseek(descriptor, 0, SEEK_SET) == 0, "seek");
      require(read(descriptor, destination, bytes) == (ssize_t)bytes, "read");
      sink = destination[0];
    } else if (!strcmp(name, "open_close")) {
      int fd = open("/busybox.elf", O_RDONLY);
      require(fd >= 0 && close(fd) == 0, "open_close");
    } else if (!strcmp(name, "write_seek")) {
      require(lseek(descriptor, 0, SEEK_SET) == 0, "write_seek");
      require(write(descriptor, source, bytes) == (ssize_t)bytes, "write");
    } else if (!strcmp(name, "fault")) {
      unsigned char *area = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      require(area != MAP_FAILED, "mmap");
      for (unsigned long offset = 0; offset < bytes; offset += PAGE_BYTES) {
        area[offset] = source[offset];
      }
      require(area[bytes - PAGE_BYTES] == source[bytes - PAGE_BYTES], "fault_data");
      require(munmap(area, bytes) == 0, "munmap");
    } else if (!strcmp(name, "fork_raw") || !strcmp(name, "fork_libc") || !strcmp(name, "exec")) {
      int raw = !strcmp(name, "fork_raw");
      pid_t child = raw ? (pid_t)syscall0(SYS_FORK) : fork();
      require(child >= 0, "fork");
      if (!child) {
        if (!strcmp(name, "exec")) {
          char *const args[] = {"sh", "-c", "exit 0", 0};
          execv("/busybox.elf", args);
          syscall1(SYS_EXIT, 1);
        } else {
          syscall1(SYS_EXIT, 0);
        }
        __builtin_unreachable();
      }
      int status = -1;
      long result = raw ? syscall3(SYS_WAITPID, child, (long)&status, 0) : waitpid(child, &status, 0);
      require(result == child && status == 0, "child_status");
    } else if (!strncmp(name, "ipc", 3)) {
      struct moss_ipc_message request = {.size = 1, .payload = {1}}, response = {0};
      // Five seconds matches the production service deadline. Zero skips explicit
      // deadline arming; the kernel still reserves a slot for inherited deadlines.
      unsigned long deadline = strstr(name, "deadline") ? now() + 5000000000UL : 0;
      require(syscall6(SYS_IPC_CALL, endpoint.send, (long)&request, (long)&response, deadline, 0, 0) == 1 &&
                  response.size == 1 && response.payload[0] == 1 && !response.capability,
              "ipc_call");
    } else {
      require(0, "unknown_workload");
    }
  }
}

// Enough raw observations for exploratory p99 (7,168 at the maximum batch size).
// This is not a confidence bound for rarer production outliers.
enum { TAIL_CAPACITY = 1024, PMU_EVENTS = 6 };
static unsigned long latencies[TAIL_CAPACITY];

static unsigned long ticks(void) {
  unsigned long value;
  asm volatile("isb; mrs %0, cntvct_el0" : "=r"(value)::"memory");
  return value;
}

static void counters(unsigned long values[PMU_EVENTS + 1]) {
  // Boot's diagnostic switch fixes this order; read access stays disabled otherwise.
  asm volatile("isb; mrs %0, pmevcntr0_el0; mrs %1, pmevcntr1_el0; "
               "mrs %2, pmevcntr2_el0; mrs %3, pmevcntr3_el0; "
               "mrs %4, pmevcntr4_el0; mrs %5, pmevcntr5_el0; mrs %6, pmccntr_el0"
               : "=r"(values[0]), "=r"(values[1]), "=r"(values[2]), "=r"(values[3]), "=r"(values[4]), "=r"(values[5]),
                 "=r"(values[6])::"memory");
}

int main(int argc, char **argv) {
  if (argc != 4 && argc != 5) {
    puts("usage: rpi-perf.elf WORKLOAD ITERATIONS BYTES [pmu]");
    return 1;
  }
  const char *name = argv[1];
  int diagnostic = argc == 5;
  require(!diagnostic || !strcmp(argv[4], "pmu"), "mode");
  unsigned long iterations = strtoul(argv[2], 0, 10), bytes = strtoul(argv[3], 0, 10);
  require(iterations > 0 && bytes > 0 && bytes <= BUFFER_BYTES, "arguments");
  require(!diagnostic || iterations <= TAIL_CAPACITY, "tail_capacity");
  pin(0);
  unsigned long frequency = 0;
  if (diagnostic) {
    struct TopInfo placement;
    require(topinfo(&placement) == 0 && placement.nr_processes <= TOP_MAX_PROCS, "placement");
    printf("RPI_PLACEMENT {\"processes\":[");
    for (unsigned long i = 0; i < placement.nr_processes; ++i) {
      const struct TopProcessInfo *process = &placement.procs[i];
      printf("%s{\"pid\":%ld,\"name\":\"%.16s\",\"cpu\":%lu}", i ? "," : "", process->pid, process->name, process->cpu);
    }
    puts("]}");
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    require(frequency > 0, "counter_frequency");
    unsigned long clock_start = now(), tick_start = ticks();
    // 20 ms makes the clock syscalls negligible while checking the hardware conversion.
    while (ticks() - tick_start < frequency / 50) {
    }
    unsigned long tick_elapsed = ticks() - tick_start, clock_elapsed = now() - clock_start;
    printf("RPI_CALIBRATION {\"frequency\":%lu,\"ticks\":%lu,\"ns\":%lu}\n", frequency, tick_elapsed, clock_elapsed);
  }
  memset(source, 0x5a, sizeof(source)); // Nonzero data catches missing/zero-filled copies.
  memset(destination, 0, sizeof(destination));
  if (!strcmp(name, "read_seek")) {
    descriptor = open("/busybox.elf", O_RDONLY);
    require(descriptor >= 0, "read_open");
  } else if (!strcmp(name, "write_seek")) {
    descriptor = open("/rpi-perf.dat", O_CREAT | O_RDWR | O_TRUNC, 0600);
    require(descriptor >= 0 && write(descriptor, source, bytes) == (ssize_t)bytes, "write_prepare");
  }
  if (!strncmp(name, "ipc", 3)) {
    start_ipc(name[3] == '1' ? 1 : 0); // ipc0/ipc1 compare same-core and cross-core rendezvous.
  }
  struct TopInfo before, after;
  for (unsigned sample = 0; sample < WARMUPS + SAMPLES; ++sample) {
    require(topinfo(&before) == 0, "top_before");
    unsigned long totals[PMU_EVENTS + 1] = {0}, start = now();
    if (diagnostic) {
      for (unsigned long i = 0; i < iterations; ++i) {
        unsigned long first[PMU_EVENTS + 1], last[PMU_EVENTS + 1];
        counters(first);
        unsigned long tick_start = ticks();
        operation(name, 1, bytes);
        unsigned long tick_elapsed = ticks() - tick_start;
        counters(last);
        // A53 is dual-issue at at most 1.4 GHz: <1 s prevents more than one
        // 32-bit instruction/event wrap. Reject a long stall instead of undercounting.
        require(tick_elapsed < frequency, "pmu_wrap_window");
        latencies[i] = tick_elapsed;
        for (unsigned counter = 0; counter < PMU_EVENTS; ++counter) {
          totals[counter] += (unsigned)(last[counter] - first[counter]);
        }
        totals[PMU_EVENTS] += last[PMU_EVENTS] - first[PMU_EVENTS];
      }
    } else {
      operation(name, iterations, bytes);
    }
    unsigned long elapsed = now() - start;
    require(topinfo(&after) == 0 && elapsed > 0, "sample_clock");
    if (!strcmp(name, "memcpy")) {
      require(memcmp(source, destination, bytes) == 0, "copy_data");
    }
    if (!strcmp(name, "write_seek")) {
      require(lseek(descriptor, 0, SEEK_SET) == 0 && read(descriptor, destination, bytes) == (ssize_t)bytes &&
                  memcmp(source, destination, bytes) == 0,
              "written_data");
    }
    // Preserve individual ticks; percentile calculations never use batch averages.
    if (diagnostic) {
      printf("RPI_PMU {\"sample\":%u,\"counters\":[", sample);
      for (unsigned counter = 0; counter <= PMU_EVENTS; ++counter) {
        printf("%s%lu", counter ? "," : "", totals[counter]);
      }
      printf("],\"ticks\":[");
      for (unsigned long i = 0; i < iterations; ++i) {
        printf("%s%lu", i ? "," : "", latencies[i]);
      }
      puts("]}");
    }
    // Serial output and correctness comparisons stay outside the timed interval.
    printf("RPI_PERF {\"workload\":\"%s\",\"sample\":%u,\"warmup\":%s,\"iterations\":%lu,"
           "\"bytes\":%lu,\"ns\":%lu,\"switches\":%lu}\n",
           name, sample, sample < WARMUPS ? "true" : "false", iterations, bytes, elapsed,
           after.total_context_switches - before.total_context_switches);
  }
  if (!strncmp(name, "ipc", 3)) {
    struct moss_ipc_message stop = {.size = 1}, response = {0};
    // The receiver exits on this message; dropping its pending reply returns EPIPE.
    require(syscall6(SYS_IPC_CALL, endpoint.send, (long)&stop, (long)&response, 0, 0, 0) == -EPIPE, "ipc_stop");
    require(syscall1(SYS_CAP_CLOSE, endpoint.send) == 0, "ipc_close");
    int status;
    require(syscall3(SYS_WAITPID, ipc_child, (long)&status, 0) == ipc_child && status == 0, "ipc_reap");
  }
  if (!strcmp(name, "read_seek") || !strcmp(name, "write_seek")) {
    require(close(descriptor) == 0, "close");
  }
  puts("RPI_PERF_DONE");
  return 0;
}
