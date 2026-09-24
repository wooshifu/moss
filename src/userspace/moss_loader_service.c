#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_code_authority_protocol.h"
#include "moss_file_protocol.h"
#include "moss_loader_protocol.h"
#include "syscall.h"

// Keep a construction request within the File Service's bounded content
// budget and the kernel's 4096-page domain limit until domain quotas exist.
enum { LOADER_MAX_PAGES = MOSS_FILE_CONTENT_BUDGET_BYTES / MOSS_MEM_OBJECT_BYTES, LOADER_MAX_HEADERS = 64 };
_Static_assert(LOADER_MAX_PAGES == MOSS_DOMAIN_MAX_IMAGE_PAGES, "loader and domain image limits must agree");
// Five vector words hold argc, both list terminators and the AT_NULL pair;
// the extra 64 bytes cover the top gap, alignment and x64 return slot.
enum { LOADER_STARTUP_BUFFER_BYTES = MOSS_IPC_MAX_MESSAGE + (MOSS_IPC_MAX_MESSAGE + 5) * sizeof(uint64_t) + 64 };
// A stalled dependency cannot hold a supervisor probe indefinitely.
#define LOADER_CALL_TIMEOUT_NS 5000000000UL

static unsigned char file_bytes[MOSS_FILE_CONTENT_BUDGET_BYTES];
static unsigned char page_bytes[LOADER_MAX_PAGES][MOSS_MEM_OBJECT_BYTES]
    __attribute__((aligned(MOSS_MEM_OBJECT_BYTES)));
static unsigned char code_bytes[LOADER_MAX_PAGES][MOSS_MEM_OBJECT_BYTES]
    __attribute__((aligned(MOSS_MEM_OBJECT_BYTES)));
static struct moss_domain_page pages[LOADER_MAX_PAGES];

struct LoaderStartup {
  unsigned char bytes[LOADER_STARTUP_BUFFER_BYTES];
  uint64_t stack_pointer;
  uint64_t size;
  uint64_t argc;
  uint64_t argv;
  uint64_t envp;
};

static long parse_handle(const char *text) {
  if (!text || *text < '0' || *text > '9')
    return 0;
  char *end = NULL;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  return errno || !value || value > LONG_MAX || *end ? 0 : (long)value;
}

static long call(long sender, const struct moss_ipc_message *request, struct moss_ipc_message *response) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - LOADER_CALL_TIMEOUT_NS)
    return -1;
  return syscall6(SYS_IPC_CALL, sender, (long)request, (long)response, (long)(now + LOADER_CALL_TIMEOUT_NS), 0, 0);
}

static int read_file(long file, size_t *size) {
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long mapped = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : -1;
  int valid = mapped > 0;
  size_t offset = 0;
  while (valid && offset <= sizeof(file_bytes)) {
    unsigned int count = sizeof(file_bytes) - offset < MOSS_MEM_OBJECT_BYTES
                             ? (unsigned int)(sizeof(file_bytes) - offset)
                             : MOSS_MEM_OBJECT_BYTES;
    struct moss_ipc_message request = {
        .size = MOSS_FILE_IO_HEADER_BYTES,
        .capability = (unsigned long)memory,
        .rights = MOSS_CAP_MAP_WRITE,
        .payload = {MOSS_FILE_READ},
    };
    moss_file_put_u64(request.payload + 1, offset);
    moss_file_put_u16(request.payload + 9, count);
    struct moss_ipc_message response = {0};
    long result = call(file, &request, &response);
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    unsigned int transferred = moss_file_get_u16(response.payload + 1);
    if (result != MOSS_FILE_IO_REPLY_BYTES || response.payload[0] != MOSS_FILE_OK || response.capability ||
        response.rights || transferred > count) {
      valid = 0;
      break;
    }
    if (transferred)
      memcpy(file_bytes + offset, (const void *)mapped, transferred);
    offset += transferred;
    if (transferred < count || !count)
      break;
  }
  if (mapped > 0 && syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0)
    _exit(1); // A leaked mapping would make repeated malformed requests exhaust this service.
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  *size = valid ? offset : 0;
  return valid;
}

