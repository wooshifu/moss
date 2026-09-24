#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_pipe_protocol.h"
#include "syscall.h"

enum { PIPE_CONTROL = 1, PIPE_READER = 2, PIPE_WRITER = 3 };
enum { PIPE_BYTES = MOSS_MEM_OBJECT_BYTES };
// ponytail: Cap pending waits at 32 of the domain's 64 capability slots;
// evict with a spurious wake if concurrency exceeds the reserved headroom.
enum { PIPE_WAIT_LIMIT = 32 };

struct Pipe {
  struct Pipe *next;
  unsigned long id;
  unsigned int head;
  unsigned int length;
  unsigned int prepared_count;
  unsigned char reader_issued;
  unsigned char writer_issued;
  unsigned char reader_closed;
  unsigned char writer_closed;
  unsigned char data[PIPE_BYTES];
};

static struct Pipe *pipes;
static unsigned int pipe_count;
static unsigned long next_id = 1;
struct PipeWaiter {
  struct Pipe *pipe;
  unsigned long reply;
  unsigned int count;
  unsigned char role;
};
static struct PipeWaiter waiters[PIPE_WAIT_LIMIT];
static unsigned int waiter_count;

static int wait_ready(const struct PipeWaiter *waiter) {
  const struct Pipe *pipe = waiter->pipe;
  return waiter->role == PIPE_READER
             ? (pipe->reader_closed || pipe->writer_closed || (pipe->length && !pipe->prepared_count))
             : (pipe->writer_closed || pipe->reader_closed || waiter->count <= PIPE_BYTES - pipe->length);
}

static void wake_waiter(unsigned int index) {
  const struct moss_ipc_message response = {.size = 1, .payload = {MOSS_PIPE_OK}};
  (void)syscall2(SYS_IPC_REPLY, (long)waiters[index].reply, (long)&response);
  (void)syscall1(SYS_CAP_CLOSE, (long)waiters[index].reply);
  --waiter_count;
  if (index < waiter_count)
    memmove(waiters + index, waiters + index + 1, (waiter_count - index) * sizeof(waiters[0]));
}

static void wake_pipe_waiters(struct Pipe *pipe, int closing) {
  for (unsigned int index = 0; index < waiter_count;) {
    if (waiters[index].pipe == pipe && (closing || wait_ready(&waiters[index])))
      wake_waiter(index);
    else
      ++index;
  }
}

static unsigned long parse_handle(const char *text) {
  if (!text)
    return 0;
  char *end = NULL;
  errno = 0;
  unsigned long handle = strtoul(text, &end, 10);
  return errno || !handle || handle > LONG_MAX || *end ? 0 : handle;
}

static struct Pipe *find_pipe(unsigned long id) {
  for (struct Pipe *pipe = pipes; pipe; pipe = pipe->next) {
    if (pipe->id == id)
      return pipe;
  }
  return NULL;
}

static void remove_pipe(struct Pipe *pipe) {
  for (struct Pipe **slot = &pipes; *slot; slot = &(*slot)->next) {
    if (*slot == pipe) {
      *slot = pipe->next;
      free(pipe);
      --pipe_count;
      return;
    }
  }
}

// Each monotonically assigned pipe ID owns three adjacent badges. A stale
// endpoint therefore cannot name a later pipe even after its object is freed.
static unsigned long badge_for(unsigned long id, unsigned int role) { return id * 3 + role; }

static unsigned int role_of(unsigned long badge) { return (unsigned int)((badge - 1) % 3) + 1; }

static unsigned long id_of(unsigned long badge) { return (badge - 1) / 3; }

static unsigned int copy_count(unsigned int count, unsigned int available) {
  return count < available ? count : available;
}

static void read_pipe(struct Pipe *pipe, void *destination, unsigned int count) {
  unsigned int first = copy_count(count, PIPE_BYTES - pipe->head);
  memcpy(destination, pipe->data + pipe->head, first);
  memcpy((unsigned char *)destination + first, pipe->data, count - first);
}

