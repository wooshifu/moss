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
    if (!boost::ut::expect(writer.try_send(sent, &payload)))
      return;
    if (!boost::ut::expect(reader.try_receive(received, &actual, sizeof(actual))))
      return;
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
  for (unsigned i = 0; i < 4; ++i)
    boost::ut::expect(ring.try_send(header, payload));
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
  if (!boost::ut::expect(first_id && second_id))
    return;
  auto first = manager.get_region_info(*first_id);
  auto second = manager.get_region_info(*second_id);
  if (!boost::ut::expect(first && second))
    return;
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
  if (!boost::ut::expect(static_cast<bool>(warmup)))
    return;
  if (!boost::ut::expect(static_cast<bool>(manager.destroy_region(*warmup))))
    return;
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
    if (!boost::ut::expect(static_cast<bool>(id)))
      return;
    // No user VMA/PTE backend exists yet: a success address would be unsafe.
    const auto mapping = manager.map_to_process(0, *id);
    boost::ut::expect(!mapping && mapping.error() == kernel::KernelError::NotSupported);
    boost::ut::expect(static_cast<bool>(manager.destroy_region(*id)));
    boost::ut::expect(!manager.get_region_info(*id));
  }
  boost::ut::expect(kernel::mm::PageFrameAllocator::get_memory_stats().free_pages == baseline);
  boost::ut::expect(manager.get_statistics().total_regions == 0);
}

} // namespace moss::test::ipc_regression
