#include "validation/internal.h"

__attribute__((noinline)) static void vm_text(void) { asm volatile("" ::: "memory"); }

enum vm_fault_access {
  VM_FAULT_READ,
  VM_FAULT_WRITE,
  VM_FAULT_EXECUTE,
};

static int vm_fault(long address, enum vm_fault_access access) {
  long child = syscall0(SYS_FORK);
  if (child == 0) {
    if (access == VM_FAULT_EXECUTE) {
      ((void (*)(void))address)();
    } else if (access == VM_FAULT_WRITE) {
      // An architectural write attempt, including to text/const data: do not
      // rely on undefined C writes to const objects surviving optimization.
#if defined(__aarch64__)
      asm volatile("strb wzr, [%0]" : : "r"(address) : "memory");
#elif defined(__x86_64__)
      asm volatile("movb $0, (%0)" : : "r"(address) : "memory");
#else
      asm volatile("sb zero, 0(%0)" : : "r"(address) : "memory");
#endif
    } else {
      unsigned char value = *(volatile unsigned char *)address;
      asm volatile("" : : "r"(value) : "memory");
    }
    _exit(94); // A forbidden access must fault, not reach this exit.
  }
  return wait_exit(child, 245); // (-SIGSEGV = -11) & 0xff = 245 in Moss's exit-code encoding.
}

unsigned long vm_private_cow(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  volatile unsigned char *data = (volatile unsigned char *)area;
  unsigned long errors = 0;
  for (unsigned i = 0; i < 8192; ++i) {
    errors |= (unsigned long)(data[i] != 0) << 4;
  }
  data[0] = 11;
  data[4096] = 22;
  data[8191] = 33;
  long child = syscall0(SYS_FORK);
  if (child == 0) {
    if (data[0] != 11 || data[4096] != 22 || data[8191] != 33) {
      _exit(91);
    }
    data[0] = 44;
    long grandchild = syscall0(SYS_FORK);
    if (grandchild == 0) {
      data[4096] = 55;
      _exit(data[0] == 44 && data[4096] == 55 && data[8191] == 33 ? 31 : 92);
    }
    _exit(wait_exit(grandchild, 31) && data[0] == 44 && data[4096] == 22 && data[8191] == 33 ? 33 : 93);
  }
  errors |= !wait_exit(child, 33);
  errors |= (unsigned long)!(data[0] == 11 && data[4096] == 22 && data[8191] == 33) << 1;
  data[0] = 77; // Last-reference COW after descendants have exited.
  errors |= (unsigned long)(data[0] != 77) << 2;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 3;
  return errors;
}

unsigned long vm_readonly_cow(void) {
  unsigned long errors = *(const volatile unsigned char *)vm_rodata != 0x5a;
  vm_text(); // Both mappings are resident before fork.
  errors |= (unsigned long)!vm_fault((long)vm_rodata, VM_FAULT_WRITE) << 1;
  errors |= (unsigned long)!vm_fault((long)vm_text, VM_FAULT_WRITE) << 2;
  errors |= (unsigned long)(*(const volatile unsigned char *)vm_rodata != 0x5a) << 3;
  return errors;
}

unsigned long vm_access_permissions(void) {
  long none = syscall6(SYS_MMAP, 0, 4096, 0, 0x22, -1, 0);
  long nx = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  long rx = syscall6(SYS_MMAP, 0, 4096, 5, 0x22, -1, 0);
  long wx = syscall6(SYS_MMAP, 0, 4096, 7, 0x22, -1, 0);
  unsigned long errors = none <= 0 || nx <= 0;
  // Native EACCES is 13 (kernel-syscall_table.cppm). Even a read-only
  // executable anonymous mapping lacks code approval.
  errors |= (unsigned long)(rx != -13) << 6;
  errors |= (unsigned long)(wx != -13) << 7;
  if (none > 0) {
    errors |= (unsigned long)!vm_fault(none, VM_FAULT_READ) << 1;
    errors |= (unsigned long)!vm_fault(none, VM_FAULT_WRITE) << 2;
    errors |= (unsigned long)(syscall2(SYS_MUNMAP, none, 4096) != 0) << 3;
  }
  if (nx > 0) {
    // Keep this page absent: an instruction miss must check EXEC before any
    // demand allocation. Otherwise it can allocate then fault indefinitely.
    errors |= (unsigned long)!vm_fault(nx, VM_FAULT_EXECUTE) << 4;
    errors |= (unsigned long)(syscall2(SYS_MUNMAP, nx, 4096) != 0) << 5;
  }
  return errors;
}