static void write_pipe(struct Pipe *pipe, const void *source, unsigned int count) {
  unsigned int tail = (pipe->head + pipe->length) % PIPE_BYTES;
  unsigned int first = copy_count(count, PIPE_BYTES - tail);
  memcpy(pipe->data + tail, source, first);
  memcpy(pipe->data, (const unsigned char *)source + first, count - first);
}

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  unsigned long receive = parse_handle(argv[1]);
  unsigned long mint = parse_handle(argv[2]);
  if (!receive || !mint)
    return 2;

  for (;;) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    long received = syscall3(SYS_IPC_RECEIVE, (long)receive, (long)&request, (long)&reply);
    if (received == -EINTR)
      continue;
    if (received < 0)
      return 1;

    struct moss_ipc_message response = {.size = 1, .payload = {MOSS_PIPE_BAD_REQUEST}};
    struct Pipe *created = NULL;
    struct Pipe *finished = NULL;
    struct Pipe *transferred_pipe = NULL;
    struct Pipe *prepared_pipe = NULL;
    struct Pipe *finished_read_pipe = NULL;
    unsigned int transferred_count = 0;
    unsigned int transferred_role = 0;
    unsigned int finish_commit = 0;
    unsigned char *issued = NULL;
    long minted = 0;
    unsigned int role = request.badge ? role_of(request.badge) : 0;
    struct Pipe *pipe = request.badge ? find_pipe(id_of(request.badge)) : NULL;
    if (request.badge && !pipe)
      response.payload[0] = MOSS_PIPE_NO_ENTRY;

    if (request.badge == 0 && request.size == 1 && request.payload[0] == MOSS_PIPE_CREATE && !request.capability &&
        !request.rights) {
      if (pipe_count == MOSS_PIPE_OBJECT_LIMIT || next_id > (LONG_MAX - PIPE_WRITER) / 3) {
        response.payload[0] = MOSS_PIPE_UNAVAILABLE;
      } else {
        created = calloc(1, sizeof(*created));
        if (!created) {
          response.payload[0] = MOSS_PIPE_UNAVAILABLE;
        } else {
          created->id = next_id;
          minted = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)badge_for(next_id, PIPE_CONTROL));
          if (minted <= 0) {
            free(created);
            created = NULL;
            response.payload[0] = MOSS_PIPE_UNAVAILABLE;
          } else {
            response.payload[0] = MOSS_PIPE_OK;
            response.capability = (unsigned long)minted;
            response.rights = MOSS_CAP_SEND;
          }
        }
      }
    } else if (pipe && role == PIPE_CONTROL && request.size == 1 && !request.capability && !request.rights) {
      if (request.payload[0] == MOSS_PIPE_CANCEL) {
        response.payload[0] = MOSS_PIPE_OK;
        finished = pipe;
      } else if (request.payload[0] == MOSS_PIPE_READ_END || request.payload[0] == MOSS_PIPE_WRITE_END) {
        unsigned int end = request.payload[0] == MOSS_PIPE_READ_END ? PIPE_READER : PIPE_WRITER;
        issued = end == PIPE_READER ? &pipe->reader_issued : &pipe->writer_issued;
        if (!*issued) {
          minted = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)badge_for(pipe->id, end));
          if (minted > 0) {
            response.payload[0] = MOSS_PIPE_OK;
            response.capability = (unsigned long)minted;
            response.rights = MOSS_CAP_SEND;
          } else {
            response.payload[0] = MOSS_PIPE_UNAVAILABLE;
            issued = NULL;
          }
        }
      }
    } else if (pipe && role == PIPE_READER && request.size == MOSS_PIPE_READ_FINISH_BYTES &&
               request.payload[0] == MOSS_PIPE_READ_FINISH && request.payload[1] <= 1 && !request.capability &&
               !request.rights && pipe->prepared_count) {
      response.payload[0] = MOSS_PIPE_OK;
      finished_read_pipe = pipe;
      finish_commit = request.payload[1];
    } else if (pipe && (role == PIPE_READER || role == PIPE_WRITER) && request.size == MOSS_PIPE_WAIT_BYTES &&
               request.payload[0] == MOSS_PIPE_WAIT && request.capability && request.rights == MOSS_CAP_SEND &&
               (role == PIPE_READER ? pipe->reader_issued && !pipe->reader_closed
                                    : pipe->writer_issued && !pipe->writer_closed)) {
      unsigned int count = moss_pipe_get_u16(request.payload + 1);
      if (count && count <= PIPE_BYTES) {
        if (waiter_count == PIPE_WAIT_LIMIT)
          wake_waiter(0); // A spurious wake frees a slot, including for a canceled caller.
        waiters[waiter_count++] =
            (struct PipeWaiter){.pipe = pipe, .reply = request.capability, .count = count, .role = (unsigned char)role};
        request.capability = 0;
        response.payload[0] = MOSS_PIPE_OK;
      }
    } else if (pipe && (role == PIPE_READER || role == PIPE_WRITER) && request.size == 1 &&
               request.payload[0] == MOSS_PIPE_CLOSE && !request.capability && !request.rights) {
      if (role == PIPE_READER) {
        pipe->reader_closed = 1;
        pipe->prepared_count = 0;
      } else
        pipe->writer_closed = 1;
      response.payload[0] = MOSS_PIPE_OK;
      if (pipe->reader_closed && pipe->writer_closed)
        finished = pipe;
    } else if (pipe && request.capability && request.size == MOSS_PIPE_IO_BYTES &&
               ((role == PIPE_READER &&
                 (request.payload[0] == MOSS_PIPE_READ || request.payload[0] == MOSS_PIPE_READ_PREPARE) &&
                 request.rights == MOSS_CAP_MAP_WRITE && pipe->reader_issued && !pipe->reader_closed) ||
                (role == PIPE_WRITER && request.payload[0] == MOSS_PIPE_WRITE && request.rights == MOSS_CAP_MAP_READ &&
                 pipe->writer_issued && !pipe->writer_closed))) {
      unsigned int count = moss_pipe_get_u16(request.payload + 1);
      if (count <= PIPE_BYTES) {
        if (!count || (role == PIPE_READER && !pipe->length && pipe->writer_closed)) {
          response.size = MOSS_PIPE_IO_REPLY_BYTES;
          response.payload[0] = MOSS_PIPE_OK;
          moss_pipe_put_u16(response.payload + 1, 0);
        } else if (role == PIPE_READER && pipe->prepared_count) {
          response.payload[0] = MOSS_PIPE_WOULD_BLOCK;
        } else if (role == PIPE_WRITER && pipe->reader_closed) {
          response.payload[0] = MOSS_PIPE_BROKEN;
        } else if ((role == PIPE_READER && !pipe->length) ||
                   (role == PIPE_WRITER && count > PIPE_BYTES - pipe->length)) {
          response.payload[0] = MOSS_PIPE_WOULD_BLOCK;
        } else {
          long mapped = syscall2(SYS_MEM_MAP, (long)request.capability,
                                 role == PIPE_READER ? MOSS_CAP_MAP_WRITE : MOSS_CAP_MAP_READ);
          if (mapped > 0) {
            unsigned int transferred = role == PIPE_READER ? copy_count(count, pipe->length) : count;
            if (role == PIPE_READER)
              read_pipe(pipe, (void *)mapped, transferred);
            else
              write_pipe(pipe, (const void *)mapped, transferred);
            response.size = MOSS_PIPE_IO_REPLY_BYTES;
            response.payload[0] = MOSS_PIPE_OK;
            moss_pipe_put_u16(response.payload + 1, transferred);
            if (request.payload[0] == MOSS_PIPE_READ_PREPARE) {
              prepared_pipe = pipe;
              transferred_count = transferred;
            } else {
              transferred_pipe = pipe;
              transferred_count = transferred;
              transferred_role = role;
            }
            if (syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0)
              return 1;
          } else {
            response.payload[0] = MOSS_PIPE_UNAVAILABLE;
          }
        }
      }
    }

    if (request.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    long sent = syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
    if (transferred_pipe && sent == 0) {
      // The shared page may be filled before replying, but ring ownership
      // changes only if the backend reply reaches its immediate caller.
      if (transferred_role == PIPE_READER) {
        transferred_pipe->head = (transferred_pipe->head + transferred_count) % PIPE_BYTES;
        transferred_pipe->length -= transferred_count;
      } else {
        transferred_pipe->length += transferred_count;
      }
    }
    if (prepared_pipe && sent == 0)
      prepared_pipe->prepared_count = transferred_count;
    if (finished_read_pipe && sent == 0) {
      if (finish_commit) {
        finished_read_pipe->head = (finished_read_pipe->head + finished_read_pipe->prepared_count) % PIPE_BYTES;
        finished_read_pipe->length -= finished_read_pipe->prepared_count;
      }
      finished_read_pipe->prepared_count = 0;
    }
    if (created) {
      if (sent == 0) {
        created->next = pipes;
        pipes = created;
        ++pipe_count;
        ++next_id;
      } else {
        free(created);
      }
    }
    if (issued && sent == 0)
      *issued = 1;
    if (minted > 0)
      (void)syscall1(SYS_CAP_CLOSE, minted);
    if (pipe)
      wake_pipe_waiters(pipe, finished != NULL);
    if (finished)
      remove_pipe(finished);
  }
}
