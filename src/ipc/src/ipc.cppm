// MOSS IPC Module - Inter-Process Communication
// Combines shared_memory, zero_copy_channel, and ipc_manager

export module moss.ipc;

import moss.intrinsics;
import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.containers;
import moss.mm;
import moss.timer;
import moss.abi;

export namespace moss::kernel::ipc {

// Forward declaration
using moss::kernel::make_unique;
using moss::kernel::unique_ptr;
using VoidResult = moss::kernel::Result<void, moss::kernel::ErrorCode>;

// ========================================================================
// shared_memory.hpp
// ========================================================================

// 共享内存权限
enum class ShmPermission : u8 {
  Read = 1,
  Write = 2,
  Execute = 4,
  ReadWrite = Read | Write,
  ReadExecute = Read | Execute,
  All = Read | Write | Execute
};

constexpr ShmPermission operator|(ShmPermission a, ShmPermission b) noexcept {
  return static_cast<ShmPermission>(static_cast<u8>(a) | static_cast<u8>(b));
}

constexpr ShmPermission operator&(ShmPermission a, ShmPermission b) noexcept {
  return static_cast<ShmPermission>(static_cast<u8>(a) & static_cast<u8>(b));
}

// 共享内存管理器统计信息
struct SharedMemoryStats {
  u64 total_regions;
  usize total_memory_usage;
  usize large_pages_used;
  usize huge_pages_used;
};

// 共享内存区域类型
enum class ShmType : u8 { Normal = 0, DeviceMemory = 1, DMA_Coherent = 2, LargePage = 3 };

// 共享内存区域描述符
struct ShmRegion {
  ShmId id;
  PhysAddr phys_base;
  VirtAddr virt_base;
  usize size;
  ShmType type;
  ShmPermission permission;
  containers::AtomicU32 ref_count;
  ProcessId owner_pid;
  u64 creation_time;
  mm::MemoryAttributes attributes;
  usize page_size;
  usize allocation_order;
  containers::IrqSpinLock lifecycle_lock;

  ShmRegion(ShmId region_id, PhysAddr phys, VirtAddr virt, usize sz, ShmType t, ShmPermission perm,
            ProcessId pid) noexcept
      : id(region_id), phys_base(phys), virt_base(virt), size(sz), type(t), permission(perm), ref_count{},
        owner_pid(pid), creation_time(0), attributes{}, page_size(PAGE_SIZE), allocation_order(0), lifecycle_lock{} {
    // The creator owns one backing reference until destroy_region() consumes it.
    // Descriptor shared_ptr references do not independently retain physical pages.
    ref_count.store(1, containers::MemoryOrder::Relaxed);
  }
};

// 进程的共享内存映射
struct ShmMapping {
  ShmId region_id;
  VirtAddr virt_addr;
  usize size;
  ShmPermission permission;
  u64 map_time;

  ShmMapping(ShmId id, VirtAddr addr, usize sz, ShmPermission perm) noexcept
      : region_id(id), virt_addr(addr), size(sz), permission(perm), map_time(0) {}

  [[nodiscard]] bool operator==(const ShmMapping &other) const noexcept {
    return region_id == other.region_id && virt_addr == other.virt_addr && size == other.size;
  }
};

// 共享内存管理器
class SharedMemoryManager {
private:
  containers::LockedHashMap<ShmId, shared_ptr<ShmRegion>> regions_;
  containers::AtomicCounter<ShmId> next_shm_id_;
  containers::AtomicCounter<u64> total_regions_;
  containers::AtomicCounter<usize> total_memory_usage_;

  // Preserve the interface's 4 GiB request ceiling. Actual contiguous storage
  // is limited by the existing buddy allocator to PAGE_SIZE << MAX_ORDER.
  static constexpr usize MAX_SHM_SIZE = 1ULL << 32;
  // All u32 bits set mark an already released backing allocation. The
  // descriptor can outlive backing while a caller retains get_region_info().
  static constexpr u32 DESTROYED_REFS = 0xFFFFFFFFU;

public:
  SharedMemoryManager() noexcept : next_shm_id_(1), total_regions_(0), total_memory_usage_(0) {}
  ~SharedMemoryManager() noexcept { cleanup_all_regions(); }

  SharedMemoryManager(const SharedMemoryManager &) = delete;
  SharedMemoryManager &operator=(const SharedMemoryManager &) = delete;
  SharedMemoryManager(SharedMemoryManager &&) = delete;
  SharedMemoryManager &operator=(SharedMemoryManager &&) = delete;

