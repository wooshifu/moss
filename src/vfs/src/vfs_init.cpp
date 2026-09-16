// MOSS VFS initialization — implementation
// Mounts root ramfs, devfs, and sets up stdio for PID 1.

module moss.vfs;

import moss.abi;
import moss.arch;
import moss.mm;

// Validation can exhaust/restore the real heap around this fallible allocation.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_fd_clone(bool) noexcept {}

namespace moss::kernel::vfs {

// -- MountTable implementation --

long MountTable::mount(const char *path, SuperBlock *sb, Dentry *root) noexcept {
  if (count_ >= MAX_MOUNTS) {
    return -static_cast<long>(VfsError::NoMemory);
  }

  auto &entry = mounts_[count_];
  // Copy path
  u32 len = 0;
  while (path[len] != '\0' && len < MAX_PATH_LEN - 1) {
    entry.path[len] = path[len];
    ++len;
  }
  entry.path[len] = '\0';
  entry.path_len = len;
  entry.parent = nullptr;
  entry.name_len = 0;
  if (len > 1) {
    u32 name_start = len;
    while (name_start && path[name_start - 1] != '/')
      --name_start;
    char parent_path[MAX_PATH_LEN];
    __builtin_memcpy(parent_path, path, name_start);
    parent_path[name_start] = 0;
    entry.parent = resolve_path_locked(parent_path);
    if (!entry.parent || !entry.parent->inode || !entry.parent->inode->is_directory())
      return -static_cast<long>(VfsError::NotDirectory);
    entry.parent->ref();
    entry.name_len = len - name_start;
  }
  entry.sb = sb;
  entry.root = root;
  entry.active = true;
  ++count_;
  return 0;
}

bool MountTable::lookup(const char *path, MountLookupResult &result) noexcept {
  // Find the deepest (longest prefix) matching mount point
  u32 best_len = 0;
  MountEntry *best = nullptr;

  for (u32 i = 0; i < count_; ++i) {
    if (!mounts_[i].active) {
      continue;
    }

    auto &m = mounts_[i];
    // Check if path starts with mount path
    bool match = true;
    for (u32 j = 0; j < m.path_len; ++j) {
      if (path[j] == '\0' || path[j] != m.path[j]) {
        match = false;
        break;
      }
    }

    // Ensure the match is at a path boundary:
    // Root "/" matches everything; others need exact boundary (end of string or '/').
    if (match && m.path_len > best_len &&
        ((m.path_len == 1 && m.path[0] == '/') || path[m.path_len] == '\0' || path[m.path_len] == '/')) {
      best_len = m.path_len;
      best = &m;
    }
  }

  if (best == nullptr) {
    return false;
  }

  result.mount = best;
  // Compute residual: skip mount prefix and leading '/'
  const char *r = path + best_len;
  if (*r == '/') {
    ++r;
  }
  result.residual = r;
  u32 rlen = 0;
  while (r[rlen] != '\0') {
    ++rlen;
  }
  result.residual_len = rlen;
  return true;
}

// -- DentryCache implementation --

void DentryCache::init() noexcept {
  for (u32 i = 0; i < CACHE_SIZE; ++i) {
    slots_[i] = nullptr;
  }
}

u32 DentryCache::hash(const Dentry *parent, const char *name, u32 name_len) noexcept {
  // FNV-1a inspired hash
  u32 h = 2166136261U;
  // Mix in parent address
  auto addr = reinterpret_cast<u64>(parent);
  for (u32 i = 0; i < 8; ++i) {
    h ^= static_cast<u32>(addr & 0xFF);
    h *= 16777619U;
    addr >>= 8;
  }
  // Mix in name
  for (u32 i = 0; i < name_len; ++i) {
    h ^= static_cast<u32>(static_cast<u8>(name[i]));
    h *= 16777619U;
  }
  return h % CACHE_SIZE;
}

void DentryCache::insert(Dentry *dentry) noexcept {
  if (dentry == nullptr) {
    return;
  }
  u32 idx = hash(dentry->parent, dentry->name, dentry->name_len);
  // Simple open addressing: find next free slot
  for (u32 i = 0; i < CACHE_SIZE; ++i) {
    u32 slot = (idx + i) % CACHE_SIZE;
    if (slots_[slot] == nullptr) {
      slots_[slot] = dentry;
      return;
    }
  }
  // Cache full — silently drop (non-fatal)
}

Dentry *DentryCache::lookup(const Dentry *parent, const char *name, u32 name_len) noexcept {
  u32 idx = hash(parent, name, name_len);
  for (u32 i = 0; i < CACHE_SIZE; ++i) {
    u32 slot = (idx + i) % CACHE_SIZE;
    Dentry *d = slots_[slot];
    if (d == nullptr) {
      continue; // A deletion can leave a hole before a colliding live entry.
    }
    if (d->parent == parent && d->name_len == name_len) {
      // Compare names
      bool eq = true;
      for (u32 j = 0; j < name_len; ++j) {
        if (d->name[j] != name[j]) {
          eq = false;
          break;
        }
      }
      if (eq) {
        return d;
      }
    }
  }
  return nullptr;
}

void DentryCache::remove(Dentry *dentry) noexcept {
  // ponytail: scan the bounded 256-slot cache; use tombstones if miss cost matters.
  for (auto &slot : slots_)
    if (slot == dentry)
      slot = nullptr;
}

// -- Inode / Dentry / File pool allocators --

static constexpr u32 MAX_INODES = 256;
static constexpr u32 MAX_DENTRIES = 256;

static Inode g_inode_pool[MAX_INODES];
static bool g_inode_used[MAX_INODES];
static containers::IrqSpinLock inode_pool_lock;

static Dentry g_dentry_pool[MAX_DENTRIES];
static bool g_dentry_used[MAX_DENTRIES];
static Dentry *find_dentry_for_inode(Inode *inode) noexcept;

static File g_file_pool[MAX_FILES];
static bool g_file_used[MAX_FILES];
static bool g_file_pool_initialized = false;
static containers::IrqSpinLock file_pool_lock;

u32 file_pool_usage() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(file_pool_lock);
  u32 files = 0;
  for (bool used : g_file_used)
    if (used)
      ++files;
  return files;
}

