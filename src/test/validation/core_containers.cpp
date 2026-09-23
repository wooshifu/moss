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
import moss.ipc;
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
#include "ipc_regression.hpp"
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

void register_containers_cases() {
  ut::register_suite("containers", [] {
    ut::register_test("queue_reuse", moss::test::queue_regression::run);
    ut::register_test("ipc_heap_rollback", ipc_heap_rollback);
    ut::register_test("ipc_shared_backing", moss::test::ipc_regression::shared_backing);
    ut::register_test("capability_process_handles", moss::test::capability_regression::process_handles);
    ut::register_test("ipc_shared_lifecycle", moss::test::ipc_regression::shared_lifecycle);
    ut::register_test("ipc_service_lifecycle", moss::test::ipc_regression::service_lifecycle);
    ut::register_test("ipc_ring_wrap", moss::test::ipc_regression::ring_wrap);
    ut::register_test("ipc_ring_geometry", moss::test::ipc_regression::ring_geometry);
    ut::register_test("ownership", container_ownership);
    ut::register_test("release_reuse", container_release_reuse);
    ut::register_test("map_ownership", container_map_ownership);
    ut::register_test("held_reader", container_held_reader);
    ut::register_test("reentry", container_reentry);
  });
}

} // namespace moss::test::validation
