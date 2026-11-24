#include "coverage_protocol.h"

#include "syscall.h"

#ifndef MOSS_COVERAGE_PROGRAM_ID
#define MOSS_COVERAGE_PROGRAM_ID "unknown"
#endif

extern const unsigned char __start___llvm_prf_cnts[];
extern const unsigned char __stop___llvm_prf_cnts[];

/* The instrumentation pass references this symbol even though the raw profile
 * is exported by the kernel wrapper rather than compiler-rt. */
__attribute__((visibility("hidden"))) int __llvm_profile_runtime;

static void moss_coverage_register(void) {
  const unsigned long long start = (unsigned long long)__start___llvm_prf_cnts;
  const unsigned long long stop = (unsigned long long)__stop___llvm_prf_cnts;
  if (stop <= start) {
    return;
  }

  struct moss_coverage_registration registration = {
      .magic = MOSS_COVERAGE_REGISTER_MAGIC,
      .version = MOSS_COVERAGE_PROTOCOL_VERSION,
      .counters = start,
      .counter_count = (stop - start) / sizeof(unsigned long long),
      .program_id = {0},
  };
  const char *id = MOSS_COVERAGE_PROGRAM_ID;
  for (unsigned int index = 0; index + 1 < MOSS_COVERAGE_PROGRAM_ID_MAX && id[index]; ++index) {
    registration.program_id[index] = id[index];
  }
  (void)syscall3(511, MOSS_COVERAGE_OP_REGISTER, (long)&registration, sizeof(registration));
}

__attribute__((section(".preinit_array"),
               used)) static void (*const moss_coverage_preinit)(void) = moss_coverage_register;
