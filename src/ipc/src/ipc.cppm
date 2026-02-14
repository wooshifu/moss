// MOSS IPC Module - Inter-Process Communication
// Combines shared_memory, zero_copy_channel, and ipc_manager
export module moss.ipc;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.containers;
import moss.mm;

export namespace moss::kernel::ipc {

// Forward declaration
using moss::kernel::unique_ptr;
using moss::kernel::make_unique;
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
enum class ShmType : u8 {
  Normal = 0,
  DeviceMemory = 1,
  DMA_Coherent = 2,
  LargePage = 3
};

// 共享内存区域描述符
struct ShmRegion {
  ShmId id;
  PhysAddr phys_base;
  VirtAddr virt_base;
  usize size;
  ShmType type;
  ShmPermission permission;
  u32 ref_count;
  ProcessId owner_pid;
  u64 creation_time;
  mm::MemoryAttributes attributes;
  usize page_size;

  ShmRegion(ShmId region_id, PhysAddr phys, VirtAddr virt, usize sz, ShmType t,
            ShmPermission perm, ProcessId pid) noexcept
      : id(region_id), phys_base(phys), virt_base(virt), size(sz), type(t),
        permission(perm), ref_count(1), owner_pid(pid), creation_time(0),
        attributes{}, page_size(PAGE_SIZE) {}
};

// 进程的共享内存映射
struct ShmMapping {
  ShmId region_id;
  VirtAddr virt_addr;
  usize size;
  ShmPermission permission;
  u64 map_time;

  ShmMapping(ShmId id, VirtAddr addr, usize sz, ShmPermission perm) noexcept
      : region_id(id), virt_addr(addr), size(sz), permission(perm),
        map_time(0) {}

  [[nodiscard]] bool operator==(const ShmMapping &other) const noexcept {
    return region_id == other.region_id && virt_addr == other.virt_addr &&
           size == other.size;
  }
};

// 共享内存管理器
class SharedMemoryManager {
private:
  containers::RcuHashMap<ShmId, ShmRegion *> regions_;
  containers::RcuHashMap<ProcessId, containers::RcuList<ShmMapping> *>
      process_mappings_;
  containers::AtomicCounter<ShmId> next_shm_id_;
  containers::AtomicCounter<u64> total_regions_;
  containers::AtomicCounter<usize> total_memory_usage_;
  containers::AtomicCounter<usize> large_pages_used_;
  containers::AtomicCounter<usize> huge_pages_used_;

  static constexpr usize MAX_SHM_SIZE = 1ULL << 32;
  static constexpr usize SHM_LARGE_PAGE_SIZE = 2 * 1024 * 1024;
  static constexpr usize SHM_HUGE_PAGE_SIZE = 1024 * 1024 * 1024;

public:
  SharedMemoryManager() noexcept
      : next_shm_id_(1), total_regions_(0), total_memory_usage_(0),
        large_pages_used_(0), huge_pages_used_(0) {}

  ~SharedMemoryManager() noexcept { cleanup_all_regions(); }

  SharedMemoryManager(const SharedMemoryManager &) = delete;
  SharedMemoryManager &operator=(const SharedMemoryManager &) = delete;
  SharedMemoryManager(SharedMemoryManager &&) = delete;
  SharedMemoryManager &operator=(SharedMemoryManager &&) = delete;

  [[nodiscard]] KernelResult<ShmId>
  create_region(ProcessId creator_pid, usize size,
                ShmType type = ShmType::Normal,
                ShmPermission permission = ShmPermission::ReadWrite) noexcept {
    if (size == 0 || size > MAX_SHM_SIZE) {
      return KernelResult<ShmId>{KernelError::InvalidArgument};
    }
    ShmId region_id = next_shm_id_.fetch_add(1, containers::MemoryOrder::Relaxed);
    usize page_size = select_page_size(size, type);
    usize aligned_size = align_up_to_page(size, page_size);
    auto phys_result = allocate_physical_memory(aligned_size, page_size);
    if (!phys_result) {
      return KernelResult<ShmId>{phys_result.error()};
    }
    PhysAddr phys_addr = *phys_result;
    VirtAddr virt_addr = allocate_kernel_virtual_address(aligned_size);
    if (virt_addr == 0) {
      free_physical_memory(phys_addr, aligned_size);
      return KernelResult<ShmId>{KernelError::OutOfMemory};
    }
    ShmRegion *region = new ShmRegion(region_id, phys_addr, virt_addr, aligned_size,
                                       type, permission, creator_pid);
    region->page_size = page_size;
    region->attributes = get_memory_attributes(type);
    auto map_result = map_kernel_memory(virt_addr, phys_addr, aligned_size,
                                        region->attributes, page_size);
    if (!map_result) {
      delete region;
      free_physical_memory(phys_addr, aligned_size);
      return KernelResult<ShmId>{map_result.error()};
    }
    regions_.insert_or_update(region_id, region);
    (void)total_regions_.fetch_add(1, containers::MemoryOrder::Relaxed);
    (void)total_memory_usage_.fetch_add(aligned_size, containers::MemoryOrder::Relaxed);
    update_page_statistics(page_size, 1);
    return KernelResult<ShmId>{region_id};
  }