  [[nodiscard]] KernelResult<ShmId> create_region(ProcessId creator_pid, usize size, ShmType type = ShmType::Normal,
                                                  ShmPermission permission = ShmPermission::ReadWrite) noexcept {
    if (size == 0 || size > MAX_SHM_SIZE)
      return KernelResult<ShmId>{KernelError::InvalidArgument};
    // The RAM direct map already supplies cacheable base-page mappings. Device,
    // DMA and requested huge-page attributes require a real mapping backend;
    // returning a RAM allocation would falsely promise those capabilities.
    if (type != ShmType::Normal)
      return KernelResult<ShmId>{KernelError::NotSupported};
    const usize aligned_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    const usize pages = aligned_size / PAGE_SIZE;
    usize order = 0;
    while (order <= ::MAX_ORDER && (usize{1} << order) < pages)
      ++order;
    if (order > ::MAX_ORDER)
      return KernelResult<ShmId>{KernelError::OutOfMemory};
    auto allocation = mm::PageFrameAllocator::allocate_pages(order);
    if (!allocation)
      return KernelResult<ShmId>{allocation.error() == mm::PageAllocError::InitializationFailed
                                     ? KernelError::InvalidState
                                     : KernelError::OutOfMemory};
    const PhysAddr phys = *allocation;
    const VirtAddr virt = phys_to_virt(phys);
    const ShmId id = next_shm_id_.fetch_add(1, containers::MemoryOrder::Relaxed);
    auto region = shared_ptr<ShmRegion>::try_make(
        [](usize bytes, usize alignment) -> void * {
          auto storage = mm::RuntimeHeapAllocator::allocate_aligned(bytes, alignment);
          return storage ? *storage : nullptr;
        },
        id, phys, virt, aligned_size, type, permission, creator_pid);
    if (!region) {
      (void)mm::PageFrameAllocator::free_pages(phys, order);
      return KernelResult<ShmId>{KernelError::OutOfMemory};
    }
    // A non-power-of-two logical region still owns a whole buddy block. Retain
    // the original order so destruction releases that exact allocation.
    region->allocation_order = order;
    region->attributes = mm::MemoryAttributes::NORMAL_CACHEABLE;
    intrinsics::memory::memset(reinterpret_cast<void *>(virt), 0, aligned_size);
    // Tracking is the third fallible heap allocation. Ordinary new would panic
    // here after backing was acquired, bypassing the public OOM rollback.
    if (!regions_.try_insert_or_update(id, region)) {
      (void)mm::PageFrameAllocator::free_pages(phys, order);
      return KernelResult<ShmId>{KernelError::OutOfMemory};
    }
    (void)total_regions_.fetch_add(1, containers::MemoryOrder::Relaxed);
    (void)total_memory_usage_.fetch_add(aligned_size, containers::MemoryOrder::Relaxed);
    return KernelResult<ShmId>{id};
  }

  [[nodiscard]] KernelResult<VirtAddr>
  map_to_process([[maybe_unused]] ProcessId pid, ShmId region_id, [[maybe_unused]] VirtAddr hint_addr = 0,
                 ShmPermission map_permission = ShmPermission::ReadWrite) noexcept {
    auto region = get_region_info(region_id);
    if (!region)
      return KernelResult<VirtAddr>{KernelError::InvalidArgument};
    containers::LockGuard<containers::IrqSpinLock> guard(region->lifecycle_lock);
    if (region->ref_count.load(containers::MemoryOrder::Relaxed) == DESTROYED_REFS)
      return KernelResult<VirtAddr>{KernelError::InvalidArgument};
    if ((region->permission & map_permission) != map_permission)
      return KernelResult<VirtAddr>{KernelError::PermissionDenied};
    // No shared VMA/PTE ownership backend exists. A fixed success address
    // cannot grant access, and must not create a reference to unmapped memory.
    return KernelResult<VirtAddr>{KernelError::NotSupported};
  }

  [[nodiscard]] VoidResult unmap_from_process([[maybe_unused]] ProcessId pid, ShmId region_id) noexcept {
    auto region = get_region_info(region_id);
    if (!region)
      return VoidResult{KernelError::InvalidArgument};
    // map_to_process cannot create mappings until its backend is implemented.
    return VoidResult{KernelError::NotFound};
  }

  [[nodiscard]] VoidResult destroy_region(ShmId region_id) noexcept {
    auto region = get_region_info(region_id);
    if (!region)
      return VoidResult{KernelError::InvalidArgument};
    // Serialize backing release with cache operations and future map admission.
    // One reference belongs to the creator; destruction consumes it. Waiting
    // for zero would leave every newly created region permanently busy.
    containers::LockGuard<containers::IrqSpinLock> guard(region->lifecycle_lock);
    const u32 references = region->ref_count.load(containers::MemoryOrder::Relaxed);
    if (references == DESTROYED_REFS)
      return VoidResult{KernelError::InvalidArgument};
    if (references != 1)
      return VoidResult{KernelError::Busy};
    auto released = mm::PageFrameAllocator::free_pages(region->phys_base, region->allocation_order);
    if (!released)
      return VoidResult{KernelError::InternalError};
    region->ref_count.store(DESTROYED_REFS, containers::MemoryOrder::Release);
    regions_.remove(region_id);
    (void)total_regions_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    (void)total_memory_usage_.fetch_sub(region->size, containers::MemoryOrder::Relaxed);
    return VoidResult{};
  }

  // This handle retains the descriptor. Backing access requires the creator to
  // keep the region alive and stop users before destroy_region(); a metadata
  // shared_ptr alone does not pin physical pages or quiesce ring operations.
  [[nodiscard]] shared_ptr<ShmRegion> get_region_info(ShmId region_id) const noexcept {
    auto found = regions_.find(region_id);
    return found ? *found : shared_ptr<ShmRegion>{};
  }

  void sync_region(ShmId region_id) noexcept {
    auto region = get_region_info(region_id);
    if (!region)
      return;
    containers::LockGuard<containers::IrqSpinLock> guard(region->lifecycle_lock);
    if (region->ref_count.load(containers::MemoryOrder::Relaxed) == DESTROYED_REFS)
      return;
    const VirtAddr end = region->virt_base + region->size;
    for (VirtAddr address = region->virt_base; address < end; address += CACHE_LINE_SIZE)
      arch::flush_cache_line(address);
    arch::memory_barrier();
  }