PoolUsage pool_usage() noexcept {
  containers::LockGuard<containers::IrqSpinLock> namespace_guard(namespace_lock);
  containers::LockGuard<containers::IrqSpinLock> inode_guard(inode_pool_lock);
  PoolUsage usage;
  for (bool used : g_inode_used)
    if (used)
      ++usage.inodes;
  for (bool used : g_dentry_used)
    if (used)
      ++usage.dentries;
  usage.files = file_pool_usage();
  return usage;
}

Inode *alloc_inode() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(inode_pool_lock);
  u32 slot = 0;
  while (slot < MAX_INODES && g_inode_used[slot]) {
    ++slot;
  }
  if (slot == MAX_INODES) {
    return nullptr;
  }
  g_inode_used[slot] = true;
  auto *inode = &g_inode_pool[slot];
  // Zero-initialize
  inode->ino = INVALID_INO;
  inode->type = FileType::Regular;
  inode->mode = 0;
  inode->nlink = 1;
  inode->uid = inode->gid = 0;
  inode->size = 0;
  inode->rdev = NO_DEVICE;
  inode->sb = nullptr;
  inode->file_ops = nullptr;
  inode->inode_ops = nullptr;
  inode->data = nullptr;
  inode->private_data = nullptr;
  inode->data_capacity = 0;
  inode->ramfs_mutable = false;
  inode->child_count = 0;
  for (u32 i = 0; i < Inode::MAX_CHILDREN; ++i) {
    inode->children[i] = nullptr;
  }
  inode->ref_count = 1;
  return inode;
}

static void free_inode(Inode *inode) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(inode_pool_lock);
  if (inode && inode->ref_count == 0) {
    if (inode->ramfs_mutable && inode->data)
      (void)mm::RuntimeHeapAllocator::deallocate(const_cast<u8 *>(inode->data), inode->data_capacity);
    g_inode_used[static_cast<usize>(inode - g_inode_pool)] = false;
  }
}

Dentry *alloc_dentry(const char *name, u32 name_len, Inode *inode, Dentry *parent) noexcept {
  u32 slot = 0;
  while (slot < MAX_DENTRIES && g_dentry_used[slot])
    ++slot;
  if (slot == MAX_DENTRIES) {
    return nullptr;
  }
  if (name_len > MAX_NAME_LEN) {
    return nullptr;
  }
  if (parent && (!parent->inode || parent->inode->child_count == Inode::MAX_CHILDREN))
    return nullptr;

  g_dentry_used[slot] = true;
  auto *d = &g_dentry_pool[slot];
  // Copy name
  for (u32 i = 0; i < name_len; ++i) {
    d->name[i] = name[i];
  }
  d->name[name_len] = '\0';
  d->name_len = name_len;
  d->inode = inode;
  d->parent = parent;
  d->ref_count = 1;

  // Register in parent's children list
  if (parent != nullptr && parent->inode != nullptr) {
    parent->ref();
    auto *pi = parent->inode;
    if (pi->child_count < Inode::MAX_CHILDREN) {
      pi->children[pi->child_count++] = d;
    }
  }

  // Insert into dcache
  g_dcache.insert(d);

  return d;
}

void release_dentry(Dentry *dentry) noexcept {
  while (dentry) {
    dentry->unref();
    if (dentry->ref_count)
      return;
    auto *parent = dentry->parent;
    if (dentry->inode) {
      dentry->inode->unref();
      free_inode(dentry->inode);
    }
    g_dentry_used[static_cast<usize>(dentry - g_dentry_pool)] = false;
    dentry->inode = nullptr;
    dentry->parent = nullptr;
    dentry = parent;
  }
}

File *alloc_file() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(file_pool_lock);
  if (!g_file_pool_initialized) {
    for (u32 i = 0; i < MAX_FILES; ++i) {
      g_file_used[i] = false;
    }
    g_file_pool_initialized = true;
  }
  for (u32 i = 0; i < MAX_FILES; ++i) {
    if (!g_file_used[i]) {
      g_file_used[i] = true;
      auto *f = &g_file_pool[i];
      f->inode = nullptr;
      f->dentry = nullptr;
      f->f_ops = nullptr;
      f->flags = 0;
      f->pos = 0;
      f->private_data = nullptr;
      f->ref_count = 1;
      return f;
    }
  }
  return nullptr;
}

void free_file(File *file) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(file_pool_lock);
  if (file == nullptr) {
    return;
  }
  auto offset = static_cast<u32>(file - g_file_pool);
  if (offset < MAX_FILES) {
    g_file_used[offset] = false;
  }
}

// -- FdTable implementation --

void release_file(File *f) noexcept {
  if (f == nullptr) {
    return;
  }
  // Only the thread performing the 1 -> 0 transition may touch the endpoint
  // afterward. A separate count load could observe another closer's zero.
  if (f->unref()) {
    // Call filesystem release if available
    if (f->f_ops != nullptr && f->f_ops->release != nullptr) {
      f->f_ops->release(f);
    }
    {
      containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
      if (f->inode != nullptr) {
        f->inode->unref();
        free_inode(f->inode);
      }
      release_dentry(f->dentry);
    }
    free_file(f);
  }
}

FdTable *FdTable::clone() const noexcept {
  moss_validation_fd_clone(true);
  auto storage = mm::RuntimeHeapAllocator::allocate(sizeof(FdTable));
  moss_validation_fd_clone(false);
  if (!storage)
    return nullptr;
  auto *table = new (*storage) FdTable();
  table->init();
  {
    containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
    table->set_working_directory(cwd_);
  }
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  for (u32 i = 0; i < MAX_FDS; ++i) {
    if (fds_[i] != nullptr) {
      table->fds_[i] = fds_[i];
      table->cloexec_[i] = cloexec_[i];
      fds_[i]->ref();
    }
  }
  return table;
}