  [[nodiscard]] KernelResult<VirtAddr> map_to_process(
      ProcessId pid, ShmId region_id, VirtAddr hint_addr = 0,
      ShmPermission map_permission = ShmPermission::ReadWrite) noexcept {
    auto region_ptr = regions_.find(region_id);
    if (region_ptr == nullptr) {
      return KernelResult<VirtAddr>{KernelError::InvalidArgument};
    }
    const ShmRegion *region = *region_ptr;
    if (region == nullptr) {
      return KernelResult<VirtAddr>{KernelError::InvalidArgument};
    }
    if ((region->permission & map_permission) != map_permission) {
      return KernelResult<VirtAddr>{KernelError::PermissionDenied};
    }
    VirtAddr user_virt_addr = allocate_user_virtual_address(pid, region->size, hint_addr);
    if (user_virt_addr == 0) {
      return KernelResult<VirtAddr>{KernelError::OutOfMemory};
    }
    auto map_result = map_user_memory(pid, user_virt_addr, region->phys_base,
                                       region->size, region->attributes, region->page_size);
    if (!map_result) {
      free_user_virtual_address(pid, user_virt_addr, region->size);
      return KernelResult<VirtAddr>{map_result.error()};
    }
    const_cast<ShmRegion *>(region)->ref_count++;
    record_process_mapping(pid, region_id, user_virt_addr, region->size, map_permission);
    return KernelResult<VirtAddr>{user_virt_addr};
  }

  [[nodiscard]] VoidResult unmap_from_process(ProcessId pid, ShmId region_id) noexcept {
    auto region_ptr = regions_.find(region_id);
    if (region_ptr == nullptr) {
      return VoidResult{KernelError::InvalidArgument};
    }
    ShmRegion *region = *region_ptr;
    if (region == nullptr) {
      return VoidResult{KernelError::InvalidArgument};
    }
    auto mapping = find_process_mapping(pid, region_id);
    if (!mapping) {
      return VoidResult{KernelError::NotFound};
    }
    unmap_user_memory(pid, mapping->virt_addr, mapping->size);
    free_user_virtual_address(pid, mapping->virt_addr, mapping->size);
    region->ref_count--;
    remove_process_mapping(pid, region_id);
    if (region->ref_count == 0) {
      (void)destroy_region(region_id);
    }
    return VoidResult{};
  }

  [[nodiscard]] VoidResult destroy_region(ShmId region_id) noexcept {
    auto region_ptr = regions_.find(region_id);
    if (region_ptr == nullptr) {
      return VoidResult{KernelError::InvalidArgument};
    }
    ShmRegion *region = *region_ptr;
    if (region == nullptr) {
      return VoidResult{KernelError::InvalidArgument};
    }
    if (region->ref_count > 0) {
      return VoidResult{KernelError::Busy};
    }
    unmap_kernel_memory(region->virt_base, region->size);
    free_physical_memory(region->phys_base, region->size);
    free_kernel_virtual_address(region->virt_base, region->size);
    (void)total_regions_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    (void)total_memory_usage_.fetch_sub(region->size, containers::MemoryOrder::Relaxed);
    update_page_statistics(region->page_size, -1);
    regions_.remove(region_id);
    delete region;
    return VoidResult{};
  }

  [[nodiscard]] const ShmRegion *get_region_info(ShmId region_id) const noexcept {
    auto region_ptr = regions_.find(region_id);
    if (region_ptr == nullptr) { return nullptr; }
    return *region_ptr;
  }

  void sync_region(ShmId region_id) noexcept {
    auto region_ptr = regions_.find(region_id);
    if (region_ptr == nullptr) { return; }
    const ShmRegion *region = *region_ptr;
    if (region == nullptr) { return; }
    flush_cache_range(region->virt_base, region->size);
  }

  [[nodiscard]] SharedMemoryStats get_statistics() const noexcept {
    return {total_regions_.load(containers::MemoryOrder::Relaxed),
            total_memory_usage_.load(containers::MemoryOrder::Relaxed),
            large_pages_used_.load(containers::MemoryOrder::Relaxed),
            huge_pages_used_.load(containers::MemoryOrder::Relaxed)};
  }

  void cleanup_process_mappings(ProcessId pid) noexcept {
    auto mappings_ptr = process_mappings_.find(pid);
    if (mappings_ptr == nullptr) { return; }
    auto *mappings = *mappings_ptr;
    if (mappings == nullptr) { return; }
    mappings->for_each([this, pid](const ShmMapping &mapping) {
      (void)unmap_from_process(pid, mapping.region_id);
    });
    process_mappings_.remove(pid);
    delete mappings;
  }

private:
  [[nodiscard]] usize select_page_size(usize size, ShmType type) const noexcept {
    if (type == ShmType::LargePage || size >= SHM_HUGE_PAGE_SIZE) { return SHM_HUGE_PAGE_SIZE; }
    if (size >= SHM_LARGE_PAGE_SIZE) { return SHM_LARGE_PAGE_SIZE; }
    return PAGE_SIZE;
  }

  [[nodiscard]] static usize align_up_to_page(usize size, usize page_size) noexcept {
    return (size + page_size - 1) & ~(page_size - 1);
  }

