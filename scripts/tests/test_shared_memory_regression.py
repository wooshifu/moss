"""Exercise production SharedMemoryManager ownership with explicit host boundaries.

The allocator, map, smart pointer and lock scaffolding below are host models,
not the kernel implementations. QEMU validation separately checks real kernel
pages and locks. Only the IPC declarations and manager methods come from src.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

SUPPORT = r"""
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <utility>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using usize = std::size_t;
using ShmId = u32;
using ProcessId = u32;
using PhysAddr = std::uintptr_t;
using VirtAddr = std::uintptr_t;
constexpr usize PAGE_SIZE = 4096;
constexpr usize CACHE_LINE_SIZE = 64;
constexpr usize MAX_ORDER = 10;
VirtAddr phys_to_virt(PhysAddr address) { return address; }

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "ownership assertion failed: %s\n", message);
    std::exit(31);
  }
}

enum class KernelError {
  InvalidArgument, NotSupported, InvalidState, OutOfMemory,
  PermissionDenied, NotFound, Busy, InternalError
};
template<class T, class E> class HostResult {
  std::optional<T> value_;
  E error_{};
public:
  HostResult(T value) : value_(value) {}
  HostResult(E error) : error_(error) {}
  explicit operator bool() const { return value_.has_value(); }
  T operator*() const { return *value_; }
  E error() const { return error_; }
};
template<class E> class HostResult<void, E> {
  bool valid_{true};
  E error_{};
public:
  HostResult() = default;
  HostResult(E error) : valid_(false), error_(error) {}
  explicit operator bool() const { return valid_; }
  E error() const { return error_; }
};
template<class T> using KernelResult = HostResult<T, KernelError>;
using VoidResult = HostResult<void, KernelError>;

namespace containers {
enum class MemoryOrder { Relaxed, Acquire, Release, AcqRel, SeqCst };
template<class T> struct AtomicCounter {
  std::atomic<T> value{0};
  AtomicCounter() = default;
  explicit AtomicCounter(T initial) : value(initial) {}
  T load(MemoryOrder = MemoryOrder::SeqCst) const { return value.load(); }
  void store(T number, MemoryOrder = MemoryOrder::SeqCst) { value.store(number); }
  T fetch_add(T number, MemoryOrder = MemoryOrder::SeqCst) { return value.fetch_add(number); }
  T fetch_sub(T number, MemoryOrder = MemoryOrder::SeqCst) { return value.fetch_sub(number); }
};
using AtomicU32 = AtomicCounter<u32>;
// Single-threaded reentry detection only: this does not model IRQ masking.
struct IrqSpinLock {
  bool held{false};
  void lock() { require(!held, "map/lifecycle lock reentered"); held = true; }
  void unlock() { require(held, "unlock without ownership"); held = false; }
};
template<class T> struct LockGuard {
  T &lock;
  explicit LockGuard(T &owner) : lock(owner) { lock.lock(); }
  ~LockGuard() { lock.unlock(); }
};
}

namespace mm {
enum class MemoryAttributes { NORMAL_CACHEABLE };
enum class PageAllocError { InitializationFailed, OutOfMemory, InvalidOrder, InvalidAddress };
enum class HeapAllocError { InitializationFailed, OutOfMemory };
struct PageFrameAllocator {
  struct Block { PhysAddr address; usize order; };
  inline static Block blocks[16]{};
  inline static usize live_pages{}, allocations{}, releases{}, release_orders[16]{};
  static HostResult<PhysAddr, PageAllocError> allocate_pages(usize order) {
    if (order > MAX_ORDER)
      return PageAllocError::InvalidOrder;
    const usize bytes = PAGE_SIZE << order;
    void *storage = nullptr;
    if (posix_memalign(&storage, bytes, bytes))
      return PageAllocError::OutOfMemory;
    for (auto &block : blocks)
      if (!block.address) {
        block = {reinterpret_cast<PhysAddr>(storage), order};
        live_pages += usize{1} << order;
        ++allocations;
        // A nonzero fill makes the production zero-initialization observable.
        std::memset(storage, 0xAD, bytes);
        return block.address;
      }
    require(false, "host PFA bookkeeping capacity exceeded");
    return PageAllocError::OutOfMemory;
  }
  static HostResult<void, PageAllocError> free_pages(PhysAddr address, usize order) {
    for (auto &block : blocks)
      if (block.address == address) {
        if (block.order != order)
          return PageAllocError::InvalidOrder;
        require(releases < 16, "host PFA release bookkeeping capacity exceeded");
        release_orders[releases++] = order;
        live_pages -= usize{1} << order;
        std::free(reinterpret_cast<void *>(address));
        block.address = 0;
        return {};
      }
    return PageAllocError::InvalidAddress;
  }
};
struct RuntimeHeapAllocator {
  inline static void *blocks[64]{};
  inline static usize calls{}, allocated{}, released{}, live{}, fail_on_call{};
  inline static bool reject_all{false};
  static HostResult<void *, HeapAllocError> allocate_aligned(usize bytes, usize alignment) {
    ++calls;
    if (reject_all || calls == fail_on_call)
      return HeapAllocError::OutOfMemory;
    if (alignment < sizeof(void *))
      alignment = sizeof(void *);
    void *storage = nullptr;
    if (posix_memalign(&storage, alignment, bytes))
      return HeapAllocError::OutOfMemory;
    for (auto &block : blocks)
      if (!block) {
        block = storage;
        ++allocated;
        ++live;
        return storage;
      }
    require(false, "host heap bookkeeping capacity exceeded");
    return HeapAllocError::OutOfMemory;
  }
  static HostResult<void, HeapAllocError> deallocate(void *storage, usize) {
    for (auto &block : blocks)
      if (block == storage) {
        block = nullptr;
        --live;
        ++released;
        std::free(storage);
        return {};
      }
    require(false, "descriptor/control/node release used the wrong allocator");
    return HeapAllocError::OutOfMemory;
  }
};
}

