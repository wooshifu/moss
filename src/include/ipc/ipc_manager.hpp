#pragma once

// 高性能IPC管理器
// 统一管理零拷贝通道、共享内存、消息传递和能力传递

#include "../../ipc/shared_memory.hpp"
#include "../../ipc/zero_copy_channel.hpp"
#include "../containers/containers.hpp"
#include "../include/result.hpp"
#include "../include/smart_ptr.hpp"
#include "../include/types.hpp"
#include "../process/process.hpp"

namespace moss::kernel::ipc {

// 简化类型使用
template <typename T> using unique_ptr = moss::kernel::UniquePtr<T>;
using moss::kernel::make_unique;
using VoidResult = moss::kernel::Result<void, moss::kernel::ErrorCode>;

// IPC端点类型
enum class EndpointType : u8 {
  Server = 0, // 服务端点
  Client = 1, // 客户端点
  Peer = 2    // 对等端点
};

// IPC服务描述符
struct ServiceDescriptor {
  ServiceId service_id;     // 服务ID
  ProcessId provider_pid;   // 服务提供者进程ID
  EndpointId endpoint_id;   // 端点ID
  const char *service_name; // 服务名称
  u32 max_clients;          // 最大客户端数量
  u32 current_clients;      // 当前客户端数量
  bool is_public;           // 是否为公共服务
  u64 creation_time;        // 创建时间

  ServiceDescriptor(ServiceId id, ProcessId pid, const char *name) noexcept
      : service_id(id), provider_pid(pid), endpoint_id(0), service_name(name),
        max_clients(256), current_clients(0), is_public(true),
        creation_time(0) {}
};

// IPC连接描述符
struct ConnectionDescriptor {
  ChannelId channel_id;  // 通道ID
  ProcessId client_pid;  // 客户端进程ID
  ProcessId server_pid;  // 服务端进程ID
  ServiceId service_id;  // 服务ID
  u64 established_time;  // 建立时间
  u64 last_activity;     // 最后活动时间
  u32 messages_sent;     // 发送消息数
  u32 messages_received; // 接收消息数
  u64 bytes_transferred; // 传输字节数

  ConnectionDescriptor(ChannelId cid, ProcessId client, ProcessId server,
                       ServiceId sid) noexcept
      : channel_id(cid), client_pid(client), server_pid(server),
        service_id(sid), established_time(0), last_activity(0),
        messages_sent(0), messages_received(0), bytes_transferred(0) {}
};

// IPC管理器主类
class IpcManager {
private:
  // 服务注册表
  containers::RcuHashMap<ServiceId, ServiceDescriptor *> services_;

  // 通道管理
  containers::RcuHashMap<ChannelId, unique_ptr<ZeroCopyChannel>> channels_;

  // 连接管理
  containers::RcuHashMap<ChannelId, ConnectionDescriptor *> connections_;

  // 进程的IPC连接映射
  containers::RcuHashMap<ProcessId, containers::RcuList<ChannelId> *>
      process_channels_;

  // ID分配器
  containers::AtomicCounter<ServiceId> next_service_id_;
  containers::AtomicCounter<ChannelId> next_channel_id_;

  // 共享内存管理器引用
  SharedMemoryManager *shared_memory_manager_;

  // 统计信息
  containers::AtomicCounter<u64> total_services_;
  containers::AtomicCounter<u64> total_channels_;
  containers::AtomicCounter<u64> messages_processed_;
  containers::AtomicCounter<u64> bytes_transferred_;

public:
  // IPC管理器统计信息
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

  // 禁用拷贝和移动
  NON_COPYABLE_NON_MOVABLE(IpcManager)

  // 注册服务
  [[nodiscard]] KernelResult<ServiceId>
  register_service(ProcessId provider_pid, const char *service_name,
                   u32 max_clients = 256) noexcept {
    if (service_name == nullptr) {
      return KernelResult<ServiceId>{KernelError::InvalidArgument};
    }

    // 检查服务名是否已存在
    if (find_service_by_name(service_name) != nullptr) {
      return KernelResult<ServiceId>{KernelError::AlreadyExists};
    }

    // 分配服务ID
    ServiceId service_id =
        next_service_id_.fetch_add(1, containers::MemoryOrder::Relaxed);

    // 创建服务描述符
    ServiceDescriptor *service =
        new ServiceDescriptor(service_id, provider_pid, service_name);
    service->max_clients = max_clients;

    // 注册服务
    services_.insert_or_update(service_id, service);
    (void)total_services_.fetch_add(1, containers::MemoryOrder::Relaxed);

    return KernelResult<ServiceId>{service_id};
  }

