#include "validation/internal.h"
#include <limits.h>

// The freestanding validation image has no libc; Clang lowers zeroing the
// fixed-size IPC message aggregate to this routine.
void *memset(void *destination, int value, unsigned long count) {
  volatile unsigned char *bytes = (volatile unsigned char *)destination;
  for (unsigned long i = 0; i < count; ++i)
    bytes[i] = (unsigned char)value;
  return destination;
}

enum {
  IPC_EPERM = 1,
  IPC_ESRCH = 3,
  IPC_EINTR = 4,
  IPC_EBADF = 9,
  IPC_ECHILD = 10,
  IPC_EAGAIN = 11,
  IPC_EACCES = 13,
  IPC_EFAULT = 14,
  IPC_EINVAL = 22,
  IPC_EPIPE = 32,
  IPC_ETIMEDOUT = 110
};
static volatile int ipc_signal_seen;
// Bounds broken test services so a regression fails instead of hanging validation.
static const unsigned long ipc_call_timeout_ns = 5000000000UL;

static void ipc_signal_handler(int signo) { ipc_signal_seen = signo; }

static long deadline_after(unsigned long offset_ns) {
  unsigned long now = 0;
  return clock_gettime_ns(&now) == 0 ? (long)(now + offset_ns) : -1;
}

static long ipc_call(unsigned long endpoint, const struct moss_ipc_message *request, struct moss_ipc_message *response,
                     long deadline) {
  return syscall6(SYS_IPC_CALL, (long)endpoint, (long)request, (long)response, deadline, 0, 0);
}

static long ipc_receive(unsigned long endpoint, struct moss_ipc_message *request, unsigned long *reply) {
  return syscall3(SYS_IPC_RECEIVE, (long)endpoint, (long)request, (long)reply);
}

static long ipc_reply(unsigned long reply, const struct moss_ipc_message *response) {
  return syscall2(SYS_IPC_REPLY, (long)reply, (long)response);
}

static int ipc_set_nice(int target) {
  // The native getpriority result uses Linux's 20 - nice encoding.
  long priority = syscall2(SYS_GETPRIORITY, 0, 0);
  return priority >= 1 && priority <= 40 && syscall1(SYS_NICE, target + priority - 20) == target;
}

static int domain_exited(unsigned long domain, int code, unsigned int signal) {
  struct moss_domain_exit status = {0};
  return syscall1(SYS_DOMAIN_WAIT, (long)domain) == 0 &&
         syscall2(SYS_DOMAIN_STATUS, (long)domain, (long)&status) == 0 && status.code == code &&
         status.signal == signal;
}

// This position-independent entry reads its exit code from the new domain's
// startup stack. It has no relocations or dependency on the caller's libc.
__attribute__((naked, noinline, used, aligned(16))) static void spawned_stack_exit(void) {
#if defined(__x86_64__)
  __asm__ volatile("mov (%rsi), %edi\n\tmov $1, %eax\n\tsyscall\n\tud2");
#elif defined(__riscv)
  __asm__ volatile("lw a0, 0(a1)\n\tli a7, 1\n\tecall\n\tebreak");
#else
  __asm__ volatile("ldr w0, [x1]\n\tmov x8, #1\n\tsvc #0\n\tbrk #0");
#endif
}

// A previously admitted domain must still execute after its approval is
// revoked. The stack supplies a native nanosecond duration and exit code.
// The immediate syscall IDs are guarded against moss/syscall_numbers.def.
_Static_assert(SYS_NANOSLEEP == 86 && SYS_EXIT == 1, "native sleep/exit syscall IDs changed");
__attribute__((naked, noinline, used, aligned(16))) static void spawned_sleep_exit(void) {
#if defined(__x86_64__)
  __asm__ volatile("mov %rsi, %rbx\n\tmov %rbx, %rdi\n\txor %esi, %esi\n\tmov $86, %eax\n\tsyscall\n\t"
                   "mov 8(%rbx), %edi\n\tmov $1, %eax\n\tsyscall\n\tud2");
#elif defined(__riscv)
  __asm__ volatile("mv s0, a1\n\tmv a0, s0\n\tli a1, 0\n\tli a7, 86\n\tecall\n\t"
                   "lw a0, 8(s0)\n\tli a7, 1\n\tecall\n\tebreak");
#else
  __asm__ volatile("mov x19, x1\n\tmov x0, x19\n\tmov x1, xzr\n\tmov x8, #86\n\tsvc #0\n\t"
                   "ldr w0, [x19, #8]\n\tmov x8, #1\n\tsvc #0\n\tbrk #0");
#endif
}

static int ipc_expect_spawn_error(long factory, struct moss_domain_spawn *image, long expected) {
  long result = syscall2(SYS_DOMAIN_SPAWN, factory, (long)image);
  if (result > 0) {
    (void)syscall1(SYS_DOMAIN_TERMINATE, result);
    (void)syscall1(SYS_DOMAIN_WAIT, result);
    (void)syscall1(SYS_CAP_CLOSE, result);
  }
  return result == expected;
}

static long ipc_return_code_cap(long source) {
  struct moss_ipc_endpoints endpoint = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&endpoint) != 0)
    return -1;
  if (syscall2(SYS_CAP_SET_INHERIT, (long)endpoint.receive, 1) != 0) {
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.receive);
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.send);
    return -1;
  }
  long child = fork();
  if (child == 0) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    const unsigned long sent_rights = MOSS_CAP_CODE_EXEC | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE;
    if (ipc_receive(endpoint.receive, &request, &reply) != 1 || request.payload[0] != 'c' ||
        request.rights != sent_rights || request.capability == 0)
      _exit(91);
    const struct moss_ipc_message response = {
        .size = 1, .capability = request.capability, .rights = MOSS_CAP_CODE_EXEC, .payload = {'c'}};
    _exit(ipc_reply(reply, &response) == 0 ? 37 : 92);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.receive);
  long returned = -1;
  if (child > 0) {
    const struct moss_ipc_message request = {.size = 1,
                                             .capability = (unsigned long)source,
                                             .rights = MOSS_CAP_CODE_EXEC | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE,
                                             .payload = {'c'}};
    struct moss_ipc_message response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    long result = deadline > 0 ? ipc_call(endpoint.send, &request, &response, deadline) : -1;
    if (result != 1 || response.payload[0] != 'c' || response.rights != MOSS_CAP_CODE_EXEC || response.capability == 0)
      (void)kill(child, SIGKILL);
    int child_ok = wait_exit(child, 37);
    if (result == 1 && response.payload[0] == 'c' && response.rights == MOSS_CAP_CODE_EXEC &&
        response.capability != 0 && child_ok)
      returned = (long)response.capability;
    else if (response.capability != 0)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.send);
  return returned;
}

// Each fork is a separate issuer incarnation. Only the explicitly inherited
// approval handle lets it issue; the caller retains revocation authority.
static long ipc_approve_from_child(long approver, long version, int delegated) {
  struct moss_ipc_endpoints endpoint = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&endpoint) != 0)
    return -1;
  if (syscall2(SYS_CAP_SET_INHERIT, (long)endpoint.receive, 1) != 0 ||
      (delegated && syscall2(SYS_CAP_SET_INHERIT, approver, 1) != 0)) {
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.receive);
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.send);
    return -1;
  }
  long child = fork();
  if (child == 0) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (syscall0(SYS_CODE_AUTHORITY) != -IPC_EACCES || ipc_receive(endpoint.receive, &request, &reply) != 1 ||
        request.capability == 0 || request.rights != MOSS_CAP_MAP_READ)
      _exit(91);
    long approved = syscall2(SYS_CODE_APPROVE, approver, (long)request.capability);
    if (!delegated) {
      const struct moss_ipc_message denied = {.size = 1, .payload = {'n'}};
      _exit(approved == -IPC_EBADF && ipc_reply(reply, &denied) == 0 ? 37 : 92);
    }
    if (approved <= 0)
      _exit(93);
    const struct moss_ipc_message issued = {.size = 1,
                                            .capability = (unsigned long)approved,
                                            .rights = MOSS_CAP_CODE_EXEC | MOSS_CAP_CODE_IDENTIFY | MOSS_CAP_TRANSFER |
                                                      MOSS_CAP_DUPLICATE,
                                            .payload = {'a'}};
    _exit(ipc_reply(reply, &issued) == 0 ? 37 : 94);
  }
  long unmarked = delegated ? syscall2(SYS_CAP_SET_INHERIT, approver, 0) : 0;
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.receive);
  long result = -1;
  if (unmarked != 0 && child > 0)
    (void)kill(child, SIGKILL);
  if (child > 0 && unmarked == 0) {
    const struct moss_ipc_message request = {
        .size = 1, .capability = (unsigned long)version, .rights = MOSS_CAP_MAP_READ, .payload = {'v'}};
    struct moss_ipc_message response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    long received = deadline > 0 ? ipc_call(endpoint.send, &request, &response, deadline) : -1;
    int valid =
        received == 1 && response.payload[0] == (delegated ? 'a' : 'n') && (response.capability != 0) == delegated &&
        response.rights ==
            (delegated ? (MOSS_CAP_CODE_EXEC | MOSS_CAP_CODE_IDENTIFY | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE) : 0);
    if (!valid)
      (void)kill(child, SIGKILL);
    int child_ok = wait_exit(child, 37);
    if (valid && child_ok)
      result = delegated ? (long)response.capability : 0;
    else if (response.capability != 0)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  if (child > 0 && unmarked != 0)
    (void)wait_exit(child, 37);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.send);
  return result;
}

