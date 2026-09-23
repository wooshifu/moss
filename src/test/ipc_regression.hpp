#pragma once

namespace moss::test::ipc_regression {

inline void ring_wrap() {
  namespace ipc = moss::kernel::ipc;
  using Ring = ipc::ZeroCopyRingBuffer<1024, 256>;
  alignas(64) moss::u8 storage[Ring::storage_size()]{};
  Ring writer(storage, sizeof(storage));
  Ring reader(storage, sizeof(storage), false);
  ipc::MessageHeader sent{}, received{};
  sent.payload_size = 1;
  for (moss::u64 sequence = 0; sequence < 64; ++sequence) {
    // A 72-byte record visits both non-64-byte offsets and wrapped headers.
    sent.sequence = sequence;
    const moss::u8 payload = static_cast<moss::u8>(sequence);
    moss::u8 actual = 0;
    if (!boost::ut::expect(writer.try_send(sent, &payload))) {
      return;
    }
    if (!boost::ut::expect(reader.try_receive(received, &actual, sizeof(actual)))) {
      return;
    }
    boost::ut::expect(received.sequence == sequence && actual == payload);
  }
  const auto stats = writer.get_statistics();
  boost::ut::expect(stats.write_count == 64 && stats.read_count == 64 && stats.used_space == 0);
}

inline void ring_geometry() {
  namespace ipc = moss::kernel::ipc;
  using Ring = ipc::ZeroCopyRingBuffer<1024, 256>;
  // Allocation padding must not expand the capacity indexed with a 1023 mask.
  alignas(64) moss::u8 storage[Ring::storage_size() + 1024]{};
  Ring ring(storage, sizeof(storage));
  Ring short_view(storage, 63);
  Ring unaligned_view(storage + 1, sizeof(storage) - 1);
  boost::ut::expect(!short_view.is_valid() && !unaligned_view.is_valid());
  ipc::MessageHeader header{};
  moss::u8 payload[192]{};
  header.payload_size = sizeof(payload);
  boost::ut::expect(!ring.try_send(header, nullptr));
  for (unsigned i = 0; i < 4; ++i) {
    boost::ut::expect(ring.try_send(header, payload));
  }
  boost::ut::expect(!ring.try_send(header, payload));
  boost::ut::expect(ring.get_statistics().used_space == 1024);
  ipc::ZeroCopyRingBuffer<1024, 254> odd_budget(storage, sizeof(storage));
  header.payload_size = 190;
  // 64 + 190 bytes rounds to 256; padding must fit the declared 254 budget.
  boost::ut::expect(!odd_budget.try_send(header, payload));
}

inline void shared_backing() {
  namespace kernel = moss::kernel;
  namespace ipc = kernel::ipc;
  ipc::SharedMemoryManager manager;
  static_assert(sizeof(manager) <= 8192, "Leave at least half of the 16 KiB kernel stack for nested calls");
  const auto first_id = manager.create_region(0, kernel::PAGE_SIZE);
  const auto second_id = manager.create_region(0, kernel::PAGE_SIZE);
  if (!boost::ut::expect(first_id && second_id)) {
    return;
  }
  auto first = manager.get_region_info(*first_id);
  auto second = manager.get_region_info(*second_id);
  if (!boost::ut::expect(first && second)) {
    return;
  }
  const bool backed = first->phys_base != second->phys_base &&
                      first->virt_base == kernel::phys_to_virt(first->phys_base) &&
                      second->virt_base == kernel::phys_to_virt(second->phys_base);
  // Check the allocator/direct-map contract before touching formerly fake VAs.
  if (boost::ut::expect(backed)) {
    auto *first_bytes = reinterpret_cast<moss::u8 *>(first->virt_base);
    auto *second_bytes = reinterpret_cast<moss::u8 *>(second->virt_base);
    boost::ut::expect(first_bytes[0] == 0 && second_bytes[0] == 0);
    first_bytes[0] = 0x35;
    second_bytes[0] = 0x79;
    boost::ut::expect(first_bytes[0] == 0x35 && second_bytes[0] == 0x79);
  }
  boost::ut::expect(static_cast<bool>(manager.destroy_region(*first_id)));
  boost::ut::expect(static_cast<bool>(manager.destroy_region(*second_id)));
  boost::ut::expect(manager.get_statistics().total_memory_usage == 0);
}

inline void shared_lifecycle() {
  namespace kernel = moss::kernel;
  namespace ipc = kernel::ipc;
  // Heap nodes may reserve slab pages on first use. Warm the manager before
  // comparing PFA counts so the test measures region backing, not slab growth.
  ipc::SharedMemoryManager manager;
  auto warmup = manager.create_region(0, kernel::PAGE_SIZE);
  if (!boost::ut::expect(static_cast<bool>(warmup))) {
    return;
  }
  if (!boost::ut::expect(static_cast<bool>(manager.destroy_region(*warmup)))) {
    return;
  }
  const auto baseline = kernel::mm::PageFrameAllocator::get_memory_stats().free_pages;
  constexpr ipc::ShmType unsupported_types[] = {ipc::ShmType::DeviceMemory, ipc::ShmType::DMA_Coherent,
                                                ipc::ShmType::LargePage};
  for (auto type : unsupported_types) {
    const auto unsupported = manager.create_region(0, kernel::PAGE_SIZE, type);
    boost::ut::expect(!unsupported && unsupported.error() == kernel::KernelError::NotSupported);
  }
  // One page beyond the maximum buddy block must fail before shifting or
  // allocating; the public request ceiling is intentionally larger.
  const auto oversized = manager.create_region(0, (kernel::PAGE_SIZE << ::MAX_ORDER) + 1);
  boost::ut::expect(!oversized && oversized.error() == kernel::KernelError::OutOfMemory);
  for (unsigned i = 0; i < 8; ++i) {
    // Three logical pages must release the original four-page buddy block.
    auto id = manager.create_region(0, 2 * kernel::PAGE_SIZE + 1);
    if (!boost::ut::expect(static_cast<bool>(id))) {
      return;
    }
    // No user VMA/PTE backend exists yet: a success address would be unsafe.
    const auto mapping = manager.map_to_process(0, *id);
    boost::ut::expect(!mapping && mapping.error() == kernel::KernelError::NotSupported);
    const auto unmapping = manager.unmap_from_process(0, *id);
    boost::ut::expect(!unmapping && unmapping.error() == kernel::KernelError::NotSupported);
    boost::ut::expect(static_cast<bool>(manager.sync_region(*id)));
    const auto cleanup = manager.cleanup_process_mappings(0);
    boost::ut::expect(!cleanup && cleanup.error() == kernel::KernelError::NotSupported);
    boost::ut::expect(static_cast<bool>(manager.destroy_region(*id)));
    boost::ut::expect(!manager.get_region_info(*id));
    const auto missing_sync = manager.sync_region(*id);
    boost::ut::expect(!missing_sync && missing_sync.error() == kernel::KernelError::InvalidArgument);
  }
  boost::ut::expect(kernel::mm::PageFrameAllocator::get_memory_stats().free_pages == baseline);
  boost::ut::expect(manager.get_statistics().total_regions == 0);
}

inline void service_lifecycle() {
  namespace ipc = moss::kernel::ipc;
  if (!boost::ut::expect(ipc::g_shared_memory_manager != nullptr)) {
    return;
  }
  const auto regions = ipc::g_shared_memory_manager->get_statistics().total_regions;
  ipc::IpcManager manager(ipc::g_shared_memory_manager);
  auto service = manager.register_service(1, "validation-service", 1);
  if (!boost::ut::expect(static_cast<bool>(service))) {
    return;
  }
  auto duplicate = manager.register_service(1, "validation-service", 1);
  boost::ut::expect(!duplicate && duplicate.error() == moss::kernel::KernelError::AlreadyExists);
  auto channel = manager.connect_to_service_by_name(2, "validation-service");
  if (!boost::ut::expect(static_cast<bool>(channel))) {
    return;
  }
  auto full = manager.connect_to_service(3, *service);
  boost::ut::expect(!full && full.error() == moss::kernel::KernelError::ResourceExhausted);
  const auto active = manager.get_statistics();
  boost::ut::expect(active.total_services == 1 && active.total_channels == 1);
  boost::ut::expect(static_cast<bool>(manager.disconnect(*channel, 2)));
  boost::ut::expect(ipc::g_shared_memory_manager->get_statistics().total_regions == regions);
  auto reconnected = manager.connect_to_service(3, *service);
  if (!boost::ut::expect(static_cast<bool>(reconnected))) {
    return;
  }
  boost::ut::expect(static_cast<bool>(manager.unregister_service(*service, 1)));
  const auto stopped = manager.get_statistics();
  boost::ut::expect(stopped.total_services == 0 && stopped.total_channels == 0);
  boost::ut::expect(manager.disconnect(*reconnected, 3).error() == moss::kernel::KernelError::NotFound);
  auto absent = manager.connect_to_service_by_name(2, "validation-service");
  boost::ut::expect(!absent && absent.error() == moss::kernel::KernelError::NotFound);
  boost::ut::expect(ipc::g_shared_memory_manager->get_statistics().total_regions == regions);
}

} // namespace moss::test::ipc_regression