FdTable::~FdTable() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  release_dentry(cwd_);
}

// ====================================================================
// pipefs implementation — anonymous pipes with 4KB ring buffer
// ====================================================================

namespace pipefs {

struct PipeWaiter {
  void *thread;
  PipeWaiter *next;
};

/// Pipe internal state — 4KB ring buffer shared between read and write ends.
struct PipeState {
  static constexpr u32 PIPE_BUF_SIZE = 4096;

  containers::IrqSpinLock lock;
  PipeWaiter *read_waiters;
  PipeWaiter *write_waiters;
  u8 buffer[PIPE_BUF_SIZE];
  u32 read_pos;
  u32 write_pos;
  u32 count;   // bytes currently in buffer
  u32 readers; // number of open read-end File objects
  u32 writers; // number of open write-end File objects
};

static constexpr u32 MAX_PIPES = 64;
static PipeState g_pipe_pool[MAX_PIPES];
static bool g_pipe_used[MAX_PIPES];
static containers::IrqSpinLock g_pipe_pool_lock;

// Keep IRQs masked across releasing the event lock and saving the sleeper's
// context. Each waiter lives on its owner's stack; blocking allocates nothing.
struct PipeGuard {
  bool restore_irqs;
  containers::IrqSpinLock &lock;
  explicit PipeGuard(PipeState &state) : restore_irqs(arch::interrupts_enabled()), lock(state.lock) {
    arch::disable_interrupts();
    lock.lock();
  }
  ~PipeGuard() {
    lock.unlock();
    if (restore_irqs)
      arch::enable_interrupts();
  }
  long wait(PipeWaiter *&head) {
    void *thread = moss::abi::bridge::moss_prepare_io_wait();
    if (!thread)
      return -static_cast<long>(VfsError::NotSupported);
    PipeWaiter waiter{thread, head};
    head = &waiter;
    lock.unlock();
    moss::abi::bridge::moss_commit_io_wait();
    lock.lock();
    auto **link = &head;
    while (*link != &waiter)
      link = &(*link)->next;
    *link = waiter.next;
    return moss::abi::bridge::moss_io_wait_interrupted() ? -static_cast<long>(VfsError::Interrupted) : 0;
  }
};

static void wake_waiters(PipeWaiter *head) noexcept {
  for (auto *waiter = head; waiter; waiter = waiter->next)
    moss::abi::bridge::moss_wake_io_waiter(waiter->thread);
}

static PipeState *alloc_pipe_state() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(g_pipe_pool_lock);
  for (u32 i = 0; i < MAX_PIPES; ++i) {
    if (!g_pipe_used[i]) {
      g_pipe_used[i] = true;
      auto *ps = &g_pipe_pool[i];
      ps->read_pos = 0;
      ps->write_pos = 0;
      ps->count = 0;
      ps->readers = 0;
      ps->writers = 0;
      ps->read_waiters = nullptr;
      ps->write_waiters = nullptr;
      return ps;
    }
  }
  return nullptr;
}

static void free_pipe_state(PipeState *ps) noexcept {
  if (ps == nullptr) {
    return;
  }
  auto offset = static_cast<u32>(ps - g_pipe_pool);
  containers::LockGuard<containers::IrqSpinLock> guard(g_pipe_pool_lock);
  if (offset < MAX_PIPES) {
    g_pipe_used[offset] = false;
  }
}

// -- Pipe read-end file operations --

static long pipe_read_release(File *file) noexcept {
  if (file == nullptr || file->private_data == nullptr) {
    return 0;
  }
  auto *ps = static_cast<PipeState *>(file->private_data);
  PipeGuard guard(*ps);
  if (ps->readers > 0) {
    --ps->readers;
  }
  wake_waiters(ps->write_waiters);
  // Free PipeState when both ends are closed
  if (ps->readers == 0 && ps->writers == 0) {
    free_pipe_state(ps);
  }
  return 0;
}

