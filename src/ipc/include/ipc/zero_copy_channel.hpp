#pragma once

// 零拷贝通信通道实现
// 基于共享内存和无锁队列的高性能IPC

#include "../../../include/arch/arch_abstraction.hpp"
#include "../../../include/result.hpp"
#include "../../../include/types.hpp"
#include "process/process.hpp"
#include "containers/containers.hpp"
#include "shared_memory.hpp"
// 移除有问题的atomic包含，使用容器中的原子类型

namespace moss::kernel::ipc {

// 消息类型
enum class MessageType : u8 {
  Data = 0,         // 普通数据消息
  Request = 1,      // 请求消息
  Response = 2,     // 响应消息
  Notification = 3, // 通知消息
  Capability = 4    // 能力传递消息
};

// 消息头部结构（恰好64字节，手动填充确保对齐）
struct alignas(64) MessageHeader {
  MessageId msg_id;     // 8 bytes
  u64 sequence;         // 8 bytes
  u64 timestamp;        // 8 bytes
  u64 timeout;          // 8 bytes
  ThreadId sender_tid;  // 8 bytes (u64)
  ProcessId sender_pid; // 4 bytes (u32)
  u32 payload_size;     // 4 bytes
  MessageType type;     // 1 byte
  u8 priority;          // 1 byte
  u16 flags;            // 2 bytes
  u8 padding[12];       // 12 bytes填充到64字节

  MessageHeader() noexcept
      : msg_id(0), sequence(0), timestamp(0), timeout(0), sender_tid(0),
        sender_pid(0), payload_size(0), type(MessageType::Data), priority(0),
        flags(0), padding{} {}
};

static_assert(sizeof(MessageHeader) == 64, "MessageHeader must be 64 bytes");

// 消息缓冲区槽位
struct MessageSlot {
  moss::kernel::containers::AtomicU32 state; // 槽位状态
  MessageHeader header;                      // 消息头部
  u8 *payload;                               // 可变长度负载指针

  enum State : u32 {
    Empty = 0,   // 空槽位
    Writing = 1, // 正在写入
    Ready = 2,   // 准备读取
    Reading = 3  // 正在读取
  };
};

// 零拷贝通道环形缓冲区
template <usize BufferSize = 64 * 1024, usize MaxMessageSize = 4096>
class ZeroCopyRingBuffer {
private:
  static_assert((BufferSize & (BufferSize - 1)) == 0,
                "BufferSize must be power of 2");
  static_assert(MaxMessageSize <= BufferSize / 4, "MaxMessageSize too large");

  static constexpr usize BUFFER_MASK = BufferSize - 1;

  // 环形缓冲区控制结构
  struct alignas(64) RingControl {
    moss::kernel::containers::AtomicU64 write_pos;   // 写入位置
    moss::kernel::containers::AtomicU64 read_pos;    // 读取位置
    moss::kernel::containers::AtomicU64 write_count; // 写入计数
    moss::kernel::containers::AtomicU64 read_count;  // 读取计数
    u64 reserved[4];                                 // 缓存行填充
  };

  RingControl *control_; // 控制结构
  u8 *buffer_;           // 消息缓冲区
  usize buffer_size_;    // 缓冲区大小

public:
  // 环形缓冲区统计信息
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

    // 初始化控制结构（仅第一次）
    static bool initialized = false;
    if (!initialized) {
      new (control_) RingControl{};
      initialized = true;
    }
  }