unsigned long ipc_domain_spawn(void) {
  // Exceed capability::Table's current 64 slots: one approved version must
  // supply a text range that cannot fit as one handle per executable page.
  enum { CODE_RANGE_TEST_PAGES = 65 };
  static unsigned char mutable_code[CODE_RANGE_TEST_PAGES * MOSS_DOMAIN_PAGE_BYTES]
      __attribute__((aligned(MOSS_DOMAIN_PAGE_BYTES)));
  static unsigned char readback[MOSS_DOMAIN_PAGE_BYTES];
  struct moss_domain_layout layout = {0};
  if (syscall1(SYS_DOMAIN_LAYOUT, (long)&layout) != 0 || layout.page_size != MOSS_DOMAIN_PAGE_BYTES ||
      layout.stack_size < 16 || layout.stack_top <= layout.stack_size)
    return 1;

  const unsigned long entry = (unsigned long)spawned_stack_exit;
  const unsigned long code_page = entry & ~(MOSS_DOMAIN_PAGE_BYTES - 1UL);
  const unsigned long stack_pointer = layout.stack_top - 16;
  const unsigned char initial_stack[16] = {37};
  // The executable version must remain unchanged after its source is edited.
  volatile unsigned char *source = mutable_code;
  const volatile unsigned char *original = (const volatile unsigned char *)code_page;
  const unsigned char snapshot_marker = 0xa5;
  const unsigned long last_page = CODE_RANGE_TEST_PAGES - 1;
  for (unsigned long i = 0; i < MOSS_DOMAIN_PAGE_BYTES; ++i) {
    source[i] = snapshot_marker;
    source[last_page * MOSS_DOMAIN_PAGE_BYTES + i] = original[i];
  }
  struct moss_domain_page page = {.address = code_page,
                                  .source = code_page,
                                  .size = MOSS_DOMAIN_PAGE_BYTES,
                                  .flags = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_EXEC};
  struct moss_domain_spawn image = {.entry = entry,
                                    .stack_pointer = stack_pointer,
                                    .stack_source = (unsigned long)initial_stack,
                                    .stack_size = sizeof(initial_stack),
                                    .arg0 = 99,
                                    .arg1 = stack_pointer,
                                    .pages = (unsigned long)&page,
                                    .page_count = 1};
  unsigned long errors = 0;
  long factory = syscall0(SYS_DOMAIN_FACTORY);
  long authority = syscall0(SYS_CODE_AUTHORITY);
  long single = syscall1(SYS_CODE_SNAPSHOT, (long)(mutable_code + last_page * MOSS_DOMAIN_PAGE_BYTES));
  errors |= (unsigned long)(single <= 0 || syscall2(SYS_CODE_READ, single, (long)readback) != 0 ||
                            readback[entry - code_page] != original[entry - code_page])
            << 18;
  errors |= (unsigned long)(single <= 0 || syscall1(SYS_CODE_PAGE_COUNT, single) != 1) << 25;
  if (single > 0)
    (void)syscall1(SYS_CAP_CLOSE, single);
  long version = syscall2(SYS_CODE_SNAPSHOT_RANGE, (long)mutable_code, CODE_RANGE_TEST_PAGES);
  if (factory <= 0 || authority <= 0 || version <= 0)
    return errors | 1;
  errors |= (unsigned long)(syscall1(SYS_CODE_PAGE_COUNT, version) != CODE_RANGE_TEST_PAGES) << 26;
  errors |= (unsigned long)(syscall2(SYS_CODE_SNAPSHOT_RANGE, (long)mutable_code, MOSS_DOMAIN_MAX_IMAGE_PAGES + 1) !=
                            -IPC_EINVAL)
            << 19;
  errors |= (unsigned long)(syscall2(SYS_CODE_SNAPSHOT_RANGE, (long)(layout.stack_top - MOSS_DOMAIN_PAGE_BYTES), 2) !=
                            -IPC_EFAULT)
            << 20;
  for (unsigned long i = 0; i < sizeof(mutable_code); ++i)
    source[i] = 0;
  errors |= (unsigned long)(syscall2(SYS_CODE_READ, version, (long)readback) != 0 || readback[0] != snapshot_marker)
            << 21;
  errors |=
      (unsigned long)(syscall6(SYS_CODE_READ_RANGE, version, 0, (long)mutable_code, CODE_RANGE_TEST_PAGES, 0, 0) != 0 ||
                      source[0] != snapshot_marker ||
                      source[last_page * MOSS_DOMAIN_PAGE_BYTES + entry - code_page] != original[entry - code_page])
      << 24;
  errors |= (unsigned long)(syscall6(SYS_CODE_READ_RANGE, version, last_page, (long)readback, 1, 0, 0) != 0) << 10;
  for (unsigned long i = 0; i < MOSS_DOMAIN_PAGE_BYTES; ++i) {
    if (readback[i] != original[i]) {
      errors |= 1UL << 11;
      break;
    }
  }
  errors |= (unsigned long)(syscall6(SYS_CODE_READ_RANGE, version, CODE_RANGE_TEST_PAGES, (long)readback, 1, 0, 0) !=
                            -IPC_EINVAL)
            << 22;
  errors |= (unsigned long)(syscall2(SYS_CODE_APPROVE, 0, version) != -IPC_EBADF) << 12;
  long reduced_authority = syscall2(SYS_CAP_DUPLICATE, authority, MOSS_CAP_TRANSFER);
  errors |=
      (unsigned long)(reduced_authority <= 0 || syscall2(SYS_CODE_APPROVE, reduced_authority, version) != -IPC_EACCES)
      << 13;
  if (reduced_authority > 0)
    (void)syscall1(SYS_CAP_CLOSE, reduced_authority);
  page.source = 0;
  page.size = 0;
  page.code = (unsigned long)version;
  page.code_page_index = last_page;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image) != -IPC_EACCES) << 14;
  long approved = syscall2(SYS_CODE_APPROVE, authority, version);
  if (approved <= 0)
    return errors | (1UL << 15);
  errors |= (unsigned long)(syscall2(SYS_CODE_READ, approved, (long)readback) != -IPC_EACCES) << 16;
  errors |= (unsigned long)(syscall1(SYS_CODE_PAGE_COUNT, approved) != -IPC_EACCES) << 27;
  page.code = (unsigned long)approved;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SPAWN, 0, (long)&image) != -IPC_EBADF);
  long limited = syscall2(SYS_CAP_DUPLICATE, factory, MOSS_CAP_TRANSFER);
  errors |= (unsigned long)(limited <= 0 || syscall2(SYS_DOMAIN_SPAWN, limited, (long)&image) != -IPC_EACCES) << 1;
  if (limited > 0)
    (void)syscall1(SYS_CAP_CLOSE, limited);
  struct moss_ipc_endpoints delegation = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&delegation) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)delegation.receive, 1) != 0)
    return errors | (1UL << 2);
  long child = fork();
  if (child == 0) {
    if (syscall0(SYS_DOMAIN_FACTORY) != -IPC_EACCES || syscall0(SYS_CODE_AUTHORITY) != -IPC_EACCES)
      _exit(96);
    struct moss_ipc_message incoming = {0};
    unsigned long reply = 0;
    struct moss_domain_spawn invalid_image = {0};
    if (ipc_receive(delegation.receive, &incoming, &reply) != 1 || incoming.rights != MOSS_CAP_DOMAIN_SPAWN ||
        incoming.capability == 0 ||
        syscall2(SYS_DOMAIN_SPAWN, (long)incoming.capability, (long)&invalid_image) != -IPC_EINVAL)
      _exit(97);
    const struct moss_ipc_message accepted = {.size = 1, .payload = {37}};
    _exit(ipc_reply(reply, &accepted) == 0 ? 37 : 98);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)delegation.receive);
  if (child > 0) {
    const struct moss_ipc_message delegated = {
        .size = 1, .capability = (unsigned long)factory, .rights = MOSS_CAP_DOMAIN_SPAWN, .payload = {37}};
    struct moss_ipc_message accepted = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    long sent = deadline > 0 ? ipc_call(delegation.send, &delegated, &accepted, deadline) : -1;
    if (sent != 1 || accepted.payload[0] != 37)
      (void)kill(child, SIGKILL);
    errors |= (unsigned long)(sent != 1 || accepted.payload[0] != 37 || !wait_exit(child, 37)) << 2;
  } else {
    errors |= 1UL << 2;
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)delegation.send);
  page.flags |= MOSS_DOMAIN_PAGE_WRITE;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image) != -IPC_EINVAL) << 3;
  page.flags &= ~MOSS_DOMAIN_PAGE_WRITE;
  image.entry = stack_pointer;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image) != -IPC_EINVAL) << 4;
  image.entry = entry;
  page.code_page_index = CODE_RANGE_TEST_PAGES;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image) != -IPC_EINVAL) << 23;
  page.code_page_index = last_page;
  page.code = 0;
  page.source = code_page;
  page.size = MOSS_DOMAIN_PAGE_BYTES;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image) != -IPC_EINVAL) << 17;
  page.flags = MOSS_DOMAIN_PAGE_READ;
  page.code_page_index = 0;
  page.source = 1;
  page.size = 1;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image) != -IPC_EFAULT) << 5;
  page.flags = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_EXEC;
  page.source = 0;
  page.size = 0;
  page.code = (unsigned long)approved;
  page.code_page_index = last_page;

  long domain = syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image);
  (void)syscall1(SYS_CAP_CLOSE, factory);
  (void)syscall1(SYS_CAP_CLOSE, authority);
  (void)syscall1(SYS_CAP_CLOSE, version);
  (void)syscall1(SYS_CAP_CLOSE, approved);
  if (domain <= 0)
    return errors | (1UL << 6);
  long diagnostic_id = syscall1(SYS_DOMAIN_ID, domain);
  errors |= (unsigned long)(diagnostic_id <= 1 || syscall3(SYS_WAITPID, diagnostic_id, 0, 1) != -IPC_ECHILD) << 7;
  errors |= (unsigned long)!domain_exited((unsigned long)domain, 37, 0) << 8;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, domain) != 0) << 9;
  return errors;
}