static long pipe_read(File *file, OutputBuffer buffer) noexcept {
  if (file == nullptr || file->private_data == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *ps = static_cast<PipeState *>(file->private_data);
  PipeGuard guard(*ps);
  if (buffer.size() == 0)
    return 0;

  while (ps->count == 0) {
    if (ps->writers == 0)
      return 0;
    if (file->flags & O_NONBLOCK)
      return -static_cast<long>(VfsError::WouldBlock);
    if (long error = guard.wait(ps->read_waiters))
      return error;
  }

  // Read up to min(count, available)
  usize count = buffer.size();
  usize avail = ps->count;
  if (count > avail) {
    count = avail;
  }

  usize copied = 0;
  for (; copied < count; ++copied) {
    if (buffer.copy_from(copied, &ps->buffer[ps->read_pos], 1) != 1)
      break;
    ps->read_pos = (ps->read_pos + 1) % PipeState::PIPE_BUF_SIZE;
  }
  ps->count -= static_cast<u32>(copied);
  if (copied)
    wake_waiters(ps->write_waiters);

  return !copied && count ? -static_cast<long>(VfsError::BadAddress) : static_cast<long>(copied);
}

static long pipe_read_write([[maybe_unused]] File *file, [[maybe_unused]] InputBuffer buffer) noexcept {
  return -static_cast<long>(VfsError::BadFd); // cannot write to read end
}

static long pipe_lseek([[maybe_unused]] File *file, [[maybe_unused]] i64 offset,
                       [[maybe_unused]] SeekWhence whence) noexcept {
  return -static_cast<long>(VfsError::IsPipe);
}

static const FileOps g_pipe_read_fops = {
    .open = nullptr,
    .release = pipe_read_release,
    .read = pipe_read,
    .write = pipe_read_write,
    .lseek = pipe_lseek,
    .ioctl = nullptr,
};

// -- Pipe write-end file operations --

static long pipe_write_release(File *file) noexcept {
  if (file == nullptr || file->private_data == nullptr) {
    return 0;
  }
  auto *ps = static_cast<PipeState *>(file->private_data);
  PipeGuard guard(*ps);
  if (ps->writers > 0) {
    --ps->writers;
  }
  wake_waiters(ps->read_waiters);
  if (ps->readers == 0 && ps->writers == 0) {
    free_pipe_state(ps);
  }
  return 0;
}

static long pipe_write(File *file, InputBuffer buffer) noexcept {
  if (file == nullptr || file->private_data == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *ps = static_cast<PipeState *>(file->private_data);
  PipeGuard guard(*ps);

  usize written = 0;
  while (written < buffer.size()) {
    if (ps->readers == 0) {
      moss::abi::bridge::moss_signal_broken_pipe();
      return written ? static_cast<long>(written) : -static_cast<long>(VfsError::BrokenPipe);
    }

    const usize free_space = PipeState::PIPE_BUF_SIZE - ps->count;
    // Small writes reserve their entire record; larger writes can stream as
    // readers drain the ring. Nonblocking writes never wait for capacity.
    const usize needed = buffer.size() <= PipeState::PIPE_BUF_SIZE ? buffer.size() : 1;
    if (free_space < needed) {
      if (file->flags & O_NONBLOCK)
        return written ? static_cast<long>(written) : -static_cast<long>(VfsError::WouldBlock);
      if (long error = guard.wait(ps->write_waiters))
        return written ? static_cast<long>(written) : error;
      continue;
    }
    usize count = buffer.size() - written;
    if (count > free_space)
      count = free_space;
    usize copied = 0;
    for (; copied < count; ++copied) {
      if (buffer.copy_to(written + copied, &ps->buffer[ps->write_pos], 1) != 1)
        break;
      ps->write_pos = (ps->write_pos + 1) % PipeState::PIPE_BUF_SIZE;
    }
    ps->count += static_cast<u32>(copied);
    written += copied;
    if (copied)
      wake_waiters(ps->read_waiters);
    if (copied != count)
      return written ? static_cast<long>(written) : -static_cast<long>(VfsError::BadAddress);
  }
  return static_cast<long>(written);
}

static long pipe_write_read([[maybe_unused]] File *file, [[maybe_unused]] OutputBuffer buffer) noexcept {
  return -static_cast<long>(VfsError::BadFd); // cannot read from write end
}

static const FileOps g_pipe_write_fops = {
    .open = nullptr,
    .release = pipe_write_release,
    .read = pipe_write_read,
    .write = pipe_write,
    .lseek = pipe_lseek,
    .ioctl = nullptr,
};

// -- Pipe inode (shared between read/write ends) --

static constexpr InodeNumber PIPEFS_BASE_INO = 10000;
static InodeNumber g_pipefs_next_ino = PIPEFS_BASE_INO;

long create_pipe(File *&read_file, File *&write_file) noexcept {
  // 1. Allocate pipe state
  PipeState *ps = alloc_pipe_state();
  if (ps == nullptr) {
    return -static_cast<long>(VfsError::NoMemory);
  }

  // 2. Allocate shared pipe inode
  Inode *pipe_inode = alloc_inode();
  if (pipe_inode == nullptr) {
    free_pipe_state(ps);
    return -static_cast<long>(VfsError::NoMemory);
  }
  pipe_inode->ino = g_pipefs_next_ino++;
  pipe_inode->type = FileType::Fifo;
  pipe_inode->mode = S_IFIFO | S_IRUSR | S_IWUSR;
  pipe_inode->private_data = ps;
  // Anonymous inodes have no directory owner: only the two File endpoints
  // hold references. The final close returns their inode slot to the pool.
  pipe_inode->ref_count = 0;

  // 3. Allocate read-end File
  read_file = alloc_file();
  if (read_file == nullptr) {
    free_inode(pipe_inode);
    free_pipe_state(ps);
    return -static_cast<long>(VfsError::NoMemory);
  }
  read_file->inode = pipe_inode;
  read_file->f_ops = &g_pipe_read_fops;
  read_file->flags = O_RDONLY;
  read_file->private_data = ps;
  pipe_inode->ref();
  ps->readers = 1;

  // 4. Allocate write-end File
  write_file = alloc_file();
  if (write_file == nullptr) {
    pipe_inode->unref();
    free_inode(pipe_inode);
    free_file(read_file);
    read_file = nullptr;
    free_pipe_state(ps);
    return -static_cast<long>(VfsError::NoMemory);
  }
  write_file->inode = pipe_inode;
  write_file->f_ops = &g_pipe_write_fops;
  write_file->flags = O_WRONLY;
  write_file->private_data = ps;
  pipe_inode->ref();
  ps->writers = 1;

  return 0;
}

} // namespace pipefs

// ====================================================================
// devfs implementation — /dev/console, /dev/null, /dev/zero
// ====================================================================

namespace devfs {

namespace uart = moss::kernel::hal::uart;

// -- Inode number assignments --
static constexpr InodeNumber DEVFS_ROOT_INO = 100;
static constexpr InodeNumber DEVFS_CONSOLE_INO = 101;
static constexpr InodeNumber DEVFS_NULL_INO = 102;
static constexpr InodeNumber DEVFS_ZERO_INO = 103;

// -- /dev/console file operations --

static long console_open([[maybe_unused]] File *file, [[maybe_unused]] Inode *inode,
                         [[maybe_unused]] u32 flags) noexcept {
  return 0;
}

static long console_release([[maybe_unused]] File *file) noexcept { return 0; }

static long console_read([[maybe_unused]] File *file, OutputBuffer buffer) noexcept {
  // Interrupt-driven, line-buffered console input with echo.
  // Each moss::abi::bridge::console_getc_blocking() call either returns instantly from the
  // ring buffer (fast path) or blocks the calling thread until the UART
  // RX interrupt delivers a character (slow path).  The CPU enters idle
  // (WFI) while blocked, so host CPU usage is ~0%.
  const usize count = buffer.size();
  usize pos = 0;
  while (pos < count) {
    int ch = moss::abi::bridge::console_getc_blocking();
    if (ch < 0) {
      continue;
    }

    // Handle backspace (DEL=0x7F or BS=0x08)
    if (ch == 0x7F || ch == 0x08) {
      if (pos > 0) {
        --pos;
        uart::putc('\b');
        uart::putc(' ');
        uart::putc('\b');
      }
      continue;
    }

    // Handle Ctrl+C → discard line, print "^C\n", restart
    if (ch == 0x03) {
      uart::putc('^');
      uart::putc('C');
      uart::putc('\r');
      uart::putc('\n');
      pos = 0;
      continue;
    }

    // Handle Enter (CR or LF)
    if (ch == '\r' || ch == '\n') {
      const u8 newline = '\n';
      if (buffer.copy_from(pos, &newline, 1) != 1)
        return pos ? static_cast<long>(pos) : -static_cast<long>(VfsError::BadAddress);
      ++pos;
      uart::putc('\r');
      uart::putc('\n');
      break;
    }

    // Echo printable characters and store in buffer
    const auto byte = static_cast<u8>(ch);
    if (buffer.copy_from(pos, &byte, 1) != 1)
      return pos ? static_cast<long>(pos) : -static_cast<long>(VfsError::BadAddress);
    uart::putc(static_cast<char>(ch));
    ++pos;
  }

  return static_cast<long>(pos);
}

static long console_write([[maybe_unused]] File *file, InputBuffer buffer) noexcept {
  const usize count = buffer.size();
  for (usize i = 0; i < count; ++i) {
    char c = 0;
    if (buffer.copy_to(i, &c, 1) != 1)
      return i ? static_cast<long>(i) : -static_cast<long>(VfsError::BadAddress);
    if (c == '\n') {
      uart::putc('\r');
    }
    uart::putc(c);
  }
  return static_cast<long>(count);
}

static long console_lseek([[maybe_unused]] File *file, [[maybe_unused]] i64 offset,
                          [[maybe_unused]] SeekWhence whence) noexcept {
  return -static_cast<long>(VfsError::IsPipe); // not seekable
}

static long console_ioctl([[maybe_unused]] File *file, u32 command, u64 argument) noexcept {
  if (command != MOSS_IOCTL_ISATTY)
    return -static_cast<long>(VfsError::NotTerminal);
  return argument ? -static_cast<long>(VfsError::InvalidArg) : 0;
}

static const FileOps g_console_fops = {
    .open = console_open,
    .release = console_release,
    .read = console_read,
    .write = console_write,
    .lseek = console_lseek,
    .ioctl = console_ioctl,
};

// -- /dev/null file operations --

static long null_read([[maybe_unused]] File *file, [[maybe_unused]] OutputBuffer buffer) noexcept {
  return 0; // EOF
}

static long null_write([[maybe_unused]] File *file, InputBuffer buffer) noexcept {
  return static_cast<long>(buffer.size()); // discard without accessing the source
}

static const FileOps g_null_fops = {
    .open = console_open,
    .release = console_release,
    .read = null_read,
    .write = null_write,
    .lseek = console_lseek,
    .ioctl = nullptr,
};

// -- /dev/zero file operations --

static long zero_read([[maybe_unused]] File *file, OutputBuffer buffer) noexcept {
  const u8 zeros[128]{};
  usize copied = 0;
  const usize count = buffer.size();
  while (copied < count) {
    usize chunk = count - copied;
    if (chunk > sizeof(zeros))
      chunk = sizeof(zeros);
    const usize done = buffer.copy_from(copied, zeros, chunk);
    copied += done;
    if (done != chunk)
      break;
  }
  return !copied && count ? -static_cast<long>(VfsError::BadAddress) : static_cast<long>(copied);
}

static const FileOps g_zero_fops = {
    .open = console_open,
    .release = console_release,
    .read = zero_read,
    .write = null_write, // discard writes
    .lseek = console_lseek,
    .ioctl = nullptr,
};

// -- devfs directory inode_ops --

static Dentry *devfs_lookup(Inode *dir, const char *name, u32 name_len) noexcept;

static const InodeOps g_devfs_dir_iops = {
    .lookup = devfs_lookup,
    .create = nullptr,
};

// -- Static storage for devfs objects --
static SuperBlock g_devfs_sb;
static bool g_devfs_initialized = false;

static Dentry *devfs_lookup(Inode *dir, const char *name, u32 name_len) noexcept {
  // Linear scan of children
  for (u32 i = 0; i < dir->child_count; ++i) {
    Dentry *d = dir->children[i];
    if (d != nullptr && d->name_len == name_len) {
      bool eq = true;
      for (u32 j = 0; j < name_len; ++j) {
        if (d->name[j] != name[j]) {
          eq = false;
          break;
        }
      }
      if (eq) {
        return d;
      }
    }
  }
  return nullptr;
}

SuperBlock *devfs_init() noexcept {
  if (g_devfs_initialized) {
    return &g_devfs_sb;
  }

  // Create devfs superblock
  g_devfs_sb.fs_name = "devfs";
  g_devfs_sb.device = 2;
  g_devfs_sb.block_size = PAGE_SIZE;
  g_devfs_sb.fs_private = nullptr;

  // Root directory inode for /dev
  Inode *root_inode = alloc_inode();
  if (root_inode == nullptr) {
    return nullptr;
  }
  root_inode->ino = DEVFS_ROOT_INO;
  root_inode->type = FileType::Directory;
  root_inode->mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
  root_inode->sb = &g_devfs_sb;
  root_inode->inode_ops = &g_devfs_dir_iops;

  // Root dentry for /dev
  Dentry *root_dentry = alloc_dentry("dev", 3, root_inode, nullptr);
  if (root_dentry == nullptr) {
    return nullptr;
  }

  g_devfs_sb.root_inode = root_inode;

  // -- Create /dev/console --
  Inode *console_inode = alloc_inode();
  if (console_inode == nullptr) {
    return nullptr;
  }
  console_inode->ino = DEVFS_CONSOLE_INO;
  console_inode->type = FileType::CharDev;
  console_inode->mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
  console_inode->sb = &g_devfs_sb;
  console_inode->file_ops = &g_console_fops;
  auto *console_d = alloc_dentry("console", 7, console_inode, root_dentry);
  if (console_d == nullptr) {
    return nullptr;
  }

  // -- Create /dev/null --
  Inode *null_inode = alloc_inode();
  if (null_inode == nullptr) {
    return nullptr;
  }
  null_inode->ino = DEVFS_NULL_INO;
  null_inode->type = FileType::CharDev;
  null_inode->mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
  null_inode->sb = &g_devfs_sb;
  null_inode->file_ops = &g_null_fops;
  auto *null_d = alloc_dentry("null", 4, null_inode, root_dentry);
  if (null_d == nullptr) {
    return nullptr;
  }

  // -- Create /dev/zero --
  Inode *zero_inode = alloc_inode();
  if (zero_inode == nullptr) {
    return nullptr;
  }
  zero_inode->ino = DEVFS_ZERO_INO;
  zero_inode->type = FileType::CharDev;
  zero_inode->mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
  zero_inode->sb = &g_devfs_sb;
  zero_inode->file_ops = &g_zero_fops;
  auto *zero_d = alloc_dentry("zero", 4, zero_inode, root_dentry);
  if (zero_d == nullptr) {
    return nullptr;
  }

  // RX must be ready before userspace can print a prompt. Lazy initialization
  // on the first read resets the PL011 FIFO and discards already-typed input.
  moss::abi::bridge::console_rx_init();
  g_devfs_initialized = true;
  return &g_devfs_sb;
}

const FileOps &console_file_ops() noexcept { return g_console_fops; }

} // namespace devfs

// ====================================================================
// ramfs implementation — read-only FS backed by initramfs CPIO
// ====================================================================

namespace ramfs {

namespace initramfs = moss::kernel::initramfs;
namespace log = moss::kernel::logging;

static constexpr InodeNumber RAMFS_ROOT_INO = 1;

// -- ramfs regular file operations --

static long ramfs_read(File *file, OutputBuffer buffer) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  if (file == nullptr || file->inode == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  Inode *inode = file->inode;
  if (inode->data == nullptr) {
    return 0;
  }

  // Clamp to remaining data from current position
  auto pos = static_cast<u64>(file->pos);
  if (pos >= inode->size) {
    return 0;
  }

  usize avail = static_cast<usize>(inode->size - pos);
  usize count = buffer.size();
  if (count > avail) {
    count = avail;
  }

  const usize copied = buffer.copy_from(0, inode->data + pos, count);
  file->pos += static_cast<i64>(copied);
  return !copied && count ? -static_cast<long>(VfsError::BadAddress) : static_cast<long>(copied);
}

static long ramfs_write(File *file, InputBuffer buffer) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  auto *inode = file->inode;
  if (!inode->ramfs_mutable)
    return -static_cast<long>(VfsError::PermDenied);
  if (file->pos < 0)
    return -static_cast<long>(VfsError::InvalidArg);
  if (!buffer.size())
    return 0;
  const u64 position = file->flags & O_APPEND ? inode->size : static_cast<u64>(file->pos);
  if (position > 0x7fffffffffffffffULL || buffer.size() > 0x7fffffffffffffffULL - position)
    return -static_cast<long>(VfsError::FileTooLarge);
  const usize end = position + buffer.size();
  usize capacity = inode->data_capacity;
  auto *data = const_cast<u8 *>(inode->data);
  const bool grows = end > capacity;
  if (grows) {
    // ponytail: contiguous, geometrically grown file buffers; use page-backed
    // storage if large/sparse files exhaust contiguous kernel heap space.
    capacity = capacity && capacity <= 0x3fffffffffffffffULL ? capacity * 2 : end;
    if (capacity < end)
      capacity = end;
    capacity = (capacity + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    auto allocation = mm::RuntimeHeapAllocator::allocate(capacity);
    if (!allocation)
      return -static_cast<long>(VfsError::NoMemory);
    data = static_cast<u8 *>(*allocation);
    if (inode->size)
      __builtin_memcpy(data, inode->data, inode->size);
  }
  if (position > inode->size)
    __builtin_memset(data + inode->size, 0, position - inode->size);
  const usize copied = buffer.copy_to(0, data + position, buffer.size());
  if (!copied) {
    if (grows)
      (void)mm::RuntimeHeapAllocator::deallocate(data, capacity);
    return -static_cast<long>(VfsError::BadAddress);
  }
  if (grows) {
    if (inode->data)
      (void)mm::RuntimeHeapAllocator::deallocate(const_cast<u8 *>(inode->data), inode->data_capacity);
    inode->data = data;
    inode->data_capacity = capacity;
  }
  file->pos = static_cast<i64>(position + copied);
  if (position + copied > inode->size)
    inode->size = position + copied;
  return static_cast<long>(copied);
}

static long ramfs_open(File *file, Inode *inode, u32 flags) noexcept {
  (void)file;
  if (!(flags & O_TRUNC))
    return 0;
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  if (!inode->ramfs_mutable)
    return -static_cast<long>(VfsError::PermDenied);
  if (inode->data)
    (void)mm::RuntimeHeapAllocator::deallocate(const_cast<u8 *>(inode->data), inode->data_capacity);
  inode->data = nullptr;
  inode->size = inode->data_capacity = 0;
  return 0;
}

static long ramfs_lseek(File *file, i64 offset, SeekWhence whence) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  if (file == nullptr || file->inode == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }

  i64 new_pos;
  switch (whence) {
  case SeekWhence::Set:
    new_pos = offset;
    break;
  case SeekWhence::Current:
    if (__builtin_add_overflow(file->pos, offset, &new_pos))
      return -static_cast<long>(VfsError::Overflow);
    break;
  case SeekWhence::End:
    if (file->inode->size > 0x7fffffffffffffffULL ||
        __builtin_add_overflow(static_cast<i64>(file->inode->size), offset, &new_pos))
      return -static_cast<long>(VfsError::Overflow);
    break;
  default:
    return -static_cast<long>(VfsError::InvalidArg);
  }
  if (new_pos < 0) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  file->pos = new_pos;
  return new_pos;
}

static const FileOps g_ramfs_file_fops = {
    .open = ramfs_open,
    .release = nullptr,
    .read = ramfs_read,
    .write = ramfs_write,
    .lseek = ramfs_lseek,
    .ioctl = nullptr,
};

// -- ramfs directory inode_ops --

static Dentry *ramfs_dir_lookup(Inode *dir, const char *name, u32 name_len) noexcept {
  for (u32 i = 0; i < dir->child_count; ++i) {
    Dentry *d = dir->children[i];
    if (d != nullptr && d->name_len == name_len) {
      bool eq = true;
      for (u32 j = 0; j < name_len; ++j) {
        if (d->name[j] != name[j]) {
          eq = false;
          break;
        }
      }
      if (eq) {
        return d;
      }
    }
  }
  return nullptr;
}

static long ramfs_create(Inode *dir, const char *name, u32 length, FileType type, u32 mode) noexcept;
static long ramfs_remove(Inode *dir, Dentry *child) noexcept;
static long ramfs_rename(Dentry *source, Dentry *parent, const char *name, u32 name_len) noexcept;
static const InodeOps g_ramfs_dir_iops = {
    .lookup = ramfs_dir_lookup,
    .create = ramfs_create,
    .remove = ramfs_remove,
    .rename = ramfs_rename,
};

// -- ramfs superblock --
static SuperBlock g_ramfs_sb;
static bool g_ramfs_initialized = false;
static InodeNumber g_ramfs_next_ino = RAMFS_ROOT_INO + 1;

static long ramfs_create(Inode *dir, const char *name, u32 length, FileType type, u32 mode) noexcept {
  // Called with namespace_lock held. Published directory entries remain owned
  // by the namespace until the removal path drops that ownership.
  if (type != FileType::Directory && type != FileType::Regular)
    return -static_cast<long>(VfsError::NotSupported);
  if (!dir || !dir->is_directory())
    return -static_cast<long>(VfsError::NotDirectory);
  if (ramfs_dir_lookup(dir, name, length))
    return -static_cast<long>(VfsError::FileExists);
  Dentry *parent = find_dentry_for_inode(dir);
  if (!parent)
    return -static_cast<long>(VfsError::NoEntry);
  Inode *inode = alloc_inode();
  if (!inode)
    return -static_cast<long>(VfsError::NoMemory);
  inode->ino = g_ramfs_next_ino++;
  inode->type = type;
  inode->mode = (type == FileType::Directory ? S_IFDIR : S_IFREG) | (mode & 0777U);
  inode->nlink = type == FileType::Directory ? 2 : 1;
  inode->sb = dir->sb;
  inode->inode_ops = type == FileType::Directory ? &g_ramfs_dir_iops : nullptr;
  inode->file_ops = type == FileType::Regular ? &g_ramfs_file_fops : nullptr;
  inode->ramfs_mutable = type == FileType::Regular;
  if (!alloc_dentry(name, length, inode, parent)) {
    inode->unref();
    free_inode(inode);
    return -static_cast<long>(VfsError::NoMemory);
  }
  if (type == FileType::Directory)
    ++dir->nlink;
  return 0;
}

static long ramfs_detach(Inode *dir, Dentry *child) noexcept {
  u32 index = 0;
  while (index < dir->child_count && dir->children[index] != child)
    ++index;
  if (index == dir->child_count)
    return -static_cast<long>(VfsError::NoEntry);
  for (u32 i = index + 1; i < dir->child_count; ++i)
    dir->children[i - 1] = dir->children[i];
  dir->children[--dir->child_count] = nullptr;
  if (child->inode->is_directory())
    --dir->nlink;
  g_dcache.remove(child);
  return 0;
}

static long ramfs_remove(Inode *dir, Dentry *child) noexcept {
  if (!child || !child->inode)
    return -static_cast<long>(VfsError::NoEntry);
  if (child->inode->child_count)
    return -static_cast<long>(VfsError::NotEmpty);
  if (long result = ramfs_detach(dir, child); result < 0)
    return result;
  child->inode->nlink = 0;
  release_dentry(child);
  return 0;
}

static long ramfs_rename(Dentry *source, Dentry *parent, const char *name, u32 name_len) noexcept {
  // The VFS holds namespace_lock throughout validation and commit. Reuse the
  // source dentry so open files and child entries retain the same identity.
  auto *old_parent = source->parent;
  auto *old_dir = old_parent->inode;
  auto *new_dir = parent->inode;
  if (ramfs_dir_lookup(old_dir, source->name, source->name_len) != source)
    return -static_cast<long>(VfsError::NoEntry);
  for (auto *ancestor = parent; ancestor; ancestor = ancestor->parent)
    if (ancestor == source)
      return -static_cast<long>(VfsError::InvalidArg);
  auto *target = ramfs_dir_lookup(new_dir, name, name_len);
  if (target == source)
    return 0;
  if (target) {
    if (source->inode->is_directory() != target->inode->is_directory())
      return -static_cast<long>(target->inode->is_directory() ? VfsError::IsDirectory : VfsError::NotDirectory);
    if (target->inode->child_count)
      return -static_cast<long>(VfsError::NotEmpty);
  } else if (old_parent != parent && new_dir->child_count == Inode::MAX_CHILDREN) {
    return -static_cast<long>(VfsError::NoMemory);
  }
  // No allocations or fallible copy operations after this point. All removed
  // entries were checked above while the namespace was exclusively held.
  if (target)
    (void)ramfs_remove(new_dir, target);
  (void)ramfs_detach(old_dir, source);
  parent->ref();
  source->parent = parent;
  __builtin_memcpy(source->name, name, name_len);
  source->name[name_len] = 0;
  source->name_len = name_len;
  new_dir->children[new_dir->child_count++] = source;
  if (source->inode->is_directory())
    ++new_dir->nlink;
  g_dcache.insert(source);
  release_dentry(old_parent);
  return 0;
}

SuperBlock *ramfs_init() noexcept {
  if (g_ramfs_initialized) {
    return &g_ramfs_sb;
  }

  auto &archive = initramfs::g_initramfs;
  if (!archive.is_initialized()) {
    log::klog::warn("ramfs: initramfs not initialized, skipping");
    // Still create an empty root
  }

  g_ramfs_sb.fs_name = "ramfs";
  g_ramfs_sb.device = 1;
  g_ramfs_sb.block_size = moss::kernel::PAGE_SIZE;
  g_ramfs_sb.fs_private = nullptr;

  // Create root directory inode
  Inode *root_inode = alloc_inode();
  if (root_inode == nullptr) {
    return nullptr;
  }
  root_inode->ino = RAMFS_ROOT_INO;
  root_inode->type = FileType::Directory;
  root_inode->nlink = 2;
  root_inode->mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
  root_inode->sb = &g_ramfs_sb;
  root_inode->inode_ops = &g_ramfs_dir_iops;

  // Root dentry (parent = nullptr for filesystem root)
  Dentry *root_dentry = alloc_dentry("/", 1, root_inode, nullptr);
  if (root_dentry == nullptr) {
    return nullptr;
  }

  g_ramfs_sb.root_inode = root_inode;

  // Populate from initramfs entries
  if (archive.is_initialized()) {
    archive.for_each([&](const initramfs::InitramfsEntry &entry) {
      Inode *file_inode = alloc_inode();
      if (file_inode == nullptr) {
        return;
      }

      file_inode->ino = g_ramfs_next_ino++;
      file_inode->type = FileType::Regular;
      file_inode->mode = S_IFREG | (entry.mode & 0777U);
      file_inode->size = entry.data_size;
      file_inode->data = entry.data;
      file_inode->sb = &g_ramfs_sb;
      file_inode->file_ops = &g_ramfs_file_fops;

      // Compute name length
      u32 nlen = 0;
      while (entry.name[nlen] != '\0' && nlen < MAX_NAME_LEN) {
        ++nlen;
      }

      auto *d = alloc_dentry(entry.name, nlen, file_inode, root_dentry);
      if (d == nullptr) {
        return;
      }

      log::klog::info("ramfs: mounted '{}' ({} bytes, ino={})", entry.name, entry.data_size, file_inode->ino);
    });
  }

  g_ramfs_initialized = true;
  log::klog::info("ramfs: initialized with {} children", root_inode->child_count);
  return &g_ramfs_sb;
}

} // namespace ramfs