  [[nodiscard]] static mm::MemoryAttributes get_memory_attributes(ShmType type) noexcept {
    switch (type) {
    case ShmType::Normal:      return mm::MemoryAttributes::NORMAL_CACHEABLE;
    case ShmType::DeviceMemory: return mm::MemoryAttributes::DEVICE_nGnRnE;
    case ShmType::DMA_Coherent: return mm::MemoryAttributes::NORMAL_NON_CACHEABLE;
    case ShmType::LargePage:   return mm::MemoryAttributes::NORMAL_CACHEABLE;
    default:                    return mm::MemoryAttributes::NORMAL_CACHEABLE;
    }
  }

  [[nodiscard]] KernelResult<PhysAddr>
  allocate_physical_memory([[maybe_unused]] usize size, [[maybe_unused]] usize page_size) noexcept {
    return KernelResult<PhysAddr>{static_cast<PhysAddr>(0x80000000)};
  }

  void free_physical_memory([[maybe_unused]] PhysAddr addr, [[maybe_unused]] usize size) noexcept {}

  [[nodiscard]] VirtAddr allocate_kernel_virtual_address([[maybe_unused]] usize size) noexcept {
    return 0xFFFF800000000000ULL;
  }

  void free_kernel_virtual_address([[maybe_unused]] VirtAddr addr, [[maybe_unused]] usize size) noexcept {}

  [[nodiscard]] VirtAddr allocate_user_virtual_address([[maybe_unused]] ProcessId pid,
                                                        [[maybe_unused]] usize size,
                                                        [[maybe_unused]] VirtAddr hint) noexcept {
    return 0x400000;
  }

  void free_user_virtual_address([[maybe_unused]] ProcessId pid,
                                  [[maybe_unused]] VirtAddr addr,
                                  [[maybe_unused]] usize size) noexcept {}

  [[nodiscard]] VoidResult map_kernel_memory([[maybe_unused]] VirtAddr virt,
                                              [[maybe_unused]] PhysAddr phys,
                                              [[maybe_unused]] usize size,
                                              [[maybe_unused]] mm::MemoryAttributes attr,
                                              [[maybe_unused]] usize page_size) noexcept {
    return VoidResult{};
  }

  void unmap_kernel_memory([[maybe_unused]] VirtAddr virt, [[maybe_unused]] usize size) noexcept {}

  [[nodiscard]] VoidResult map_user_memory([[maybe_unused]] ProcessId pid,
                                            [[maybe_unused]] VirtAddr virt,
                                            [[maybe_unused]] PhysAddr phys,
                                            [[maybe_unused]] usize size,
                                            [[maybe_unused]] mm::MemoryAttributes attr,
                                            [[maybe_unused]] usize page_size) noexcept {
    return VoidResult{};
  }

  void unmap_user_memory([[maybe_unused]] ProcessId pid,
                          [[maybe_unused]] VirtAddr virt,
                          [[maybe_unused]] usize size) noexcept {}

  void flush_cache_range(VirtAddr addr, usize size) noexcept {
    VirtAddr end = addr + size;
    for (VirtAddr va = addr; va < end; va += CACHE_LINE_SIZE) {
      arch::flush_cache_line(va);
    }
    arch::memory_barrier();
  }

  void record_process_mapping(ProcessId pid, ShmId region_id,
                              VirtAddr virt_addr, usize size,
                              ShmPermission permission) noexcept {
    auto mappings_ptr = process_mappings_.find(pid);
    containers::RcuList<ShmMapping> *mappings = nullptr;
    if (mappings_ptr != nullptr) { mappings = *mappings_ptr; }
    if (mappings == nullptr) {
      mappings = new containers::RcuList<ShmMapping>();
      process_mappings_.insert_or_update(pid, mappings);
    }
    mappings->push_front(ShmMapping(region_id, virt_addr, size, permission));
  }

  [[nodiscard]] containers::Optional<ShmMapping>
  find_process_mapping(ProcessId pid, ShmId region_id) noexcept {
    auto mappings_ptr = process_mappings_.find(pid);
    if (mappings_ptr == nullptr) { return containers::Optional<ShmMapping>{}; }
    auto *mappings = *mappings_ptr;
    if (mappings == nullptr) { return containers::Optional<ShmMapping>{}; }
    const ShmMapping *found = mappings->find_if(
        [region_id](const ShmMapping &mapping) { return mapping.region_id == region_id; });
    return found ? containers::Optional<ShmMapping>{*found} : containers::Optional<ShmMapping>{};
  }

  void remove_process_mapping(ProcessId pid, ShmId region_id) noexcept {
    auto mappings_ptr = process_mappings_.find(pid);
    if (mappings_ptr == nullptr) { return; }
    auto *mappings = *mappings_ptr;
    if (mappings == nullptr) { return; }
    const ShmMapping *found = mappings->find_if(
        [region_id](const ShmMapping &mapping) { return mapping.region_id == region_id; });
    if (found != nullptr) { mappings->remove(*found); }
  }