unsigned long vm_kernel_isolation(long target) {
  // Private validation controls 50/51 prepare an actual kernel mapping and
  // check its contents after the child is reaped; keep validation.cpp in sync.
  enum { ISOLATION_PREPARE = 50, ISOLATION_VERIFY = 51 };
  // One native base page proves legal user loads/stores survive every attack.
  const long page_bytes = 4096;
  long area = syscall6(SYS_MMAP, 0, page_bytes, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  volatile unsigned *user = (volatile unsigned *)area;
  const unsigned canary = 0x5a39c681; // Nonzero mixed bytes expose clobbering and demand-zero replacement.
  *user = canary;
  const long parent = getpid();
  unsigned long errors = 0;
  // Exercise both the identity mapping and its high direct-map alias. Each
  // operation gets a separate child so a rejected read cannot hide an allowed write.
  for (long alias = 0; alias < 2; ++alias) {
    for (long write = 0; write < 2; ++write) {
      long address = control(ISOLATION_PREPARE, target, alias);
      if (!address) {
        errors |= 1;
        continue;
      }
      long child = fork();
      if (child == 0) {
        // Use aligned 32-bit accesses for the interrupt-controller registers
        // as well as RAM. Inline assembly keeps forbidden writes to text/RO
        // objects from being optimized away as undefined C behavior.
        if (write) {
#if defined(__aarch64__)
          asm volatile("str wzr, [%0]" : : "r"(address) : "memory");
#elif defined(__x86_64__)
          asm volatile("movl $0, (%0)" : : "r"(address) : "memory");
#else
          asm volatile("sw zero, 0(%0)" : : "r"(address) : "memory");
#endif
        } else {
          unsigned value = *(volatile unsigned *)address;
          asm volatile("" : : "r"(value) : "memory");
        }
        _exit(94); // A completed forbidden access must never share the fault exit marker.
      }
      // Moss currently encodes fatal page faults as (-SIGSEGV)&255, not the
      // POSIX wait signal encoding. The exact code rejects unrelated exits.
      errors |= (unsigned long)!wait_exit(child, 245) << 1;
      errors |= (unsigned long)!control(ISOLATION_VERIFY, target, alias) << 2;
      errors |= (unsigned long)(getpid() != parent || *user != canary) << 3;
      *user = canary ^ 1U;
      errors |= (unsigned long)(*user != (canary ^ 1U)) << 4;
      *user = canary;
    }
  }
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, page_bytes) != 0) << 5;
  return errors;
}