  [[nodiscard]] SharedMemoryStats get_statistics() const noexcept {
    return {.total_regions = total_regions_.load(containers::MemoryOrder::Relaxed),
            .total_memory_usage = total_memory_usage_.load(containers::MemoryOrder::Relaxed),
            .large_pages_used = 0,
            .huge_pages_used = 0};
  }

  void cleanup_process_mappings([[maybe_unused]] ProcessId pid) noexcept {
    // There are no successful process mappings to revoke in the current backend.
  }

private:
  void cleanup_all_regions() noexcept {
    // Destruction requires quiesced callers. Iterate without allocating a
    // snapshot, so teardown still works with an exhausted heap. The callback
    // must not reenter this map; detach/delete its nodes after iteration.
    regions_.for_each([](const auto &entry) {
      auto &region = *entry.value;
      containers::LockGuard<containers::IrqSpinLock> guard(region.lifecycle_lock);
      if (region.ref_count.load(containers::MemoryOrder::Relaxed) == 1 &&
          mm::PageFrameAllocator::free_pages(region.phys_base, region.allocation_order))
        region.ref_count.store(DESTROYED_REFS, containers::MemoryOrder::Release);
    });
    regions_.clear();
    total_regions_.store(0, containers::MemoryOrder::Relaxed);
    total_memory_usage_.store(0, containers::MemoryOrder::Relaxed);
  }
};

// 全局共享内存管理器实例
extern SharedMemoryManager *g_shared_memory_manager;

// ========================================================================
// zero_copy_channel.hpp
// ========================================================================

// 消息类型
enum class MessageType : u8 { Data = 0, Request = 1, Response = 2, Notification = 3, Capability = 4 };

// 固定 64 字节消息布局；显式尾填充保留协议大小，而不是有效 payload。
// 64 字节对齐只约束对象本身，环内 8 字节步进并不保证每条消息同样对齐。
// 此协议大小及对齐的精确选值依据尚未记录。
struct alignas(64) MessageHeader {
  MessageId msg_id;
  u64 sequence;
  u64 timestamp;
  u64 timeout;
  ThreadId sender_tid;
  ProcessId sender_pid;
  u32 payload_size;
  MessageType type;
  u8 priority;
  u16 flags;
  u8 padding[12];

  MessageHeader() noexcept
      : msg_id(0), sequence(0), timestamp(0), timeout(0), sender_tid(0), sender_pid(0), payload_size(0),
        type(MessageType::Data), priority(0), flags(0), padding{} {}
};

static_assert(sizeof(MessageHeader) == 64, "MessageHeader must be 64 bytes");

// 消息缓冲区槽位
struct MessageSlot {
  containers::AtomicU32 state;
  MessageHeader header;
  u8 *payload;

