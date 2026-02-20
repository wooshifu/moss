// MOSS VFS initialization — implementation
// Mounts root ramfs, devfs, and sets up stdio for PID 1.

module;

module moss.vfs;

namespace moss::kernel::vfs {

// -- MountTable implementation --

long MountTable::mount(const char* path, SuperBlock* sb, Dentry* root) noexcept {
    if (count_ >= MAX_MOUNTS) {
        return -static_cast<long>(VfsError::NoMemory);
    }

    auto& entry = mounts_[count_];
    // Copy path
    u32 len = 0;
    while (path[len] != '\0' && len < MAX_PATH_LEN - 1) {
        entry.path[len] = path[len];
        ++len;
    }
    entry.path[len] = '\0';
    entry.path_len = len;
    entry.sb = sb;
    entry.root = root;
    entry.active = true;
    ++count_;
    return 0;
}

bool MountTable::lookup(const char* path, MountLookupResult& result) noexcept {
    // Find the deepest (longest prefix) matching mount point
    u32 best_len = 0;
    MountEntry* best = nullptr;

    for (u32 i = 0; i < count_; ++i) {
        if (!mounts_[i].active) continue;

        auto& m = mounts_[i];
        // Check if path starts with mount path
        bool match = true;
        for (u32 j = 0; j < m.path_len; ++j) {
            if (path[j] == '\0' || path[j] != m.path[j]) {
                match = false;
                break;
            }
        }

        // Ensure the match is at a path boundary
        if (match && m.path_len > best_len) {
            // Root "/" matches everything; others need boundary check
            if (m.path_len == 1 && m.path[0] == '/') {
                best_len = m.path_len;
                best = &m;
            } else if (path[m.path_len] == '\0' || path[m.path_len] == '/') {
                best_len = m.path_len;
                best = &m;
            }
        }
    }

    if (best == nullptr) return false;

    result.mount = best;
    // Compute residual: skip mount prefix and leading '/'
    const char* r = path + best_len;
    if (*r == '/') ++r;
    result.residual = r;
    u32 rlen = 0;
    while (r[rlen] != '\0') ++rlen;
    result.residual_len = rlen;
    return true;
}

// -- DentryCache implementation --

void DentryCache::init() noexcept {
    for (u32 i = 0; i < CACHE_SIZE; ++i) {
        slots_[i] = nullptr;
    }
}

u32 DentryCache::hash(const Dentry* parent, const char* name, u32 name_len) noexcept {
    // FNV-1a inspired hash
    u32 h = 2166136261u;
    // Mix in parent address
    auto addr = reinterpret_cast<u64>(parent);
    for (u32 i = 0; i < 8; ++i) {
        h ^= static_cast<u32>(addr & 0xFF);
        h *= 16777619u;
        addr >>= 8;
    }
    // Mix in name
    for (u32 i = 0; i < name_len; ++i) {
        h ^= static_cast<u32>(static_cast<u8>(name[i]));
        h *= 16777619u;
    }
    return h % CACHE_SIZE;
}

void DentryCache::insert(Dentry* dentry) noexcept {
    if (dentry == nullptr) return;
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

Dentry* DentryCache::lookup(const Dentry* parent,
                             const char* name, u32 name_len) noexcept {
    u32 idx = hash(parent, name, name_len);
    for (u32 i = 0; i < CACHE_SIZE; ++i) {
        u32 slot = (idx + i) % CACHE_SIZE;
        Dentry* d = slots_[slot];
        if (d == nullptr) return nullptr;  // empty slot → miss
        if (d->parent == parent && d->name_len == name_len) {
            // Compare names
            bool eq = true;
            for (u32 j = 0; j < name_len; ++j) {
                if (d->name[j] != name[j]) { eq = false; break; }
            }
            if (eq) return d;
        }
    }
    return nullptr;
}

// -- Inode / Dentry / File pool allocators --

static constexpr u32 MAX_INODES = 256;
static constexpr u32 MAX_DENTRIES = 256;

static Inode  g_inode_pool[MAX_INODES];
static u32    g_inode_count = 0;

static Dentry g_dentry_pool[MAX_DENTRIES];
static u32    g_dentry_count = 0;

static File   g_file_pool[MAX_FILES];
static bool   g_file_used[MAX_FILES];
static bool   g_file_pool_initialized = false;

Inode* alloc_inode() noexcept {
    if (g_inode_count >= MAX_INODES) return nullptr;
    auto* inode = &g_inode_pool[g_inode_count++];
    // Zero-initialize
    inode->ino = INVALID_INO;
    inode->type = FileType::Regular;
    inode->mode = 0;
    inode->nlink = 1;
    inode->size = 0;
    inode->rdev = NO_DEVICE;
    inode->sb = nullptr;
    inode->file_ops = nullptr;
    inode->inode_ops = nullptr;
    inode->data = nullptr;
    inode->private_data = nullptr;
    inode->child_count = 0;
    for (u32 i = 0; i < Inode::MAX_CHILDREN; ++i) {
        inode->children[i] = nullptr;
    }
    inode->ref_count = 1;
    return inode;
}

Dentry* alloc_dentry(const char* name, u32 name_len,
                      Inode* inode, Dentry* parent) noexcept {
    if (g_dentry_count >= MAX_DENTRIES) return nullptr;
    if (name_len > MAX_NAME_LEN) return nullptr;

    auto* d = &g_dentry_pool[g_dentry_count++];
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
        auto* pi = parent->inode;
        if (pi->child_count < Inode::MAX_CHILDREN) {
            pi->children[pi->child_count++] = d;
        }
    }

    // Insert into dcache
    g_dcache.insert(d);

    return d;
}

File* alloc_file() noexcept {
    if (!g_file_pool_initialized) {
        for (u32 i = 0; i < MAX_FILES; ++i) {
            g_file_used[i] = false;
        }
        g_file_pool_initialized = true;
    }
    for (u32 i = 0; i < MAX_FILES; ++i) {
        if (!g_file_used[i]) {
            g_file_used[i] = true;
            auto* f = &g_file_pool[i];
            f->inode = nullptr;
            f->dentry = nullptr;
            f->f_ops = nullptr;
            f->flags = 0;
            f->pos = 0;
            f->private_data = nullptr;
            f->ref_count = 0;
            return f;
        }
    }
    return nullptr;
}

void free_file(File* file) noexcept {
    if (file == nullptr) return;
    auto offset = static_cast<u32>(file - g_file_pool);
    if (offset < MAX_FILES) {
        g_file_used[offset] = false;
    }
}

// -- FdTable implementation --

void FdTable::release_file(File* f) noexcept {
    if (f == nullptr) return;
    f->unref();
    if (f->ref_count == 0) {
        // Call filesystem release if available
        if (f->f_ops != nullptr && f->f_ops->release != nullptr) {
            f->f_ops->release(f);
        }
        if (f->inode != nullptr) {
            f->inode->unref();
        }
        free_file(f);
    }
}

FdTable* FdTable::clone() const noexcept {
    // Allocate from heap (kernel operator new panics on OOM)
    auto* raw = new char[sizeof(FdTable)];
    auto* table = new (raw) FdTable();
    table->init();
    for (u32 i = 0; i < MAX_FDS; ++i) {
        if (fds_[i] != nullptr) {
            table->fds_[i] = fds_[i];
            fds_[i]->ref();
        }
    }
    return table;
}

// ====================================================================
// pipefs implementation — anonymous pipes with 4KB ring buffer
// ====================================================================

namespace pipefs {

/// Pipe internal state — 4KB ring buffer shared between read and write ends.
struct PipeState {
    static constexpr u32 PIPE_BUF_SIZE = 4096;

