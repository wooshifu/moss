#pragma once

#include <stdint.h>

// Native image construction ABI. SYS_DOMAIN_SPAWN requires a factory handle;
// the caller supplies data-page bytes, approved code versions, and the startup stack. The kernel validates
// mappings and publishes the domain only after all resources are prepared.
enum {
  MOSS_DOMAIN_PAGE_BYTES = 4096,
  // Bound one construction request until per-domain memory quotas exist.
  MOSS_DOMAIN_MAX_IMAGE_PAGES = 4096,
  MOSS_DOMAIN_PAGE_READ = 1U << 0,
  MOSS_DOMAIN_PAGE_WRITE = 1U << 1,
  MOSS_DOMAIN_PAGE_EXEC = 1U << 2,
};

struct moss_domain_page {
  uint64_t address; // Page-aligned target virtual address.
  uint64_t source;  // Caller buffer for a non-executable page.
  uint64_t size;    // Bytes copied from source; zero fills the remainder.
  uint64_t flags;
  // Executable pages use only an approved immutable code version. source and
  // size must be zero; code_page_index selects a complete page in that version.
  uint64_t code;
  uint64_t code_page_index;
};

struct moss_domain_spawn {
  uint64_t entry;
  uint64_t stack_pointer;
  uint64_t stack_source; // Bytes to place at stack_pointer; ignored when size is zero.
  uint64_t stack_size;
  uint64_t arg0;
  uint64_t arg1;
  uint64_t arg2;
  uint64_t pages; // Array of moss_domain_page.
  uint64_t page_count;
  uint64_t capabilities; // Array of moss_fork_capability from syscall.h.
  uint64_t capability_count;
};

// The loader obtains the active ISA/MMU layout before building its stack.
// RISC-V may select Sv39 or Sv48 at boot, so compile-time addresses alone
// cannot describe every running system.
struct moss_domain_layout {
  uint64_t page_size;
  uint64_t user_begin;
  uint64_t user_end;
  uint64_t stack_top;
  uint64_t stack_size;
  uint64_t stack_max;
  uint64_t heap_start;
  uint64_t heap_reserve;
  uint64_t sigreturn_page;
  uint64_t mmap_base;
};