  enum State : u32 { Empty = 0, Writing = 1, Ready = 2, Reading = 3 };
};

// Variable-length records share one IRQ-safe lock in the control block. Hold
// it through copying and cursor publication/reclamation: reserving a cursor
// alone cannot make an incomplete record readable or a busy slot reusable.
// The default 64 KiB/4 KiB budgets retain the existing policy; exact sizing
// evidence is unrecorded. Record boundaries use eight-byte wire alignment.
template <usize BufferSize = 64 * 1024, usize MaxMessageSize = 4096> class ZeroCopyRingBuffer {
private:
  static_assert(BufferSize != 0 && (BufferSize & (BufferSize - 1)) == 0, "BufferSize must be power of 2");
  static_assert(MaxMessageSize >= sizeof(MessageHeader) && MaxMessageSize <= BufferSize / 4,
                "Message budget must fit a header and at most one quarter of the ring");
  static constexpr usize BUFFER_MASK = BufferSize - 1;
  // Keep the existing 64-byte control extent; use its formerly reserved bytes
  // for the shared lock so separate views serialize on the same ownership.
  static constexpr usize CONTROL_BYTES = 64;
  static_assert(sizeof(containers::IrqSpinLock) < CONTROL_BYTES - 4 * sizeof(u64));
  struct alignas(CONTROL_BYTES) RingControl {
    containers::AtomicU64 write_pos;
    containers::AtomicU64 read_pos;
    containers::AtomicU64 write_count;
    containers::AtomicU64 read_count;
    containers::IrqSpinLock lock;
    u8 reserved[CONTROL_BYTES - 4 * sizeof(u64) - sizeof(containers::IrqSpinLock)]{};
  };
  static_assert(sizeof(RingControl) == CONTROL_BYTES);

  RingControl *control_{nullptr};
  u8 *buffer_{nullptr};

public:
  struct RingBufferStats {
    u64 write_count;
    u64 read_count;
    u64 pending_messages;
    usize used_space;
  };

  [[nodiscard]] static constexpr usize storage_size() noexcept { return CONTROL_BYTES + BufferSize; }

  // A creator constructs the control block once. Later views attach with
  // initialize_control=false while its lifetime remains externally owned.
  ZeroCopyRingBuffer(void *shared_memory, usize size, bool initialize_control = true) noexcept {
    if (!shared_memory || size < storage_size() ||
        (reinterpret_cast<usize>(shared_memory) & (alignof(RingControl) - 1)))
      return;
    control_ = static_cast<RingControl *>(shared_memory);
    buffer_ = static_cast<u8 *>(shared_memory) + CONTROL_BYTES;
    if (initialize_control)
      new (control_) RingControl{};
  }

  [[nodiscard]] bool is_valid() const noexcept { return control_ != nullptr; }

  [[nodiscard]] bool try_send(const MessageHeader &header, const void *payload) noexcept {
    if (!is_valid() || header.payload_size > MaxMessageSize - sizeof(MessageHeader) ||
        (header.payload_size && !payload))
      return false;
    const usize total_size = ring_align_up(sizeof(MessageHeader) + header.payload_size, 8);
    if (total_size > MaxMessageSize)
      return false; // Wire padding is part of the declared record budget.
    containers::LockGuard<containers::IrqSpinLock> guard(control_->lock);
    const u64 write_pos = control_->write_pos.load(containers::MemoryOrder::Relaxed);
    const u64 read_pos = control_->read_pos.load(containers::MemoryOrder::Relaxed);
    // Capacity is the masked ring extent, never the caller's larger allocation.
    if (write_pos - read_pos > BufferSize - total_size)
      return false;
    copy_to_ring(write_pos, &header, sizeof(header));
    if (header.payload_size)
      copy_to_ring(write_pos + sizeof(header), payload, header.payload_size);
    // Publish only after header and payload are complete. The shared lock
    // covers competing writers/readers as well as the release-store ordering.
    control_->write_pos.store(write_pos + total_size, containers::MemoryOrder::Release);
    (void)control_->write_count.fetch_add(1, containers::MemoryOrder::Relaxed);
    return true;
  }

  [[nodiscard]] bool try_receive(MessageHeader &header, void *payload, usize max_payload_size) noexcept {
    if (!is_valid())
      return false;
    containers::LockGuard<containers::IrqSpinLock> guard(control_->lock);
    const u64 read_pos = control_->read_pos.load(containers::MemoryOrder::Relaxed);
    const u64 write_pos = control_->write_pos.load(containers::MemoryOrder::Relaxed);
    const u64 available = write_pos - read_pos;
    if (available < sizeof(MessageHeader) || available > BufferSize)
      return false;
    // Copy the wire representation into an aligned live object. Eight-byte
    // record offsets and wrapped fragments cannot be cast to a 64-byte header.
    MessageHeader candidate{};
    copy_from_ring(&candidate, read_pos, sizeof(candidate));
    if (candidate.payload_size > MaxMessageSize - sizeof(MessageHeader) || candidate.payload_size > max_payload_size ||
        (candidate.payload_size && !payload))
      return false;
    const usize total_size = ring_align_up(sizeof(MessageHeader) + candidate.payload_size, 8);
    if (total_size > MaxMessageSize || total_size > available)
      return false;
    if (candidate.payload_size)
      copy_from_ring(payload, read_pos + sizeof(candidate), candidate.payload_size);
    header = candidate;
    // Reclaim storage only after consumption, preventing a writer from
    // overwriting payload that a reader has claimed but has not copied yet.
    control_->read_pos.store(read_pos + total_size, containers::MemoryOrder::Release);
    (void)control_->read_count.fetch_add(1, containers::MemoryOrder::Relaxed);
    return true;
  }

  [[nodiscard]] RingBufferStats get_statistics() const noexcept {
    if (!is_valid())
      return {};
    containers::LockGuard<containers::IrqSpinLock> guard(control_->lock);
    const u64 written = control_->write_count.load(containers::MemoryOrder::Relaxed);
    const u64 read = control_->read_count.load(containers::MemoryOrder::Relaxed);
    return {written, read, written - read,
            static_cast<usize>(control_->write_pos.load(containers::MemoryOrder::Relaxed) -
                               control_->read_pos.load(containers::MemoryOrder::Relaxed))};
  }

private:
  void copy_to_ring(u64 position, const void *source, usize size) noexcept {
    const usize offset = static_cast<usize>(position) & BUFFER_MASK;
    const usize first = (size < BufferSize - offset) ? size : BufferSize - offset;
    intrinsics::memory::memcpy(buffer_ + offset, source, first);
    if (size > first)
      intrinsics::memory::memcpy(buffer_, static_cast<const u8 *>(source) + first, size - first);
  }

  void copy_from_ring(void *destination, u64 position, usize size) const noexcept {
    const usize offset = static_cast<usize>(position) & BUFFER_MASK;
    const usize first = (size < BufferSize - offset) ? size : BufferSize - offset;
    intrinsics::memory::memcpy(destination, buffer_ + offset, first);
    if (size > first)
      intrinsics::memory::memcpy(static_cast<u8 *>(destination) + first, buffer_, size - first);
  }

  static constexpr usize ring_align_up(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
  }
};

// 零拷贝通道
class ZeroCopyChannel {
private:
  ChannelId channel_id_;
  ProcessId client_pid_;
  ProcessId server_pid_;
  ShmId client_to_server_shm_;
  ShmId server_to_client_shm_;
  unique_ptr<ZeroCopyRingBuffer<>> client_to_server_;
  unique_ptr<ZeroCopyRingBuffer<>> server_to_client_;
  containers::AtomicU64 client_wait_seq_;
  containers::AtomicU64 server_wait_seq_;
  containers::AtomicU64 messages_sent_;
  containers::AtomicU64 messages_received_;
  containers::AtomicU64 bytes_transferred_;

public:
  struct ChannelStats {
    u64 messages_sent;
    u64 messages_received;
    u64 bytes_transferred;
    u64 client_to_server_pending;
    u64 server_to_client_pending;
  };