// Model the two callback allocations used by core SharedPtr::try_make.
// std::shared_ptr supplies host reference counting; its own control block is
// outside the injected heap. Destruction models the kernel's compatible delete.
template<class T> class shared_ptr {
  struct KernelControl { T *pointer; std::atomic<u32> references; };
  std::shared_ptr<T> pointer_;
public:
  shared_ptr() = default;
  template<class Allocate, class... Args>
  static shared_ptr try_make(Allocate allocate, Args &&...args) {
    auto *storage = allocate(sizeof(T), alignof(T));
    if (!storage)
      return {};
    auto *object = new (storage) T(std::forward<Args>(args)...);
    auto *control_storage = allocate(sizeof(KernelControl), alignof(KernelControl));
    if (!control_storage) {
      object->~T();
      require(bool(mm::RuntimeHeapAllocator::deallocate(object, 0)), "object rollback release failed");
      return {};
    }
    auto *control = new (control_storage) KernelControl{object, 1};
    shared_ptr result;
    result.pointer_ = std::shared_ptr<T>(object, [control](T *pointer) {
      pointer->~T();
      control->~KernelControl();
      require(bool(mm::RuntimeHeapAllocator::deallocate(pointer, 0)), "object final release failed");
      require(bool(mm::RuntimeHeapAllocator::deallocate(control, 0)), "control final release failed");
    });
    return result;
  }
  explicit operator bool() const { return bool(pointer_); }
  T *operator->() const { return pointer_.get(); }
  T &operator*() const { return *pointer_; }
  T *get() const { return pointer_.get(); }
  void reset() { pointer_.reset(); }
};