  // 取消注册服务
  [[nodiscard]] VoidResult unregister_service(ServiceId service_id,
                                              ProcessId provider_pid) noexcept {
    auto service_ptr = services_.find(service_id);
    if (service_ptr == nullptr) {
      return VoidResult{KernelError::NotFound};
    }
    const ServiceDescriptor *service = *service_ptr;

    // 验证权限
    if (service->provider_pid != provider_pid) {
      return VoidResult{KernelError::PermissionDenied};
    }

    // 关闭所有连接到此服务的通道
    close_service_channels(service_id);

    // 移除服务
    services_.remove(service_id);
    delete service;

    (void)total_services_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    return VoidResult{};
  }

  // 连接到服务
  [[nodiscard]] KernelResult<ChannelId>
  connect_to_service(ProcessId client_pid, ServiceId service_id) noexcept {
    // 查找服务
    auto service_ptr = services_.find(service_id);
    if (service_ptr == nullptr) {
      return KernelResult<ChannelId>{KernelError::NotFound};
    }
    ServiceDescriptor *service = *service_ptr;

    // 检查客户端数量限制
    if (service->current_clients >= service->max_clients) {
      return KernelResult<ChannelId>{KernelError::ResourceExhausted};
    }

    // 分配通道ID
    ChannelId channel_id =
        next_channel_id_.fetch_add(1, containers::MemoryOrder::Relaxed);

    // 创建零拷贝通道
    auto channel = make_unique<ZeroCopyChannel>(channel_id, client_pid,
                                                service->provider_pid);
    if (!channel) {
      return KernelResult<ChannelId>{KernelError::OutOfMemory};
    }

    // 初始化通道
    auto init_result = channel->initialize();
    if (!init_result) {
      return KernelResult<ChannelId>{init_result.error()};
    }

    // 创建连接描述符
    ConnectionDescriptor *conn = new ConnectionDescriptor(
        channel_id, client_pid, service->provider_pid, service_id);

    // 注册通道和连接
    channels_.insert_or_update(channel_id, moss::move(channel));
    connections_.insert_or_update(channel_id, conn);

    // 更新统计
    service->current_clients++;
    (void)total_channels_.fetch_add(1, containers::MemoryOrder::Relaxed);

    // 记录进程通道映射
    add_process_channel(client_pid, channel_id);
    add_process_channel(service->provider_pid, channel_id);

    return KernelResult<ChannelId>{channel_id};
  }

  // 根据服务名连接
  [[nodiscard]] KernelResult<ChannelId>
  connect_to_service_by_name(ProcessId client_pid,
                             const char *service_name) noexcept {
    ServiceDescriptor *service = find_service_by_name(service_name);
    if (service == nullptr) {
      return KernelResult<ChannelId>{KernelError::NotFound};
    }

    return connect_to_service(client_pid, service->service_id);
  }

  // 断开连接
  [[nodiscard]] VoidResult disconnect(ChannelId channel_id,
                                      ProcessId requester_pid) noexcept {
    // 查找连接
    auto conn_ptr = connections_.find(channel_id);
    if (conn_ptr == nullptr) {
      return VoidResult{KernelError::NotFound};
    }
    ConnectionDescriptor *conn = *conn_ptr;

    // 验证权限
    if (conn->client_pid != requester_pid &&
        conn->server_pid != requester_pid) {
      return VoidResult{KernelError::PermissionDenied};
    }

    // 移除通道
    channels_.remove(channel_id);

    // 更新服务统计
    auto service_ptr = services_.find(conn->service_id);
    if (service_ptr != nullptr) {
      ServiceDescriptor *service = *service_ptr;
      service->current_clients--;
    }

    // 移除进程通道映射
    remove_process_channel(conn->client_pid, channel_id);
    remove_process_channel(conn->server_pid, channel_id);

    // 移除连接描述符
    connections_.remove(channel_id);
    delete conn;

    (void)total_channels_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    return VoidResult{};
  }