  ZeroCopyChannel(ChannelId id, ProcessId client, ProcessId server) noexcept
      : channel_id_(id), client_pid_(client), server_pid_(server), client_to_server_shm_(0), server_to_client_shm_(0),
        client_wait_seq_(0), server_wait_seq_(0), messages_sent_(0), messages_received_(0), bytes_transferred_(0) {}

  ~ZeroCopyChannel() noexcept { cleanup(); }

  ZeroCopyChannel(const ZeroCopyChannel &) = delete;
  ZeroCopyChannel &operator=(const ZeroCopyChannel &) = delete;
  ZeroCopyChannel(ZeroCopyChannel &&) = delete;
  ZeroCopyChannel &operator=(ZeroCopyChannel &&) = delete;

  [[nodiscard]] VoidResult initialize() noexcept {
    if (!g_shared_memory_manager)
      return VoidResult{KernelError::InvalidState};
    if (client_to_server_shm_ || server_to_client_shm_)
      return VoidResult{KernelError::Busy};
    // The backing contains the shared control block and masked data extent;
    // sizeof the local view contains neither and cannot describe wire storage.
    auto c2s_result = g_shared_memory_manager->create_region(client_pid_, ZeroCopyRingBuffer<>::storage_size(),
                                                             ShmType::Normal, ShmPermission::ReadWrite);
    if (!c2s_result) {
      return VoidResult{c2s_result.error()};
    }
    client_to_server_shm_ = *c2s_result;
    auto s2c_result = g_shared_memory_manager->create_region(server_pid_, ZeroCopyRingBuffer<>::storage_size(),
                                                             ShmType::Normal, ShmPermission::ReadWrite);
    if (!s2c_result) {
      cleanup();
      return VoidResult{s2c_result.error()};
    }
    server_to_client_shm_ = *s2c_result;
    auto c2s_region = g_shared_memory_manager->get_region_info(client_to_server_shm_);
    auto s2c_region = g_shared_memory_manager->get_region_info(server_to_client_shm_);
    if (!c2s_region || !s2c_region) {
      cleanup();
      return VoidResult{KernelError::InternalError};
    }
    // Views also need fallible storage: make_unique uses ordinary new and
    // would panic on OOM before the channel could release both regions.
    auto create_view = [](const ShmRegion &region) {
      auto storage =
          mm::RuntimeHeapAllocator::allocate_aligned(sizeof(ZeroCopyRingBuffer<>), alignof(ZeroCopyRingBuffer<>));
      return storage ? unique_ptr<ZeroCopyRingBuffer<>>{new (*storage) ZeroCopyRingBuffer<>(
                           reinterpret_cast<void *>(region.virt_base), region.size)}
                     : unique_ptr<ZeroCopyRingBuffer<>>{};
    };
    client_to_server_ = create_view(*c2s_region);
    server_to_client_ = create_view(*s2c_region);
    if (!client_to_server_ || !server_to_client_) {
      cleanup();
      return VoidResult{KernelError::OutOfMemory};
    }
    if (!client_to_server_->is_valid() || !server_to_client_->is_valid()) {
      cleanup();
      return VoidResult{KernelError::InternalError};
    }
    return VoidResult{};
  }