namespace containers {
inline usize registered_nodes = 0;
// The map model uses the same fallible callback and fatal ordinary-new
// admission behavior as the actual LockedHashMap; no host container allocation
// may accidentally bypass the injected tracking-node failure.
template<class K, class V> class LockedHashMap {
  struct Entry { K key; V value; };
  struct Node { Node *next; Entry entry; };
  Node *head_{nullptr};
  mutable IrqSpinLock lock_{};
  static Node *allocate(const K &key, const V &value, bool fallible) {
    auto storage = mm::RuntimeHeapAllocator::allocate_aligned(sizeof(Node), alignof(Node));
    if (!storage) {
      if (fallible)
        return nullptr;
      std::fprintf(stderr, "host model of kernel panic: infallible map node heap call=%zu\n",
                   mm::RuntimeHeapAllocator::calls);
      std::abort();
    }
    return new (*storage) Node{nullptr, {key, value}};
  }
  static void release(Node *node) {
    node->~Node();
    require(bool(mm::RuntimeHeapAllocator::deallocate(node, 0)), "map node release failed");
  }
  void publish(Node *node) {
    LockGuard<IrqSpinLock> guard(lock_);
    node->next = head_;
    head_ = node;
    ++registered_nodes;
  }
public:
  ~LockedHashMap() { clear(); }
  void insert_or_update(const K &key, const V &value) { publish(allocate(key, value, false)); }
  bool try_insert_or_update(const K &key, const V &value) {
    auto *node = allocate(key, value, true);
    if (!node)
      return false;
    publish(node);
    return true;
  }
  std::optional<V> find(const K &key) const {
    LockGuard<IrqSpinLock> guard(lock_);
    for (auto *node = head_; node; node = node->next)
      if (node->entry.key == key)
        return node->entry.value;
    return {};
  }
  bool remove(const K &key) {
    Node *removed = nullptr;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      auto **link = &head_;
      while (*link && (*link)->entry.key != key)
        link = &(*link)->next;
      if (*link) {
        removed = *link;
        *link = removed->next;
        --registered_nodes;
      }
    }
    if (removed)
      release(removed);
    return removed != nullptr;
  }
  template<class F> void for_each(F callback) const {
    LockGuard<IrqSpinLock> guard(lock_);
    for (auto *node = head_; node; node = node->next)
      callback(static_cast<const Entry &>(node->entry));
  }
  template<class F> void for_each_snapshot(F callback) const {
    Node *snapshot = nullptr;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      for (auto *node = head_; node; node = node->next) {
        auto *copy = allocate(node->entry.key, node->entry.value, false);
        copy->next = snapshot;
        snapshot = copy;
      }
    }
    while (snapshot) {
      auto *next = snapshot->next;
      callback(static_cast<const Entry &>(snapshot->entry));
      release(snapshot);
      snapshot = next;
    }
  }
  void clear() {
    Node *removed = nullptr;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      removed = head_;
      head_ = nullptr;
    }
    while (removed) {
      auto *next = removed->next;
      --registered_nodes;
      release(removed);
      removed = next;
    }
  }
};
}
namespace intrinsics::memory {
void *memset(void *destination, int value, usize size) { return std::memset(destination, value, size); }
}
namespace arch {
void flush_cache_line(VirtAddr) {}
void memory_barrier() {}
}
"""

CASES = r"""
void empty_ownership() {
  require(mm::PageFrameAllocator::live_pages == 0, "physical backing leaked");
  require(mm::RuntimeHeapAllocator::live == 0, "descriptor/control/node allocation leaked");
  require(containers::registered_nodes == 0, "region still registered");
}