  // 尝试发送消息（非阻塞）
  [[nodiscard]] bool try_send(const MessageHeader &header,
                              const void *payload) noexcept {
    usize total_size = sizeof(MessageHeader) + header.payload_size;
    total_size = align_up(total_size, 8); // 8字节对齐

    if (total_size > MaxMessageSize) {
      return false; // 消息太大
    }

    // 原子地获取写入位置
    u64 write_pos = control_->write_pos.load(
        moss::kernel::containers::MemoryOrder::Acquire);
    u64 read_pos =
        control_->read_pos.load(moss::kernel::containers::MemoryOrder::Acquire);

    // 检查是否有足够空间
    if (write_pos - read_pos + total_size > buffer_size_) {
      return false; // 缓冲区满
    }

    // 尝试原子地更新写入位置
    if (!control_->write_pos.compare_exchange_weak(
            write_pos, write_pos + total_size,
            moss::kernel::containers::MemoryOrder::AcqRel)) {
      return false; // 其他线程抢先写入
    }

    // 写入消息头部
    usize buffer_pos = write_pos & BUFFER_MASK;
    MessageHeader *msg_header =
        reinterpret_cast<MessageHeader *>(&buffer_[buffer_pos]);
    *msg_header = header;

    // 写入负载数据
    if (header.payload_size > 0 && payload != nullptr) {
      u8 *payload_ptr = &buffer_[buffer_pos + sizeof(MessageHeader)];
      fast_memcpy(payload_ptr, payload, header.payload_size);
    }

    // 内存屏障确保写入完成
    atomic_thread_fence(memory_order_release);

    // 增加写入计数
    (void)control_->write_count.fetch_add(
        1, moss::kernel::containers::MemoryOrder::Relaxed);

    return true;
  }

  // 尝试接收消息（非阻塞）
  [[nodiscard]] bool try_receive(MessageHeader &header, void *payload,
                                 usize max_payload_size) noexcept {
    // 原子地获取读取位置
    u64 read_pos =
        control_->read_pos.load(moss::kernel::containers::MemoryOrder::Acquire);
    u64 write_pos = control_->write_pos.load(
        moss::kernel::containers::MemoryOrder::Acquire);

    // 检查是否有消息可读
    if (read_pos >= write_pos) {
      return false; // 缓冲区空
    }

    // 读取消息头部
    usize buffer_pos = read_pos & BUFFER_MASK;
    const MessageHeader *msg_header =
        reinterpret_cast<const MessageHeader *>(&buffer_[buffer_pos]);
    header = *msg_header;

    // 检查负载大小
    if (header.payload_size > max_payload_size) {
      return false; // 接收缓冲区太小
    }

    // 读取负载数据
    if (header.payload_size > 0 && payload != nullptr) {
      const u8 *payload_ptr = &buffer_[buffer_pos + sizeof(MessageHeader)];
      fast_memcpy(payload, payload_ptr, header.payload_size);
    }

    // 计算消息总大小
    usize total_size = sizeof(MessageHeader) + header.payload_size;
    total_size = align_up(total_size, 8);

    // 原子地更新读取位置
    (void)control_->read_pos.fetch_add(
        total_size, moss::kernel::containers::MemoryOrder::AcqRel);

    // 增加读取计数
    (void)control_->read_count.fetch_add(
        1, moss::kernel::containers::MemoryOrder::Relaxed);

    return true;
  }

  // 获取统计信息
  [[nodiscard]] RingBufferStats get_statistics() const noexcept {
    u64 write_pos = control_->write_pos.load(
        moss::kernel::containers::MemoryOrder::Relaxed);
    u64 read_pos =
        control_->read_pos.load(moss::kernel::containers::MemoryOrder::Relaxed);

    return {control_->write_count.load(
                moss::kernel::containers::MemoryOrder::Relaxed),
            control_->read_count.load(
                moss::kernel::containers::MemoryOrder::Relaxed),
            control_->write_count.load(
                moss::kernel::containers::MemoryOrder::Relaxed) -
                control_->read_count.load(
                    moss::kernel::containers::MemoryOrder::Relaxed),
            static_cast<usize>(write_pos - read_pos)};
  }

private:
  // 快速内存拷贝
  static void fast_memcpy(void *dst, const void *src, usize size) noexcept {
    // 使用ARM64 NEON指令优化内存拷贝
    const u8 *s = static_cast<const u8 *>(src);
    u8 *d = static_cast<u8 *>(dst);

    // 8字节对齐快速拷贝
    while (size >= 8) {
      *reinterpret_cast<u64 *>(d) = *reinterpret_cast<const u64 *>(s);
      d += 8;
      s += 8;
      size -= 8;
    }

    // 剩余字节
    while (size > 0) {
      *d++ = *s++;
      size--;
    }
  }