unsigned long ipc_code_revocation(void) {
  static unsigned char code_copy[MOSS_DOMAIN_PAGE_BYTES] __attribute__((aligned(MOSS_DOMAIN_PAGE_BYTES)));
  struct moss_domain_layout layout = {0};
  if (syscall1(SYS_DOMAIN_LAYOUT, (long)&layout) != 0 || layout.page_size != MOSS_DOMAIN_PAGE_BYTES ||
      layout.stack_size < 16 || layout.stack_top <= layout.stack_size)
    return 1;

  const unsigned long entry = (unsigned long)spawned_sleep_exit;
  const unsigned long code_page = entry & ~(MOSS_DOMAIN_PAGE_BYTES - 1UL);
  const unsigned long stack_pointer = layout.stack_top - 16;
  // Keep the first domain alive while its already admitted code is revoked.
  const unsigned long live_domain_delay_ns = 300000000UL;
  const unsigned long initial_stack[2] = {live_domain_delay_ns, 37};
  const volatile unsigned char *original = (const volatile unsigned char *)code_page;
  for (unsigned long i = 0; i < MOSS_DOMAIN_PAGE_BYTES; ++i)
    code_copy[i] = original[i];
  struct moss_domain_page page = {.address = code_page, .flags = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_EXEC};
  struct moss_domain_spawn image = {.entry = entry,
                                    .stack_pointer = stack_pointer,
                                    .stack_source = (unsigned long)initial_stack,
                                    .stack_size = sizeof(initial_stack),
                                    .arg1 = stack_pointer,
                                    .pages = (unsigned long)&page,
                                    .page_count = 1};
  unsigned long errors = 0;
  long factory = syscall0(SYS_DOMAIN_FACTORY);
  long authority = syscall0(SYS_CODE_AUTHORITY);
  long other_scope = syscall0(SYS_CODE_AUTHORITY);
  long version = syscall1(SYS_CODE_SNAPSHOT, (long)code_copy);
  long approved = 0, identify = 0, stale = 0, transferred = 0, revoker = 0, approver = 0, second = 0;
  long domain = 0, second_domain = 0;
  if (factory <= 0 || authority <= 0 || other_scope <= 0 || version <= 0) {
    errors |= 1;
    goto cleanup;
  }
  for (unsigned long i = 0; i < MOSS_DOMAIN_PAGE_BYTES; ++i)
    code_copy[i] = 0;
  approved = syscall2(SYS_CODE_APPROVE, authority, version);
  if (approved <= 0) {
    errors |= 1UL << 1;
    goto cleanup;
  }
  identify = syscall2(SYS_CAP_DUPLICATE, approved, MOSS_CAP_CODE_IDENTIFY);
  stale = syscall2(SYS_CAP_DUPLICATE, approved, MOSS_CAP_CODE_EXEC | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE);
  revoker = syscall2(SYS_CAP_DUPLICATE, authority, MOSS_CAP_CODE_REVOKE);
  approver = syscall2(SYS_CAP_DUPLICATE, authority, MOSS_CAP_CODE_APPROVE);
  if (identify <= 0 || stale <= 0 || revoker <= 0 || approver <= 0) {
    errors |= (unsigned long)(identify <= 0) << 2 | (unsigned long)(stale <= 0) << 3 |
              (unsigned long)(revoker <= 0) << 4 | (unsigned long)(approver <= 0) << 5;
    goto cleanup;
  }
  page.code = (unsigned long)approved;
  domain = syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image);
  if (domain <= 0) {
    errors |= 1UL << 3;
    goto cleanup;
  }
  struct moss_domain_exit status = {0};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, domain, (long)&status) != -IPC_EAGAIN) << 4;
  page.code = (unsigned long)identify;
  errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 5;
  page.code = (unsigned long)approved;
  errors |= (unsigned long)(syscall2(SYS_CODE_REVOKE, approver, identify) != -IPC_EACCES) << 6;
  errors |= (unsigned long)(syscall2(SYS_CODE_APPROVE, revoker, version) != -IPC_EACCES) << 7;
  errors |= (unsigned long)(syscall2(SYS_CODE_REVOKE, other_scope, identify) != -IPC_EACCES) << 8;
  errors |= (unsigned long)(syscall2(SYS_CODE_REVOKE, revoker, stale) != -IPC_EACCES) << 9;
  errors |= (unsigned long)(syscall2(SYS_CODE_REVOKE, revoker, identify) != 0) << 10;
  errors |= (unsigned long)(syscall2(SYS_CODE_REVOKE, revoker, identify) != 0) << 11;
  errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 12;
  page.code = (unsigned long)stale;
  errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 13;
  transferred = ipc_return_code_cap(stale);
  errors |= (unsigned long)(transferred <= 0) << 21;
  if (transferred > 0) {
    page.code = (unsigned long)transferred;
    errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 22;
  }
  page.code = (unsigned long)stale;
  second = syscall2(SYS_CODE_APPROVE, approver, version);
  if (second <= 0) {
    errors |= 1UL << 14;
    goto cleanup;
  }
  errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 15;
  page.code = (unsigned long)second;
  second_domain = syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image);
  errors |= (unsigned long)(second_domain <= 0) << 16;
  errors |= (unsigned long)(syscall2(SYS_CODE_REVOKE, revoker, second) != 0) << 17;
  errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 18;

cleanup:
  if (domain > 0)
    errors |= (unsigned long)!domain_exited((unsigned long)domain, 37, 0) << 19;
  if (second_domain > 0)
    errors |= (unsigned long)!domain_exited((unsigned long)second_domain, 37, 0) << 20;
  const long handles[] = {domain,   second_domain, second,  approver,  revoker,     transferred, stale,
                          identify, approved,      version, authority, other_scope, factory};
  for (unsigned long i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
    if (handles[i] > 0)
      (void)syscall1(SYS_CAP_CLOSE, handles[i]);
  }
  return errors;
}

unsigned long ipc_code_service_survival(void) {
  static unsigned char code_copy[MOSS_DOMAIN_PAGE_BYTES] __attribute__((aligned(MOSS_DOMAIN_PAGE_BYTES)));
  struct moss_domain_layout layout = {0};
  if (syscall1(SYS_DOMAIN_LAYOUT, (long)&layout) != 0 || layout.page_size != MOSS_DOMAIN_PAGE_BYTES ||
      layout.stack_size < 16 || layout.stack_top <= layout.stack_size)
    return 1;

  const unsigned long entry = (unsigned long)spawned_stack_exit;
  const unsigned long code_page = entry & ~(MOSS_DOMAIN_PAGE_BYTES - 1UL);
  const unsigned long stack_pointer = layout.stack_top - 16;
  const unsigned char initial_stack[16] = {37};
  const volatile unsigned char *original = (const volatile unsigned char *)code_page;
  for (unsigned long i = 0; i < MOSS_DOMAIN_PAGE_BYTES; ++i)
    code_copy[i] = original[i];
  struct moss_domain_page page = {.address = code_page, .flags = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_EXEC};
  struct moss_domain_spawn image = {.entry = entry,
                                    .stack_pointer = stack_pointer,
                                    .stack_source = (unsigned long)initial_stack,
                                    .stack_size = sizeof(initial_stack),
                                    .arg1 = stack_pointer,
                                    .pages = (unsigned long)&page,
                                    .page_count = 1};
  unsigned long errors = 0;
  long factory = syscall0(SYS_DOMAIN_FACTORY);
  long authority = syscall0(SYS_CODE_AUTHORITY);
  long version = syscall1(SYS_CODE_SNAPSHOT, (long)code_copy);
  long approver = 0, revoker = 0, first = 0, replacement = 0, domain = 0;
  if (factory <= 0 || authority <= 0 || version <= 0) {
    errors |= 1;
    goto cleanup;
  }
  approver = syscall2(SYS_CAP_DUPLICATE, authority, MOSS_CAP_CODE_APPROVE | MOSS_CAP_DUPLICATE);
  revoker = syscall2(SYS_CAP_DUPLICATE, authority, MOSS_CAP_CODE_REVOKE);
  if (approver <= 0 || revoker <= 0) {
    errors |= 1UL << 1;
    goto cleanup;
  }
  if (syscall1(SYS_CAP_CLOSE, authority) != 0) {
    errors |= 1UL << 15;
    goto cleanup;
  }
  authority = 0;
  first = ipc_approve_from_child(approver, version, 1);
  if (first <= 0) {
    errors |= 1UL << 2;
    goto cleanup;
  }
  page.code = (unsigned long)first;
  domain = syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image);
  errors |= (unsigned long)(domain <= 0) << 3;
  if (domain > 0) {
    errors |= (unsigned long)!domain_exited((unsigned long)domain, 37, 0) << 4;
    (void)syscall1(SYS_CAP_CLOSE, domain);
    domain = 0;
  }
  errors |= (unsigned long)(ipc_approve_from_child(approver, version, 0) != 0) << 5;
  errors |= (unsigned long)(syscall2(SYS_CODE_APPROVE, revoker, version) != -IPC_EACCES) << 6;
  errors |= (unsigned long)(syscall2(SYS_CODE_REVOKE, revoker, first) != 0) << 7;
  errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 8;
  replacement = ipc_approve_from_child(approver, version, 1);
  if (replacement <= 0) {
    errors |= 1UL << 9;
    goto cleanup;
  }
  errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 10;
  page.code = (unsigned long)replacement;
  domain = syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image);
  errors |= (unsigned long)(domain <= 0) << 11;
  if (domain > 0) {
    errors |= (unsigned long)!domain_exited((unsigned long)domain, 37, 0) << 12;
    (void)syscall1(SYS_CAP_CLOSE, domain);
    domain = 0;
  }
  errors |= (unsigned long)(syscall2(SYS_CODE_REVOKE, revoker, replacement) != 0) << 13;
  errors |= (unsigned long)!ipc_expect_spawn_error(factory, &image, -IPC_EACCES) << 14;

cleanup:
  {
    const long handles[] = {domain, replacement, first, revoker, approver, version, authority, factory};
    for (unsigned long i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
      if (handles[i] > 0)
        (void)syscall1(SYS_CAP_CLOSE, handles[i]);
    }
  }
  return errors;
}