unsigned long vm_brk_lifecycle(void) {
  // Moss exposes 4 KiB user pages; three pages make one fully released page
  // remain after shrinking to a deliberately non-page-aligned break.
  enum {
    PAGE_BYTES = 4096,
    GROW_PAGES = 3,
    // 37 is an arbitrary non-power-of-two offset; any value in this page would
    // exercise the same partial-page ABI without resembling an alignment.
    PARTIAL_BYTES = 37,
  };
  // Page four leaves one unmapped guard page after the three-page heap. Growing
  // through page five must therefore collide with, rather than skip, the mmap.
  enum { COLLISION_PAGE = 4, COLLISION_GROW_PAGES = 5 };
  enum {
    MMAP_PROT_READ = 1U << 0,
    MMAP_PROT_WRITE = 1U << 1,
    MAP_PRIVATE_ANONYMOUS = 0x22, // Moss's supported MAP_PRIVATE | MAP_ANONYMOUS pair.
  };
  // Complementary nonzero bytes distinguish retained heap data, mmap data and
  // the zero-fill required after a fully released page is grown again.
  enum { STALE_PATTERN = 0x5a, COLLISION_PATTERN = 0xa5 };
  // One bit per observation keeps the serial failure mask independently decodable.
  enum {
    ERR_BASE = 1UL << 0,
    ERR_INITIAL_VISIBLE = 1UL << 1,
    ERR_GROW = 1UL << 2,
    ERR_SHRINK = 1UL << 3,
    ERR_RELEASED_VISIBLE = 1UL << 4,
    ERR_REGROW = 1UL << 5,
    ERR_STALE_CONTENT = 1UL << 6,
    ERR_COLLISION_MAP = 1UL << 7,
    ERR_COLLISION_GROW = 1UL << 8,
    ERR_PARTIAL_COMMIT = 1UL << 9,
    ERR_COLLISION_CONTENT = 1UL << 10,
    ERR_GAP_VISIBLE = 1UL << 11,
    ERR_COLLISION_UNMAP = 1UL << 12,
    ERR_RESET = 1UL << 13,
    ERR_RESET_VISIBLE = 1UL << 14,
    ERR_FINAL_REGROW = 1UL << 15,
    ERR_FINAL_CONTENT = 1UL << 16,
    ERR_FINAL_RESET = 1UL << 17,
  };

  const long base = syscall1(SYS_BRK, 0);
  if (base <= 0 || (base & (PAGE_BYTES - 1)) != 0) {
    return ERR_BASE;
  }

  unsigned long errors = (unsigned long)!vm_fault(base, VM_FAULT_READ) * ERR_INITIAL_VISIBLE;
  const long grown = base + (long)GROW_PAGES * PAGE_BYTES;
  if (syscall1(SYS_BRK, grown) != grown) {
    return errors | ERR_GROW;
  }
  volatile unsigned char *const first = (volatile unsigned char *)base;
  // The third page is wholly beyond the partially retained second page.
  volatile unsigned char *const released = (volatile unsigned char *)(base + 2L * PAGE_BYTES);
  *first = STALE_PATTERN;
  *released = STALE_PATTERN;

  const long partial = base + PAGE_BYTES + PARTIAL_BYTES;
  errors |= (unsigned long)(syscall1(SYS_BRK, partial) != partial) * ERR_SHRINK;
  errors |= (unsigned long)!vm_fault((long)released, VM_FAULT_READ) * ERR_RELEASED_VISIBLE;
  errors |= (unsigned long)(syscall1(SYS_BRK, grown) != grown) * ERR_REGROW;
  errors |= (unsigned long)(*released != 0) * ERR_STALE_CONTENT;

  const long collision_address = base + (long)COLLISION_PAGE * PAGE_BYTES;
  long collision =
      syscall6(SYS_MMAP, collision_address, PAGE_BYTES, MMAP_PROT_READ | MMAP_PROT_WRITE, MAP_PRIVATE_ANONYMOUS, -1, 0);
  errors |= (unsigned long)(collision != collision_address) * ERR_COLLISION_MAP;
  if (collision == collision_address) {
    volatile unsigned char *const collision_byte = (volatile unsigned char *)collision;
    *collision_byte = COLLISION_PATTERN;
    const long rejected = base + (long)COLLISION_GROW_PAGES * PAGE_BYTES;
    errors |= (unsigned long)(syscall1(SYS_BRK, rejected) != grown) * ERR_COLLISION_GROW;
    errors |= (unsigned long)(syscall1(SYS_BRK, 0) != grown) * ERR_PARTIAL_COMMIT;
    errors |= (unsigned long)(*collision_byte != COLLISION_PATTERN) * ERR_COLLISION_CONTENT;
    errors |= (unsigned long)!vm_fault(base + (long)GROW_PAGES * PAGE_BYTES, VM_FAULT_READ) * ERR_GAP_VISIBLE;
    errors |= (unsigned long)(syscall2(SYS_MUNMAP, collision, PAGE_BYTES) != 0) * ERR_COLLISION_UNMAP;
  } else if (collision > 0) {
    (void)syscall2(SYS_MUNMAP, collision, PAGE_BYTES);
  }

  errors |= (unsigned long)(syscall1(SYS_BRK, base) != base) * ERR_RESET;
  errors |= (unsigned long)!vm_fault(base, VM_FAULT_READ) * ERR_RESET_VISIBLE;
  const long one_page = base + PAGE_BYTES;
  errors |= (unsigned long)(syscall1(SYS_BRK, one_page) != one_page) * ERR_FINAL_REGROW;
  errors |= (unsigned long)(*first != 0) * ERR_FINAL_CONTENT;
  errors |= (unsigned long)(syscall1(SYS_BRK, base) != base) * ERR_FINAL_RESET;
  return errors;
}