  void update_page_statistics(usize page_size, i32 delta) noexcept {
    if (page_size == SHM_LARGE_PAGE_SIZE) {
      if (delta > 0) { (void)large_pages_used_.fetch_add(static_cast<usize>(delta), containers::MemoryOrder::Relaxed); }
      else { (void)large_pages_used_.fetch_sub(static_cast<usize>(-delta), containers::MemoryOrder::Relaxed); }
    } else if (page_size == SHM_HUGE_PAGE_SIZE) {
      if (delta > 0) { (void)huge_pages_used_.fetch_add(static_cast<usize>(delta), containers::MemoryOrder::Relaxed); }
      else { (void)huge_pages_used_.fetch_sub(static_cast<usize>(-delta), containers::MemoryOrder::Relaxed); }
    }
  }

  void cleanup_all_regions() noexcept {
    regions_.for_each([this](const auto &entry) { (void)destroy_region(entry.key); });
  }
};

// 全局共享内存管理器实例
extern SharedMemoryManager *g_shared_memory_manager;

// ========================================================================
// zero_copy_channel.hpp
// ========================================================================

// 消息类型
enum class MessageType : u8 {
  Data = 0,
  Request = 1,
  Response = 2,
  Notification = 3,
  Capability = 4
};

// 消息头部结构
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
      : msg_id(0), sequence(0), timestamp(0), timeout(0), sender_tid(0),
        sender_pid(0), payload_size(0), type(MessageType::Data), priority(0),
        flags(0), padding{} {}
};

static_assert(sizeof(MessageHeader) == 64, "MessageHeader must be 64 bytes");

// 消息缓冲区槽位
struct MessageSlot {
  containers::AtomicU32 state;
  MessageHeader header;
  u8 *payload;

  enum State : u32 { Empty = 0, Writing = 1, Ready = 2, Reading = 3 };
};

// 零拷贝通道环形缓冲区
template <usize BufferSize = 64 * 1024, usize MaxMessageSize = 4096>
class ZeroCopyRingBuffer {
private:
  static_assert((BufferSize & (BufferSize - 1)) == 0, "BufferSize must be power of 2");
  static_assert(MaxMessageSize <= BufferSize / 4, "MaxMessageSize too large");
  static constexpr usize BUFFER_MASK = BufferSize - 1;

  struct alignas(64) RingControl {
    containers::AtomicU64 write_pos;
    containers::AtomicU64 read_pos;
    containers::AtomicU64 write_count;
    containers::AtomicU64 read_count;
    u64 reserved[4];
  };

  RingControl *control_;
  u8 *buffer_;
  usize buffer_size_;

public:
  struct RingBufferStats {
    u64 write_count;
    u64 read_count;
    u64 pending_messages;
    usize used_space;
  };

  ZeroCopyRingBuffer(void *shared_memory, usize size) noexcept
      : buffer_size_(size - sizeof(RingControl)) {
    control_ = static_cast<RingControl *>(shared_memory);
    buffer_ = static_cast<u8 *>(shared_memory) + sizeof(RingControl);
    static bool initialized = false;
    if (!initialized) {
      new (control_) RingControl{};
      initialized = true;
    }
  }

  [[nodiscard]] bool try_send(const MessageHeader &header, const void *payload) noexcept {
    usize total_size = sizeof(MessageHeader) + header.payload_size;
    total_size = ring_align_up(total_size, 8);
    if (total_size > MaxMessageSize) { return false; }
    u64 write_pos = control_->write_pos.load(containers::MemoryOrder::Acquire);
    u64 read_pos = control_->read_pos.load(containers::MemoryOrder::Acquire);
    if (write_pos - read_pos + total_size > buffer_size_) { return false; }
    if (!control_->write_pos.compare_exchange_weak(write_pos, write_pos + total_size,
                                                     containers::MemoryOrder::AcqRel)) {
      return false;
    }
    usize buffer_pos = write_pos & BUFFER_MASK;
    MessageHeader *msg_header = reinterpret_cast<MessageHeader *>(&buffer_[buffer_pos]);
    *msg_header = header;
    if (header.payload_size > 0 && payload != nullptr) {
      u8 *payload_ptr = &buffer_[buffer_pos + sizeof(MessageHeader)];
      fast_memcpy(payload_ptr, payload, header.payload_size);
    }
    atomic_thread_fence(memory_order_release);
    (void)control_->write_count.fetch_add(1, containers::MemoryOrder::Relaxed);
    return true;
  }

  [[nodiscard]] bool try_receive(MessageHeader &header, void *payload,
                                 usize max_payload_size) noexcept {
    u64 read_pos = control_->read_pos.load(containers::MemoryOrder::Acquire);
    u64 write_pos = control_->write_pos.load(containers::MemoryOrder::Acquire);
    if (read_pos >= write_pos) { return false; }
    usize buffer_pos = read_pos & BUFFER_MASK;
    const MessageHeader *msg_header = reinterpret_cast<const MessageHeader *>(&buffer_[buffer_pos]);
    header = *msg_header;
    if (header.payload_size > max_payload_size) { return false; }
    if (header.payload_size > 0 && payload != nullptr) {
      const u8 *payload_ptr = &buffer_[buffer_pos + sizeof(MessageHeader)];
      fast_memcpy(payload, payload_ptr, header.payload_size);
    }
    usize total_size = sizeof(MessageHeader) + header.payload_size;
    total_size = ring_align_up(total_size, 8);
    (void)control_->read_pos.fetch_add(total_size, containers::MemoryOrder::AcqRel);
    (void)control_->read_count.fetch_add(1, containers::MemoryOrder::Relaxed);
    return true;
  }