unsigned long ipc_roundtrip(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  long limited = syscall2(SYS_CAP_DUPLICATE, (long)pair.send, MOSS_CAP_SEND);
  unsigned long errors = limited <= 0;
  if (limited > 0) {
    errors |= (unsigned long)(syscall2(SYS_CAP_DUPLICATE, limited, MOSS_CAP_SEND) != -IPC_EACCES) << 1;
    errors |= (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, limited, 1) != -IPC_EACCES) << 2;
    struct moss_ipc_message unused = {0};
    unsigned long reply = 0;
    errors |= (unsigned long)(ipc_receive((unsigned long)limited, &unused, &reply) != -IPC_EINVAL) << 3;
  }
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0)
    errors |= 1UL << 4;
  long child = fork();
  if (child == 0) {
    unsigned cpu1 = 2; // Affinity mask bit 1 selects CPU1, away from the parent on CPU0.
    unsigned long migration_wait = 1000000UL;
    if (syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(cpu1), (long)&cpu1) != 0 || nanosleep_ns(&migration_wait) != 0 ||
        current_cpu() != 1)
      _exit(96);
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (syscall1(SYS_CAP_CLOSE, (long)pair.send) != -IPC_EBADF)
      _exit(91);
    if (ipc_receive(pair.receive, &request, &reply) != 2 || request.size != 2 || request.payload[0] != 'h' ||
        request.payload[1] != 'i' || request.capability != 0)
      _exit(92);
    if (syscall2(SYS_CAP_DUPLICATE, (long)reply, MOSS_CAP_SEND) != -IPC_EACCES)
      _exit(93);
    const struct moss_ipc_message response = {.size = 2, .payload = {'o', 'k'}};
    if (ipc_reply(reply, &response) != 0 || syscall1(SYS_CAP_CLOSE, (long)reply) != -IPC_EBADF)
      _exit(94);
    _exit(syscall1(SYS_CAP_CLOSE, (long)pair.receive) == 0 ? 37 : 95);
  }
  if (child < 0) {
    syscall1(SYS_CAP_CLOSE, (long)pair.receive);
    syscall1(SYS_CAP_CLOSE, (long)pair.send);
    return errors | (1UL << 5);
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 6;
  const struct moss_ipc_message request = {.size = 2, .payload = {'h', 'i'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
  errors |= (unsigned long)(result != 2 || response.size != 2 || response.payload[0] != 'o' ||
                            response.payload[1] != 'k' || response.capability != 0)
            << 7;
  errors |= (unsigned long)!wait_exit(child, 37) << 8;
  errors |= (unsigned long)(ipc_call(pair.send, &request, &response, 0) != -IPC_EPIPE) << 9;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 10;
  if (limited > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, limited) != 0) << 11;
  return errors;
}

unsigned long ipc_badged_sender(void) {
  enum { kTestBadge = 7 }; // Nonzero object identity; unbadged endpoints deliver zero.
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  long minted = syscall2(SYS_IPC_MINT_BADGE, (long)pair.send, kTestBadge);
  long limited = minted > 0 ? syscall2(SYS_CAP_DUPLICATE, minted, MOSS_CAP_SEND) : -1;
  long attenuated = syscall2(SYS_CAP_DUPLICATE, (long)pair.send, MOSS_CAP_SEND | MOSS_CAP_MINT);
  unsigned long errors = (unsigned long)(minted <= 0 || limited <= 0 || attenuated <= 0);
  if (errors || syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) {
    errors |= 2;
    goto done;
  }

  long child = fork();
  if (child == 0) {
    for (unsigned int i = 0; i < 2; ++i) {
      struct moss_ipc_message request = {0};
      unsigned long reply = 0;
      unsigned long expected_badge = i == 0 ? kTestBadge : 0;
      if (ipc_receive(pair.receive, &request, &reply) != 1 || request.badge != expected_badge ||
          request.payload[0] != 'b')
        _exit(91);
      const struct moss_ipc_message response = {.size = 1, .payload = {'b'}};
      if (ipc_reply(reply, &response) != 0)
        _exit(92);
    }
    _exit(37);
  }
  if (child < 0) {
    errors |= 4;
    goto done;
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
  pair.receive = 0;
  errors |= (unsigned long)(syscall2(SYS_IPC_MINT_BADGE, minted, kTestBadge) != -IPC_EACCES) << 3;
  errors |= (unsigned long)(syscall2(SYS_IPC_MINT_BADGE, limited, kTestBadge) != -IPC_EACCES) << 4;
  errors |= (unsigned long)(syscall2(SYS_IPC_MINT_BADGE, attenuated, kTestBadge) != -IPC_EACCES) << 8;
  struct moss_ipc_message request = {.size = 1, .badge = kTestBadge, .payload = {'b'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  errors |=
      (unsigned long)(deadline <= 0 || ipc_call((unsigned long)limited, &request, &response, deadline) != -IPC_EINVAL)
      << 5;
  request.badge = 0;
  deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call((unsigned long)limited, &request, &response, deadline) : -1;
  errors |= (unsigned long)(result != 1 || response.payload[0] != 'b' || response.badge != 0) << 6;
  if (result == 1) {
    deadline = deadline_after(ipc_call_timeout_ns);
    result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
    errors |= (unsigned long)(result != 1 || response.badge != 0) << 9;
  }
  if (result != 1)
    (void)kill(child, SIGKILL);
  errors |= (unsigned long)!wait_exit(child, 37) << 7;

done:
  if (attenuated > 0)
    (void)syscall1(SYS_CAP_CLOSE, attenuated);
  if (limited > 0)
    (void)syscall1(SYS_CAP_CLOSE, limited);
  if (minted > 0)
    (void)syscall1(SYS_CAP_CLOSE, minted);
  if (pair.receive)
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)pair.send);
  return errors;
}

static unsigned long ipc_claimed_deadline(struct moss_ipc_endpoints pair) {
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.send, 1) != 0) {
    return 1;
  }
  long done[2];
  if (pipe(done) != 0) {
    return 2;
  }
  long child = fork();
  if (child == 0) {
    close((int)done[0]);
    const struct moss_ipc_message request = {.size = 1, .payload = {'t'}};
    struct moss_ipc_message response = {0};
    // Two seconds lets the forked caller enqueue while leaving room under
    // the five-second case watchdog to observe expiry and clean up.
    long deadline = deadline_after(2000000000UL);
    const unsigned char expired = deadline > 0 && ipc_call(pair.send, &request, &response, deadline) == -IPC_ETIMEDOUT;
    const int reported = write((int)done[1], &expired, 1) == 1;
    _exit(expired && reported ? 37 : 91);
  }
  close((int)done[1]);
  if (child < 0) {
    close((int)done[0]);
    return 4;
  }
  struct moss_ipc_message request = {0};
  unsigned long reply = 0;
  if (ipc_receive(pair.receive, &request, &reply) != 1 || request.payload[0] != 't') {
    (void)kill(child, SIGKILL);
    close((int)done[0]);
    (void)wait_exit(child, 37);
    if (reply) {
      (void)syscall1(SYS_CAP_CLOSE, (long)reply);
    }
    return 8;
  }
  unsigned char expired = 0;
  // Wait until the caller has observed expiry before attempting its reply.
  unsigned long errors = read((int)done[0], &expired, 1) != 1 || expired != 1;
  close((int)done[0]);
  const struct moss_ipc_message answer = {.size = 1, .payload = {'r'}};
  errors |= (unsigned long)(ipc_reply(reply, &answer) != -IPC_ETIMEDOUT) << 1;
  errors |= (unsigned long)!wait_exit(child, 37) << 2;
  return errors;
}

unsigned long ipc_deadline(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  unsigned long errors = 0;
  struct moss_ipc_message request = {.size = MOSS_IPC_MAX_MESSAGE + 1, .payload = {7}};
  struct moss_ipc_message response = {0};
  errors |= (unsigned long)(ipc_call(pair.send, &request, &response, 0) != -IPC_EINVAL);
  request.size = 1;
  // Three expired calls reuse the same bounded queue slots and timer state.
  for (unsigned i = 0; i < 3; ++i) {
    long deadline = deadline_after(1000000UL); // 1 ms is a fixture timeout, not a throughput target.
    if (deadline <= 0 || ipc_call(pair.send, &request, &response, deadline) != -IPC_ETIMEDOUT) {
      errors |= 2;
      break;
    }
  }
  if (!errors) {
    errors |= ipc_claimed_deadline(pair) << 5;
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 2;
  errors |= (unsigned long)(ipc_call(pair.send, &request, &response, 0) != -IPC_EPIPE) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 4;
  return errors;
}

unsigned long ipc_peer_death(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0 || syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(pair.receive, &request, &reply) != 1 || request.size != 1 || request.payload[0] != 19)
      _exit(91);
    // Exit without replying: the one-shot reply capability must wake the caller.
    _exit(37);
  }
  if (child < 0) {
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.send);
    return 2;
  }
  // Keep this receiver alive: EPIPE must come from the dead reply owner,
  // not from closing the channel's last receive capability.
  unsigned long errors = 0;
  const struct moss_ipc_message request = {.size = 1, .payload = {19}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
  if (result != -IPC_EPIPE) {
    (void)kill(child, SIGKILL);
  }
  errors |= (unsigned long)(result != -IPC_EPIPE) << 1;
  errors |= (unsigned long)!wait_exit(child, 37) << 2;

  if (!errors) {
    long replacement = fork();
    if (replacement == 0) {
      struct moss_ipc_message next = {0};
      unsigned long reply = 0;
      if (ipc_receive(pair.receive, &next, &reply) != 1 || next.payload[0] != 20) {
        _exit(93);
      }
      const struct moss_ipc_message answer = {.size = 1, .payload = {'r'}};
      _exit(ipc_reply(reply, &answer) == 0 ? 37 : 94);
    }
    if (replacement < 0) {
      errors |= 1UL << 4;
    } else {
      const struct moss_ipc_message next = {.size = 1, .payload = {20}};
      deadline = deadline_after(ipc_call_timeout_ns);
      result = deadline > 0 ? ipc_call(pair.send, &next, &response, deadline) : -1;
      if (result != 1) {
        (void)kill(replacement, SIGKILL);
      }
      errors |= (unsigned long)(result != 1 || response.payload[0] != 'r') << 5;
      errors |= (unsigned long)!wait_exit(replacement, 37) << 6;
    }
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0);
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 3;
  return errors;
}