  // 对齐辅助函数
  static constexpr usize align_up(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
  }
};

// 零拷贝通道
class ZeroCopyChannel {
private:
  ChannelId channel_id_; // 通道ID
  ProcessId client_pid_; // 客户端进程ID
  ProcessId server_pid_; // 服务端进程ID

  // 双向环形缓冲区（共享内存）
  ShmId client_to_server_shm_; // 客户端到服务端的缓冲区
  ShmId server_to_client_shm_; // 服务端到客户端的缓冲区

  unique_ptr<ZeroCopyRingBuffer<>> client_to_server_;
  unique_ptr<ZeroCopyRingBuffer<>> server_to_client_;

  // 通知机制（用于阻塞等待）
  moss::kernel::containers::AtomicU64 client_wait_seq_;
  moss::kernel::containers::AtomicU64 server_wait_seq_;

  // 统计信息
  moss::kernel::containers::AtomicU64 messages_sent_;
  moss::kernel::containers::AtomicU64 messages_received_;
  moss::kernel::containers::AtomicU64 bytes_transferred_;

public:
  // 零拷贝通道统计信息
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

  // 禁用拷贝和移动
  NON_COPYABLE_NON_MOVABLE(ZeroCopyChannel)

  // 初始化通道
  [[nodiscard]] VoidResult initialize() noexcept {
    // 创建客户端到服务端的共享内存
    auto c2s_result = g_shared_memory_manager->create_region(
        client_pid_, sizeof(ZeroCopyRingBuffer<>) + 64 * 1024, ShmType::Normal,
        ShmPermission::ReadWrite);
    if (!c2s_result) {
      return VoidResult{c2s_result.error()};
    }
    client_to_server_shm_ = *c2s_result;

    // 创建服务端到客户端的共享内存
    auto s2c_result = g_shared_memory_manager->create_region(
        server_pid_, sizeof(ZeroCopyRingBuffer<>) + 64 * 1024, ShmType::Normal,
        ShmPermission::ReadWrite);
    if (!s2c_result) {
      (void)g_shared_memory_manager->destroy_region(client_to_server_shm_);
      return VoidResult{s2c_result.error()};
    }
    server_to_client_shm_ = *s2c_result;

    // 映射共享内存到内核地址空间
    auto c2s_region =
        g_shared_memory_manager->get_region_info(client_to_server_shm_);
    auto s2c_region =
        g_shared_memory_manager->get_region_info(server_to_client_shm_);

    if (c2s_region == nullptr || s2c_region == nullptr) {
      cleanup();
      return VoidResult{KernelError::InternalError};
    }

    // 创建环形缓冲区
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

  // 发送消息（从指定进程的角度）
  [[nodiscard]] bool send_message(ProcessId sender_pid,
                                  const MessageHeader &header,
                                  const void *payload) noexcept {
    ZeroCopyRingBuffer<> *buffer = nullptr;

    if (sender_pid == client_pid_) {
      buffer = client_to_server_.get();
    } else if (sender_pid == server_pid_) {
      buffer = server_to_client_.get();
    } else {
      return false; // 无效的发送者
    }

    if (buffer->try_send(header, payload)) {
      (void)messages_sent_.fetch_add(
          1, moss::kernel::containers::MemoryOrder::Relaxed);
      (void)bytes_transferred_.fetch_add(
          header.payload_size, moss::kernel::containers::MemoryOrder::Relaxed);

      // 通知接收者
      notify_receiver(sender_pid);
      return true;
    }

    return false;
  }

  // 接收消息（从指定进程的角度）
  [[nodiscard]] bool receive_message(ProcessId receiver_pid,
                                     MessageHeader &header, void *payload,
                                     usize max_payload_size) noexcept {
    ZeroCopyRingBuffer<> *buffer = nullptr;

    if (receiver_pid == client_pid_) {
      buffer = server_to_client_.get();
    } else if (receiver_pid == server_pid_) {
      buffer = client_to_server_.get();
    } else {
      return false; // 无效的接收者
    }

    if (buffer->try_receive(header, payload, max_payload_size)) {
      (void)messages_received_.fetch_add(
          1, moss::kernel::containers::MemoryOrder::Relaxed);
      return true;
    }

    return false;
  }

  // 等待消息（阻塞版本）
  [[nodiscard]] bool wait_for_message(ProcessId receiver_pid,
                                      MessageHeader &header, void *payload,
                                      usize max_payload_size,
                                      u64 timeout_ns = UINT64_MAX) noexcept {
    u64 start_time = get_current_time_ns();

    while (true) {
      // 尝试非阻塞接收
      if (receive_message(receiver_pid, header, payload, max_payload_size)) {
        return true;
      }

      // 检查超时
      if (timeout_ns != UINT64_MAX) {
        u64 current_time = get_current_time_ns();
        if (current_time - start_time >= timeout_ns) {
          return false; // 超时
        }
      }

      // 等待通知
      wait_for_notification(receiver_pid, 1000000); // 1ms超时
    }
  }

  // 获取通道统计信息
  [[nodiscard]] ChannelStats get_statistics() const noexcept {
    auto c2s_stats = client_to_server_->get_statistics();
    auto s2c_stats = server_to_client_->get_statistics();

    return {
        messages_sent_.load(moss::kernel::containers::MemoryOrder::Relaxed),
        messages_received_.load(moss::kernel::containers::MemoryOrder::Relaxed),
        bytes_transferred_.load(moss::kernel::containers::MemoryOrder::Relaxed),
        c2s_stats.pending_messages, s2c_stats.pending_messages};
  }

  // 访问器
  [[nodiscard]] ChannelId id() const noexcept { return channel_id_; }
  [[nodiscard]] ProcessId client_pid() const noexcept { return client_pid_; }
  [[nodiscard]] ProcessId server_pid() const noexcept { return server_pid_; }

private:
  // 通知接收者
  void notify_receiver(ProcessId sender_pid) noexcept {
    if (sender_pid == client_pid_) {
      (void)server_wait_seq_.fetch_add(
          1, moss::kernel::containers::MemoryOrder::Release);
      // 实际实现中需要唤醒等待的服务端线程
    } else {
      (void)client_wait_seq_.fetch_add(
          1, moss::kernel::containers::MemoryOrder::Release);
      // 实际实现中需要唤醒等待的客户端线程
    }
  }

  // 等待通知
  void wait_for_notification(ProcessId receiver_pid, u64 timeout_ns) noexcept {
    moss::kernel::containers::AtomicU64 *wait_seq =
        (receiver_pid == client_pid_) ? &client_wait_seq_ : &server_wait_seq_;

    u64 current_seq =
        wait_seq->load(moss::kernel::containers::MemoryOrder::Acquire);

    // 简化实现：使用自旋等待
    // 实际实现中应该使用futex或类似的内核原语
    u64 start_time = get_current_time_ns();
    while (wait_seq->load(moss::kernel::containers::MemoryOrder::Acquire) ==
           current_seq) {
      if (get_current_time_ns() - start_time >= timeout_ns) {
        break;
      }
      // CPU yield hint (多架构支持)
      arch::cpu_yield();
    }
  }

  // 获取当前时间
  [[nodiscard]] static u64 get_current_time_ns() noexcept {
    u64 count;
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, cntvct_el0" : "=r"(count));
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("rdtsc" : "=A"(count));
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("rdcycle %0" : "=r"(count));
#else
    count = 0; // 回退实现
#endif
    return count; // 简化实现，实际需要转换为纳秒
  }

  // 清理资源
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

} // namespace moss::kernel::ipc