static int overlaps(uint64_t start, uint64_t end, uint64_t other_start, uint64_t other_end) {
  return start < other_end && end > other_start;
}

static int prepare_startup(const struct moss_ipc_message *request, const struct moss_domain_layout *layout,
                           struct LoaderStartup *startup) {
  if (request->size < MOSS_LOADER_RUN_HEADER_BYTES || request->size > sizeof(request->payload) ||
      layout->stack_top < layout->stack_size)
    return 0;
  const size_t argc = request->payload[1];
  const size_t count = argc + request->payload[2];
  const size_t string_bytes = request->size - MOSS_LOADER_RUN_HEADER_BYTES;
  if (count > string_bytes || layout->stack_top < 16 + string_bytes)
    return 0;

  size_t cursor = MOSS_LOADER_RUN_HEADER_BYTES;
  for (size_t index = 0; index < count; ++index) {
    const unsigned char *end = memchr(request->payload + cursor, 0, request->size - cursor);
    if (!end)
      return 0;
    cursor = (size_t)(end - request->payload) + 1;
  }
  if (cursor != request->size)
    return 0;

  // Match the existing Moss exec startup vector so native C entry and mlibc
  // consume the same argc/argv/envp layout on all three supported ISAs.
  const uint64_t strings_base = (layout->stack_top - 16 - string_bytes) & ~(uint64_t)7;
  const size_t vector_bytes = (count + 5) * sizeof(uint64_t);
  if (strings_base < vector_bytes + sizeof(uint64_t))
    return 0;
  const uint64_t vector_base = (strings_base - vector_bytes) & ~(uint64_t)15;
  if (vector_base < sizeof(uint64_t))
    return 0;
  uint64_t stack_pointer = vector_base;
#if defined(__x86_64__)
  stack_pointer -= sizeof(uint64_t); // C entry expects RSP % 16 == 8.
#endif
  if (stack_pointer < layout->stack_top - layout->stack_size ||
      layout->stack_top - stack_pointer > sizeof(startup->bytes))
    return 0;

  startup->stack_pointer = stack_pointer;
  startup->size = layout->stack_top - stack_pointer;
  startup->argc = argc;
  startup->argv = vector_base + sizeof(uint64_t);
  startup->envp = startup->argv + (argc + 1) * sizeof(uint64_t);
  memset(startup->bytes, 0, startup->size);
  memcpy(startup->bytes + strings_base - stack_pointer, request->payload + MOSS_LOADER_RUN_HEADER_BYTES, string_bytes);
  memcpy(startup->bytes + vector_base - stack_pointer, &startup->argc, sizeof(startup->argc));
  cursor = MOSS_LOADER_RUN_HEADER_BYTES;
  for (size_t index = 0; index < count; ++index) {
    const uint64_t pointer = strings_base + cursor - MOSS_LOADER_RUN_HEADER_BYTES;
    const size_t word = 1 + index + (index >= argc);
    memcpy(startup->bytes + vector_base - stack_pointer + word * sizeof(uint64_t), &pointer, sizeof(pointer));
    const unsigned char *end = memchr(request->payload + cursor, 0, request->size - cursor);
    cursor = (size_t)(end - request->payload) + 1;
  }
  return 1;
}