unsigned long ipc_nested_roundtrip(void) {
  struct moss_ipc_endpoints front = {0, 0}, back = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&front) != 0 || syscall1(SYS_IPC_CREATE, (long)&back) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)front.receive, 1) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)back.receive, 1) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)back.send, 1) != 0)
    return 1;
  long backend = fork();
  if (backend == 0) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(back.receive, &request, &reply) != 1 || request.payload[0] != 'b')
      _exit(91);
    const struct moss_ipc_message response = {.size = 1, .payload = {'c'}};
    _exit(ipc_reply(reply, &response) == 0 ? 37 : 92);
  }
  if (backend < 0)
    return 2;
  long frontend = fork();
  if (frontend == 0) {
    struct moss_ipc_message request = {0}, response = {0};
    unsigned long reply = 0;
    if (ipc_receive(front.receive, &request, &reply) != 1 || request.payload[0] != 'a')
      _exit(93);
    const struct moss_ipc_message nested = {.size = 1, .payload = {'b'}};
    long deadline = deadline_after(ipc_call_timeout_ns);
    if (deadline <= 0 || ipc_call(back.send, &nested, &response, deadline) != 1 || response.payload[0] != 'c')
      _exit(94);
    const struct moss_ipc_message delivered = {.size = 1, .payload = {'d'}};
    _exit(ipc_reply(reply, &delivered) == 0 ? 37 : 95);
  }
  if (frontend < 0) {
    kill(backend, SIGKILL);
    wait_exit(backend, 37);
    return 4;
  }
  unsigned long errors = syscall1(SYS_CAP_CLOSE, (long)front.receive) != 0;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)back.receive) != 0);
  const struct moss_ipc_message request = {.size = 1, .payload = {'a'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call(front.send, &request, &response, deadline) : -1;
  if (result != 1)
    kill(frontend, SIGKILL);
  errors |= (unsigned long)(result != 1 || response.payload[0] != 'd') << 1;
  errors |= (unsigned long)!wait_exit(frontend, 37) << 2;
  if (errors)
    kill(backend, SIGKILL);
  errors |= (unsigned long)!wait_exit(backend, 37) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)front.send) != 0 ||
                            syscall1(SYS_CAP_CLOSE, (long)back.send) != 0)
            << 4;
  return errors;
}

unsigned long ipc_priority_latency(void) {
  // All participants share the validation worker's CPU. A low-priority
  // receiver must answer a high-priority caller before unrelated runnable
  // work completes; the bound is a regression fixture, not a product SLA.
  // Eight hogs and a two-second run keep the CPU contested beyond the
  // one-second call deadline in the no-donation control.
  enum { HOG_COUNT = 8, HOG_CLOCK_SAMPLE_SPINS = 4096, SERVER_WORK_ITERATIONS = 10000000 };
  const unsigned long hog_duration_ns = 2000000000UL;
  const unsigned long call_budget_ns = 1000000000UL;
  struct moss_ipc_endpoints endpoint = {0, 0};
  long server_ready[2] = {-1, -1};
  long hog_start[2] = {-1, -1};
  long hog_ready[2] = {-1, -1};
  long hogs[HOG_COUNT] = {0};
  long server = -1;
  unsigned long errors = 0;
  unsigned long hog_count = 0;
  const unsigned int cpu_zero = 1;
  long priority = syscall2(SYS_GETPRIORITY, 0, 0);
  if (priority < 1 || priority > 40)
    return 1;
  const int original_nice = 20 - (int)priority;
  int parent_nice_changed = 0;
  if (syscall1(SYS_IPC_CREATE, (long)&endpoint) != 0 || syscall2(SYS_CAP_SET_INHERIT, (long)endpoint.receive, 1) != 0 ||
      pipe(server_ready) != 0) {
    errors |= 1;
    goto cleanup;
  }
  server = fork();
  if (server == 0) {
    close(server_ready[0]);
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.send);
    int nice_ok = ipc_set_nice(19);
    long affinity = syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(cpu_zero), (long)&cpu_zero);
    const char ready = affinity != 0 ? 'a' : !nice_ok ? 'n' : 'r';
    if (write(server_ready[1], &ready, 1) != 1)
      _exit(91);
    close(server_ready[1]);
    if (ready != 'r')
      _exit(91);
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(endpoint.receive, &request, &reply) != 1 || request.payload[0] != 'p')
      _exit(92);
    // Ten million volatile additions exceeded the initial wakeup slice in
    // x64 QEMU calibration; a shorter reply hid missing priority donation.
    volatile unsigned long work = 0;
    for (unsigned long i = 0; i < SERVER_WORK_ITERATIONS; ++i)
      work += i;
    const struct moss_ipc_message response = {.size = 1, .payload = {'q'}};
    _exit(ipc_reply(reply, &response) == 0 ? 37 : 93);
  }
  close(server_ready[1]);
  server_ready[1] = -1;
  if (server < 0) {
    errors |= 1UL << 1;
    goto cleanup;
  }
  char ready = 0;
  if (read(server_ready[0], &ready, 1) != 1 || ready != 'r') {
    errors |= (1UL << 2) | ((unsigned long)(unsigned char)ready << 16);
    goto cleanup;
  }
  close(server_ready[0]);
  server_ready[0] = -1;
  if (syscall1(SYS_CAP_CLOSE, (long)endpoint.receive) != 0) {
    errors |= 1UL << 3;
    goto cleanup;
  }
  endpoint.receive = 0;
  if (pipe(hog_start) != 0 || pipe(hog_ready) != 0) {
    errors |= 1UL << 3;
    goto cleanup;
  }
  for (; hog_count < HOG_COUNT; ++hog_count) {
    long child = fork();
    if (child == 0) {
      close(hog_start[1]);
      close(hog_ready[0]);
      char gate = 0;
      if (!ipc_set_nice(0) || syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(cpu_zero), (long)&cpu_zero) != 0 ||
          read(hog_start[0], &gate, 1) != 1 || gate != 'g' || write(hog_ready[1], &gate, 1) != 1)
        _exit(94);
      close(hog_start[0]);
      close(hog_ready[1]);
      unsigned long start = 0, now = 0;
      if (clock_gettime_ns(&start) != 0)
        _exit(95);
      now = start;
      volatile unsigned long spins = 0;
      while (now - start < hog_duration_ns) {
        ++spins;
        // Sample the clock every 4096 spins so syscalls do not dominate load.
        if ((spins & (HOG_CLOCK_SAMPLE_SPINS - 1)) == 0 && clock_gettime_ns(&now) != 0)
          _exit(96);
      }
      _exit(37);
    }
    if (child < 0) {
      errors |= 1UL << 4;
      goto cleanup;
    }
    hogs[hog_count] = child;
  }
  close(hog_start[0]);
  hog_start[0] = -1;
  close(hog_ready[1]);
  hog_ready[1] = -1;
  for (unsigned long i = 0; i < HOG_COUNT; ++i) {
    const char gate = 'g';
    if (write(hog_start[1], &gate, 1) != 1) {
      errors |= 1UL << 5;
      goto cleanup;
    }
  }
  close(hog_start[1]);
  hog_start[1] = -1;
  for (unsigned long i = 0; i < HOG_COUNT; ++i) {
    char gate = 0;
    if (read(hog_ready[0], &gate, 1) != 1 || gate != 'g') {
      errors |= 1UL << 6;
      goto cleanup;
    }
  }
  close(hog_ready[0]);
  hog_ready[0] = -1;
  (void)sched_yield();
  parent_nice_changed = 1;
  if (!ipc_set_nice(-10)) {
    errors |= 1UL << 7;
    goto cleanup;
  }
  unsigned long start = 0, end = 0;
  if (clock_gettime_ns(&start) != 0 || start > (unsigned long)LONG_MAX - call_budget_ns) {
    errors |= 1UL << 8;
  } else {
    const struct moss_ipc_message request = {.size = 1, .payload = {'p'}};
    struct moss_ipc_message response = {0};
    long result = ipc_call(endpoint.send, &request, &response, (long)(start + call_budget_ns));
    if (clock_gettime_ns(&end) != 0 || end < start)
      errors |= 1UL << 9;
    else if (end - start > call_budget_ns)
      errors |= (1UL << 10) | ((end - start) / 1000000UL << 16);
    if (result != 1 || response.payload[0] != 'q')
      errors |= 1UL << 11;
  }
cleanup:
  if (parent_nice_changed && !ipc_set_nice(original_nice))
    errors |= 1UL << 12;
  if (server_ready[0] >= 0)
    close(server_ready[0]);
  if (server_ready[1] >= 0)
    close(server_ready[1]);
  if (hog_start[0] >= 0)
    close(hog_start[0]);
  if (hog_start[1] >= 0)
    close(hog_start[1]);
  if (hog_ready[0] >= 0)
    close(hog_ready[0]);
  if (hog_ready[1] >= 0)
    close(hog_ready[1]);
  if (errors && server > 0)
    (void)kill(server, SIGKILL);
  for (unsigned long i = 0; i < hog_count; ++i) {
    if (errors)
      (void)kill(hogs[i], SIGKILL);
    int clean_exit = wait_exit(hogs[i], 37);
    if (!errors && !clean_exit)
      errors |= 1UL << 13;
  }
  if (server > 0 && !wait_exit(server, 37))
    errors |= 1UL << 14;
  if (endpoint.receive)
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.receive);
  if (endpoint.send)
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoint.send);
  return errors;
}