  [[nodiscard]] bool send_message(ProcessId sender_pid, const MessageHeader &header, const void *payload) noexcept {
    ZeroCopyRingBuffer<> *buffer = nullptr;
    if (sender_pid == client_pid_) {
      buffer = client_to_server_.get();
    } else if (sender_pid == server_pid_) {
      buffer = server_to_client_.get();
    } else {
      return false;
    }
    if (buffer && buffer->try_send(header, payload)) {
      (void)messages_sent_.fetch_add(1, containers::MemoryOrder::Relaxed);
      (void)bytes_transferred_.fetch_add(header.payload_size, containers::MemoryOrder::Relaxed);
      notify_receiver(sender_pid);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool receive_message(ProcessId receiver_pid, MessageHeader &header, void *payload,
                                     usize max_payload_size) noexcept {
    ZeroCopyRingBuffer<> *buffer = nullptr;
    if (receiver_pid == client_pid_) {
      buffer = server_to_client_.get();
    } else if (receiver_pid == server_pid_) {
      buffer = client_to_server_.get();
    } else {
      return false;
    }
    if (buffer && buffer->try_receive(header, payload, max_payload_size)) {
      (void)messages_received_.fetch_add(1, containers::MemoryOrder::Relaxed);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool wait_for_message(ProcessId receiver_pid, MessageHeader &header, void *payload,
                                      usize max_payload_size, u64 timeout_ns = static_cast<u64>(-1)) noexcept {
    if (!client_to_server_ || !server_to_client_ || (receiver_pid != client_pid_ && receiver_pid != server_pid_) ||
        !timer::TimerSubsystem::instance().is_initialized())
      return false;
    // u64 最大值表示无限等待，普通值以 ns 计时；每轮最多轮询 1 ms 通知
    // 后重查环内容，以免仅依赖一次通知快照。1 ms 的精确选值依据未记录。
    u64 start_time = get_current_time_ns();
    while (true) {
      if (receive_message(receiver_pid, header, payload, max_payload_size)) {
        return true;
      }
      u64 max_timeout = static_cast<u64>(-1);
      if (timeout_ns != max_timeout) {
        u64 current_time = get_current_time_ns();
        if (current_time - start_time >= timeout_ns) {
          return false;
        }
      }
      wait_for_notification(receiver_pid, 1000000);
    }
  }

  [[nodiscard]] ChannelStats get_statistics() const noexcept {
    if (!client_to_server_ || !server_to_client_)
      return {};
    auto c2s_stats = client_to_server_->get_statistics();
    auto s2c_stats = server_to_client_->get_statistics();
    return {.messages_sent = messages_sent_.load(containers::MemoryOrder::Relaxed),
            .messages_received = messages_received_.load(containers::MemoryOrder::Relaxed),
            .bytes_transferred = bytes_transferred_.load(containers::MemoryOrder::Relaxed),
            .client_to_server_pending = c2s_stats.pending_messages,
            .server_to_client_pending = s2c_stats.pending_messages};
  }

  [[nodiscard]] ChannelId id() const noexcept { return channel_id_; }
  [[nodiscard]] ProcessId client_pid() const noexcept { return client_pid_; }
  [[nodiscard]] ProcessId server_pid() const noexcept { return server_pid_; }

private:
  void notify_receiver(ProcessId sender_pid) noexcept {
    if (sender_pid == client_pid_) {
      (void)server_wait_seq_.fetch_add(1, containers::MemoryOrder::Release);
    } else {
      (void)client_wait_seq_.fetch_add(1, containers::MemoryOrder::Release);
    }
  }

  void wait_for_notification(ProcessId receiver_pid, u64 timeout_ns) noexcept {
    containers::AtomicU64 *wait_seq = (receiver_pid == client_pid_) ? &client_wait_seq_ : &server_wait_seq_;
    u64 current_seq = wait_seq->load(containers::MemoryOrder::Acquire);
    u64 start_time = get_current_time_ns();
    while (wait_seq->load(containers::MemoryOrder::Acquire) == current_seq) {
      if (get_current_time_ns() - start_time >= timeout_ns) {
        break;
      }
      arch::cpu_yield();
    }
  }

  [[nodiscard]] static u64 get_current_time_ns() noexcept {
    // Architectural counters return cycles, not ns; use the calibrated timer
    // clock so timeout budgets mean the same duration on every architecture.
    return timer::TimerSubsystem::instance().now_ns();
  }

  void cleanup() noexcept {
    // Callers quiesce users before teardown; release local views before freeing
    // their backing so no surviving member points at a returned buddy block.
    client_to_server_.reset();
    server_to_client_.reset();
    if (client_to_server_shm_ != 0) {
      (void)g_shared_memory_manager->destroy_region(client_to_server_shm_);
      client_to_server_shm_ = 0;
    }
    if (server_to_client_shm_ != 0) {
      (void)g_shared_memory_manager->destroy_region(server_to_client_shm_);
      server_to_client_shm_ = 0;
    }
  }
};

// ========================================================================
// ipc_manager.hpp
// ========================================================================

// IPC端点类型
enum class EndpointType : u8 { Server = 0, Client = 1, Peer = 2 };

// IPC服务描述符
struct ServiceDescriptor {
  ServiceId service_id;
  ProcessId provider_pid;
  EndpointId endpoint_id;
  const char *service_name; // 借用名称字符串，注册者须保证它比服务描述符存活更久。
  u32 max_clients;
  containers::AtomicU32 current_clients;
  bool is_public;
  u64 creation_time;

  ServiceDescriptor(ServiceId id, ProcessId pid, const char *name) noexcept
      // 默认最多 256 客户端限制每服务连接增长，具体容量的选取依据未记录。
      : service_id(id), provider_pid(pid), endpoint_id(0), service_name(name), max_clients(256), current_clients{},
        is_public(true), creation_time(0) {}
};

// IPC连接描述符
struct ConnectionDescriptor {
  ChannelId channel_id;
  ProcessId client_pid;
  ProcessId server_pid;
  ServiceId service_id;
  u64 established_time;
  u64 last_activity;
  containers::AtomicU32 messages_sent;
  containers::AtomicU32 messages_received;
  containers::AtomicU64 bytes_transferred;

  ConnectionDescriptor(ChannelId cid, ProcessId client, ProcessId server, ServiceId sid) noexcept
      : channel_id(cid), client_pid(client), server_pid(server), service_id(sid), established_time(0), last_activity(0),
        messages_sent{}, messages_received{}, bytes_transferred{} {}
};

// IPC管理器主类
class IpcManager {
private:
  containers::LockedHashMap<ServiceId, shared_ptr<ServiceDescriptor>> services_;
  containers::LockedHashMap<ChannelId, shared_ptr<ZeroCopyChannel>> channels_;
  containers::LockedHashMap<ChannelId, shared_ptr<ConnectionDescriptor>> connections_;
  containers::LockedHashMap<ProcessId, shared_ptr<containers::LockedList<ChannelId>>> process_channels_;
  containers::AtomicCounter<ServiceId> next_service_id_;
  containers::AtomicCounter<ChannelId> next_channel_id_;
  SharedMemoryManager *shared_memory_manager_;
  containers::AtomicCounter<u64> total_services_;
  containers::AtomicCounter<u64> total_channels_;
  containers::AtomicCounter<u64> messages_processed_;
  containers::AtomicCounter<u64> bytes_transferred_;

public:
  struct IpcStats {
    u64 total_services;
    u64 total_channels;
    u64 messages_processed;
    u64 bytes_transferred;
  };

  IpcManager(SharedMemoryManager *shm_manager) noexcept
      : next_service_id_(1), next_channel_id_(1), shared_memory_manager_(shm_manager), total_services_(0),
        total_channels_(0), messages_processed_(0), bytes_transferred_(0) {}

  ~IpcManager() noexcept { cleanup(); }

  IpcManager(const IpcManager &) = delete;
  IpcManager &operator=(const IpcManager &) = delete;
  IpcManager(IpcManager &&) = delete;
  IpcManager &operator=(IpcManager &&) = delete;

  [[nodiscard]] KernelResult<ServiceId> register_service(ProcessId provider_pid, const char *service_name,
                                                         u32 max_clients = 256) noexcept {
    // 默认值与 ServiceDescriptor 的 256 客户端预算一致；非硬件协议上限。
    if (service_name == nullptr) {
      return KernelResult<ServiceId>{KernelError::InvalidArgument};
    }
    if (static_cast<bool>(find_service_by_name(service_name))) {
      return KernelResult<ServiceId>{KernelError::AlreadyExists};
    }
    ServiceId service_id = next_service_id_.fetch_add(1, containers::MemoryOrder::Relaxed);
    auto service = make_shared<ServiceDescriptor>(service_id, provider_pid, service_name);
    service->max_clients = max_clients;
    services_.insert_or_update(service_id, service);
    (void)total_services_.fetch_add(1, containers::MemoryOrder::Relaxed);
    return KernelResult<ServiceId>{service_id};
  }

  [[nodiscard]] VoidResult unregister_service(ServiceId service_id, ProcessId provider_pid) noexcept {
    auto service_ptr = services_.find(service_id);
    if (!service_ptr) {
      return VoidResult{KernelError::NotFound};
    }
    auto service = *service_ptr;
    if (service->provider_pid != provider_pid) {
      return VoidResult{KernelError::PermissionDenied};
    }
    if (!services_.remove(service_id)) {
      return VoidResult{KernelError::NotFound};
    }
    close_service_channels(service_id);
    (void)total_services_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    return VoidResult{};
  }

  [[nodiscard]] KernelResult<ChannelId> connect_to_service(ProcessId client_pid, ServiceId service_id) noexcept {
    auto service_ptr = services_.find(service_id);
    if (!service_ptr) {
      return KernelResult<ChannelId>{KernelError::NotFound};
    }
    auto service = *service_ptr;
    if (service->current_clients.load(containers::MemoryOrder::Acquire) >= service->max_clients) {
      return KernelResult<ChannelId>{KernelError::ResourceExhausted};
    }
    ChannelId channel_id = next_channel_id_.fetch_add(1, containers::MemoryOrder::Relaxed);
    auto channel = make_shared<ZeroCopyChannel>(channel_id, client_pid, service->provider_pid);
    if (!channel) {
      return KernelResult<ChannelId>{KernelError::OutOfMemory};
    }
    auto init_result = channel->initialize();
    if (!init_result) {
      return KernelResult<ChannelId>{init_result.error()};
    }
    auto conn = make_shared<ConnectionDescriptor>(channel_id, client_pid, service->provider_pid, service_id);
    channels_.insert_or_update(channel_id, moss::move(channel));
    connections_.insert_or_update(channel_id, conn);
    (void)service->current_clients.fetch_add(1, containers::MemoryOrder::AcqRel);
    (void)total_channels_.fetch_add(1, containers::MemoryOrder::Relaxed);
    add_process_channel(client_pid, channel_id);
    add_process_channel(service->provider_pid, channel_id);
    return KernelResult<ChannelId>{channel_id};
  }

  [[nodiscard]] KernelResult<ChannelId> connect_to_service_by_name(ProcessId client_pid,
                                                                   const char *service_name) noexcept {
    auto service = find_service_by_name(service_name);
    if (!service) {
      return KernelResult<ChannelId>{KernelError::NotFound};
    }
    return connect_to_service(client_pid, service->service_id);
  }

  [[nodiscard]] VoidResult disconnect(ChannelId channel_id, ProcessId requester_pid) noexcept {
    auto conn_ptr = connections_.find(channel_id);
    if (!conn_ptr) {
      return VoidResult{KernelError::NotFound};
    }
    auto conn = *conn_ptr;
    if (conn->client_pid != requester_pid && conn->server_pid != requester_pid) {
      return VoidResult{KernelError::PermissionDenied};
    }
    if (!connections_.remove(channel_id)) {
      return VoidResult{KernelError::NotFound};
    }
    channels_.remove(channel_id);
    auto service_ptr = services_.find(conn->service_id);
    if (static_cast<bool>(service_ptr)) {
      auto service = *service_ptr;
      (void)service->current_clients.fetch_sub(1, containers::MemoryOrder::AcqRel);
    }
    remove_process_channel(conn->client_pid, channel_id);
    remove_process_channel(conn->server_pid, channel_id);
    (void)total_channels_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    return VoidResult{};
  }

  [[nodiscard]] bool send_message(ChannelId channel_id, ProcessId sender_pid, const MessageHeader &header,
                                  const void *payload) noexcept {
    auto channel = channels_.find(channel_id);
    if (!channel || !*channel) {
      return false;
    }
    bool success = (*channel)->send_message(sender_pid, header, payload);
    if (success) {
      (void)messages_processed_.fetch_add(1, containers::MemoryOrder::Relaxed);
      (void)bytes_transferred_.fetch_add(header.payload_size, containers::MemoryOrder::Relaxed);
      update_connection_statistics(channel_id, true, header.payload_size);
    }
    return success;
  }

  [[nodiscard]] bool receive_message(ChannelId channel_id, ProcessId receiver_pid, MessageHeader &header, void *payload,
                                     usize max_payload_size) noexcept {
    auto channel = channels_.find(channel_id);
    if (!channel || !*channel) {
      return false;
    }
    bool success = (*channel)->receive_message(receiver_pid, header, payload, max_payload_size);
    if (success) {
      (void)messages_processed_.fetch_add(1, containers::MemoryOrder::Relaxed);
      update_connection_statistics(channel_id, false, header.payload_size);
    }
    return success;
  }

  [[nodiscard]] bool wait_for_message(ChannelId channel_id, ProcessId receiver_pid, MessageHeader &header,
                                      void *payload, usize max_payload_size,
                                      u64 timeout_ns = static_cast<u64>(-1)) noexcept {
    auto channel = channels_.find(channel_id);
    if (!channel || !*channel) {
      return false;
    }
    return (*channel)->wait_for_message(receiver_pid, header, payload, max_payload_size, timeout_ns);
  }

  void get_service_list([[maybe_unused]] ProcessId requester_pid, void (*callback)(const ServiceDescriptor &, void *),
                        void *context) const noexcept {
    services_.for_each_snapshot([callback, context](const auto &entry) {
      const ServiceDescriptor &service = *entry.value;
      if (service.is_public) {
        callback(service, context);
      }
    });
  }

  [[nodiscard]] IpcStats get_statistics() const noexcept {
    return {.total_services = total_services_.load(containers::MemoryOrder::Relaxed),
            .total_channels = total_channels_.load(containers::MemoryOrder::Relaxed),
            .messages_processed = messages_processed_.load(containers::MemoryOrder::Relaxed),
            .bytes_transferred = bytes_transferred_.load(containers::MemoryOrder::Relaxed)};
  }

  void get_process_connections(ProcessId pid, void (*callback)(const ConnectionDescriptor &, void *),
                               void *context) const noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (!channels_ptr) {
      return;
    }
    auto channels = *channels_ptr;
    if (!channels) {
      return;
    }
    channels->for_each_snapshot([this, callback, context](const ChannelId &channel_id) {
      auto conn_ptr = connections_.find(channel_id);
      if (static_cast<bool>(conn_ptr)) {
        callback(**conn_ptr, context);
      }
    });
  }

  void cleanup_process_ipc(ProcessId pid) noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (!channels_ptr) {
      return;
    }
    auto channels = *channels_ptr;
    if (!channels) {
      return;
    }
    channels->for_each_snapshot([this, pid](const ChannelId &channel_id) { (void)disconnect(channel_id, pid); });
    process_channels_.remove(pid);
    if (shared_memory_manager_ != nullptr) {
      shared_memory_manager_->cleanup_process_mappings(pid);
    }
  }

private:
  [[nodiscard]] shared_ptr<ServiceDescriptor> find_service_by_name(const char *name) noexcept {
    shared_ptr<ServiceDescriptor> found_service;
    services_.for_each_snapshot([name, &found_service](const auto &entry) {
      auto service = entry.value;
      if (moss::abi::bridge::strcmp(service->service_name, name) == 0) {
        found_service = service;
        return;
      }
    });
    return found_service;
  }

  void close_service_channels(ServiceId service_id) noexcept {
    connections_.for_each_snapshot([this, service_id](const auto &entry) {
      auto conn = entry.value;
      if (conn->service_id == service_id) {
        (void)disconnect(conn->channel_id, conn->client_pid);
      }
    });
  }

  void add_process_channel(ProcessId pid, ChannelId channel_id) noexcept {
    auto channels =
        process_channels_.get_or_insert(pid, [] { return make_shared<containers::LockedList<ChannelId>>(); });
    channels->push_front(channel_id);
  }

  void remove_process_channel(ProcessId pid, ChannelId channel_id) noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (static_cast<bool>(channels_ptr)) {
      auto channels = *channels_ptr;
      if (static_cast<bool>(channels)) {
        channels->remove(channel_id);
      }
    }
  }

  void update_connection_statistics(ChannelId channel_id, bool is_send, u32 bytes) noexcept {
    auto conn_ptr = connections_.find(channel_id);
    if (!conn_ptr) {
      return;
    }
    auto conn = *conn_ptr;
    if (static_cast<bool>(conn)) {
      if (is_send) {
        (void)conn->messages_sent.fetch_add(1, containers::MemoryOrder::Relaxed);
      } else {
        (void)conn->messages_received.fetch_add(1, containers::MemoryOrder::Relaxed);
      }
      (void)conn->bytes_transferred.fetch_add(bytes, containers::MemoryOrder::Relaxed);
      conn->last_activity = get_current_time();
    }
  }

  [[nodiscard]] static u64 get_current_time() noexcept { return arch::get_timestamp_counter(); }

  void cleanup() noexcept {
    channels_.clear();
    connections_.clear();
    services_.clear();
    process_channels_.clear();
  }
};

// 全局IPC管理器实例
extern IpcManager *g_ipc_manager;

// === Module-level variable definitions ===

// Global shared memory manager instance
SharedMemoryManager *g_shared_memory_manager = nullptr;

// Global IPC manager instance
IpcManager *g_ipc_manager = nullptr;

} // namespace moss::kernel::ipc
