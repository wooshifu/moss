#pragma once

#include <stdint.h>

// Fixed-width, pointer-free system resource snapshot shared with libc.
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
