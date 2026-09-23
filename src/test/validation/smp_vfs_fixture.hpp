#pragma once

#include "validation/smp_cases.hpp"

namespace moss::test::validation {
using namespace moss::kernel;
struct FileReferences {
  vfs::FdTable reader, writers[2];
  // phase is the owner's release-published milestone; arrived is the peer's
  // acknowledgement. Each ownership cycle uses two milestones (clone/release),
  // so subsequent scenarios start after 2 * cycles. Acquire waits make the
  // associated fixture fields visible for quiescent reference-count checks.
  u32 phase = 0, arrived = 0;
  u32 peer_cpu = ~0U;
  process::Thread *peer_thread = nullptr;
  long shared_read_fd = -1, shared_read_result = -1;
  long shared_write_fd = -1, shared_write_result = -1;
  long shared_open_result = -1;
  // Distinct arbitrary negative sentinels expose unexpected output mutation;
  // successful descriptors are nonnegative, so neither can look like success.
  long shared_pipe_result = -1, shared_pipe_fds[2] = {-37, -73};
  u32 pipe_published = 0;
  vfs::FdTable *native_table = nullptr;
  long native_result = 0, native_fds[2] = {-1, -1};
  u32 copyout_ready = 0;
  u32 dup_ready = 0;
  long dup_result = -1;
  u8 shared_byte = 0;
  // Bounded stress dimensions: repeat 1000 ownership cycles and hold 64 copies
  // per cycle to exercise refcounts and pool reuse, not an API capacity limit.
  static constexpr u32 cycles = 1000, copies = 64;

  template <typename Condition> static void require(Condition valid) {
    if (!boost::ut::expect(static_cast<Condition &&>(valid))) {
      // Ownership may already be corrupt. Preserve the failure and discard this
      // guest without releasing suspect pointers or leaving a peer unbounded.
      end_case();
      finish("file_ownership");
    }
  }

  static void require_refs(vfs::File *file, u32 expected, u32 cycle) {
    const u32 actual = file->ref_count;
    if (actual != expected) {
      logging::klog::error("File references cycle {}: expected {}, observed {}", cycle, expected, actual);
    }
    require(actual == expected);
  }

  long peer();
  bool dup_peer();
  void owner();
};
} // namespace moss::test::validation