    u8 buffer[PIPE_BUF_SIZE];
    u32 read_pos;
    u32 write_pos;
    u32 count;          // bytes currently in buffer
    u32 readers;        // number of open read-end File objects
    u32 writers;        // number of open write-end File objects
};

static constexpr u32 MAX_PIPES = 64;
static PipeState g_pipe_pool[MAX_PIPES];
static bool g_pipe_used[MAX_PIPES];
static bool g_pipe_pool_initialized = false;

static PipeState* alloc_pipe_state() noexcept {
    if (!g_pipe_pool_initialized) {
        for (u32 i = 0; i < MAX_PIPES; ++i) {
            g_pipe_used[i] = false;
        }
        g_pipe_pool_initialized = true;
    }
    for (u32 i = 0; i < MAX_PIPES; ++i) {
        if (!g_pipe_used[i]) {
            g_pipe_used[i] = true;
            auto* ps = &g_pipe_pool[i];
            ps->read_pos = 0;
            ps->write_pos = 0;
            ps->count = 0;
            ps->readers = 0;
            ps->writers = 0;
            return ps;
        }
    }
    return nullptr;
}

static void free_pipe_state(PipeState* ps) noexcept {
    if (ps == nullptr) return;
    auto offset = static_cast<u32>(ps - g_pipe_pool);
    if (offset < MAX_PIPES) {
        g_pipe_used[offset] = false;
    }
}

// -- Pipe read-end file operations --

static long pipe_read_release(File* file) noexcept {
    if (file == nullptr || file->private_data == nullptr) return 0;
    auto* ps = static_cast<PipeState*>(file->private_data);
    if (ps->readers > 0) --ps->readers;
    // Free PipeState when both ends are closed
    if (ps->readers == 0 && ps->writers == 0) {
        free_pipe_state(ps);
    }
    return 0;
}

static long pipe_read(File* file, u8* buf, usize count) noexcept {
    if (file == nullptr || file->private_data == nullptr) {
        return -static_cast<long>(VfsError::InvalidArg);
    }
    auto* ps = static_cast<PipeState*>(file->private_data);

    // Nothing to read
    if (ps->count == 0) {
        // Write end closed → EOF
        if (ps->writers == 0) return 0;
        // Write end still open but buffer empty → would block; return 0 (non-blocking)
        return 0;
    }

    // Read up to min(count, available)
    usize avail = ps->count;
    if (count > avail) count = avail;

    for (usize i = 0; i < count; ++i) {
        buf[i] = ps->buffer[ps->read_pos];
        ps->read_pos = (ps->read_pos + 1) % PipeState::PIPE_BUF_SIZE;
    }
    ps->count -= static_cast<u32>(count);

    return static_cast<long>(count);
}

static long pipe_read_write([[maybe_unused]] File* file,
                             [[maybe_unused]] const u8* buf,
                             [[maybe_unused]] usize count) noexcept {
    return -static_cast<long>(VfsError::BadFd);  // cannot write to read end
}

static long pipe_lseek([[maybe_unused]] File* file,
                        [[maybe_unused]] i64 offset,
                        [[maybe_unused]] SeekWhence whence) noexcept {
    return -static_cast<long>(VfsError::IsPipe);
}

static const FileOps g_pipe_read_fops = {
    .open    = nullptr,
    .release = pipe_read_release,
    .read    = pipe_read,
    .write   = pipe_read_write,
    .lseek   = pipe_lseek,
    .ioctl   = nullptr,
};

// -- Pipe write-end file operations --

static long pipe_write_release(File* file) noexcept {
    if (file == nullptr || file->private_data == nullptr) return 0;
    auto* ps = static_cast<PipeState*>(file->private_data);
    if (ps->writers > 0) --ps->writers;
    if (ps->readers == 0 && ps->writers == 0) {
        free_pipe_state(ps);
    }
    return 0;
}

static long pipe_write(File* file, const u8* buf, usize count) noexcept {
    if (file == nullptr || file->private_data == nullptr) {
        return -static_cast<long>(VfsError::InvalidArg);
    }
    auto* ps = static_cast<PipeState*>(file->private_data);

    // Read end closed → broken pipe
    if (ps->readers == 0) {
        return -static_cast<long>(VfsError::InvalidArg);  // EPIPE equivalent
    }

    // Write up to min(count, free space)
    u32 free_space = PipeState::PIPE_BUF_SIZE - ps->count;
    if (count > free_space) count = free_space;
    if (count == 0) return 0;  // buffer full, non-blocking

    for (usize i = 0; i < count; ++i) {
        ps->buffer[ps->write_pos] = buf[i];
        ps->write_pos = (ps->write_pos + 1) % PipeState::PIPE_BUF_SIZE;
    }
    ps->count += static_cast<u32>(count);

    return static_cast<long>(count);
}

static long pipe_write_read([[maybe_unused]] File* file,
                             [[maybe_unused]] u8* buf,
                             [[maybe_unused]] usize count) noexcept {
    return -static_cast<long>(VfsError::BadFd);  // cannot read from write end
}

static const FileOps g_pipe_write_fops = {
    .open    = nullptr,
    .release = pipe_write_release,
    .read    = pipe_write_read,
    .write   = pipe_write,
    .lseek   = pipe_lseek,
    .ioctl   = nullptr,
};

// -- Pipe inode (shared between read/write ends) --

static constexpr InodeNumber PIPEFS_BASE_INO = 10000;
static InodeNumber g_pipefs_next_ino = PIPEFS_BASE_INO;

long create_pipe(File*& read_file, File*& write_file) noexcept {
    // 1. Allocate pipe state
    PipeState* ps = alloc_pipe_state();
    if (ps == nullptr) {
        return -static_cast<long>(VfsError::NoMemory);
    }

    // 2. Allocate shared pipe inode
    Inode* pipe_inode = alloc_inode();
    if (pipe_inode == nullptr) {
        free_pipe_state(ps);
        return -static_cast<long>(VfsError::NoMemory);
    }
    pipe_inode->ino = g_pipefs_next_ino++;
    pipe_inode->type = FileType::Fifo;
    pipe_inode->mode = S_IFIFO | S_IRUSR | S_IWUSR;
    pipe_inode->private_data = ps;

    // 3. Allocate read-end File
    read_file = alloc_file();
    if (read_file == nullptr) {
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

static long console_open([[maybe_unused]] File* file,
                          [[maybe_unused]] Inode* inode,
                          [[maybe_unused]] u32 flags) noexcept {
    return 0;
}

static long console_release([[maybe_unused]] File* file) noexcept {
    return 0;
}

static long console_read([[maybe_unused]] File* file,
                          u8* buf, usize count) noexcept {
    // Polling-based, line-buffered console input with echo.
    // Reads characters from UART RX FIFO, echoes them back, and returns
    // when a newline is received or the buffer is full.
    usize pos = 0;
    while (pos < count) {
        int ch = uart::getc();
        if (ch < 0) {
            // No data available
            if (pos > 0) break;  // Return partial line if we have data
            // Low-power wait: WFE sleeps until next event (timer IRQ wakes us)
#if defined(__aarch64__)
            asm volatile("wfe");
#else
            // x86_64 / RISC-V: pause hint
            asm volatile("" ::: "memory");
#endif
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
            uart::putc('\r');
            uart::putc('\n');
            if (pos < count) buf[pos++] = '\n';
            break;
        }

        // Echo printable characters and store in buffer
        uart::putc(static_cast<char>(ch));
        buf[pos++] = static_cast<u8>(ch);
    }
    return static_cast<long>(pos);
}

static long console_write([[maybe_unused]] File* file,
                           const u8* buf, usize count) noexcept {
    for (usize i = 0; i < count; ++i) {
        char c = static_cast<char>(buf[i]);
        if (c == '\n') uart::putc('\r');
        uart::putc(c);
    }
    return static_cast<long>(count);
}

static long console_lseek([[maybe_unused]] File* file,
                           [[maybe_unused]] i64 offset,
                           [[maybe_unused]] SeekWhence whence) noexcept {
    return -static_cast<long>(VfsError::IsPipe);  // not seekable
}

static const FileOps g_console_fops = {
    .open    = console_open,
    .release = console_release,
    .read    = console_read,
    .write   = console_write,
    .lseek   = console_lseek,
    .ioctl   = nullptr,
};

// -- /dev/null file operations --

static long null_read([[maybe_unused]] File* file,
                       [[maybe_unused]] u8* buf,
                       [[maybe_unused]] usize count) noexcept {
    return 0;  // EOF
}

static long null_write([[maybe_unused]] File* file,
                        [[maybe_unused]] const u8* buf,
                        usize count) noexcept {
    return static_cast<long>(count);  // discard
}

static const FileOps g_null_fops = {
    .open    = console_open,
    .release = console_release,
    .read    = null_read,
    .write   = null_write,
    .lseek   = console_lseek,
    .ioctl   = nullptr,
};

// -- /dev/zero file operations --

static long zero_read([[maybe_unused]] File* file,
                       u8* buf, usize count) noexcept {
    for (usize i = 0; i < count; ++i) {
        buf[i] = 0;
    }
    return static_cast<long>(count);
}

static const FileOps g_zero_fops = {
    .open    = console_open,
    .release = console_release,
    .read    = zero_read,
    .write   = null_write,  // discard writes
    .lseek   = console_lseek,
    .ioctl   = nullptr,
};

// -- devfs directory inode_ops --

static Dentry* devfs_lookup(Inode* dir, const char* name, u32 name_len) noexcept;

static const InodeOps g_devfs_dir_iops = {
    .lookup = devfs_lookup,
    .create = nullptr,
};

// -- Static storage for devfs objects --
static SuperBlock g_devfs_sb;
static bool g_devfs_initialized = false;

static Dentry* devfs_lookup(Inode* dir, const char* name, u32 name_len) noexcept {
    // Linear scan of children
    for (u32 i = 0; i < dir->child_count; ++i) {
        Dentry* d = dir->children[i];
        if (d != nullptr && d->name_len == name_len) {
            bool eq = true;
            for (u32 j = 0; j < name_len; ++j) {
                if (d->name[j] != name[j]) { eq = false; break; }
            }
            if (eq) return d;
        }
    }
    return nullptr;
}

SuperBlock* devfs_init() noexcept {
    if (g_devfs_initialized) return &g_devfs_sb;

    // Create devfs superblock
    g_devfs_sb.fs_name = "devfs";
    g_devfs_sb.block_size = PAGE_SIZE;
    g_devfs_sb.fs_private = nullptr;

    // Root directory inode for /dev
    Inode* root_inode = alloc_inode();
    if (root_inode == nullptr) return nullptr;
    root_inode->ino = DEVFS_ROOT_INO;
    root_inode->type = FileType::Directory;
    root_inode->mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    root_inode->sb = &g_devfs_sb;
    root_inode->inode_ops = &g_devfs_dir_iops;

    // Root dentry for /dev
    Dentry* root_dentry = alloc_dentry("dev", 3, root_inode, nullptr);
    if (root_dentry == nullptr) return nullptr;

    g_devfs_sb.root_inode = root_inode;

    // -- Create /dev/console --
    Inode* console_inode = alloc_inode();
    if (console_inode == nullptr) return nullptr;
    console_inode->ino = DEVFS_CONSOLE_INO;
    console_inode->type = FileType::CharDev;
    console_inode->mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
    console_inode->sb = &g_devfs_sb;
    console_inode->file_ops = &g_console_fops;
    auto* console_d = alloc_dentry("console", 7, console_inode, root_dentry);
    if (console_d == nullptr) return nullptr;

    // -- Create /dev/null --
    Inode* null_inode = alloc_inode();
    if (null_inode == nullptr) return nullptr;
    null_inode->ino = DEVFS_NULL_INO;
    null_inode->type = FileType::CharDev;
    null_inode->mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    null_inode->sb = &g_devfs_sb;
    null_inode->file_ops = &g_null_fops;
    auto* null_d = alloc_dentry("null", 4, null_inode, root_dentry);
    if (null_d == nullptr) return nullptr;

    // -- Create /dev/zero --
    Inode* zero_inode = alloc_inode();
    if (zero_inode == nullptr) return nullptr;
    zero_inode->ino = DEVFS_ZERO_INO;
    zero_inode->type = FileType::CharDev;
    zero_inode->mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    zero_inode->sb = &g_devfs_sb;
    zero_inode->file_ops = &g_zero_fops;
    auto* zero_d = alloc_dentry("zero", 4, zero_inode, root_dentry);
    if (zero_d == nullptr) return nullptr;

    g_devfs_initialized = true;
    return &g_devfs_sb;
}

const FileOps& console_file_ops() noexcept {
    return g_console_fops;
}

} // namespace devfs

// ====================================================================
// ramfs implementation — read-only FS backed by initramfs CPIO
// ====================================================================

namespace ramfs {

namespace initramfs = moss::kernel::initramfs;
namespace log = moss::kernel::logging;

static constexpr InodeNumber RAMFS_ROOT_INO = 1;

// -- ramfs regular file operations --

static long ramfs_read(File* file, u8* buf, usize count) noexcept {
    if (file == nullptr || file->inode == nullptr) {
        return -static_cast<long>(VfsError::InvalidArg);
    }
    Inode* inode = file->inode;
    if (inode->data == nullptr) return 0;

    // Clamp to remaining data from current position
    auto pos = static_cast<u64>(file->pos);
    if (pos >= inode->size) return 0;

    usize avail = static_cast<usize>(inode->size - pos);
    if (count > avail) count = avail;

    const u8* src = inode->data + pos;
    for (usize i = 0; i < count; ++i) {
        buf[i] = src[i];
    }
    file->pos += static_cast<i64>(count);
    return static_cast<long>(count);
}

static long ramfs_write([[maybe_unused]] File* file,
                         [[maybe_unused]] const u8* buf,
                         [[maybe_unused]] usize count) noexcept {
    return -static_cast<long>(VfsError::PermDenied);  // read-only
}

static long ramfs_lseek(File* file, i64 offset, SeekWhence whence) noexcept {
    if (file == nullptr || file->inode == nullptr) {
        return -static_cast<long>(VfsError::InvalidArg);
    }

    i64 new_pos;
    switch (whence) {
    case SeekWhence::Set:
        new_pos = offset;
        break;
    case SeekWhence::Current:
        new_pos = file->pos + offset;
        break;
    case SeekWhence::End:
        new_pos = static_cast<i64>(file->inode->size) + offset;
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
    .open    = nullptr,
    .release = nullptr,
    .read    = ramfs_read,
    .write   = ramfs_write,
    .lseek   = ramfs_lseek,
    .ioctl   = nullptr,
};

// -- ramfs directory inode_ops --

static Dentry* ramfs_dir_lookup(Inode* dir, const char* name, u32 name_len) noexcept {
    for (u32 i = 0; i < dir->child_count; ++i) {
        Dentry* d = dir->children[i];
        if (d != nullptr && d->name_len == name_len) {
            bool eq = true;
            for (u32 j = 0; j < name_len; ++j) {
                if (d->name[j] != name[j]) { eq = false; break; }
            }
            if (eq) return d;
        }
    }
    return nullptr;
}

static const InodeOps g_ramfs_dir_iops = {
    .lookup = ramfs_dir_lookup,
    .create = nullptr,
};

// -- ramfs superblock --
static SuperBlock g_ramfs_sb;
static bool g_ramfs_initialized = false;
static InodeNumber g_ramfs_next_ino = RAMFS_ROOT_INO + 1;

SuperBlock* ramfs_init() noexcept {
    if (g_ramfs_initialized) return &g_ramfs_sb;

    auto& archive = initramfs::g_initramfs;
    if (!archive.is_initialized()) {
        log::klog::warn("ramfs: initramfs not initialized, skipping");
        // Still create an empty root
    }

    g_ramfs_sb.fs_name = "ramfs";
    g_ramfs_sb.block_size = moss::kernel::PAGE_SIZE;
    g_ramfs_sb.fs_private = nullptr;

    // Create root directory inode
    Inode* root_inode = alloc_inode();
    if (root_inode == nullptr) return nullptr;
    root_inode->ino = RAMFS_ROOT_INO;
    root_inode->type = FileType::Directory;
    root_inode->mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    root_inode->sb = &g_ramfs_sb;
    root_inode->inode_ops = &g_ramfs_dir_iops;

    // Root dentry (parent = nullptr for filesystem root)
    Dentry* root_dentry = alloc_dentry("/", 1, root_inode, nullptr);
    if (root_dentry == nullptr) return nullptr;

    g_ramfs_sb.root_inode = root_inode;

    // Populate from initramfs entries
    if (archive.is_initialized()) {
        archive.for_each([&](const initramfs::InitramfsEntry& entry) {
            Inode* file_inode = alloc_inode();
            if (file_inode == nullptr) return;

            file_inode->ino = g_ramfs_next_ino++;
            file_inode->type = FileType::Regular;
            file_inode->mode = S_IFREG | (entry.mode & 0777u);
            file_inode->size = entry.data_size;
            file_inode->data = entry.data;
            file_inode->sb = &g_ramfs_sb;
            file_inode->file_ops = &g_ramfs_file_fops;

            // Compute name length
            u32 nlen = 0;
            while (entry.name[nlen] != '\0' && nlen < MAX_NAME_LEN) ++nlen;

            auto* d = alloc_dentry(entry.name, nlen, file_inode, root_dentry);
            if (d == nullptr) return;

            log::klog::info("ramfs: mounted '{}' ({} bytes, ino={})",
                            entry.name, entry.data_size, file_inode->ino);
        });
    }

    g_ramfs_initialized = true;
    log::klog::info("ramfs: initialized with {} children",
                    root_inode->child_count);
    return &g_ramfs_sb;
}

} // namespace ramfs

// ====================================================================
// VFS initialization — mount root + devfs + stdio
// ====================================================================

namespace log = moss::kernel::logging;

/// Helper: find a dentry in the pool by scanning for matching inode.
static Dentry* find_dentry_for_inode(Inode* inode) noexcept {
    for (u32 i = 0; i < g_dentry_count; ++i) {
        if (g_dentry_pool[i].inode == inode) {
            return &g_dentry_pool[i];
        }
    }
    return nullptr;
}

void vfs_init() noexcept {
    g_mount_table.init();
    g_dcache.init();

    // 1. Mount ramfs as root filesystem "/"
    auto* ramfs_sb = ramfs::ramfs_init();
    if (ramfs_sb != nullptr && ramfs_sb->root_inode != nullptr) {
        Dentry* ramfs_root = find_dentry_for_inode(ramfs_sb->root_inode);
        if (ramfs_root != nullptr) {
            g_mount_table.mount("/", ramfs_sb, ramfs_root);
            log::klog::info("vfs: mounted ramfs at /");
        }
    } else {
        log::klog::warn("vfs: ramfs_init failed, no root filesystem");
    }

    // 2. Mount devfs at "/dev"
    auto* devfs_sb = devfs::devfs_init();
    if (devfs_sb != nullptr && devfs_sb->root_inode != nullptr) {
        Dentry* devfs_root = find_dentry_for_inode(devfs_sb->root_inode);
        if (devfs_root != nullptr) {
            g_mount_table.mount("/dev", devfs_sb, devfs_root);
            log::klog::info("vfs: mounted devfs at /dev");
        }
    }

    log::klog::info("vfs: initialization complete");
}

void vfs_init_stdio(void* fd_table) noexcept {
    if (fd_table == nullptr) return;

    auto* fdt = static_cast<FdTable*>(fd_table);

    // Open /dev/console for stdin (fd 0), stdout (fd 1), stderr (fd 2)
    Dentry* console_dentry = resolve_path("/dev/console");
    if (console_dentry == nullptr || console_dentry->inode == nullptr) {
        log::klog::warn("vfs: cannot open /dev/console for stdio");
        return;
    }

    for (int i = 0; i < 3; ++i) {
        File* f = alloc_file();
        if (f == nullptr) break;

        f->inode = console_dentry->inode;
        f->dentry = console_dentry;
        f->f_ops = console_dentry->inode->file_ops;
        f->flags = O_RDWR;
        f->pos = 0;
        console_dentry->inode->ref();

        long fd = fdt->alloc_fd(f);
        if (fd < 0) {
            free_file(f);
            break;
        }
    }

    log::klog::info("vfs: stdio initialized (fd 0/1/2 -> /dev/console)");
}

} // namespace moss::kernel::vfs
