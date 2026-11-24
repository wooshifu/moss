#include "coverage_protocol.h"

import moss.std;
import moss.types;
import moss.mm;
import moss.process;
import moss.containers;
import moss.hal.uart;

using moss::kernel::PhysAddr;
using moss::kernel::u32;
using moss::kernel::u64;
using moss::kernel::usize;
using moss::kernel::VirtAddr;

#ifndef MOSS_COVERAGE_ARENA_BYTES
#define MOSS_COVERAGE_ARENA_BYTES (32ULL * 1024ULL * 1024ULL)
#endif

extern "C" {

alignas(16) unsigned char moss_coverage_arena[MOSS_COVERAGE_ARENA_BYTES];
unsigned long long moss_coverage_used;
unsigned long long moss_coverage_errors;

extern unsigned char __start___llvm_prf_cnts[];
extern unsigned char __stop___llvm_prf_cnts[];

// Instrumented objects reference this compiler-rt symbol even when the raw
// profile is exported by this wrapper instead of compiler-rt's writer.
__attribute__((visibility("hidden"))) int __llvm_profile_runtime = 0;

long __real_moss_validation_call(long op, long arg1, long arg2) noexcept;
void __real_moss_validation_address_space_retiring(unsigned long long root) noexcept;
}

namespace {

constexpr usize kMaxRegistrations = 64;
constexpr u64 kMaxCounters = 1ULL << 20;

struct Registration {
  PhysAddr root;
  moss::kernel::process::AddressSpace *space;
  VirtAddr counters;
  u64 counter_count;
  char program_id[MOSS_COVERAGE_PROGRAM_ID_MAX];
  bool used;
};

Registration registrations[kMaxRegistrations]{};
Registration kernel_registration{};
bool kernel_snapshot_done = false;
moss::kernel::containers::IrqSpinLock coverage_lock;

void copy_program_id(char *destination, const char *source) {
  usize index = 0;
  while (index + 1 < MOSS_COVERAGE_PROGRAM_ID_MAX && source[index]) {
    destination[index] = source[index];
    ++index;
  }
  destination[index] = 0;
}

bool append_record(const Registration &registration) {
  if (!registration.used || !registration.counter_count) {
    return false;
  }
  const u64 raw_size = MOSS_COVERAGE_RAW_HEADER_SIZE + registration.counter_count * sizeof(u64);
  const usize record_size = sizeof(moss_coverage_record_header) + static_cast<usize>(raw_size);
  if (moss_coverage_used + record_size > MOSS_COVERAGE_ARENA_BYTES) {
    ++moss_coverage_errors;
    return false;
  }

  auto *record = reinterpret_cast<moss_coverage_record_header *>(moss_coverage_arena + moss_coverage_used);
  record->magic = MOSS_COVERAGE_RECORD_MAGIC;
  record->version = MOSS_COVERAGE_PROTOCOL_VERSION;
  record->counter_count = static_cast<u32>(registration.counter_count);
  record->program_id_length = 0;
  while (record->program_id_length + 1 < MOSS_COVERAGE_PROGRAM_ID_MAX &&
         registration.program_id[record->program_id_length]) {
    record->program_id[record->program_id_length] = registration.program_id[record->program_id_length];
    ++record->program_id_length;
  }
  record->program_id[record->program_id_length] = 0;
  record->raw_size = raw_size;

  auto *raw = reinterpret_cast<u64 *>(moss_coverage_arena + moss_coverage_used + sizeof(*record));
  raw[0] = MOSS_COVERAGE_RAW_MAGIC;
  raw[1] = MOSS_COVERAGE_RAW_VERSION;
  raw[2] = 0;
  raw[3] = 0;
  raw[4] = 0;
  raw[5] = registration.counter_count;
  for (usize index = 6; index < 18; ++index) {
    raw[index] = 0;
  }
  raw[18] = MOSS_COVERAGE_RAW_VALUE_KIND_LAST;

  if (registration.space) {
    const usize remaining =
        registration.space->copy_from_user(raw + MOSS_COVERAGE_RAW_HEADER_QWORDS, registration.counters,
                                           static_cast<usize>(registration.counter_count) * sizeof(u64));
    if (remaining != 0) {
      ++moss_coverage_errors;
      return false;
    }
  } else {
    auto *counters = reinterpret_cast<const u64 *>(registration.counters);
    for (usize index = 0; index < registration.counter_count; ++index) {
      raw[MOSS_COVERAGE_RAW_HEADER_QWORDS + index] = counters[index];
    }
  }
  moss_coverage_used += record_size;
  return true;
}

void register_kernel_locked() {
  if (kernel_registration.used) {
    return;
  }
  const VirtAddr start = reinterpret_cast<VirtAddr>(__start___llvm_prf_cnts);
  const VirtAddr stop = reinterpret_cast<VirtAddr>(__stop___llvm_prf_cnts);
  if (stop <= start) {
    kernel_registration.used = true;
    kernel_snapshot_done = true;
    return;
  }
  const u64 counter_count = (stop - start) / sizeof(u64);
  if (counter_count > kMaxCounters) {
    ++moss_coverage_errors;
    kernel_registration.used = true;
    kernel_snapshot_done = true;
    return;
  }
  kernel_registration.root = 0;
  kernel_registration.space = nullptr;
  kernel_registration.counters = start;
  kernel_registration.counter_count = counter_count;
  copy_program_id(kernel_registration.program_id, "kernel");
  kernel_registration.used = true;
}

long register_user(long user_registration, long size) {
  if (size != static_cast<long>(sizeof(moss_coverage_registration))) {
    return -22;
  }
  auto process = moss::kernel::process::current_process();
  if (!process) {
    return -3;
  }
  auto space = process->address_space();
  if (!space) {
    return -14;
  }

  moss_coverage_registration registration{};
  if (space->copy_from_user(&registration, static_cast<VirtAddr>(user_registration), sizeof(registration)) != 0) {
    return -14;
  }
  if (registration.magic != MOSS_COVERAGE_REGISTER_MAGIC || registration.version != MOSS_COVERAGE_PROTOCOL_VERSION ||
      registration.counter_count == 0 || registration.counter_count > kMaxCounters ||
      !moss::kernel::mm::PageTableManager::is_user_range(
          registration.counters, static_cast<usize>(registration.counter_count) * sizeof(u64))) {
    return -22;
  }

  moss::kernel::containers::LockGuard<moss::kernel::containers::IrqSpinLock> guard(coverage_lock);
  Registration *slot = nullptr;
  for (auto &entry : registrations) {
    if (entry.used && entry.root == space->pgd_phys) {
      slot = &entry;
      break;
    }
  }
  if (!slot) {
    for (auto &entry : registrations) {
      if (!entry.used) {
        slot = &entry;
        break;
      }
    }
  }
  if (!slot) {
    return -28; // ENOSPC
  }
  slot->root = space->pgd_phys;
  slot->space = space.get();
  slot->counters = registration.counters;
  slot->counter_count = registration.counter_count;
  copy_program_id(slot->program_id, registration.program_id);
  slot->used = true;
  return 0;
}

void snapshot_all() {
  moss::kernel::containers::LockGuard<moss::kernel::containers::IrqSpinLock> guard(coverage_lock);
  register_kernel_locked();
  if (!kernel_snapshot_done) {
    (void)append_record(kernel_registration);
    kernel_snapshot_done = true;
  }
  for (auto &entry : registrations) {
    if (!entry.used) {
      continue;
    }
    (void)append_record(entry);
    entry = {};
  }
}

void retire_address_space(PhysAddr root) {
  moss::kernel::containers::LockGuard<moss::kernel::containers::IrqSpinLock> guard(coverage_lock);
  for (auto &entry : registrations) {
    if (entry.used && entry.root == root) {
      (void)append_record(entry);
      entry = {};
      return;
    }
  }
}

} // namespace

extern "C" long __wrap_moss_validation_call(long op, long arg1, long arg2) noexcept {
  {
    moss::kernel::containers::LockGuard<moss::kernel::containers::IrqSpinLock> guard(coverage_lock);
    register_kernel_locked();
  }
  if (op == MOSS_COVERAGE_OP_REGISTER) {
    return register_user(arg1, arg2);
  }
  if (op == MOSS_COVERAGE_OP_FINALIZE) {
    snapshot_all();
    return 0;
  }
  if (op == 3) {
    snapshot_all();
  }
  return __real_moss_validation_call(op, arg1, arg2);
}

extern "C" void __wrap_moss_validation_address_space_retiring(unsigned long long root) noexcept {
  retire_address_space(root);
  __real_moss_validation_address_space_retiring(root);
}

extern "C" void moss_coverage_finalize() noexcept {
  snapshot_all();
  moss::kernel::hal::uart::puts("MOSS_COVERAGE_FINALIZED\n");
}