  // 发送消息
  [[nodiscard]] bool send_message(ChannelId channel_id, ProcessId sender_pid,
                                  const MessageHeader &header,
                                  const void *payload) noexcept {
    // 查找通道
    const auto *channel = channels_.find(channel_id);
    if (channel == nullptr || !*channel) {
      return false;
    }

    // 发送消息
    bool success = (*channel)->send_message(sender_pid, header, payload);
    if (success) {
      // 更新统计
      (void)messages_processed_.fetch_add(1, containers::MemoryOrder::Relaxed);
      (void)bytes_transferred_.fetch_add(header.payload_size,
                                         containers::MemoryOrder::Relaxed);

      // 更新连接统计
      update_connection_statistics(channel_id, true, header.payload_size);
    }

    return success;
  }

  // 接收消息
  [[nodiscard]] bool receive_message(ChannelId channel_id,
                                     ProcessId receiver_pid,
                                     MessageHeader &header, void *payload,
                                     usize max_payload_size) noexcept {
    // 查找通道
    const auto *channel = channels_.find(channel_id);
    if (channel == nullptr || !*channel) {
      return false;
    }

    // 接收消息
    bool success = (*channel)->receive_message(receiver_pid, header, payload,
                                               max_payload_size);
    if (success) {
      // 更新统计
      (void)messages_processed_.fetch_add(1, containers::MemoryOrder::Relaxed);

      // 更新连接统计
      update_connection_statistics(channel_id, false, header.payload_size);
    }

    return success;
  }

  // 等待消息（阻塞版本）
  [[nodiscard]] bool wait_for_message(ChannelId channel_id,
                                      ProcessId receiver_pid,
                                      MessageHeader &header, void *payload,
                                      usize max_payload_size,
                                      u64 timeout_ns = UINT64_MAX) noexcept {
    // 查找通道
    const auto *channel = channels_.find(channel_id);
    if (channel == nullptr || !*channel) {
      return false;
    }

    return (*channel)->wait_for_message(receiver_pid, header, payload,
                                        max_payload_size, timeout_ns);
  }

  // 获取服务列表
  void get_service_list([[maybe_unused]] ProcessId requester_pid,
                        void (*callback)(const ServiceDescriptor &, void *),
                        void *context) const noexcept {
    services_.for_each([callback, context](const auto &entry) {
      const ServiceDescriptor &service = *entry.value;
      if (service.is_public) {
        callback(service, context);
      }
    });
  }

  // 获取IPC统计信息
  [[nodiscard]] IpcStats get_statistics() const noexcept {
    return {total_services_.load(containers::MemoryOrder::Relaxed),
            total_channels_.load(containers::MemoryOrder::Relaxed),
            messages_processed_.load(containers::MemoryOrder::Relaxed),
            bytes_transferred_.load(containers::MemoryOrder::Relaxed)};
  }

  // 获取进程的IPC连接信息
  void get_process_connections(ProcessId pid,
                               void (*callback)(const ConnectionDescriptor &,
                                                void *),
                               void *context) const noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (channels_ptr == nullptr) {
      return;
    }
    const auto *channels = *channels_ptr;
    if (channels == nullptr) {
      return;
    }

    channels->for_each([this, callback, context](const ChannelId &channel_id) {
      auto conn_ptr = connections_.find(channel_id);
      if (conn_ptr != nullptr) {
        const ConnectionDescriptor *conn = *conn_ptr;
        callback(*conn, context);
      }
    });
  }

  // 清理进程的所有IPC资源
  void cleanup_process_ipc(ProcessId pid) noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (channels_ptr == nullptr) {
      return;
    }
    const auto *channels = *channels_ptr;
    if (channels == nullptr) {
      return;
    }

    // 收集要关闭的通道ID
    containers::RcuList<ChannelId> channels_to_close;
    channels->for_each([&channels_to_close](const ChannelId &channel_id) {
      channels_to_close.push_front(channel_id);
    });

    // 关闭所有通道
    channels_to_close.for_each([this, pid](const ChannelId &channel_id) {
      (void)disconnect(channel_id, pid);
    });

    // 清理进程通道列表
    process_channels_.remove(pid);

    // 清理共享内存映射
    if (shared_memory_manager_ != nullptr) {
      shared_memory_manager_->cleanup_process_mappings(pid);
    }
  }