static unsigned long ipc_claimed_signal_cancel(struct moss_ipc_endpoints pair) {
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) {
    return 1;
  }
  long release[2];
  if (pipe(release) != 0) {
    return 2;
  }
  const long parent = getpid();
  long child = fork();
  if (child == 0) {
    close((int)release[1]);
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(pair.receive, &request, &reply) != 1 || request.payload[0] != 'c') {
      _exit(91);
    }
    if (kill(parent, SIGUSR1) != 0) {
      _exit(92);
    }
    unsigned char ignored = 0;
    if (read((int)release[0], &ignored, 1) != 0) {
      _exit(93);
    }
    const struct moss_ipc_message answer = {.size = 1, .payload = {'r'}};
    _exit(ipc_reply(reply, &answer) == -IPC_EPIPE ? 37 : 94);
  }
  close((int)release[0]);
  if (child < 0) {
    close((int)release[1]);
    return 4;
  }
  ipc_signal_seen = 0;
  const struct moss_ipc_message request = {.size = 1, .payload = {'c'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
  const int canceled = result == -IPC_EINTR && ipc_signal_seen == SIGUSR1;
  unsigned long errors = (unsigned long)!canceled;
  // EOF releases the service only after the caller has observed cancellation.
  if (!canceled) {
    (void)kill(child, SIGKILL);
  }
  close((int)release[1]);
  errors |= (unsigned long)!wait_exit(child, 37) << 2;
  return errors;
}

static unsigned long ipc_reply_signal_races(struct moss_ipc_endpoints pair) {
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) {
    return 1;
  }
  unsigned long errors = 0;
  const long parent = getpid();
  for (unsigned mode = 0; mode < 2; ++mode) {
    long outcome[2];
    if (pipe(outcome) != 0) {
      return errors | (1UL << (mode * 4));
    }
    ipc_signal_seen = 0;
    long child = fork();
    if (child == 0) {
      close((int)outcome[0]);
      struct moss_ipc_message request = {0};
      unsigned long reply = 0;
      if (ipc_receive(pair.receive, &request, &reply) != 1 || request.payload[0] != 'r') {
        _exit(91);
      }
      const struct moss_ipc_message answer = {.size = 1, .payload = {'y'}};
      long result = 0;
      if (mode == 0) {
        result = ipc_reply(reply, &answer);
        if (kill(parent, SIGUSR1) != 0) {
          _exit(92);
        }
      } else {
        if (kill(parent, SIGUSR1) != 0) {
          _exit(92);
        }
        result = ipc_reply(reply, &answer);
      }
      _exit(write((int)outcome[1], &result, sizeof(result)) == sizeof(result) ? 37 : 93);
    }
    close((int)outcome[1]);
    if (child < 0) {
      close((int)outcome[0]);
      return errors | (1UL << (mode * 4));
    }
    const struct moss_ipc_message request = {.size = 1, .payload = {'r'}};
    struct moss_ipc_message response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    long result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
    if (result != 1 && result != -IPC_EINTR) {
      (void)kill(child, SIGKILL);
    }
    long replied = -1;
    // The signal under test may interrupt either blocking operation.
    long received = 0;
    do {
      received = read((int)outcome[0], &replied, sizeof(replied));
    } while (received == -IPC_EINTR);
    const int reported = received == sizeof(replied);
    close((int)outcome[0]);
    const int reply_won = result == 1 && response.size == 1 && response.payload[0] == 'y' && replied == 0;
    const int cancel_won = result == -IPC_EINTR && replied == -IPC_EPIPE;
    // The first ordering commits reply before signal; the second allows
    // either winner, but the caller and reply holder must agree on it.
    errors |= (unsigned long)!(reply_won || (mode == 1 && cancel_won)) << (mode * 4);
    errors |= (unsigned long)!reported << (mode * 4 + 2);
    int child_status = 0;
    long waited = 0;
    do {
      waited = syscall3(SYS_WAITPID, child, (long)&child_status, 0);
    } while (waited == -IPC_EINTR);
    errors |= (unsigned long)(ipc_signal_seen != SIGUSR1) << (mode * 4 + 1);
    errors |= (unsigned long)(waited != child || ((child_status >> 8) & 255) != 37) << (mode * 4 + 3);
    if (errors) {
      break;
    }
  }
  return errors;
}

unsigned long ipc_signal_cancel(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  struct sigaction_t action = {(unsigned long)ipc_signal_handler, 0, 0};
  struct sigaction_t old_action = {0, 0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  if (moss_sigaction(SIGUSR1, &action, &old_action) != 0)
    return 2;
  ipc_signal_seen = 0;
  long parent = getpid();
  long child = fork();
  if (child == 0) {
    // Give the parent time to enter the blocking call before sending SIGUSR1.
    unsigned long delay = 10000000UL;
    _exit(nanosleep_ns(&delay) == 0 && kill(parent, SIGUSR1) == 0 ? 37 : 91);
  }
  unsigned long errors = child < 0;
  if (child > 0) {
    const struct moss_ipc_message request = {0};
    struct moss_ipc_message response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    errors |= (unsigned long)(deadline <= 0 || ipc_call(pair.send, &request, &response, deadline) != -IPC_EINTR ||
                              ipc_signal_seen != SIGUSR1)
              << 1;
    errors |= (unsigned long)!wait_exit(child, 37) << 2;
  }
  if (!errors) {
    errors |= ipc_claimed_signal_cancel(pair) << 6;
  }
  if (!errors) {
    errors |= ipc_reply_signal_races(pair) << 9;
  }
  errors |= (unsigned long)(moss_sigaction(SIGUSR1, &old_action, 0) != 0) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 4;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 5;
  return errors;
}

unsigned long ipc_capability_transfer(void) {
  struct moss_ipc_endpoints control = {0, 0}, delegated = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&control) != 0 || syscall1(SYS_IPC_CREATE, (long)&delegated) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)control.receive, 1) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    struct moss_ipc_message request = {0};
    unsigned long control_reply = 0;
    if (ipc_receive(control.receive, &request, &control_reply) != 1 || request.payload[0] != 'a' ||
        request.capability == 0 || request.rights != MOSS_CAP_SEND ||
        syscall2(SYS_CAP_DUPLICATE, (long)request.capability, MOSS_CAP_SEND) != -IPC_EACCES)
      _exit(91);
    struct moss_ipc_endpoints returned = {0, 0};
    if (syscall1(SYS_IPC_CREATE, (long)&returned) != 0)
      _exit(92);
    const struct moss_ipc_message answer = {
        .size = 1, .capability = returned.receive, .rights = MOSS_CAP_RECEIVE, .payload = {'a'}};
    if (ipc_reply(control_reply, &answer) != 0 || syscall1(SYS_CAP_CLOSE, (long)returned.receive) != 0)
      _exit(93);
    const struct moss_ipc_message delegated_request = {.size = 1, .payload = {'b'}};
    struct moss_ipc_message delegated_response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    if (deadline <= 0 || ipc_call(request.capability, &delegated_request, &delegated_response, deadline) != 1 ||
        delegated_response.payload[0] != 'b')
      _exit(94);
    const struct moss_ipc_message returned_request = {.size = 1, .payload = {'c'}};
    struct moss_ipc_message returned_response = {0};
    deadline = deadline_after(ipc_call_timeout_ns);
    if (deadline <= 0 || ipc_call(returned.send, &returned_request, &returned_response, deadline) != 1 ||
        returned_response.payload[0] != 'c')
      _exit(95);
    _exit(37);
  }
  if (child < 0)
    return 2;
  unsigned long errors = syscall1(SYS_CAP_CLOSE, (long)control.receive) != 0;
  const struct moss_ipc_message request = {
      .size = 1, .capability = delegated.send, .rights = MOSS_CAP_SEND, .payload = {'a'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long completed = deadline > 0 ? ipc_call(control.send, &request, &response, deadline) : -1;
  errors |= (unsigned long)(completed != 1 || response.payload[0] != 'a' || response.capability == 0 ||
                            response.rights != MOSS_CAP_RECEIVE)
            << 1;
  if (completed == 1 && response.capability != 0) {
    errors |= (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, (long)response.capability, 1) != -IPC_EACCES) << 2;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)delegated.send) != 0) << 3;
    struct moss_ipc_message incoming = {0};
    unsigned long reply = 0;
    errors |= (unsigned long)(ipc_receive(delegated.receive, &incoming, &reply) != 1 || incoming.payload[0] != 'b')
              << 4;
    const struct moss_ipc_message accepted = {.size = 1, .payload = {'b'}};
    errors |= (unsigned long)(ipc_reply(reply, &accepted) != 0) << 5;
    reply = 0;
    errors |= (unsigned long)(ipc_receive(response.capability, &incoming, &reply) != 1 || incoming.payload[0] != 'c')
              << 6;
    const struct moss_ipc_message returned = {.size = 1, .payload = {'c'}};
    errors |= (unsigned long)(ipc_reply(reply, &returned) != 0) << 7;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)response.capability) != 0) << 8;
  }
  errors |= (unsigned long)!wait_exit(child, 37) << 9;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)control.send) != 0) << 10;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)delegated.receive) != 0) << 11;
  return errors;
}

unsigned long ipc_delivery_rollback(void) {
  struct moss_ipc_endpoints control = {0, 0}, delegated = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&control) != 0 || syscall1(SYS_IPC_CREATE, (long)&delegated) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)control.receive, 1) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    struct moss_ipc_message first = {0};
    // The request copyout succeeds, but the reply-handle copyout fails.
    if (ipc_receive(control.receive, &first, 0) != -IPC_EFAULT || first.size != 1 || first.capability == 0 ||
        first.rights != MOSS_CAP_SEND || syscall1(SYS_CAP_CLOSE, (long)first.capability) != -IPC_EBADF ||
        syscall2(SYS_CAP_DUPLICATE, (long)first.capability, MOSS_CAP_SEND) != -IPC_EBADF)
      _exit(91);
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(control.receive, &request, &reply) != 1 || request.payload[0] != 'r' || request.capability == 0 ||
        request.capability == first.capability || syscall1(SYS_CAP_CLOSE, (long)request.capability) != 0)
      _exit(92);
    const struct moss_ipc_message response = {.size = 1, .payload = {'r'}};
    _exit(ipc_reply(reply, &response) == 0 ? 37 : 93);
  }
  if (child < 0)
    return 2;
  unsigned long errors = syscall1(SYS_CAP_CLOSE, (long)control.receive) != 0;
  const struct moss_ipc_message request = {
      .size = 1, .capability = delegated.send, .rights = MOSS_CAP_SEND, .payload = {'r'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  errors |= (unsigned long)(deadline <= 0 || ipc_call(control.send, &request, &response, deadline) != 1 ||
                            response.payload[0] != 'r')
            << 1;
  errors |= (unsigned long)!wait_exit(child, 37) << 2;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)control.send) != 0) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)delegated.send) != 0) << 4;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)delegated.receive) != 0) << 5;
  return errors;
}

