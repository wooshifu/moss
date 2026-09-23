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
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/memory_internal.hpp"
#include "validation/smp_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::test::validation {
struct ConcurrentValue {
  u32 *destroyed;
  explicit ConcurrentValue(u32 *counter) : destroyed(counter) {}
  ~ConcurrentValue() { __atomic_fetch_add(destroyed, 1U, __ATOMIC_RELEASE); }
};

// Two real userspace threads enter this test through the validation syscall.
// Only CPU0 records assertions; acquire/release handshakes publish peer results.
struct ContainerInterleaving {
  containers::LockedHashMap<u32, shared_ptr<ConcurrentValue>, 1> map;
  containers::LockedList<u32> list;
  u32 phase = 0;
  u32 factories = 0;
  u32 inserted = 0;
  u32 destroyed = 0;
  u32 peer_cpu = 0;
  bool peer_ok = false;
  bool peer_inserted = false;
  bool peer_removed = false;
  bool peer_unlinked = false;
  VirtAddr peer_value = 0;

  static void wait_for(const u32 &value, u32 expected) {
    // These milestones only increase without wrapping. A later phase also
    // satisfies the barrier; equality could miss a fast owner's transition.
    // The host's case deadline bounds a stuck peer; no guest-clock dependency.
    while (__atomic_load_n(&value, __ATOMIC_ACQUIRE) < expected) {
      arch::cpu_yield();
    }
  }

  shared_ptr<ConcurrentValue> race_insert(bool &list_inserted, u32 actor) {
    // 42 is an arbitrary list value, and map key 2 differs from the earlier
    // key 1. Actor bits 1/2 identify owner/peer; their OR (3) waits for both
    // contenders, while factories==2 forces both through the lookup race.
    list_inserted = list.push_front_unless([](u32 value) { return value == 42; }, u32{42});
    auto result = map.get_or_insert(u32{2}, [&] {
      auto candidate = make_shared<ConcurrentValue>(&destroyed);
      __atomic_fetch_add(&factories, 1U, __ATOMIC_RELEASE);
      wait_for(factories, 2); // Force both creators past the initial lookup.
      return candidate;
    });
    if (actor == 2) {
      peer_value = reinterpret_cast<VirtAddr>(result.get());
    }
    __atomic_fetch_or(&inserted, actor, __ATOMIC_RELEASE);
    wait_for(inserted, 3);
    return result;
  }

  bool peer() {
    peer_cpu = arch::get_current_cpu_id();
    auto *thread = process::CfsScheduler::get_current_task();
    // The peer is pinned to logical CPU1: its affinity bitmap is bit 1 (2),
    // rather than the numeric CPU ID itself.
    peer_ok = peer_cpu == 1 && thread && thread->cpu_affinity_mask.low_word() == 2;
    wait_for(phase, 1);
    auto held = map.find(u32{1});
    peer_ok = peer_ok && held && *held;
    __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
    wait_for(phase, 3);
    const bool alive = __atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 0;
    peer_ok = peer_ok && alive;
    if (held && alive) {
      peer_ok = peer_ok && (*held)->destroyed == &destroyed;
    }
    held.reset();
    __atomic_store_n(&phase, 4U, __ATOMIC_RELEASE);
    wait_for(phase, 5);
    auto winner = race_insert(peer_inserted, 2);
    peer_removed = map.remove(u32{2});
    peer_unlinked = list.remove(u32{42});
    winner.reset();
    __atomic_store_n(&phase, 6U, __ATOMIC_RELEASE);
    return peer_ok;
  }

  void owner() {
    __atomic_store_n(&phase, 1U, __ATOMIC_RELEASE);
    wait_for(phase, 2);
    ut::expect(map.remove(u32{1}));
    ut::expect(__atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 0);
    __atomic_store_n(&phase, 3U, __ATOMIC_RELEASE);
    wait_for(phase, 4);
    ut::expect(__atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 1);
    __atomic_store_n(&phase, 5U, __ATOMIC_RELEASE);
    bool list_inserted = false;
    auto winner = race_insert(list_inserted, 1);
    ut::expect(reinterpret_cast<VirtAddr>(winner.get()) == peer_value);
    const bool removed = map.remove(u32{2});
    const bool unlinked = list.remove(u32{42});
    winner.reset();
    wait_for(phase, 6);
    ut::expect(peer_ok && peer_cpu == 1 && affinity_valid());
    ut::expect(list_inserted != peer_inserted && removed != peer_removed && unlinked != peer_unlinked);
    ut::expect(map.empty() && list.empty());
    ut::expect(__atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 3);
    logging::klog::info("Container interleaving: owner CPU0, reader CPU{}, {} values destroyed", peer_cpu,
                        __atomic_load_n(&destroyed, __ATOMIC_ACQUIRE));
  }
};
ContainerInterleaving *container_interleaving = nullptr;

static void empty_case() {}
void register_containers_smp() {
  ut::register_suite("containers.smp", [] { ut::register_test("interleaving", empty_case); });
}

long start_containers_smp() {
  const char *selection = selected_suite();
  if (ut::same_id(selection, "containers.smp")) {
    start_case("interleaving");
    if (!ut::expect(g_num_cpus >= 2)) {
      end_case();
      finish("requires_smp");
    }
    container_interleaving = new ContainerInterleaving();
    container_interleaving->map.insert_or_update(u32{1},
                                                 make_shared<ConcurrentValue>(&container_interleaving->destroyed));
    return 3;
  }
  return 0;
}

long control_containers_smp(long op, long arg1, [[maybe_unused]] long arg2) {
  const char *selection = selected_suite();
  const char *active_case = running_case();
  if (ut::same_id(selection, "containers.smp") && active_case && container_interleaving) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return container_interleaving->peer() ? 1 : 0;
    }
    if (op == 8) {
      if (!ut::expect(arg1 && affinity_valid())) {
        end_case();
        finish("worker_setup");
      }
      arch::enable_interrupts();
      container_interleaving->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete container_interleaving;
      container_interleaving = nullptr;
      end_case();
      finish();
    }
  }
  invalid_control();
}
} // namespace moss::test::validation
