import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.timer;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;
import moss.capability;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "capability_regression.hpp"
#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/core_cases.hpp"
#include "validation/memory_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;
using moss::test::validation::HeapPressure;

namespace moss::test::validation {
struct ContainerValue {
  u32 value;
  u32 *destroyed;
  ContainerValue(u32 v, u32 *counter) : value(v), destroyed(counter) {}
  ~ContainerValue() { ++*destroyed; }
};

void container_ownership() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  // If reachable nodes have already been reclaimed, do not walk them again
  // during failed-case cleanup. The host discards this suite's kernel.
  auto *list = new containers::LockedList<ContainerValue>();
  for (u32 value = 1; value <= 3; ++value) {
    list->push_front(value, &destroyed);
  }
  logging::klog::info("Container ownership: {} reachable values destroyed after insertion", destroyed);
  if (!ut::expect(destroyed == 0)) {
    return;
  }
  u32 count = 0;
  u32 sum = 0;
  list->for_each([&](const ContainerValue &value) {
    ++count;
    sum += value.value;
  });
  ut::expect(count == 3 && sum == 6);
  delete list;
  ut::expect(destroyed == 3);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void container_release_reuse() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  auto *list = new containers::LockedList<ContainerValue>();
  constexpr u32 length = 1024;
  for (u32 i = 0; i < length; ++i) {
    list->push_front(i, &destroyed);
    if (!ut::expect(destroyed == 0)) {
      return;
    }
  }
  if (!ut::expect(destroyed == 0 && list->size() == length)) {
    return;
  }
  constexpr u32 removed[] = {0, 511, length - 1}; // Tail, interior, head.
  u32 deleted = 0;
  for (u32 id : removed) {
    if (!ut::expect(list->remove_if([&](const ContainerValue &item) { return item.value == id; }))) {
      return;
    }
    if (!ut::expect(destroyed == ++deleted)) {
      return;
    }
    list->for_each([&](const ContainerValue &item) { ut::expect(item.value != id); });
  }
  u32 count = 0;
  u32 sum = 0;
  list->for_each([&](const ContainerValue &value) {
    ++count;
    sum += value.value;
  });
  ut::expect(count == length - 3 && sum == length * (length - 1) / 2 - 511 - (length - 1));
  list->clear(); // No bounded retirement queue remains.
  if (!ut::expect(destroyed == length && list->empty() && list->size() == 0)) {
    return;
  }
  list->push_front(length, &destroyed);
  ut::expect(list->size() == 1 && destroyed == length);
  delete list;
  ut::expect(destroyed == length + 1);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void container_map_ownership() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  // Force collisions and use real owned values, as the IPC channel map does.
  auto *map = new containers::LockedHashMap<u32, shared_ptr<ContainerValue>, 1>();
  for (u32 key = 1; key <= 3; ++key) {
    map->insert_or_update(key, make_shared<ContainerValue>(key, &destroyed));
  }
  if (!ut::expect(destroyed == 0 && map->size() == 3)) {
    return;
  }
  map->insert_or_update(u32{2}, make_shared<ContainerValue>(u32{20}, &destroyed));
  if (!ut::expect(destroyed == 1 && map->size() == 3)) {
    return;
  }
  for (u32 key = 1; key <= 3; ++key) {
    auto value = map->find(key);
    ut::expect(value && *value && (*value)->value == (key == 2 ? 20 : key));
  }
  u32 deleted = 1;
  constexpr u32 keys[] = {1, 3, 2};
  for (u32 key : keys) {
    ut::expect(map->remove(key));
    ut::expect(!map->remove(key));
    if (!ut::expect(destroyed == ++deleted && !map->find(key))) {
      return;
    }
  }
  ut::expect(map->empty());
  delete map;
  ut::expect(destroyed == 4);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}
void container_held_reader() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  auto *map = new containers::LockedHashMap<u32, shared_ptr<ContainerValue>, 1>();
  map->insert_or_update(u32{1}, make_shared<ContainerValue>(u32{1}, &destroyed));
  {
    auto borrowed = map->find(u32{1});
    if (!ut::expect(borrowed && *borrowed)) {
      return;
    }
    ut::expect(map->remove(u32{1}));
    logging::klog::info("Container held reader: {} values destroyed before reader release", destroyed);
    if (!ut::expect(destroyed == 0)) {
      return; // Do not dereference reclaimed storage; discard this failed suite.
    }
    ut::expect((*borrowed)->value == 1);
  }
  delete map;
  ut::expect(destroyed == 1);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