static int plan_elf(size_t size, const struct moss_domain_layout *layout, size_t *page_count, size_t *code_count,
                    uint64_t *entry) {
  if (size < sizeof(Elf64_Ehdr) || layout->page_size != MOSS_MEM_OBJECT_BYTES ||
      layout->stack_top < layout->stack_size || layout->user_begin >= layout->user_end)
    return 0;
  Elf64_Ehdr header;
  memcpy(&header, file_bytes, sizeof(header));
#if defined(__x86_64__)
  const unsigned int machine = EM_X86_64;
#elif defined(__aarch64__)
  const unsigned int machine = EM_AARCH64;
#else
  const unsigned int machine = EM_RISCV;
#endif
  if (memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
      header.e_version != EV_CURRENT || header.e_type != ET_EXEC || header.e_machine != machine ||
      header.e_ehsize != sizeof(header) || header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum ||
      header.e_phnum > LOADER_MAX_HEADERS || header.e_phoff < sizeof(header) || header.e_phoff > size ||
      header.e_phnum > (size - header.e_phoff) / sizeof(Elf64_Phdr))
    return 0;

  *page_count = 0;
  *code_count = 0;
  *entry = header.e_entry;
  int entry_in_code = 0;
  for (unsigned int index = 0; index < header.e_phnum; ++index) {
    Elf64_Phdr segment;
    memcpy(&segment, file_bytes + header.e_phoff + index * sizeof(segment), sizeof(segment));
    if (segment.p_type == PT_INTERP || segment.p_type == PT_DYNAMIC)
      return 0; // No interpreter or relocator exists in this loader yet.
    if (segment.p_type == PT_TLS) {
      // TLS is runtime metadata. The static runtime installs each thread's
      // pointer; file-backed templates must also be covered by a LOAD below.
      if (segment.p_offset > size || segment.p_filesz > size - segment.p_offset || segment.p_filesz > segment.p_memsz)
        return 0;
      if (segment.p_memsz &&
          (segment.p_vaddr < layout->user_begin || segment.p_vaddr >= layout->user_end ||
           segment.p_memsz > layout->user_end - segment.p_vaddr || (segment.p_flags & ~(PF_R | PF_W)) ||
           (segment.p_align > 1 && ((segment.p_align & (segment.p_align - 1)) ||
                                    ((segment.p_offset ^ segment.p_vaddr) & (segment.p_align - 1))))))
        return 0;
      continue;
    }
    if (segment.p_type != PT_LOAD || !segment.p_memsz)
      continue;
    if (segment.p_offset > size || segment.p_filesz > size - segment.p_offset || segment.p_filesz > segment.p_memsz ||
        !(segment.p_flags & PF_R) || (segment.p_flags & ~(PF_R | PF_W | PF_X)) ||
        ((segment.p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) || segment.p_vaddr < layout->user_begin ||
        segment.p_vaddr >= layout->user_end || segment.p_memsz > layout->user_end - segment.p_vaddr ||
        ((segment.p_offset ^ segment.p_vaddr) & (MOSS_MEM_OBJECT_BYTES - 1)) ||
        (segment.p_align > 1 &&
         ((segment.p_align & (segment.p_align - 1)) || ((segment.p_offset ^ segment.p_vaddr) & (segment.p_align - 1)))))
      return 0;

    uint64_t memory_end = segment.p_vaddr + segment.p_memsz;
    if (memory_end > UINT64_MAX - (MOSS_MEM_OBJECT_BYTES - 1))
      return 0;
    uint64_t page_start = segment.p_vaddr & ~(uint64_t)(MOSS_MEM_OBJECT_BYTES - 1);
    uint64_t page_end = (memory_end + MOSS_MEM_OBJECT_BYTES - 1) & ~(uint64_t)(MOSS_MEM_OBJECT_BYTES - 1);
    uint64_t prefix = segment.p_vaddr - page_start;
    if (page_start < layout->user_begin || page_end > layout->user_end || segment.p_offset < prefix ||
        overlaps(page_start, page_end, layout->stack_top - layout->stack_size, layout->stack_top) ||
        overlaps(page_start, page_end, layout->sigreturn_page, layout->sigreturn_page + MOSS_MEM_OBJECT_BYTES) ||
        overlaps(page_start, page_end, layout->heap_start, layout->heap_start + layout->heap_reserve) ||
        (page_end - page_start) / MOSS_MEM_OBJECT_BYTES > LOADER_MAX_PAGES - *page_count)
      return 0;
    uint64_t file_start = segment.p_offset - prefix;
    uint64_t file_span = prefix + segment.p_filesz;
    for (uint64_t address = page_start; address < page_end; address += MOSS_MEM_OBJECT_BYTES) {
      for (size_t previous = 0; previous < *page_count; ++previous) {
        if (pages[previous].address == address)
          return 0; // Overlapping segments cannot silently combine permissions or file bytes.
      }
      unsigned char *bytes = page_bytes[*page_count];
      memset(bytes, 0, MOSS_MEM_OBJECT_BYTES);
      uint64_t file_offset = address - page_start;
      if (file_offset < file_span) {
        size_t copied =
            file_span - file_offset < MOSS_MEM_OBJECT_BYTES ? (size_t)(file_span - file_offset) : MOSS_MEM_OBJECT_BYTES;
        memcpy(bytes, file_bytes + file_start + file_offset, copied);
      }
      struct moss_domain_page *page = &pages[*page_count];
      *page = (struct moss_domain_page){.address = address,
                                        .flags = MOSS_DOMAIN_PAGE_READ |
                                                 ((segment.p_flags & PF_W) ? MOSS_DOMAIN_PAGE_WRITE : 0) |
                                                 ((segment.p_flags & PF_X) ? MOSS_DOMAIN_PAGE_EXEC : 0)};
      if (segment.p_flags & PF_X) {
        memcpy(code_bytes[*code_count], bytes, MOSS_MEM_OBJECT_BYTES);
        page->code_page_index = (*code_count)++;
        if (header.e_entry >= segment.p_vaddr && header.e_entry - segment.p_vaddr < segment.p_filesz)
          entry_in_code = 1;
      } else {
        page->source = (uint64_t)bytes;
        page->size = MOSS_MEM_OBJECT_BYTES;
      }
      ++*page_count;
    }
  }
  for (unsigned int index = 0; index < header.e_phnum; ++index) {
    Elf64_Phdr tls;
    memcpy(&tls, file_bytes + header.e_phoff + index * sizeof(tls), sizeof(tls));
    if (tls.p_type != PT_TLS || !tls.p_filesz)
      continue;
    int covered = 0;
    for (unsigned int candidate = 0; candidate < header.e_phnum; ++candidate) {
      Elf64_Phdr load;
      memcpy(&load, file_bytes + header.e_phoff + candidate * sizeof(load), sizeof(load));
      if (load.p_type != PT_LOAD || !load.p_memsz || tls.p_vaddr < load.p_vaddr ||
          tls.p_vaddr > load.p_vaddr + load.p_memsz || tls.p_memsz > load.p_vaddr + load.p_memsz - tls.p_vaddr ||
          tls.p_offset < load.p_offset || tls.p_offset > load.p_offset + load.p_filesz ||
          tls.p_filesz > load.p_offset + load.p_filesz - tls.p_offset ||
          tls.p_vaddr - load.p_vaddr != tls.p_offset - load.p_offset)
        continue;
      covered = 1;
      break;
    }
    if (!covered)
      return 0;
  }
  return *page_count && *code_count && entry_in_code;
}

static long request_approval(long authority, long version) {
  const struct moss_ipc_message request = {
      .size = 1, .capability = (unsigned long)version, .rights = MOSS_CAP_MAP_READ, .payload = {MOSS_CODE_APPROVE}};
  struct moss_ipc_message response = {0};
  long result = call(authority, &request, &response);
  if (result == 1 && response.size == 1 && response.payload[0] == MOSS_CODE_OK && response.capability &&
      response.rights == (MOSS_CAP_CODE_EXEC | MOSS_CAP_CODE_IDENTIFY | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE))
    return (long)response.capability;
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return -1;
}

static unsigned char load_and_spawn(const struct moss_ipc_message *request, long file, long authority, long factory,
                                    long *domain) {
  struct moss_domain_layout layout = {0};
  if (syscall1(SYS_DOMAIN_LAYOUT, (long)&layout) != 0)
    return MOSS_LOADER_UNAVAILABLE;
  struct LoaderStartup startup;
  if (!prepare_startup(request, &layout, &startup))
    return MOSS_LOADER_BAD_REQUEST;
  // The caller can retain or copy its file sender. Seal the object here so
  // no later write can change bytes between the Loader's page reads.
  struct moss_ipc_message seal_request = {.size = 1, .payload = {MOSS_FILE_SEAL}};
  struct moss_ipc_message seal_response = {0};
  long sealed = call(file, &seal_request, &seal_response);
  if (seal_response.capability) {
    (void)syscall1(SYS_CAP_CLOSE, (long)seal_response.capability);
  }
  if (sealed != 1 || seal_response.size != 1 || seal_response.payload[0] != MOSS_FILE_OK || seal_response.capability ||
      seal_response.rights) {
    return MOSS_LOADER_NO_IMAGE;
  }
  size_t size = 0;
  if (!read_file(file, &size))
    return MOSS_LOADER_NO_IMAGE;
  size_t page_count = 0, code_count = 0;
  uint64_t entry = 0;
  if (!plan_elf(size, &layout, &page_count, &code_count, &entry))
    return MOSS_LOADER_BAD_IMAGE;

  long version = syscall2(SYS_CODE_SNAPSHOT_RANGE, (long)code_bytes, (long)code_count);
  if (version <= 0)
    return MOSS_LOADER_UNAVAILABLE;
  long approval = request_approval(authority, version);
  (void)syscall1(SYS_CAP_CLOSE, version);
  if (approval <= 0)
    return MOSS_LOADER_UNAVAILABLE;
  for (size_t index = 0; index < page_count; ++index) {
    if (pages[index].flags & MOSS_DOMAIN_PAGE_EXEC)
      pages[index].code = (uint64_t)approval;
  }
  struct moss_domain_spawn image = {.entry = entry,
                                    .stack_pointer = startup.stack_pointer,
                                    .stack_source = (uint64_t)startup.bytes,
                                    .stack_size = startup.size,
                                    .arg0 = startup.argc,
                                    .arg1 = startup.argv,
                                    .arg2 = startup.envp,
                                    .pages = (uint64_t)pages,
                                    .page_count = page_count};
  *domain = syscall2(SYS_DOMAIN_SPAWN, factory, (long)&image);
  (void)syscall1(SYS_CAP_CLOSE, approval);
  return *domain > 0 ? MOSS_LOADER_OK : MOSS_LOADER_UNAVAILABLE;
}

int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  long receive = parse_handle(argv[1]);
  long authority = parse_handle(argv[2]);
  long factory = parse_handle(argv[3]);
  if (!receive || !authority || !factory)
    return 2;

  for (;;) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    long received = syscall3(SYS_IPC_RECEIVE, receive, (long)&request, (long)&reply);
    if (received == -EINTR)
      continue;
    if (received < 0)
      return 1;
    struct moss_ipc_message response = {.size = 1, .payload = {MOSS_LOADER_BAD_REQUEST}};
    long domain = 0;
    if (request.capability && request.rights == MOSS_CAP_SEND && request.badge == 0 &&
        request.size >= MOSS_LOADER_RUN_HEADER_BYTES && request.payload[0] == MOSS_LOADER_RUN) {
      response.payload[0] = load_and_spawn(&request, (long)request.capability, authority, factory, &domain);
      if (domain > 0) {
        response.capability = (unsigned long)domain;
        // Child registration delegates observation, identity and signaling;
        // the supervisor keeps termination authority for recovery.
        response.rights = MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL |
                          MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE;
      }
    }
    if (request.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    long replied = syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
    if (replied != 0)
      (void)syscall1(SYS_CAP_CLOSE, (long)reply);
    if (domain > 0) {
      if (replied != 0) {
        (void)syscall1(SYS_DOMAIN_TERMINATE, domain);
        (void)syscall1(SYS_DOMAIN_WAIT, domain);
      }
      (void)syscall1(SYS_CAP_CLOSE, domain);
    }
  }
}