  [[nodiscard]] RingBufferStats get_statistics() const noexcept {
    u64 write_pos = control_->write_pos.load(containers::MemoryOrder::Relaxed);
    u64 read_pos = control_->read_pos.load(containers::MemoryOrder::Relaxed);
    return {control_->write_count.load(containers::MemoryOrder::Relaxed),
            control_->read_count.load(containers::MemoryOrder::Relaxed),
            control_->write_count.load(containers::MemoryOrder::Relaxed) -
                control_->read_count.load(containers::MemoryOrder::Relaxed),
            static_cast<usize>(write_pos - read_pos)};
  }

private:
  static void fast_memcpy(void *dst, const void *src, usize size) noexcept {
    const u8 *s = static_cast<const u8 *>(src);
    u8 *d = static_cast<u8 *>(dst);
    while (size >= 8) {
      *reinterpret_cast<u64 *>(d) = *reinterpret_cast<const u64 *>(s);
      d += 8; s += 8; size -= 8;
    }
    while (size > 0) { *d++ = *s++; size--; }
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
      : channel_id_(id), client_pid_(client), server_pid_(server),
        client_to_server_shm_(0), server_to_client_shm_(0), client_wait_seq_(0),
        server_wait_seq_(0), messages_sent_(0), messages_received_(0),
        bytes_transferred_(0) {}

  ~ZeroCopyChannel() noexcept { cleanup(); }

  ZeroCopyChannel(const ZeroCopyChannel &) = delete;
  ZeroCopyChannel &operator=(const ZeroCopyChannel &) = delete;
  ZeroCopyChannel(ZeroCopyChannel &&) = delete;
  ZeroCopyChannel &operator=(ZeroCopyChannel &&) = delete;

  [[nodiscard]] VoidResult initialize() noexcept {
    auto c2s_result = g_shared_memory_manager->create_region(
        client_pid_, sizeof(ZeroCopyRingBuffer<>) + 64 * 1024, ShmType::Normal,
        ShmPermission::ReadWrite);
    if (!c2s_result) { return VoidResult{c2s_result.error()}; }
    client_to_server_shm_ = *c2s_result;
    auto s2c_result = g_shared_memory_manager->create_region(
        server_pid_, sizeof(ZeroCopyRingBuffer<>) + 64 * 1024, ShmType::Normal,
        ShmPermission::ReadWrite);
    if (!s2c_result) {
      (void)g_shared_memory_manager->destroy_region(client_to_server_shm_);
      return VoidResult{s2c_result.error()};
    }
    server_to_client_shm_ = *s2c_result;
    auto c2s_region = g_shared_memory_manager->get_region_info(client_to_server_shm_);
    auto s2c_region = g_shared_memory_manager->get_region_info(server_to_client_shm_);
    if (c2s_region == nullptr || s2c_region == nullptr) {
      cleanup();
      return VoidResult{KernelError::InternalError};
    }
    client_to_server_ = make_unique<ZeroCopyRingBuffer<>>(
        reinterpret_cast<void *>(c2s_region->virt_base), c2s_region->size);
    server_to_client_ = make_unique<ZeroCopyRingBuffer<>>(
        reinterpret_cast<void *>(s2c_region->virt_base), s2c_region->size);
    if (!client_to_server_ || !server_to_client_) {
      cleanup();
      return VoidResult{KernelError::OutOfMemory};
    }
    return VoidResult{};
  }

  [[nodiscard]] bool send_message(ProcessId sender_pid, const MessageHeader &header,
                                  const void *payload) noexcept {
    ZeroCopyRingBuffer<> *buffer = nullptr;
    if (sender_pid == client_pid_) { buffer = client_to_server_.get(); }
    else if (sender_pid == server_pid_) { buffer = server_to_client_.get(); }
    else { return false; }
    if (buffer->try_send(header, payload)) {
      (void)messages_sent_.fetch_add(1, containers::MemoryOrder::Relaxed);
      (void)bytes_transferred_.fetch_add(header.payload_size, containers::MemoryOrder::Relaxed);
      notify_receiver(sender_pid);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool receive_message(ProcessId receiver_pid, MessageHeader &header,
                                     void *payload, usize max_payload_size) noexcept {
    ZeroCopyRingBuffer<> *buffer = nullptr;
    if (receiver_pid == client_pid_) { buffer = server_to_client_.get(); }
    else if (receiver_pid == server_pid_) { buffer = client_to_server_.get(); }
    else { return false; }
    if (buffer->try_receive(header, payload, max_payload_size)) {
      (void)messages_received_.fetch_add(1, containers::MemoryOrder::Relaxed);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool wait_for_message(ProcessId receiver_pid, MessageHeader &header,
                                      void *payload, usize max_payload_size,
                                      u64 timeout_ns = static_cast<u64>(-1)) noexcept {
    u64 start_time = get_current_time_ns();
    while (true) {
      if (receive_message(receiver_pid, header, payload, max_payload_size)) { return true; }
      u64 max_timeout = static_cast<u64>(-1);
      if (timeout_ns != max_timeout) {
        u64 current_time = get_current_time_ns();
        if (current_time - start_time >= timeout_ns) { return false; }
      }
      wait_for_notification(receiver_pid, 1000000);
    }
  }

  [[nodiscard]] ChannelStats get_statistics() const noexcept {
    auto c2s_stats = client_to_server_->get_statistics();
    auto s2c_stats = server_to_client_->get_statistics();
    return {messages_sent_.load(containers::MemoryOrder::Relaxed),
            messages_received_.load(containers::MemoryOrder::Relaxed),
            bytes_transferred_.load(containers::MemoryOrder::Relaxed),
            c2s_stats.pending_messages, s2c_stats.pending_messages};
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
    containers::AtomicU64 *wait_seq =
        (receiver_pid == client_pid_) ? &client_wait_seq_ : &server_wait_seq_;
    u64 current_seq = wait_seq->load(containers::MemoryOrder::Acquire);
    u64 start_time = get_current_time_ns();
    while (wait_seq->load(containers::MemoryOrder::Acquire) == current_seq) {
      if (get_current_time_ns() - start_time >= timeout_ns) { break; }
      arch::cpu_yield();
    }
  }

  [[nodiscard]] static u64 get_current_time_ns() noexcept {
    u64 count;
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, cntvct_el0" : "=r"(count));
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("rdtsc" : "=A"(count));
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("rdcycle %0" : "=r"(count));
#else
    count = 0;
#endif
    return count;
  }

  void cleanup() noexcept {
    if (client_to_server_shm_ != 0) {
      (void)g_shared_memory_manager->destroy_region(client_to_server_shm_);
      client_to_server_shm_ = 0;
    }
    if (server_to_client_shm_ != 0) {
      (void)g_shared_memory_manager->destroy_region(server_to_client_shm_);
      server_to_client_shm_ = 0;
    }
    client_to_server_.reset();
    server_to_client_.reset();
  }
};

// ========================================================================
// ipc_manager.hpp
// ========================================================================

// IPC端点类型
enum class EndpointType : u8 {
  Server = 0,
  Client = 1,
  Peer = 2
};

// IPC服务描述符
struct ServiceDescriptor {
  ServiceId service_id;
  ProcessId provider_pid;
  EndpointId endpoint_id;
  const char *service_name;
  u32 max_clients;
  u32 current_clients;
  bool is_public;
  u64 creation_time;

  ServiceDescriptor(ServiceId id, ProcessId pid, const char *name) noexcept
      : service_id(id), provider_pid(pid), endpoint_id(0), service_name(name),
        max_clients(256), current_clients(0), is_public(true), creation_time(0) {}
};

// IPC连接描述符
struct ConnectionDescriptor {
  ChannelId channel_id;
  ProcessId client_pid;
  ProcessId server_pid;
  ServiceId service_id;
  u64 established_time;
  u64 last_activity;
  u32 messages_sent;
  u32 messages_received;
  u64 bytes_transferred;

  ConnectionDescriptor(ChannelId cid, ProcessId client, ProcessId server, ServiceId sid) noexcept
      : channel_id(cid), client_pid(client), server_pid(server), service_id(sid),
        established_time(0), last_activity(0), messages_sent(0), messages_received(0),
        bytes_transferred(0) {}
};

// IPC管理器主类
class IpcManager {
private:
  containers::RcuHashMap<ServiceId, ServiceDescriptor *> services_;
  containers::RcuHashMap<ChannelId, unique_ptr<ZeroCopyChannel>> channels_;
  containers::RcuHashMap<ChannelId, ConnectionDescriptor *> connections_;
  containers::RcuHashMap<ProcessId, containers::RcuList<ChannelId> *> process_channels_;
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
      : next_service_id_(1), next_channel_id_(1),
        shared_memory_manager_(shm_manager), total_services_(0),
        total_channels_(0), messages_processed_(0), bytes_transferred_(0) {}

  ~IpcManager() noexcept { cleanup(); }

  IpcManager(const IpcManager &) = delete;
  IpcManager &operator=(const IpcManager &) = delete;
  IpcManager(IpcManager &&) = delete;
  IpcManager &operator=(IpcManager &&) = delete;

  [[nodiscard]] KernelResult<ServiceId>
  register_service(ProcessId provider_pid, const char *service_name,
                   u32 max_clients = 256) noexcept {
    if (service_name == nullptr) { return KernelResult<ServiceId>{KernelError::InvalidArgument}; }
    if (find_service_by_name(service_name) != nullptr) {
      return KernelResult<ServiceId>{KernelError::AlreadyExists};
    }
    ServiceId service_id = next_service_id_.fetch_add(1, containers::MemoryOrder::Relaxed);
    ServiceDescriptor *service = new ServiceDescriptor(service_id, provider_pid, service_name);
    service->max_clients = max_clients;
    services_.insert_or_update(service_id, service);
    (void)total_services_.fetch_add(1, containers::MemoryOrder::Relaxed);
    return KernelResult<ServiceId>{service_id};
  }

  [[nodiscard]] VoidResult unregister_service(ServiceId service_id, ProcessId provider_pid) noexcept {
    auto service_ptr = services_.find(service_id);
    if (service_ptr == nullptr) { return VoidResult{KernelError::NotFound}; }
    const ServiceDescriptor *service = *service_ptr;
    if (service->provider_pid != provider_pid) { return VoidResult{KernelError::PermissionDenied}; }
    close_service_channels(service_id);
    services_.remove(service_id);
    delete service;
    (void)total_services_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    return VoidResult{};
  }

  [[nodiscard]] KernelResult<ChannelId> connect_to_service(ProcessId client_pid, ServiceId service_id) noexcept {
    auto service_ptr = services_.find(service_id);
    if (service_ptr == nullptr) { return KernelResult<ChannelId>{KernelError::NotFound}; }
    ServiceDescriptor *service = *service_ptr;
    if (service->current_clients >= service->max_clients) {
      return KernelResult<ChannelId>{KernelError::ResourceExhausted};
    }
    ChannelId channel_id = next_channel_id_.fetch_add(1, containers::MemoryOrder::Relaxed);
    auto channel = make_unique<ZeroCopyChannel>(channel_id, client_pid, service->provider_pid);
    if (!channel) { return KernelResult<ChannelId>{KernelError::OutOfMemory}; }
    auto init_result = channel->initialize();
    if (!init_result) { return KernelResult<ChannelId>{init_result.error()}; }
    ConnectionDescriptor *conn = new ConnectionDescriptor(channel_id, client_pid, service->provider_pid, service_id);
    channels_.insert_or_update(channel_id, moss::move(channel));
    connections_.insert_or_update(channel_id, conn);
    service->current_clients++;
    (void)total_channels_.fetch_add(1, containers::MemoryOrder::Relaxed);
    add_process_channel(client_pid, channel_id);
    add_process_channel(service->provider_pid, channel_id);
    return KernelResult<ChannelId>{channel_id};
  }

  [[nodiscard]] KernelResult<ChannelId> connect_to_service_by_name(ProcessId client_pid, const char *service_name) noexcept {
    ServiceDescriptor *service = find_service_by_name(service_name);
    if (service == nullptr) { return KernelResult<ChannelId>{KernelError::NotFound}; }
    return connect_to_service(client_pid, service->service_id);
  }

  [[nodiscard]] VoidResult disconnect(ChannelId channel_id, ProcessId requester_pid) noexcept {
    auto conn_ptr = connections_.find(channel_id);
    if (conn_ptr == nullptr) { return VoidResult{KernelError::NotFound}; }
    ConnectionDescriptor *conn = *conn_ptr;
    if (conn->client_pid != requester_pid && conn->server_pid != requester_pid) {
      return VoidResult{KernelError::PermissionDenied};
    }
    channels_.remove(channel_id);
    auto service_ptr = services_.find(conn->service_id);
    if (service_ptr != nullptr) { ServiceDescriptor *service = *service_ptr; service->current_clients--; }
    remove_process_channel(conn->client_pid, channel_id);
    remove_process_channel(conn->server_pid, channel_id);
    connections_.remove(channel_id);
    delete conn;
    (void)total_channels_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    return VoidResult{};
  }

  [[nodiscard]] bool send_message(ChannelId channel_id, ProcessId sender_pid,
                                  const MessageHeader &header, const void *payload) noexcept {
    const auto *channel = channels_.find(channel_id);
    if (channel == nullptr || !*channel) { return false; }
    bool success = (*channel)->send_message(sender_pid, header, payload);
    if (success) {
      (void)messages_processed_.fetch_add(1, containers::MemoryOrder::Relaxed);
      (void)bytes_transferred_.fetch_add(header.payload_size, containers::MemoryOrder::Relaxed);
      update_connection_statistics(channel_id, true, header.payload_size);
    }
    return success;
  }

  [[nodiscard]] bool receive_message(ChannelId channel_id, ProcessId receiver_pid,
                                     MessageHeader &header, void *payload, usize max_payload_size) noexcept {
    const auto *channel = channels_.find(channel_id);
    if (channel == nullptr || !*channel) { return false; }
    bool success = (*channel)->receive_message(receiver_pid, header, payload, max_payload_size);
    if (success) {
      (void)messages_processed_.fetch_add(1, containers::MemoryOrder::Relaxed);
      update_connection_statistics(channel_id, false, header.payload_size);
    }
    return success;
  }

  [[nodiscard]] bool wait_for_message(ChannelId channel_id, ProcessId receiver_pid,
                                      MessageHeader &header, void *payload,
                                      usize max_payload_size, u64 timeout_ns = static_cast<u64>(-1)) noexcept {
    const auto *channel = channels_.find(channel_id);
    if (channel == nullptr || !*channel) { return false; }
    return (*channel)->wait_for_message(receiver_pid, header, payload, max_payload_size, timeout_ns);
  }

  void get_service_list([[maybe_unused]] ProcessId requester_pid,
                        void (*callback)(const ServiceDescriptor &, void *),
                        void *context) const noexcept {
    services_.for_each([callback, context](const auto &entry) {
      const ServiceDescriptor &service = *entry.value;
      if (service.is_public) { callback(service, context); }
    });
  }

  [[nodiscard]] IpcStats get_statistics() const noexcept {
    return {total_services_.load(containers::MemoryOrder::Relaxed),
            total_channels_.load(containers::MemoryOrder::Relaxed),
            messages_processed_.load(containers::MemoryOrder::Relaxed),
            bytes_transferred_.load(containers::MemoryOrder::Relaxed)};
  }

  void get_process_connections(ProcessId pid,
                               void (*callback)(const ConnectionDescriptor &, void *),
                               void *context) const noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (channels_ptr == nullptr) { return; }
    const auto *channels = *channels_ptr;
    if (channels == nullptr) { return; }
    channels->for_each([this, callback, context](const ChannelId &channel_id) {
      auto conn_ptr = connections_.find(channel_id);
      if (conn_ptr != nullptr) { callback(**conn_ptr, context); }
    });
  }

  void cleanup_process_ipc(ProcessId pid) noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (channels_ptr == nullptr) { return; }
    const auto *channels = *channels_ptr;
    if (channels == nullptr) { return; }
    containers::RcuList<ChannelId> channels_to_close;
    channels->for_each([&channels_to_close](const ChannelId &channel_id) {
      channels_to_close.push_front(channel_id);
    });
    channels_to_close.for_each([this, pid](const ChannelId &channel_id) {
      (void)disconnect(channel_id, pid);
    });
    process_channels_.remove(pid);
    if (shared_memory_manager_ != nullptr) {
      shared_memory_manager_->cleanup_process_mappings(pid);
    }
  }

private:
  [[nodiscard]] ServiceDescriptor *find_service_by_name(const char *name) noexcept {
    ServiceDescriptor *found_service = nullptr;
    services_.for_each([name, &found_service](const auto &entry) {
      const ServiceDescriptor *service = entry.value;
      if (string_compare(service->service_name, name) == 0) {
        found_service = const_cast<ServiceDescriptor *>(service);
        return;
      }
    });
    return found_service;
  }

  void close_service_channels(ServiceId service_id) noexcept {
    containers::RcuList<ChannelId> channels_to_close;
    connections_.for_each([service_id, &channels_to_close](const auto &entry) {
      const ConnectionDescriptor *conn = entry.value;
      if (conn->service_id == service_id) { channels_to_close.push_front(conn->channel_id); }
    });
    channels_to_close.for_each([this](const ChannelId &channel_id) {
      auto conn_ptr = connections_.find(channel_id);
      if (conn_ptr != nullptr) {
        const ConnectionDescriptor *conn = *conn_ptr;
        (void)disconnect(channel_id, conn->client_pid);
      }
    });
  }

  void add_process_channel(ProcessId pid, ChannelId channel_id) noexcept {
    auto channels_ptr = process_channels_.find(pid);
    containers::RcuList<ChannelId> *channels = nullptr;
    if (channels_ptr != nullptr) { channels = *channels_ptr; }
    if (channels == nullptr) {
      channels = new containers::RcuList<ChannelId>();
      process_channels_.insert_or_update(pid, channels);
    }
    channels->push_front(channel_id);
  }

  void remove_process_channel(ProcessId pid, ChannelId channel_id) noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (channels_ptr != nullptr) {
      auto *channels = *channels_ptr;
      if (channels != nullptr) {
        const ChannelId *found = channels->find_if(
            [channel_id](const ChannelId &id) { return id == channel_id; });
        if (found != nullptr) { channels->remove(*found); }
      }
    }
  }

  void update_connection_statistics(ChannelId channel_id, bool is_send, u32 bytes) noexcept {
    auto conn_ptr = connections_.find(channel_id);
    if (conn_ptr == nullptr) { return; }
    ConnectionDescriptor *conn = *conn_ptr;
    if (conn != nullptr) {
      if (is_send) { conn->messages_sent++; } else { conn->messages_received++; }
      conn->bytes_transferred += bytes;
      conn->last_activity = get_current_time();
    }
  }

  [[nodiscard]] static int string_compare(const char *s1, const char *s2) noexcept {
    while (*s1 && *s2 && *s1 == *s2) { s1++; s2++; }
    return *s1 - *s2;
  }

  [[nodiscard]] static u64 get_current_time() noexcept {
    u64 count;
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, cntvct_el0" : "=r"(count));
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("rdtsc" : "=A"(count));
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("rdcycle %0" : "=r"(count));
#else
    count = 0;
#endif
    return count;
  }

  void cleanup() noexcept {
    channels_.for_each([](const auto &entry) { (void)entry; });
    connections_.for_each([](const auto &entry) { (void)entry; delete entry.value; });
    services_.for_each([](const auto &entry) { (void)entry; delete entry.value; });
    process_channels_.for_each([](const auto &entry) { (void)entry; delete entry.value; });
  }
};

// 全局IPC管理器实例
extern IpcManager *g_ipc_manager;

// 便利的C风格接口
extern "C" {
KernelError ipc_register_service(ProcessId pid, const char *name, ServiceId *out_id);
KernelError ipc_connect_to_service(ProcessId pid, const char *name, ChannelId *out_channel);
KernelError ipc_send_message(ChannelId channel, ProcessId sender,
                             const MessageHeader *header, const void *payload);
KernelError ipc_receive_message(ChannelId channel, ProcessId receiver,
                                MessageHeader *header, void *payload, usize max_size);
KernelError ipc_disconnect(ChannelId channel, ProcessId pid);
}

} // namespace moss::kernel::ipc