unsigned long ipc_memory_object(void) {
  if (syscall1(SYS_MEM_CREATE, 0) != -IPC_EINVAL)
    return 1;
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  if (memory <= 0)
    return 2;
  long read_only = syscall2(SYS_CAP_DUPLICATE, memory, MOSS_CAP_MAP_READ);
  if (read_only <= 0)
    return 4;
  unsigned long errors = syscall2(SYS_MEM_MAP, read_only, MOSS_CAP_MAP_WRITE) != -IPC_EACCES;
  long writable_addr = syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE);
  long readable_addr = syscall2(SYS_MEM_MAP, read_only, MOSS_CAP_MAP_READ);
  if (writable_addr <= 0 || readable_addr <= 0)
    return errors | 2;
  volatile unsigned char *writable = (volatile unsigned char *)writable_addr;
  volatile const unsigned char *readable = (volatile const unsigned char *)readable_addr;
  writable[0] = 'm';
  errors |= (unsigned long)(readable[0] != 'm') << 1;

  struct moss_ipc_endpoints control = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&control) != 0 || syscall2(SYS_CAP_SET_INHERIT, (long)control.receive, 1) != 0)
    return errors | 4;
  long child = fork();
  if (child == 0) {
    // This inherited writable VMA must remain shared after fork.
    if (writable[0] != 'm')
      _exit(91);
    writable[1] = 'f';
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(control.receive, &request, &reply) != 1 || request.capability == 0 ||
        request.rights != (MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) ||
        syscall2(SYS_CAP_DUPLICATE, (long)request.capability, MOSS_CAP_MAP_READ) != -IPC_EACCES)
      _exit(92);
    long transferred_addr = syscall2(SYS_MEM_MAP, (long)request.capability, MOSS_CAP_MAP_WRITE);
    if (transferred_addr <= 0)
      _exit(93);
    volatile unsigned char *transferred = (volatile unsigned char *)transferred_addr;
    if (transferred[0] != 'm' || transferred[1] != 'f')
      _exit(94);
    transferred[0] = 's';
    if (syscall1(SYS_CAP_CLOSE, (long)request.capability) != 0 || transferred[0] != 's')
      _exit(95);
    const struct moss_ipc_message response = {.size = 1, .payload = {'s'}};
    _exit(ipc_reply(reply, &response) == 0 ? 37 : 96);
  }
  if (child < 0)
    return errors | 8;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)control.receive) != 0) << 3;
  const struct moss_ipc_message request = {.size = 1,
                                           .capability = (unsigned long)memory,
                                           .rights = MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE,
                                           .payload = {'m'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long completed = deadline > 0 ? ipc_call(control.send, &request, &response, deadline) : -1;
  if (completed != 1)
    kill(child, SIGKILL); // A failed enqueue could leave the child waiting to receive forever.
  errors |= (unsigned long)(completed != 1 || response.payload[0] != 's') << 4;
  errors |= (unsigned long)!wait_exit(child, 37) << 5;
  errors |= (unsigned long)(readable[0] != 's' || readable[1] != 'f') << 6;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, memory) != 0 || syscall1(SYS_CAP_CLOSE, read_only) != 0 ||
                            writable[0] != 's')
            << 7;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, writable_addr, MOSS_MEM_OBJECT_BYTES) != 0 ||
                            syscall2(SYS_MUNMAP, readable_addr, MOSS_MEM_OBJECT_BYTES) != 0)
            << 8;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)control.send) != 0) << 9;
  return errors;
}

unsigned long ipc_domain_control(void) {
  // A failed termination must complete with exit 97 within one second,
  // rather than leave the validation runner waiting on an immortal child.
  enum { DOMAIN_CHILD_SLEEP_NS = 10000000, DOMAIN_CHILD_SLEEP_CYCLES = 100, DOMAIN_OBSERVER_DELAY_NS = 50000000 };
  unsigned long errors = (unsigned long)(syscall1(SYS_FORK_DOMAIN, 0) != -IPC_EFAULT);
  long self = syscall0(SYS_DOMAIN_SELF);
  errors |= (unsigned long)(self <= 0) << 33;
  if (self > 0) {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, self) != getpid()) << 34;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, self) != -IPC_EINVAL) << 35;
    long another_self = syscall0(SYS_DOMAIN_SELF);
    errors |= (unsigned long)(another_self <= 0) << 37;
    if (another_self > 0) {
      errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, self, another_self) != 1) << 38;
      (void)syscall1(SYS_CAP_CLOSE, another_self);
    }
  }
  unsigned long domain = 0;
  long child = syscall1(SYS_FORK_DOMAIN, (long)&domain);
  if (child == 0) {
    if (domain != 0 || getppid() != 0)
      _exit(96); // Native children have neither parent authority nor POSIX parentage.
    for (unsigned int attempt = 0; attempt < DOMAIN_CHILD_SLEEP_CYCLES; ++attempt) {
      unsigned long delay = DOMAIN_CHILD_SLEEP_NS;
      (void)nanosleep_ns(&delay);
    }
    _exit(97); // Bound a failed termination test instead of hanging the suite.
  }
  if (child <= 1 || !domain) {
    if (domain) {
      (void)syscall1(SYS_DOMAIN_TERMINATE, (long)domain);
      (void)syscall1(SYS_DOMAIN_WAIT, (long)domain);
      (void)syscall1(SYS_CAP_CLOSE, (long)domain);
    }
    if (self > 0)
      errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, self) != 0) << 36;
    return errors | 2;
  }

  long inspect = syscall2(SYS_CAP_DUPLICATE, (long)domain, MOSS_CAP_DOMAIN_INSPECT);
  long observe = syscall2(SYS_CAP_DUPLICATE, (long)domain, MOSS_CAP_DOMAIN_OBSERVE);
  struct moss_domain_exit status = {0};
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, (long)domain) != child) << 2;
  errors |= (unsigned long)(syscall3(SYS_WAITPID, child, 0, 1) != -IPC_ECHILD) << 24;
  errors |= (unsigned long)(syscall2(SYS_KILL, child, 0) != -IPC_EPERM) << 30;
  errors |= (unsigned long)(syscall2(SYS_KILL, child, SIGKILL) != -IPC_EPERM) << 31;
  unsigned int affinity = 1;
  errors |= (unsigned long)(syscall3(SYS_SCHED_SETAFFINITY, child, sizeof(affinity), (long)&affinity) != -IPC_EPERM)
            << 32;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, (long)domain, (long)&status) != -IPC_EAGAIN) << 27;
  errors |= (unsigned long)(inspect <= 0) << 3;
  if (inspect > 0) {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, inspect) != child) << 4;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, inspect) != -IPC_EACCES) << 5;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, inspect) != -IPC_EACCES) << 20;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, inspect, (long)&status) != -IPC_EACCES) << 28;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, inspect) != 1) << 40;
  }
  errors |= (unsigned long)(observe <= 0) << 13;
  if (observe > 0) {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, observe) != -IPC_EACCES) << 14;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, observe) != -IPC_EACCES) << 15;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, observe) != -IPC_EACCES) << 41;
  }
  if (self > 0)
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, self, (long)domain) != 0) << 39;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, 0) != -IPC_EBADF) << 42;
  struct moss_ipc_endpoints pair = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0) {
    errors |= 1UL << 43;
  } else {
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, (long)pair.send) != -IPC_EINVAL) << 44;
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.send);
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
  }
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != 0) << 6;
  if (observe > 0)
    errors |= (unsigned long)!domain_exited((unsigned long)observe, 0, SIGKILL) << 16;
  errors |= (unsigned long)!domain_exited(domain, 0, SIGKILL) << 7;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, (long)domain) != 0) << 17;
  errors |= (unsigned long)(syscall3(SYS_WAITPID, child, 0, 1) != -IPC_ECHILD) << 25;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != -IPC_ESRCH) << 8;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, (long)domain) != child) << 9;
  if (inspect > 0) {
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, inspect) != 1) << 45;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, inspect) != 0) << 10;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, inspect) != -IPC_EBADF) << 46;
  }
  if (observe > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, observe) != 0) << 18;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)domain) != 0) << 11;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != -IPC_EBADF) << 12;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, (long)domain) != -IPC_EBADF) << 19;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, (long)domain, (long)&status) != -IPC_EBADF) << 29;

  unsigned long natural_domain = 0;
  long natural_child = syscall1(SYS_FORK_DOMAIN, (long)&natural_domain);
  if (natural_child == 0) {
    // PID 1 denies child signals; a non-supervisor native domain must allow
    // that POSIX relationship without granting scheduling control.
    long ordinary_child = fork();
    if (ordinary_child == 0) {
      unsigned int affinity = 1;
      long parent = getppid();
      _exit(parent > 1 && syscall2(SYS_KILL, parent, 0) == 0 &&
                    syscall3(SYS_SCHED_SETAFFINITY, parent, sizeof(affinity), (long)&affinity) == -IPC_EPERM
                ? 37
                : 96);
    }
    if (!wait_exit(ordinary_child, 37))
      _exit(96);
    // This fixture delay exercises the blocking path, not a timing contract.
    unsigned long delay = DOMAIN_OBSERVER_DELAY_NS;
    if (nanosleep_ns(&delay) != 0)
      _exit(96);
    _exit(37);
  }
  if (natural_child <= 1 || !natural_domain) {
    errors |= 1UL << 21;
    if (natural_domain) {
      (void)syscall1(SYS_DOMAIN_TERMINATE, (long)natural_domain);
      (void)syscall1(SYS_DOMAIN_WAIT, (long)natural_domain);
      (void)syscall1(SYS_CAP_CLOSE, (long)natural_domain);
    }
  } else {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, (long)natural_domain) != 0 ||
                              syscall2(SYS_DOMAIN_STATUS, (long)natural_domain, 1) != -IPC_EFAULT)
              << 21;
    errors |= (unsigned long)!domain_exited(natural_domain, 37, 0) << 22;
    errors |= (unsigned long)(syscall3(SYS_WAITPID, natural_child, 0, 1) != -IPC_ECHILD) << 26;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)natural_domain) != 0) << 23;
  }
  if (self > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, self) != 0) << 36;
  return errors;
}