struct ReentrantValue {
  using Map = containers::LockedHashMap<u32, shared_ptr<ReentrantValue>, 1>;
  Map *owner;
  u32 *destroyed;
  ReentrantValue(Map *map, u32 *counter) : owner(map), destroyed(counter) {}
  ~ReentrantValue() {
    // This would deadlock if remove/replacement invoked destructors under the map lock.
    ut::expect(owner->size() <= 1);
    ++*destroyed;
  }
};

void container_reentry() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  {
    containers::LockedList<u32> list;
    list.push_front(u32{1});
    auto copy = list.find(u32{1});
    ut::expect(list.update_if([](u32 value) { return value == 1; }, [](u32 &value) { value = 2; }));
    ut::expect(copy && *copy == 1 && !list.find(u32{1}));
    ut::expect(!list.push_front_unless([](u32 value) { return value == 2; }, u32{3}));
    list.push_front(u32{3});
    u32 count = 0;
    list.for_each_snapshot([&](u32 value) {
      ut::expect(list.remove(value));
      ++count;
    });
    ut::expect(count == 2 && list.empty());

    containers::LockedHashMap<u32, u32, 1> values;
    ut::expect(values.get_or_insert(u32{1}, [] { return u32{10}; }) == 10);
    ut::expect(values.get_or_insert(u32{1}, [] { return u32{20}; }) == 10);
    values.insert_or_update(u32{2}, u32{20});
    auto wider_key = values.find(u64{1});
    ut::expect(wider_key && *wider_key == 10);
    count = 0;
    values.for_each_snapshot([&](const auto &entry) {
      ut::expect(values.remove(entry.key));
      ++count;
    });
    ut::expect(count == 2 && values.empty());

    ReentrantValue::Map map;
    u32 destroyed = 0;
    map.insert_or_update(u32{1}, make_shared<ReentrantValue>(&map, &destroyed));
    map.insert_or_update(u32{1}, make_shared<ReentrantValue>(&map, &destroyed));
    ut::expect(destroyed == 1);
    auto held = map.find(u32{1});
    map.clear();
    ut::expect(held && destroyed == 1);
    held.reset();
    ut::expect(destroyed == 2);
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void container_atomic_orders() {
  containers::AtomicU32 value{0};
  ut::expect(value.load(containers::MemoryOrder::Consume) == 0);
  value.store(1, containers::MemoryOrder::Relaxed);
  ut::expect(value.load(containers::MemoryOrder::Acquire) == 1);
  value.store(2, containers::MemoryOrder::Release);
  ut::expect(value.load(containers::MemoryOrder::AcqRel) == 2);
  ut::expect(value.load(containers::MemoryOrder::SeqCst) == 2);
  volatile int invalid = 99;
  auto order = containers::to_intrinsics_order(static_cast<containers::MemoryOrder>(invalid));
  ut::expect(static_cast<int>(order) >= 0);
}

void container_wait_queue() {
  containers::WaitQueue queue;
  int first = 0;
  int second = 0;
  int third = 0;
  queue.add_waiter(&first);
  queue.add_waiter(&second, true);
  queue.add_waiter(&third, true);
  u32 woken = 0;
  ut::expect(queue.wake_up([&](void *) { ++woken; }) == 2);
  ut::expect(woken == 2);
  queue.clear();
  ut::expect(!queue.has_waiters());
  queue.add_waiter(&first);
  queue.add_waiter(&second);
  ut::expect(queue.wake_one([&](void *thread) { ut::expect(thread == &second); }));
  queue.remove_waiter(&second);
  ut::expect(queue.wake_one([&](void *thread) { ut::expect(thread == &first); }));
  queue.remove_waiter(&first);
  ut::expect(!queue.wake_one([](void *) {}));
  ut::expect(!queue.has_waiters());
  ut::expect(queue.try_add_waiter(&first));
}

void container_slab_reuse() {
  containers::SlabAllocator allocator;
  auto too_large = allocator.allocate(5000);
  ut::expect(!too_large && too_large.error() == containers::SlabError::InvalidSize);
  ut::expect(allocator.deallocate(nullptr, 8).has_value());
  ut::expect(!allocator.deallocate(reinterpret_cast<void *>(1), 5000));

  void *objects[520]{};
  for (auto &object : objects) {
    auto result = allocator.allocate(8);
    ut::expect(result.has_value());
    object = result.has_value() ? *result : nullptr;
  }
  for (auto object : objects) {
    ut::expect(allocator.deallocate(object, 8).has_value());
  }
  for (auto &object : objects) {
    auto result = allocator.allocate(8);
    ut::expect(result.has_value());
    object = result.has_value() ? *result : nullptr;
  }
  for (auto object : objects) {
    ut::expect(allocator.deallocate(object, 8).has_value());
  }
}

void capability_table_edges() {
  using namespace capability;
  Table source;
  Table target;
  auto object = make_shared<Object>(ObjectType::Memory);
  ut::expect(!source.install({}, rights::MAP_READ).has_value());
  ut::expect(!source.install(object, 0).has_value());
  auto handle = source.install(object, rights::MAP_READ | rights::MAP_WRITE | rights::TRANSFER | rights::DUPLICATE);
  if (!ut::expect(handle.has_value())) {
    return;
  }
  ut::expect(!source.lookup(INVALID_HANDLE, ObjectType::Memory, rights::MAP_READ).has_value());
  ut::expect(!source.lookup(*handle, ObjectType::Endpoint, rights::MAP_READ).has_value());
  ut::expect(!source.lookup(*handle, ObjectType::Memory, rights::SEND).has_value());
  ut::expect(!source.duplicate(INVALID_HANDLE, rights::MAP_READ).has_value());
  ut::expect(!source.duplicate(*handle, rights::SEND).has_value());
  ut::expect(!source.duplicate(*handle, 0).has_value());
  Escrow empty_escrow;
  empty_escrow = moss::move(empty_escrow);
  IpcCapture empty_capture;
  empty_capture = moss::move(empty_capture);
  ut::expect(!source.capture_for_ipc(INVALID_HANDLE, rights::MAP_READ).has_value());
  ut::expect(!source.capture_for_ipc(*handle, 0).has_value());
  ut::expect(!source.capture_for_ipc(*handle, rights::SEND).has_value());
  ut::expect(!source.reserve_escrow(empty_escrow).has_value());
  ut::expect(!source.transfer_to(source, *handle, rights::MAP_READ, false).has_value());
  const auto duplicate = source.duplicate(*handle, rights::MAP_READ | rights::DUPLICATE);
  ut::expect(duplicate.has_value());
  if (duplicate) {
    ut::expect(source.set_inheritable(*duplicate, true).has_value());
    ut::expect(source.set_keep_on_exec(*duplicate, true).has_value());
    ut::expect(source.set_inheritable(INVALID_HANDLE, true).error() == ErrorCode::NotFound);
    ut::expect(source.set_keep_on_exec(INVALID_HANDLE, true).error() == ErrorCode::NotFound);
  }

  ForkSelection selections[1]{{.handle = *handle,
                               .rights = rights::MAP_READ | rights::DUPLICATE,
                               .flags = fork_flags::INHERIT}};
  ut::expect(source.clone_selected_to(target, selections, 1).has_value());
  ut::expect(!source.clone_selected_to(source, selections, 1).has_value());
  ut::expect(!source.clone_selected_to(target, selections, 1).has_value());
  ut::expect(!source.clone_selected_to(target, nullptr, 1).has_value());
  ut::expect(!source.clone_selected_to(target, selections, Table::capacity() + 1).has_value());
  ForkSelection bad[1]{{.handle = *handle, .rights = rights::SEND, .flags = 0}};
  Table scratch;
  ut::expect(!source.clone_selected_to(scratch, bad, 1).has_value());
  bad[0].rights = rights::MAP_READ | rights::DUPLICATE;
  bad[0].flags = 2;
  ut::expect(!source.clone_selected_to(scratch, bad, 1).has_value());
  bad[0].handle = INVALID_HANDLE;
  ut::expect(!source.clone_selected_to(scratch, bad, 1).has_value());

  Table full;
  for (usize i = 0; i < Table::capacity(); ++i) {
    ut::expect(full.install(make_shared<Object>(ObjectType::Endpoint), rights::SEND).has_value());
  }
  ut::expect(full.install(make_shared<Object>(ObjectType::Endpoint), rights::SEND).error() ==
             ErrorCode::ResourceExhausted);

  auto capture = source.capture_for_ipc(*handle, rights::MAP_READ);
  ut::expect(capture.has_value());
  if (capture) {
    Escrow escrow = capture->take_escrow();
    ut::expect(!capture->escrow());
    Table destination;
    auto reserved = destination.reserve_escrow(escrow);
    ut::expect(reserved.has_value());
    if (reserved) {
      ut::expect(destination.publish_reserved(*reserved).has_value());
      ut::expect(destination.close(*reserved).has_value());
    }
  }
  ut::expect(source.close(*handle).has_value());
  ut::expect(!source.close(*handle).has_value());

  Table waitables;
  const ObjectType waitable_types[] = {ObjectType::Endpoint,      ObjectType::Memory,
                                       ObjectType::DomainFactory, ObjectType::CodeVersion,
                                       ObjectType::CodeAuthority, ObjectType::CodeApproval,
                                       ObjectType::DomainScope,   ObjectType::SchedulingProfile,
                                       static_cast<ObjectType>(255)};
  for (const auto type : waitable_types) {
    auto installed = waitables.install(make_shared<Object>(type), rights::MAP_READ);
    ut::expect(installed.has_value());
    if (installed) {
      ut::expect(!waitables.lookup_waitable(*installed).has_value());
    }
  }
  auto domain = waitables.install(make_shared<Object>(ObjectType::Domain), rights::DOMAIN_OBSERVE);
  ut::expect(domain.has_value() && waitables.lookup_waitable(*domain).has_value());
  auto denied = waitables.install(make_shared<Object>(ObjectType::Domain), rights::MAP_READ);
  ut::expect(denied.has_value() && !waitables.lookup_waitable(*denied).has_value());

  auto first_reserved = waitables.reserve(make_shared<Object>(ObjectType::Memory), rights::MAP_READ);
  auto second_reserved = waitables.reserve(make_shared<Object>(ObjectType::Memory), rights::MAP_READ);
  if (ut::expect(first_reserved.has_value() && second_reserved.has_value())) {
    ut::expect(waitables.publish_reserved(*first_reserved, *second_reserved).has_value());
    ut::expect(!waitables.publish_reserved(*first_reserved).has_value());
    ut::expect(!waitables.publish_reserved(*first_reserved, *first_reserved).has_value());
  }
  auto unpublished = waitables.reserve(make_shared<Object>(ObjectType::Memory), rights::MAP_READ);
  if (ut::expect(unpublished.has_value())) {
    ut::expect(waitables.publish_reserved(*unpublished, INVALID_HANDLE).has_value());
    ut::expect(!waitables.discard_reserved(*unpublished).has_value());
  }
  ut::expect(!source.clone_inheritable_to(source).has_value());
  ut::expect(!source.clone_inheritable_to(target).has_value());
  Table inherited_target;
  ut::expect(source.clone_inheritable_to(inherited_target).has_value());
  source.clear();
}

void container_resource_edges();
void map_and_wait_edges();

void register_containers_cases() {
  ut::register_suite("containers", [] {
    ut::register_test("queue_reuse", moss::test::queue_regression::run);
    ut::register_test("capability_process_handles", moss::test::capability_regression::process_handles);
    ut::register_test("ownership", container_ownership);
    ut::register_test("release_reuse", container_release_reuse);
    ut::register_test("map_ownership", container_map_ownership);
    ut::register_test("held_reader", container_held_reader);
    ut::register_test("reentry", container_reentry);
    ut::register_test("atomic_orders", container_atomic_orders);
    ut::register_test("wait_queue", container_wait_queue);
    ut::register_test("slab_reuse", container_slab_reuse);
    ut::register_test("capability_limits", capability_table_edges);
    ut::register_test("resource_edges", container_resource_edges);
    ut::register_test("map_and_wait_edges", map_and_wait_edges);
  });
}

void container_resource_edges() {
  using namespace containers;
  SlabCache cache(8);
  ut::expect(cache.object_size() == 8);
  ut::expect(cache.total_objects() == 0);
  ut::expect(cache.utilization() == 0);
  auto first = cache.allocate();
  if (ut::expect(static_cast<bool>(first))) {
    ut::expect(cache.allocated_objects() == 1);
    ut::expect(cache.utilization() <= 100);
    ut::expect(cache.deallocate(*first).has_value());
    ut::expect(cache.allocated_objects() == 0);
    ut::expect(cache.utilization() == 0);
  }
  ut::expect(cache.deallocate(nullptr).has_value());
  ut::expect(!cache.deallocate(reinterpret_cast<void *>(1)).has_value());

  // A middle page must be removed from the full list before it becomes the
  // empty-list head; otherwise it could be allocated twice.
  static void *objects[1536]{};
  for (auto &object : objects) {
    auto allocated = cache.allocate();
    if (!ut::expect(static_cast<bool>(allocated))) {
      break;
    }
    object = *allocated;
  }
  for (usize i = 512; i < 1024; ++i) {
    ut::expect(cache.deallocate(objects[i]).has_value());
  }
  for (usize i = 0; i < 1536; ++i) {
    if (i < 512 || i >= 1024) {
      ut::expect(cache.deallocate(objects[i]).has_value());
    }
  }
  ut::expect(cache.allocated_objects() == 0);

  SlabCache pressured(8);
  {
    PagePressure pressure;
    if (ut::expect(pressure.acquire(0))) {
      ut::expect(!pressured.allocate());
    }
  }
  auto recovered = pressured.allocate();
  if (ut::expect(static_cast<bool>(recovered))) {
    ut::expect(pressured.deallocate(*recovered).has_value());
  }

  ut::expect(ContainerLibrary::initialize());
  ContainerLibrary::print_statistics();
  ut::expect(ContainerLibrary::is_initialized());

  Optional<u32> empty;
  Optional<u32> engaged(7u);
  Optional<u32> target(9u);
  target = moss::move(engaged);
  ut::expect(target.has_value() && *target == 7u && !engaged.has_value());
  empty = moss::move(target);
  ut::expect(empty.has_value() && *empty == 7u && !target.has_value());
  empty = Optional<u32>{};
  ut::expect(!empty.has_value());
  Optional<u32> another(11u);
  engaged = moss::move(another);
  ut::expect(engaged.has_value() && *engaged == 11u);
  target = engaged;
  ut::expect(target.has_value() && *target == 11u && engaged.has_value());
  target.reset();
  ut::expect(!target.has_value());
}

void map_and_wait_edges() {
  containers::LockedList<u32> list;
  ut::expect(!list.update_if_unless([](const u32 &) { return true; }, [](const u32 &) { return false; },
                                    [](u32 &) {}));
  list.push_front(1U);
  list.push_front(2U);
  list.push_front(3U);
  ut::expect(!list.update_if_unless([](const u32 &value) { return value == 2U; },
                                    [](const u32 &value) { return value == 3U; }, [](u32 &) {}));
  ut::expect(!list.update_if_unless([](const u32 &value) { return value == 99U; },
                                    [](const u32 &) { return true; }, [](u32 &) {}));
  ut::expect(list.update_if_unless([](const u32 &value) { return value == 2U; },
                                   [](const u32 &value) { return value == 99U; },
                                   [](u32 &value) { value = 20U; }));
  u32 visited = 0;
  list.for_each([&](const u32 &) { ++visited; });
  ut::expect(visited == 3);
  u32 snapshot = 0;
  list.for_each_snapshot([&](const u32 &) { ++snapshot; });
  ut::expect(snapshot == 3);
  list.clear();
  ut::expect(list.empty());

  containers::WaitQueue queue;
  u32 wakes = 0;
  ut::expect(queue.wake_up([&](void *) { ++wakes; }) == 0);
  ut::expect(!queue.wake_one([&](void *) { ++wakes; }));
  u32 first = 1, second = 2, third = 3;
  queue.add_waiter(&first, true);
  queue.add_waiter(&second, true);
  queue.add_waiter(&third, false);
  wakes = 0;
  ut::expect(queue.wake_up([&](void *) { ++wakes; }) == 2);
  ut::expect(wakes == 2);
  ut::expect(queue.wake_one([&](void *) { ++wakes; }));
  queue.clear();
  ut::expect(!queue.has_waiters());

  containers::LockedHashMap<u32, u32, 4> map;
  for (u32 i = 0; i < 16; ++i) {
    map.insert_or_update(i, i * 10U);
  }
  for (u32 i = 0; i < 16; ++i) {
    map.insert_or_update(i, i * 20U);
  }
  auto found = map.find(7U);
  ut::expect(found.has_value() && *found == 140U);
  ut::expect(!map.find(99U).has_value());
  ut::expect(map.get_or_insert(7U, [] { return 0U; }) == 140U);
  ut::expect(map.get_or_insert(99U, [] { return 990U; }) == 990U);
  ut::expect(map.extract(99U).has_value());
  ut::expect(!map.extract(99U).has_value());
  {
    HeapPressure pressure;
    if (ut::expect(pressure.acquire(sizeof(u64)))) {
      ut::expect(!map.try_insert_or_update(100U, 1000U));
      pressure.release();
    }
  }
  map.clear();
  map.clear();
  ut::expect(true);
}

} // namespace moss::test::validation