private:
  // 根据服务名查找服务
  [[nodiscard]] ServiceDescriptor *
  find_service_by_name(const char *name) noexcept {
    ServiceDescriptor *found_service = nullptr;

    services_.for_each([name, &found_service](const auto &entry) {
      const ServiceDescriptor *service = entry.value;
      if (string_compare(service->service_name, name) == 0) {
        found_service = const_cast<ServiceDescriptor *>(service);
        return; // 找到后提前返回
      }
    });

    return found_service;
  }

  // 关闭服务的所有通道
  void close_service_channels(ServiceId service_id) noexcept {
    containers::RcuList<ChannelId> channels_to_close;

    connections_.for_each([service_id, &channels_to_close](const auto &entry) {
      const ConnectionDescriptor *conn = entry.value;
      if (conn->service_id == service_id) {
        channels_to_close.push_front(conn->channel_id);
      }
    });

    channels_to_close.for_each([this](const ChannelId &channel_id) {
      auto conn_ptr = connections_.find(channel_id);
      if (conn_ptr != nullptr) {
        const ConnectionDescriptor *conn = *conn_ptr;
        (void)disconnect(channel_id, conn->client_pid);
      }
    });
  }

  // 添加进程通道映射
  void add_process_channel(ProcessId pid, ChannelId channel_id) noexcept {
    auto channels_ptr = process_channels_.find(pid);
    containers::RcuList<ChannelId> *channels = nullptr;
    if (channels_ptr != nullptr) {
      channels = *channels_ptr;
    }
    if (channels == nullptr) {
      channels = new containers::RcuList<ChannelId>();
      process_channels_.insert_or_update(pid, channels);
    }
    channels->push_front(channel_id);
  }

  // 移除进程通道映射
  void remove_process_channel(ProcessId pid, ChannelId channel_id) noexcept {
    auto channels_ptr = process_channels_.find(pid);
    if (channels_ptr != nullptr) {
      auto *channels = *channels_ptr;
      if (channels != nullptr) {
        // 使用find_if找到要删除的元素，然后用remove删除
        const ChannelId *found = channels->find_if(
            [channel_id](const ChannelId &id) { return id == channel_id; });
        if (found != nullptr) {
          channels->remove(*found);
        }
      }
    }
  }

  // 更新连接统计
  void update_connection_statistics(ChannelId channel_id, bool is_send,
                                    u32 bytes) noexcept {
    auto conn_ptr = connections_.find(channel_id);
    if (conn_ptr == nullptr) {
      return;
    }
    ConnectionDescriptor *conn = *conn_ptr;
    if (conn != nullptr) {
      if (is_send) {
        conn->messages_sent++;
      } else {
        conn->messages_received++;
      }
      conn->bytes_transferred += bytes;
      conn->last_activity = get_current_time();
    }
  }

  // 简单的字符串比较
  [[nodiscard]] static int string_compare(const char *s1,
                                          const char *s2) noexcept {
    while (*s1 && *s2 && *s1 == *s2) {
      s1++;
      s2++;
    }
    return *s1 - *s2;
  }

  // 获取当前时间
  [[nodiscard]] static u64 get_current_time() noexcept {
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
    return count;
  }

  // 清理所有资源
  void cleanup() noexcept {
    // 清理所有通道
    channels_.for_each([](const auto &entry) {
      (void)entry; // 避免未使用参数警告
                   // unique_ptr会自动清理
    });
    // channels_ 中的 unique_ptr 会自动清理

    // 清理连接描述符
    connections_.for_each([](const auto &entry) {
      (void)entry; // 避免未使用参数警告
      delete entry.value;
    });

    // 清理服务描述符
    services_.for_each([](const auto &entry) {
      (void)entry; // 避免未使用参数警告
      delete entry.value;
    });

    // 清理进程通道列表
    process_channels_.for_each([](const auto &entry) {
      (void)entry; // 避免未使用参数警告
      delete entry.value;
    });
  }
};

// 全局IPC管理器实例
extern IpcManager *g_ipc_manager;

// 便利的C风格接口（用于系统调用）
extern "C" {
KernelError ipc_register_service(ProcessId pid, const char *name,
                                 ServiceId *out_id);
KernelError ipc_connect_to_service(ProcessId pid, const char *name,
                                   ChannelId *out_channel);
KernelError ipc_send_message(ChannelId channel, ProcessId sender,
                             const MessageHeader *header, const void *payload);
KernelError ipc_receive_message(ChannelId channel, ProcessId receiver,
                                MessageHeader *header, void *payload,
                                usize max_size);
KernelError ipc_disconnect(ChannelId channel, ProcessId pid);
}

} // namespace moss::kernel::ipc