// ====================================================================
// VFS initialization — mount root + devfs + stdio
// ====================================================================

namespace log = moss::kernel::logging;

/// Helper: find a dentry in the pool by scanning for matching inode.
static Dentry *find_dentry_for_inode(Inode *inode) noexcept {
  for (u32 i = 0; i < MAX_DENTRIES; ++i) {
    if (g_dentry_used[i] && g_dentry_pool[i].inode == inode) {
      return &g_dentry_pool[i];
    }
  }
  return nullptr;
}

void vfs_init() noexcept {
  g_mount_table.init();
  g_dcache.init();

  // 1. Mount ramfs as root filesystem "/"
  auto *ramfs_sb = ramfs::ramfs_init();
  if (ramfs_sb != nullptr && ramfs_sb->root_inode != nullptr) {
    Dentry *ramfs_root = find_dentry_for_inode(ramfs_sb->root_inode);
    if (ramfs_root != nullptr) {
      g_mount_table.mount("/", ramfs_sb, ramfs_root);
      log::klog::info("vfs: mounted ramfs at /");
    }
  } else {
    log::klog::warn("vfs: ramfs_init failed, no root filesystem");
  }

  // 2. Mount devfs at "/dev"
  auto *devfs_sb = devfs::devfs_init();
  if (devfs_sb != nullptr && devfs_sb->root_inode != nullptr) {
    Dentry *devfs_root = find_dentry_for_inode(devfs_sb->root_inode);
    if (devfs_root != nullptr) {
      g_mount_table.mount("/dev", devfs_sb, devfs_root);
      log::klog::info("vfs: mounted devfs at /dev");
    }
  }

  log::klog::info("vfs: initialization complete");
}

void vfs_init_stdio(void *fd_table) noexcept {
  if (fd_table == nullptr) {
    return;
  }

  auto *fdt = static_cast<FdTable *>(fd_table);

  // Use the ordinary open path so stdio retains the same inode/dentry owners.
  for (int i = 0; i < 3; ++i) {
    if (syscall::do_open(fdt, "/dev/console", O_RDWR, 0) < 0) {
      log::klog::warn("vfs: cannot open /dev/console for stdio");
      break;
    }
  }

  log::klog::info("vfs: stdio initialized (fd 0/1/2 -> /dev/console)");
}

} // namespace moss::kernel::vfs
