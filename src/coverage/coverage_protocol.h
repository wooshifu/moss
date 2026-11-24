#pragma once

/*
 * Private coverage protocol shared by the kernel wrapper and the userspace
 * preinit runtime. These values are not production syscall ABI: the wrapper is
 * linked only into MOSS_COVERAGE images and normal builds leave syscall 511
 * returning ENOSYS.
 */

#define MOSS_COVERAGE_REGISTER_MAGIC 0x4d435647ULL
#define MOSS_COVERAGE_RECORD_MAGIC 0x4d435652U
#define MOSS_COVERAGE_PROTOCOL_VERSION 1U
/* High private opcodes avoid the validation protocol's small control numbers. */
#define MOSS_COVERAGE_OP_REGISTER 0x4d435650L
#define MOSS_COVERAGE_OP_FINALIZE 0x4d435651L
#define MOSS_COVERAGE_PROGRAM_ID_MAX 32
#define MOSS_COVERAGE_RAW_HEADER_SIZE 152
#define MOSS_COVERAGE_RAW_HEADER_QWORDS 19
#define MOSS_COVERAGE_RAW_MAGIC 0xff6c70726f667281ULL
#define MOSS_COVERAGE_RAW_VERSION 11ULL
#define MOSS_COVERAGE_RAW_VALUE_KIND_LAST 2ULL

struct moss_coverage_registration {
  unsigned long long magic;
  unsigned long long version;
  unsigned long long counters;
  unsigned long long counter_count;
  char program_id[MOSS_COVERAGE_PROGRAM_ID_MAX];
};

struct moss_coverage_record_header {
  unsigned int magic;
  unsigned int version;
  unsigned int counter_count;
  unsigned int program_id_length;
  unsigned long long raw_size;
  char program_id[MOSS_COVERAGE_PROGRAM_ID_MAX];
};