unsigned long ipc_domain_selection(void) {
  struct moss_ipc_endpoints pair = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  unsigned long errors = (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, (long)pair.send, 1) != 0);

  unsigned long empty_domain = 0;
  long empty_child = syscall1(SYS_FORK_DOMAIN, (long)&empty_domain);
  if (empty_child == 0)
    _exit(getppid() == 0 && syscall1(SYS_CAP_CLOSE, (long)pair.send) == -IPC_EBADF &&
                  syscall1(SYS_CAP_CLOSE, (long)pair.receive) == -IPC_EBADF
              ? 37
              : 96);
  errors |= (unsigned long)(empty_child <= 1 || !empty_domain) << 1;
  if (empty_domain)
    errors |= (unsigned long)!domain_exited(empty_domain, 37, 0) << 2;
  if (empty_child > 1)
    errors |= (unsigned long)(syscall3(SYS_WAITPID, empty_child, 0, 1) != -IPC_ECHILD) << 17;
  if (empty_domain)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)empty_domain) != 0) << 3;

  unsigned long inherited_domain = 0;
  long inherited_child = syscall1(SYS_FORK_DOMAIN_INHERIT, (long)&inherited_domain);
  if (inherited_child == 0) {
    if (getppid() != 0 || syscall1(SYS_CAP_CLOSE, (long)pair.receive) != -IPC_EBADF)
      _exit(96);
    char inherited_arg[MOSS_DECIMAL_BUFFER_SIZE];
    (void)ultoa(pair.send, inherited_arg, sizeof(inherited_arg));
    const char *args[] = {"cap-present", inherited_arg, 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(96);
  }
  errors |= (unsigned long)(inherited_child <= 1 || !inherited_domain) << 29;
  if (inherited_domain)
    errors |= (unsigned long)!domain_exited(inherited_domain, 37, 0) << 30;
  if (inherited_child > 1)
    errors |= (unsigned long)(syscall3(SYS_WAITPID, inherited_child, 0, 1) != -IPC_ECHILD) << 31;
  if (inherited_domain)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)inherited_domain) != 0) << 32;
  errors |= (unsigned long)(syscall1(SYS_FORK_DOMAIN_INHERIT, 0) != -IPC_EFAULT) << 33;

  const struct moss_fork_capability selected[] = {{pair.receive, MOSS_CAP_RECEIVE, 0}};
  unsigned long selected_domain = 0;
  long selected_child = syscall3(SYS_FORK_DOMAIN_SELECT, (long)&selected_domain, (long)selected, 1);
  if (selected_child == 0) {
    long denied_duplicate = syscall2(SYS_CAP_DUPLICATE, (long)pair.receive, MOSS_CAP_RECEIVE);
    long grandchild = syscall0(SYS_FORK);
    if (grandchild == 0)
      _exit(syscall1(SYS_CAP_CLOSE, (long)pair.receive) == -IPC_EBADF ? 37 : 96);
    if (denied_duplicate != -IPC_EACCES || grandchild <= 1 || !wait_exit(grandchild, 37) ||
        syscall1(SYS_CAP_CLOSE, (long)pair.send) != -IPC_EBADF)
      _exit(96);
    char selected_arg[MOSS_DECIMAL_BUFFER_SIZE];
    (void)ultoa(pair.receive, selected_arg, sizeof(selected_arg));
    const char *args[] = {"cap-present", selected_arg, 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(96);
  }
  errors |= (unsigned long)(selected_child <= 1 || !selected_domain) << 4;
  if (selected_domain)
    errors |= (unsigned long)!domain_exited(selected_domain, 37, 0) << 5;
  if (selected_child > 1)
    errors |= (unsigned long)(syscall3(SYS_WAITPID, selected_child, 0, 1) != -IPC_ECHILD) << 18;
  if (selected_domain)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)selected_domain) != 0) << 6;

  unsigned long rejected_domain = 0;
  const struct moss_fork_capability duplicated[] = {{pair.receive, MOSS_CAP_RECEIVE, 0},
                                                    {pair.receive, MOSS_CAP_RECEIVE, 0}};
  errors |=
      (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)duplicated, 2) != -IPC_EINVAL ||
                      rejected_domain != 0)
      << 7;
  long limited = syscall2(SYS_CAP_DUPLICATE, (long)pair.receive, MOSS_CAP_RECEIVE);
  errors |= (unsigned long)(limited <= 0) << 8;
  if (limited > 0) {
    const struct moss_fork_capability unauthorized[] = {{(unsigned long)limited, MOSS_CAP_RECEIVE, 0}};
    errors |= (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)unauthorized, 1) !=
                                  -IPC_EACCES ||
                              rejected_domain != 0)
              << 9;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, limited) != 0) << 10;
  }
  long exec_limited = syscall2(SYS_CAP_DUPLICATE, (long)pair.receive, MOSS_CAP_RECEIVE);
  errors |= (unsigned long)(exec_limited <= 0) << 19;
  if (exec_limited > 0) {
    errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, exec_limited, 1) != -IPC_EACCES) << 20;
    errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, exec_limited, 0) != 0) << 21;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, exec_limited) != 0) << 22;
  }
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, (long)pair.send, 2) != -IPC_EINVAL) << 23;
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, 0, 1) != -IPC_EBADF) << 24;
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) << 25;
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, (long)pair.send, 0) != 0) << 26;
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, (long)pair.receive, 0) != 0 ||
                            syscall2(SYS_CAP_SET_EXEC, (long)pair.receive, 1) != 0)
            << 27;
  long exec_child = fork();
  if (exec_child == 0) {
    if (syscall2(SYS_CAP_SET_EXEC, (long)pair.send, 0) != 0)
      _exit(96);
    char closed_arg[MOSS_DECIMAL_BUFFER_SIZE], kept_arg[MOSS_DECIMAL_BUFFER_SIZE];
    (void)ultoa(pair.send, closed_arg, sizeof(closed_arg));
    (void)ultoa(pair.receive, kept_arg, sizeof(kept_arg));
    const char *args[] = {"cap-exec", closed_arg, kept_arg, 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(96);
  }
  errors |= (unsigned long)(exec_child <= 1 || !wait_exit(exec_child, 37)) << 28;
  errors |= (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, 0, 1) != -IPC_EFAULT) << 11;
  // A bit outside the published selection flags must be rejected.
  const struct moss_fork_capability invalid_flags[] = {{pair.receive, MOSS_CAP_RECEIVE, MOSS_FORK_CAP_INHERIT << 1}};
  errors |=
      (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)invalid_flags, 1) != -IPC_EINVAL ||
                      rejected_domain != 0)
      << 14;
  const struct moss_fork_capability unauthorized_inherit[] = {{pair.receive, MOSS_CAP_RECEIVE, MOSS_FORK_CAP_INHERIT}};
  errors |= (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)unauthorized_inherit, 1) !=
                                -IPC_EACCES ||
                            rejected_domain != 0)
            << 15;
  const struct moss_fork_capability excess_rights[] = {{pair.receive, MOSS_CAP_RECEIVE | MOSS_CAP_MINT, 0}};
  errors |=
      (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)excess_rights, 1) != -IPC_EACCES ||
                      rejected_domain != 0)
      << 16;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 12;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 13;
  return errors;
}

unsigned long ipc_domain_wait_any(void) {
  enum { CHILD_DELAY_NS = 10000000, SLOW_CHILD_CYCLES = 100, FAST_CHILD_CYCLES = 5 };
  unsigned long slow_domain = 0;
  long slow = syscall1(SYS_FORK_DOMAIN, (long)&slow_domain);
  if (slow == 0) {
    for (unsigned int i = 0; i < SLOW_CHILD_CYCLES; ++i) {
      unsigned long delay = CHILD_DELAY_NS;
      (void)nanosleep_ns(&delay);
    }
    _exit(97); // A failed termination must not hang validation indefinitely.
  }
  if (slow <= 1 || !slow_domain)
    return 1;

  unsigned long fast_domain = 0;
  long fast = syscall1(SYS_FORK_DOMAIN, (long)&fast_domain);
  if (fast == 0) {
    for (unsigned int i = 0; i < FAST_CHILD_CYCLES; ++i) {
      unsigned long delay = CHILD_DELAY_NS;
      (void)nanosleep_ns(&delay);
    }
    _exit(37);
  }
  if (fast <= 1 || !fast_domain) {
    (void)syscall1(SYS_DOMAIN_TERMINATE, (long)slow_domain);
    (void)syscall1(SYS_DOMAIN_WAIT, (long)slow_domain);
    (void)syscall1(SYS_CAP_CLOSE, (long)slow_domain);
    return 2;
  }

  unsigned long errors = 0;
  const unsigned long domains[] = {slow_domain, fast_domain};
  struct moss_domain_exit status = {0};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 2) != 1) << 0;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, (long)fast_domain, (long)&status) != 0) << 20;
  errors |= (unsigned long)(status.code != 37 || status.signal != 0) << 1;
  errors |= (unsigned long)(syscall3(SYS_WAITPID, fast, 0, 1) != -IPC_ECHILD) << 17;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, (long)fast_domain, 1) != -IPC_EFAULT) << 18;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 0) != -IPC_EINVAL) << 2;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, 0, 1) != -IPC_EFAULT) << 3;
  const unsigned long bad[] = {0};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)bad, 1) != -IPC_EBADF) << 4;

  long inspect = syscall2(SYS_CAP_DUPLICATE, (long)slow_domain, MOSS_CAP_DOMAIN_INSPECT);
  long observe = syscall2(SYS_CAP_DUPLICATE, (long)fast_domain, MOSS_CAP_DOMAIN_OBSERVE);
  errors |= (unsigned long)(inspect <= 0 || observe <= 0) << 5;
  if (inspect > 0) {
    const unsigned long unauthorized[] = {(unsigned long)inspect};
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)unauthorized, 1) != -IPC_EACCES) << 6;
  }
  if (observe > 0) {
    const unsigned long duplicate_target[] = {fast_domain, (unsigned long)observe};
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)duplicate_target, 2) != -IPC_EINVAL) << 7;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)&observe, 1) != 0) << 8;
  }

  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)slow_domain) != 0) << 9;
  const unsigned long slow_only[] = {slow_domain};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)slow_only, 1) != 0) << 10;
  errors |= (unsigned long)!domain_exited(slow_domain, 0, SIGKILL) << 11;
  status = (struct moss_domain_exit){0};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 2) != 0 ||
                            syscall2(SYS_DOMAIN_STATUS, (long)slow_domain, (long)&status) != 0 || status.code != 0 ||
                            status.signal != SIGKILL)
            << 16;
  errors |= (unsigned long)(syscall3(SYS_WAITPID, slow, 0, 1) != -IPC_ECHILD) << 19;
  if (inspect > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, inspect) != 0) << 12;
  if (observe > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, observe) != 0) << 13;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)slow_domain) != 0) << 14;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)fast_domain) != 0) << 15;
  return errors;
}