int main(int argc, char **argv) {
  require(argc == 2, "one bounded case must be selected");
  const int which = std::atoi(argv[1]);
  if (which >= 1 && which <= 3) {
    {
      SharedMemoryManager manager;
      mm::RuntimeHeapAllocator::fail_on_call = which;
      // Three logical pages require one four-page buddy allocation.
      auto created = manager.create_region(0, 2 * PAGE_SIZE + 1);
      require(!created && created.error() == KernelError::OutOfMemory,
              "create must report injected descriptor/control/tracking OOM");
      empty_ownership();
      const auto stats = manager.get_statistics();
      require(stats.total_regions == 0 && stats.total_memory_usage == 0, "failed creation changed statistics");
      require(!manager.get_region_info(1), "failed creation published the region");
      require(mm::RuntimeHeapAllocator::calls == usize(which), "injection did not reach the selected stage");
      require(mm::RuntimeHeapAllocator::released == usize(which - 1), "earlier heap allocations were not rolled back");
      require(mm::PageFrameAllocator::allocations == 1 && mm::PageFrameAllocator::releases == 1 &&
              mm::PageFrameAllocator::release_orders[0] == 2, "backing rollback changed the original buddy order");
    }
    empty_ownership();
    return 0;
  }
  if (which == 4) {
    usize calls_before_teardown;
    {
      SharedMemoryManager manager;
      auto first = manager.create_region(0, 2 * PAGE_SIZE + 1);
      auto second = manager.create_region(0, 64 + 64 * 1024);
      require(bool(first) && bool(second), "teardown fixtures failed to allocate");
      require(mm::PageFrameAllocator::live_pages == 4 + 32, "teardown fixtures have the wrong backing size");
      calls_before_teardown = mm::RuntimeHeapAllocator::calls;
      // Existing backing must be releasable when every new heap request fails.
      mm::RuntimeHeapAllocator::reject_all = true;
    }
    require(mm::RuntimeHeapAllocator::calls == calls_before_teardown, "manager teardown allocated heap storage");
    require(mm::PageFrameAllocator::releases == 2, "manager teardown did not release both allocations");
    empty_ownership();
    return 0;
  }
  require(which == 5 || which == 6, "unknown geometry case");
  {
    SharedMemoryManager manager;
    // The default wire storage is a 64-byte control block plus 65536 data bytes.
    const usize request = which == 5 ? 2 * PAGE_SIZE + 1 : 64 + 64 * 1024;
    const usize logical_pages = which == 5 ? 3 : 17;
    const usize order = which == 5 ? 2 : 5;
    auto first_id = manager.create_region(0, request);
    auto second_id = manager.create_region(0, request);
    require(bool(first_id) && bool(second_id), "independent backing fixtures failed to allocate");
    auto first = manager.get_region_info(*first_id);
    auto second = manager.get_region_info(*second_id);
    require(bool(first) && bool(second), "created backing was not registered");
    require(first->size == logical_pages * PAGE_SIZE && second->size == logical_pages * PAGE_SIZE,
            "logical page alignment changed the requested extent");
    require(first->allocation_order == order && second->allocation_order == order, "descriptor lost the buddy order");
    require(first->phys_base != second->phys_base && first->virt_base == phys_to_virt(first->phys_base) &&
            second->virt_base == phys_to_virt(second->phys_base),
            "regions do not own distinct host-backed direct maps");
    require(mm::PageFrameAllocator::live_pages == 2 * (usize{1} << order), "physical buddy page count is incorrect");
    auto *left = reinterpret_cast<u8 *>(first->virt_base);
    auto *right = reinterpret_cast<u8 *>(second->virt_base);
    for (usize i = 0; i < first->size; ++i)
      require(left[i] == 0 && right[i] == 0, "production creation did not clear the logical backing");
    left[0] = 0x35;
    right[0] = 0x79;
    require(left[0] == 0x35 && right[0] == 0x79, "one region overwrote another's backing");
    const auto mapping = manager.map_to_process(0, *first_id);
    require(!mapping && mapping.error() == KernelError::NotSupported, "mapping promised a missing user backend");
    require(first->ref_count.load() == 1, "unsupported mapping acquired a backing reference");
    auto stats = manager.get_statistics();
    require(stats.total_regions == 2 && stats.total_memory_usage == 2 * logical_pages * PAGE_SIZE,
            "logical region accounting is inconsistent");
    require(bool(manager.destroy_region(*first_id)) && bool(manager.destroy_region(*second_id)),
            "original-order destruction failed");
    require(mm::PageFrameAllocator::releases == 2 && mm::PageFrameAllocator::release_orders[0] == order &&
            mm::PageFrameAllocator::release_orders[1] == order, "destruction freed a different buddy order");
    require(mm::PageFrameAllocator::live_pages == 0, "metadata handles incorrectly retained backing pages");
    require(!manager.get_region_info(*first_id) && !manager.get_region_info(*second_id),
            "destroyed regions remain registered");
    stats = manager.get_statistics();
    require(stats.total_regions == 0 && stats.total_memory_usage == 0, "destruction did not restore statistics");
    first.reset();
    second.reset();
    empty_ownership();
  }
  empty_ownership();
}
"""


@pytest.fixture(scope="module")
def shared_memory_binary(tmp_path_factory):
    compiler = shutil.which("clang++")
    if not compiler:
        pytest.skip("clang++ required for production shared-memory ownership checks")
    source = (ROOT / "src/ipc/src/ipc.cppm").read_text()
    # Preserve the complete production declarations and manager methods.
    body = source[source.index("enum class ShmPermission") : source.index("// 全局共享内存管理器实例")]
    directory = tmp_path_factory.mktemp("shared-memory-production")
    cpp = directory / "shared_memory.cpp"
    binary = directory / "shared_memory"
    cpp.write_text(SUPPORT + body + CASES)
    result = subprocess.run(
        [compiler, "-std=c++23", "-O1", str(cpp), "-o", str(binary)],
        capture_output=True,
        text=True,
        check=False,
        timeout=90,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


def run_case(binary, case):
    result = subprocess.run([str(binary), str(case)], capture_output=True, text=True, check=False, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("stage", [1, 2, 3], ids=["descriptor", "control", "tracking_node"])
def test_create_rolls_back_each_heap_failure(shared_memory_binary, stage):
    run_case(shared_memory_binary, stage)


def test_manager_teardown_needs_no_new_heap_allocation(shared_memory_binary):
    run_case(shared_memory_binary, 4)


@pytest.mark.parametrize(
    "case", [5, 6], ids=["three_logical_four_physical", "default_seventeen_logical_thirtytwo_physical"]
)
def test_backing_is_distinct_zeroed_and_released_at_original_order(shared_memory_binary, case):
    run_case(shared_memory_binary, case)
