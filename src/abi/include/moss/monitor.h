#pragma once

#include <stdint.h>

// Fixed-width, pointer-free monitoring ABI shared by the kernel and services.
// A command summary fits alongside identity and accounting in one 256-byte IPC
// payload. It is a bounded display string, never an executable path or authority.
enum { MOSS_MONITOR_NAME_BYTES = 16, MOSS_MONITOR_COMMAND_BYTES = 96 };

struct moss_domain_info {
  uint64_t sampled_ns;
  uint64_t cpu_time_ns; // All threads; actual charged time, including kernel execution.
  uint64_t start_time_ns;
  uint64_t virtual_bytes;
  uint64_t resident_bytes; // Shared/COW pages count in each domain that maps them.
  uint32_t uid;
  uint32_t gid;
  uint32_t cpu;
  uint32_t threads;
  int32_t nice;
  uint32_t state; // ProcessState; terminal domain state overrides thread state.
  char name[MOSS_MONITOR_NAME_BYTES];
  char command[MOSS_MONITOR_COMMAND_BYTES];
};

struct moss_system_info {
  uint64_t uptime_ns;
  // The allocator's managed RAM excludes firmware reservations, the kernel
  // image, allocator metadata and the boot archive. Free is a live snapshot.
  uint64_t memory_total_bytes;
  uint64_t memory_free_bytes;
  uint64_t cpu_count; // Configured logical CPUs discovered from firmware.
  uint64_t runnable_threads;
  uint64_t idle_time_ns;     // Sum over CPUs; divide deltas by cpu_count * elapsed time.
  uint64_t online_cpu_count; // CPUs that published runtime readiness.
  uint64_t process_count;
};

struct moss_process_info {
  uint64_t pid;
  uint64_t ppid;
  uint64_t pgid;
  uint64_t sid;
  uint64_t ceiling; // Highest identity at scan start; new forks wait for the next scan.
  struct moss_domain_info domain;
};

#ifdef __cplusplus
extern "C" {
#endif
// libc adapters return an errno value (zero on success); ESRCH ends a scan.
// exact selects a single compatibility PID, otherwise cursor is exclusive.
int moss_monitor_process(uint64_t cursor, uint64_t ceiling, int exact, struct moss_process_info *info);
int moss_monitor_system(struct moss_system_info *info);
// Console input adapter: byte or -1 with errno, bounded by milliseconds.
int moss_monitor_key(unsigned timeout_ms);
#ifdef __cplusplus
}
#endif
