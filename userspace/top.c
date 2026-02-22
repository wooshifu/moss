// MOSS top — system monitoring tool
//
// Displays real-time CPU load, memory usage, and per-process statistics.
// Refreshes periodically, then exits.
//
// Usage: top.elf [-d INTERVAL_MS] [-t DURATION_S]
//   -d  Refresh interval in milliseconds (default: 1000)
//   -t  Total duration in seconds (default: 10)
//
// Examples:
//   top.elf                  # 1s interval, 10s total
//   top.elf -d 500           # 500ms interval, 10s total
//   top.elf -t 30            # 1s interval, 30s total
//   top.elf -d 2000 -t 60   # 2s interval, 60s total

#include "syscall.h"

// ANSI escape sequences for terminal control
#define ESC_CLEAR "\033[2J"
#define ESC_HOME "\033[H"
#define ESC_BOLD "\033[1m"
#define ESC_RESET "\033[0m"
#define ESC_CYAN "\033[36m"
#define ESC_YELLOW "\033[33m"
#define ESC_GREEN "\033[32m"
#define ESC_RED "\033[31m"

// Process state names
static const char *state_name(unsigned long state) {
  switch (state) {
  case 0:
    return "Created";
  case 1:
    return "Ready  ";
  case 2:
    return "Running";
  case 3:
    return "Blocked";
  case 4:
    return "Dead   ";
  case 5:
    return "Zombie ";
  default:
    return "???    ";
  }
}

// Print a horizontal rule
static void print_ruler(void) { print("────────────────────────────────────────────────────────────────────────\n"); }

// Print the system summary header
static void print_header(struct TopInfo *info) {
  unsigned long uptime_ms = info->uptime_ns / 1000000;
  unsigned long uptime_s = uptime_ms / 1000;

  print(ESC_BOLD ESC_CYAN "MOSS top" ESC_RESET);
  print(" - uptime: ");
  print_ulong(uptime_s);
  print(".");
  // Print fractional seconds (1 digit)
  print_ulong((uptime_ms / 100) % 10);
  print("s");

  print("  procs: ");
  print_ulong(info->total_processes);

  print("  ctx_sw: ");
  print_ulong(info->total_context_switches);

  print("  preempt: ");
  print_ulong(info->total_preemptions);

  print("  forks: ");
  print_ulong(info->total_forks);
  print("\n");
}

// Print memory information
static void print_memory(struct TopInfo *info) {
  unsigned long page_kb = info->page_size / 1024;
  if (page_kb == 0)
    page_kb = 4; // default 4KB pages

  unsigned long total_kb = info->mem_total_pages * page_kb;
  unsigned long used_kb = info->mem_used_pages * page_kb;
  unsigned long free_kb = info->mem_free_pages * page_kb;

  // Calculate usage percentage
  unsigned long usage_pct = 0;
  if (info->mem_total_pages > 0)
    usage_pct = (info->mem_used_pages * 100) / info->mem_total_pages;

  print(ESC_BOLD "Memory:" ESC_RESET " ");
  print_ulong(total_kb);
  print("KB total  ");

  // Color used memory: green < 50%, yellow 50-80%, red > 80%
  if (usage_pct > 80)
    print(ESC_RED);
  else if (usage_pct > 50)
    print(ESC_YELLOW);
  else
    print(ESC_GREEN);
  print_ulong(used_kb);
  print("KB used");
  print(ESC_RESET "  ");

  print_ulong(free_kb);
  print("KB free  (");
  print_ulong(usage_pct);
  print("%)\n");
}

// Print CPU load information with usage percentages
static void print_cpus(struct TopInfo *info, struct TopInfo *prev) {
  unsigned long nr_cpus = info->nr_cpus;
  if (nr_cpus > TOP_MAX_CPUS)
    nr_cpus = TOP_MAX_CPUS;

  // Calculate total CPU usage across all CPUs
  unsigned long total_load = 0;
  unsigned long active_cpus = 0;
  for (unsigned long i = 0; i < nr_cpus; i++) {
    total_load += info->cpu_load[i];
    if (info->cpu_nr_running[i] > 0)
      active_cpus++;
  }
  unsigned long avg_load = (nr_cpus > 0) ? (total_load / nr_cpus) : 0;

  // Overall CPU usage line
  print(ESC_BOLD "CPU:" ESC_RESET " avg_load=");
  if (avg_load > 70)
    print(ESC_RED);
  else if (avg_load > 40)
    print(ESC_YELLOW);
  else
    print(ESC_GREEN);
  print_ulong(avg_load);
  print("%");
  print(ESC_RESET);
  print("  active_cpus=");
  print_ulong(active_cpus);
  print("/");
  print_ulong(nr_cpus);

  // Total running tasks
  unsigned long total_running = 0;
  for (unsigned long i = 0; i < nr_cpus; i++)
    total_running += info->cpu_nr_running[i];
  print("  tasks_running=");
  print_ulong(total_running);
  print("\n");

  // Per-CPU lines: 4 CPUs per line
  for (unsigned long i = 0; i < nr_cpus; i++) {
    if (i % 4 == 0)
      print("  ");

    // CPU label
    print("CPU");
    print_ulong(i);
    print(":");

    // Load with color coding
    unsigned long load = info->cpu_load[i];
    if (load > 70)
      print(ESC_RED);
    else if (load > 40)
      print(ESC_YELLOW);
    else
      print(ESC_GREEN);
    print_num_padded(load, 3);
    print("%");
    print(ESC_RESET);

    print(" r=");
    print_ulong(info->cpu_nr_running[i]);

    // CPU usage % based on exec time delta
    if (prev != 0 && prev->uptime_ns > 0) {
      unsigned long dt = info->uptime_ns - prev->uptime_ns;
      if (dt > 0) {
        unsigned long idle_delta = info->cpu_idle_time_ns[i] - prev->cpu_idle_time_ns[i];
        unsigned long busy_pct = 0;
        if (dt > idle_delta)
          busy_pct = ((dt - idle_delta) * 100) / dt;
        if (busy_pct > 100)
          busy_pct = 100;
        print(" use=");
        print_ulong(busy_pct);
        print("%");
      }
    }

    if ((i + 1) % 4 == 0 || i == nr_cpus - 1)
      print("\n");
    else
      print(" | ");
  }
}

