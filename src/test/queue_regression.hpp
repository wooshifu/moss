#pragma once

// Included after moss.containers/moss.std and the kernel UT framework.
namespace moss::test::queue_regression {

struct PoolValue {
  inline static unsigned live = 0;
  unsigned value = 0;
  PoolValue() noexcept { ++live; }
  ~PoolValue() noexcept { --live; }
};

inline void run() {
  namespace containers = moss::kernel::containers;
  containers::MPSCQueue<unsigned> intrusive;
  containers::QueueNode<unsigned> first(1U), second(2U);
  intrusive.enqueue(&first);
  intrusive.enqueue(&second);
  if (!boost::ut::expect(intrusive.try_dequeue() == &first)) {
    return;
  }
  // Returning the old head must not replace the unread node's next link.
  intrusive.enqueue(&first);
  if (!boost::ut::expect(intrusive.try_dequeue() == &second && intrusive.try_dequeue() == &first)) {
    return;
  }
  boost::ut::expect(intrusive.empty() && intrusive.try_dequeue() == nullptr);

  // Four nodes leave three unread links when the first node is returned.
  // Page alignment rounds this pool to multiple pages; persistent raw storage
  // keeps it off the 16 KiB kernel stack, with construction after MM startup.
  using Pool = containers::ObjectPool<PoolValue, 4>;
  alignas(Pool) static moss::u8 storage[sizeof(Pool)];
  auto *pool = new (storage) Pool();
  {
    struct Cleanup {
      Pool *pool;
      ~Cleanup() { pool->~Pool(); }
    } cleanup{pool};
    auto *node = pool->allocate();
    if (!boost::ut::expect(node != nullptr)) {
      return;
    }
    node->data.value = 99; // Nonzero payload detects a missing default reset.
    pool->deallocate(node);
    containers::QueueNode<PoolValue> *nodes[Pool::pool_size()]{};
    for (moss::kernel::usize i = 0; i < Pool::pool_size(); ++i) {
      nodes[i] = pool->allocate();
      if (!boost::ut::expect(nodes[i] != nullptr)) {
        return;
      }
      boost::ut::expect(nodes[i]->data.value == 0);
      for (moss::kernel::usize previous = 0; previous < i; ++previous) {
        boost::ut::expect(nodes[i] != nodes[previous]);
      }
    }
    boost::ut::expect(!pool->has_available() && pool->allocate() == nullptr);
    for (auto *entry : nodes) {
      pool->deallocate(entry);
    }
    boost::ut::expect(pool->has_available() && PoolValue::live == Pool::pool_size());
  }
  boost::ut::expect(PoolValue::live == 0);

  // Two four-slot lanes hold three values each. Drain one lane by consumer ID,
  // then steal the remaining lane to check full/empty and reuse paths.
  containers::MPMCQueue<unsigned, 2, 4> distributed;
  constexpr unsigned capacity = 2 * containers::SPSCQueue<unsigned, 4>::capacity();
  for (unsigned value = 0; value < capacity; ++value) {
    boost::ut::expect(distributed.try_enqueue(value));
  }
  boost::ut::expect(!distributed.try_enqueue(capacity));
  bool seen[capacity]{};
  for (unsigned i = 0; i < capacity; ++i) {
    unsigned value = capacity;
    if (!boost::ut::expect(i < capacity / 2 ? distributed.try_dequeue(0, value) : distributed.try_dequeue_any(value))) {
      return;
    }
    if (!boost::ut::expect(value < capacity)) {
      return;
    }
    boost::ut::expect(!seen[value]);
    seen[value] = true;
  }
  unsigned value = 0;
  boost::ut::expect(distributed.empty() && distributed.approximate_total_size() == 0 &&
                    !distributed.try_dequeue_any(value) && !distributed.try_dequeue(2, value));
}

} // namespace moss::test::queue_regression