// Print the process table
static void print_process_table(struct TopInfo *info, struct TopInfo *prev) {
  // Table header
  print(ESC_BOLD);
  print("  PID  PPID  CPU  NI");
  print("      VRUNTIME   EXEC_TIME  LAVG  UTIL  STATE    NAME\n");
  print(ESC_RESET);

  for (unsigned long i = 0; i < info->nr_processes; i++) {
    struct TopProcessInfo *p = &info->procs[i];

    // PID
    print_num_padded((unsigned long)p->pid, 5);
    print(" ");

    // PPID
    print_num_padded((unsigned long)p->ppid, 5);
    print(" ");

    // CPU
    print_num_padded(p->cpu, 4);
    print(" ");

    // Nice
    print_snum_padded(p->nice, 3);
    print(" ");

    // Vruntime (in microseconds for readability)
    unsigned long vrt_us = p->vruntime / 1000;
    print_num_padded(vrt_us, 12);
    print(" ");

    // Exec time (convert ns to human-readable)
    unsigned long exec_ms = p->sum_exec_runtime / 1000000;
    if (exec_ms >= 1000) {
      // Show in seconds
      print_num_padded(exec_ms / 1000, 7);
      print(".");
      print_ulong((exec_ms / 100) % 10);
      print("s ");
    } else {
      // Show in milliseconds
      print_num_padded(exec_ms, 8);
      print("ms");
    }

    // Load avg
    print_num_padded(p->load_avg, 5);
    print(" ");

    // Util avg
    print_num_padded(p->util_avg, 5);
    print(" ");

    // State
    print(" ");
    unsigned long st = p->state;
    if (st == 2)
      print(ESC_GREEN); // Running
    else if (st == 3)
      print(ESC_YELLOW); // Blocked
    else if (st >= 4)
      print(ESC_RED); // Dead/Zombie
    print(state_name(st));
    print(ESC_RESET);
    print("  ");

    // Name
    if (p->name[0])
      print(p->name);
    else {
      print("pid=");
      print_ulong((unsigned long)p->pid);
    }
    print("\n");
  }
}

// Simple string-to-unsigned-long conversion
static unsigned long parse_ulong(const char *s) {
  unsigned long val = 0;
  while (*s >= '0' && *s <= '9') {
    val = val * 10 + (unsigned long)(*s - '0');
    s++;
  }
  return val;
}

void _start(long argc, char **argv) {
  // Defaults: 1s refresh interval, 10s total duration
  unsigned long interval_ms = 1000;
  unsigned long duration_s = 10;

  // Parse command-line arguments
  for (long i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1] == 'd' && argv[i][2] == '\0') {
      if (i + 1 < argc) {
        interval_ms = parse_ulong(argv[++i]);
        if (interval_ms == 0)
          interval_ms = 100; // minimum 100ms
      }
    } else if (argv[i][0] == '-' && argv[i][1] == 't' && argv[i][2] == '\0') {
      if (i + 1 < argc) {
        duration_s = parse_ulong(argv[++i]);
        if (duration_s == 0)
          duration_s = 1;
      }
    }
  }

  unsigned long sleep_ns = interval_ms * 1000000UL;
  unsigned long total_iterations = (duration_s * 1000) / interval_ms;
  if (total_iterations == 0)
    total_iterations = 1;

  // Two TopInfo buffers for delta calculation
  struct TopInfo info_a;
  struct TopInfo info_b;
  struct TopInfo *cur = &info_a;
  struct TopInfo *prev = &info_b;

  // Initial snapshot (prev)
  long ret = topinfo(prev);
  if (ret < 0) {
    print("top: topinfo syscall failed (");
    print_long(ret);
    print(")\n");
    _exit(1);
  }

  // Wait one interval for first delta
  nanosleep_ns(&sleep_ns);

  for (unsigned long i = 0; i < total_iterations; i++) {
    // Get current snapshot
    ret = topinfo(cur);
    if (ret < 0) {
      print("top: topinfo failed\n");
      _exit(1);
    }

    // Clear screen and draw
    print(ESC_CLEAR ESC_HOME);

    print_header(cur);
    print_memory(cur);
    print_cpus(cur, prev);
    print_ruler();
    print_process_table(cur, prev);

    // Swap buffers
    struct TopInfo *tmp = prev;
    prev = cur;
    cur = tmp;

    // Sleep
    nanosleep_ns(&sleep_ns);
  }

  print("\ntop: finished (");
  print_ulong(total_iterations);
  print(" iterations)\n");
  _exit(0);
}
