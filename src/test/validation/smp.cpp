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
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
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

// Exercise the production Process getter/replacement boundary on distinct CPUs.
// The target is deliberately not scheduled: software readers must retain its
// tables independently of both Process lifetime and any active hardware root.
struct AddressSpaceReaders {
  unique_ptr<process::Process> target;
  PhysAddr old_root = 0, new_root = 0, data = 0;
  u16 old_asid = 0, new_asid = 0;
  u64 contents = 0;
  u32 phase = 0, arrived = 0;
  bool peer_ok = false;
  using Pfa = mm::PageFrameAllocator;
  using Tables = mm::PageTableManager;

  template <typename Condition> static void require(Condition valid) {
    if (!ut::expect(static_cast<Condition &&>(valid))) {
      end_case();
      finish("address_space_setup");
    }
  }

  bool old_alive() const { return Pfa::page_ref_get(old_root) == 1 && Pfa::page_ref_get(data) == 1; }

  bool peer() {
    auto *thread = process::CfsScheduler::get_current_task();
    // CPU1 is selected by bit 1 in the affinity mask, not by mask value 1.
    peer_ok = arch::get_current_cpu_id() == 1 && thread && thread->cpu_affinity_mask.low_word() == 2;
    // arrived acknowledges reader milestones, phase releases writer milestones.
    // Both are monotonic; the host's normal case deadline bounds a stuck peer.
    __atomic_store_n(&arrived, 1U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 1);
    {
      auto held = target->address_space();
      peer_ok = peer_ok && held && held->pgd_phys == old_root;
      __atomic_store_n(&arrived, 2U, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, 2);
      // A broken implementation may have freed both pages. Detect that through
      // PFA metadata before dereferencing either the old object or its tables.
      const bool alive = old_alive();
      peer_ok = peer_ok && alive;
      if (alive && held) {
        auto *pte = Tables::get_user_pte(held->pgd_phys, process::user_layout::CODE_BASE);
        peer_ok = peer_ok && held->pgd_phys == old_root && held->asid == old_asid && pte &&
                  pte->get_phys_addr() == data && memory_hash(phys_to_virt(data), page_size) == contents &&
                  held->allows_user_access(process::user_layout::CODE_BASE, page_size, process::vma_flags::WRITE);
      }
      auto latest = target->address_space();
      peer_ok = peer_ok && latest && latest->pgd_phys == new_root && latest->asid == new_asid;
      __atomic_store_n(&arrived, 3U, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, 3);
      const bool latest_alive = Pfa::page_ref_get(new_root) == 1;
      peer_ok = peer_ok && old_alive() && latest_alive;
      if (latest_alive && latest) {
        peer_ok = peer_ok && latest->pgd_phys == new_root;
      }
    }
    __atomic_store_n(&arrived, 4U, __ATOMIC_RELEASE);
    // Do not exit/reap the real worker while the owner checks fixture cleanup.
    ContainerInterleaving::wait_for(phase, 4);
    return peer_ok;
  }

  void owner() {
    ContainerInterleaving::wait_for(arrived, 1);
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    const auto pages = Pfa::get_memory_stats().free_pages;
    target = make_unique<process::Process>(INVALID_PROCESS_ID);
    auto original = process::user_space::create_user_address_space();
    auto replacement = process::user_space::create_user_address_space();
    require(original && replacement);
    old_root = (*original)->pgd_phys;
    new_root = (*replacement)->pgd_phys;
    old_asid = (*original)->asid;
    new_asid = (*replacement)->asid;
    require((*original)->add_vma(process::user_layout::CODE_BASE, process::user_layout::CODE_BASE + page_size,
                                 process::vma_flags::READ | process::vma_flags::WRITE));
    auto allocated = mm::allocate_pages(0);
    require(allocated.has_value());
    data = *allocated;
    // An arbitrary nonzero pattern detects clobbering or recycled/demand-zero
    // storage, independently of the table and allocator-reference assertions.
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(data));
    for (usize index = 0; index < page_size; ++index) {
      bytes[index] = 0x5a;
    }
    contents = memory_hash(phys_to_virt(data), page_size);
    require(Tables::map_user_page(old_root, process::user_layout::CODE_BASE, data, mm::page_perms::USER_RW));
    require(target->set_address_space(moss::move(*original)));
    __atomic_store_n(&phase, 1U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 2);
    require(target->set_address_space(moss::move(*replacement)));
    ut::expect(old_alive());
    logging::klog::info("Address-space replacement: old root refs {}, data refs {}", Pfa::page_ref_get(old_root),
                        Pfa::page_ref_get(data));
    __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 3);
    target->clear_address_space();
    ut::expect(!target->address_space());
    target->clear_address_space(); // Exit teardown is idempotent, even with held readers.
    ut::expect(old_alive() && Pfa::page_ref_get(new_root) == 1);
    {
      auto other = process::user_space::create_user_address_space();
      require(other.has_value());
      // Neither detached version's translation tag may be leased to a new
      // image while the peer still owns it, even after Process detachment.
      ut::expect((*other)->asid != old_asid && (*other)->asid != new_asid);
    }
    target.reset();
    ut::expect(old_alive() && Pfa::page_ref_get(new_root) == 1);
    __atomic_store_n(&phase, 3U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 4);
    ut::expect(peer_ok && affinity_valid());
    ut::expect(Pfa::page_ref_get(old_root) == 0 && Pfa::page_ref_get(new_root) == 0 && Pfa::page_ref_get(data) == 0);
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 4U, __ATOMIC_RELEASE);
  }
};
AddressSpaceReaders *address_space_readers = nullptr;

struct UserCopyVersion {
  shared_ptr<process::Process> owner;
  shared_ptr<process::AddressSpace> original, replacement;
  VirtAddr address = 0;
  bool armed = false, switched = false;

  static void activate(const shared_ptr<process::AddressSpace> &space) {
    process::CfsScheduler::use_address_space(space);
  }

  void replace(PhysAddr root, VirtAddr user_address) {
    if (!armed || root != original->pgd_phys || user_address != address) {
      return;
    }
    armed = false;
    // Model resuming a previously admitted copy after image replacement. This
    // is not a complete exec/scheduler test: only the production copy's binding
    // to its selected version is under test, and both roots are kept owned.
    activate(replacement);
    switched = owner->set_address_space(replacement).has_value();
  }

  void restore() const {
    activate(original);
    AddressSpaceReaders::require(owner->set_address_space(original));
  }
};
UserCopyVersion *user_copy_version = nullptr;

void user_copy_version_binding() {
  using Tables = mm::PageTableManager;
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    UserCopyVersion probe;
    probe.owner = process::current_process();
    probe.original = probe.owner ? probe.owner->address_space() : shared_ptr<process::AddressSpace>{};
    auto replacement = process::user_space::create_user_address_space();
    AddressSpaceReaders::require(probe.original && replacement);
    probe.replacement = moss::move(*replacement);
    // The first unused mmap cursor keeps the test outside the worker's code,
    // stack and heap. No userspace thread runs while this transient VMA exists.
    probe.address = probe.original->mmap_next;
    AddressSpaceReaders::require(!probe.original->find_vma(probe.address));
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
    auto old_page = mm::allocate_pages(0), new_page = mm::allocate_pages(0);
    AddressSpaceReaders::require(old_page && new_page);
    {
      auto transaction = probe.original->lock_vm();
      AddressSpaceReaders::require(
          probe.original->add_vma(probe.address, probe.address + page_size, flags, process::VmaType::MMAP));
      AddressSpaceReaders::require(
          Tables::map_user_page(probe.original->pgd_phys, probe.address, *old_page, mm::page_perms::USER_RW));
      auto *unobserved = Tables::get_user_pte(probe.original->pgd_phys, probe.address);
      // This fresh mapping has never been accessed through a user translation.
      // Clear the eager defaults so the test observes actual copy accounting.
      u64 observations = mm::page_attr::AF;
#if defined(MOSS_ARCH_X64) || defined(MOSS_ARCH_RISCV64)
      observations |= mm::page_attr::DIRTY;
#endif
      unobserved->raw &= ~observations;
    }
    AddressSpaceReaders::require(
        probe.replacement->add_vma(probe.address, probe.address + page_size, flags, process::VmaType::MMAP));
    AddressSpaceReaders::require(
        Tables::map_user_page(probe.replacement->pgd_phys, probe.address, *new_page, mm::page_perms::USER_RW));
    auto *old_data = reinterpret_cast<u8 *>(phys_to_virt(*old_page));
    auto *new_data = reinterpret_cast<u8 *>(phys_to_virt(*new_page));
    // Distinct arbitrary bytes distinguish the admitted image, replacement
    // image and copied payload without relying on allocator contents.
    constexpr u8 admitted = 0x5a, replaced = 0xa5, payload = 0x3c;
    old_data[0] = admitted;
    new_data[0] = replaced;
    user_copy_version = &probe;
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    u8 received = 0;
    probe.armed = true;
    const auto unread = process::copy_from_user(&received, probe.address, sizeof(received));
    probe.restore();
    ut::expect(probe.switched && unread == 0);
    ut::expect(received == admitted);
    const auto *read = Tables::get_user_pte(probe.original->pgd_phys, probe.address);
    ut::expect(read && (read->raw & mm::page_attr::AF) != 0);
#if defined(MOSS_ARCH_X64) || defined(MOSS_ARCH_RISCV64)
    ut::expect(read && (read->raw & mm::page_attr::DIRTY) == 0);
#endif
    probe.armed = true;
    probe.switched = false;
    const auto unwritten = process::copy_to_user(probe.address, &payload, sizeof(payload));
    probe.restore();
    user_copy_version = nullptr;
    ut::expect(probe.switched && unwritten == 0);
    ut::expect(old_data[0] == payload && new_data[0] == replaced);
    const auto *written = Tables::get_user_pte(probe.original->pgd_phys, probe.address);
    ut::expect(written && (written->raw & mm::page_attr::AF) != 0);
#if defined(MOSS_ARCH_X64) || defined(MOSS_ARCH_RISCV64)
    // A kernel-alias write must record dirtiness on the user leaf, not merely
    // on the alias used for copying, so later reclaim observes the write.
    ut::expect(written && (written->raw & mm::page_attr::DIRTY) != 0);
#endif
    {
      auto transaction = probe.original->lock_vm();
      Tables::unmap_user_page(probe.original->pgd_phys, probe.address);
      ut::expect(probe.original->remove_vma(probe.address, probe.address + page_size));
    }
    if (interrupts) {
      arch::enable_interrupts();
    }
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

void raw_user_copy_fixup() {
  using Tables = mm::PageTableManager;
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    auto owner = process::current_process();
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    AddressSpaceReaders::require(static_cast<bool>(as));
    const VirtAddr address = as->mmap_next;
    AddressSpaceReaders::require(!as->find_vma(address) && !as->find_vma(address + page_size));
    auto allocated = mm::allocate_pages(0);
    AddressSpaceReaders::require(allocated.has_value());
    {
      auto transaction = as->lock_vm();
      constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
      AddressSpaceReaders::require(as->add_vma(address, address + page_size, flags));
      AddressSpaceReaders::require(Tables::map_user_page(as->pgd_phys, address, *allocated, mm::page_perms::USER_RW));
    }
    const auto *absent = Tables::get_user_pte(as->pgd_phys, address + page_size);
    AddressSpaceReaders::require(!absent || !absent->is_valid());
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(*allocated));
    constexpr u8 original = 0x5a, sentinel = 0xa5; // Distinguish copied and untouched bytes.
    bytes[page_size - 1] = original;
    // Two bytes starting at the final resident byte force exactly one success
    // followed by an unresolvable fault in each real assembly primitive.
    u8 output[]{sentinel, sentinel};
    const u8 input[]{sentinel, original};
    const VirtAddr last = address + page_size - 1;
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    // Do not hold a VM transaction: native exception entry acquires it. This
    // single-threaded fixture retains the active root until both faults return.
    ut::expect(
        moss::abi::uaccess::moss_raw_copy_from_user(output, reinterpret_cast<const void *>(last), sizeof(output)) == 1);
    ut::expect(output[0] == original && output[1] == sentinel);
    ut::expect(moss::abi::uaccess::moss_raw_copy_to_user(reinterpret_cast<void *>(last), input, sizeof(input)) == 1);
    ut::expect(bytes[page_size - 1] == sentinel);
    {
      auto transaction = as->lock_vm();
      Tables::unmap_user_page(as->pgd_phys, address);
      ut::expect(as->remove_vma(address, address + page_size));
    }
    if (interrupts) {
      arch::enable_interrupts();
    }
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

struct InterruptUnbind {
  interrupts::GenericInterruptController *controller = moss::boot::g_gic_controller;
  // The QEMU profiles leave the source adjacent to the UART unused; the test
  // only exercises software admission and never enables this hardware IRQ.
  interrupts::InterruptId irq = platform::hardware.uart.irq + 1;
  shared_ptr<interrupts::InterruptDescriptor> descriptor;
  u32 entered = 0, finished = 0;

  void prepare() {
    AddressSpaceReaders::require(controller && !controller->get_interrupt_info(irq));
    AddressSpaceReaders::require(
        controller->register_interrupt(irq, +[](u32, void *) noexcept {}, this, "unbind-lifetime"));
    descriptor = controller->get_interrupt_info(irq);
    AddressSpaceReaders::require(descriptor && descriptor->context == this);
  }

  bool peer() {
    const bool right_cpu = arch::get_current_cpu_id() == 1;
    if (!descriptor->begin_callback())
      return false;
    __atomic_store_n(&entered, 1U, __ATOMIC_RELEASE);
    // Hold the old callback while the owner starts unbinding. A second
    // admission must fail before the first callback releases its context.
    while (descriptor->begin_callback()) {
      descriptor->end_callback();
      arch::cpu_yield();
    }
    const bool unbinding = controller->enable_interrupt(irq).error() == ErrorCode::ResourceBusy &&
                           controller->unregister_interrupt(irq).error() == ErrorCode::ResourceBusy;
    // Unregister may return as soon as the callback lease reaches zero.
    // Publish the completed checks before releasing that lease.
    __atomic_store_n(&finished, 1U, __ATOMIC_RELEASE);
    descriptor->end_callback();
    return right_cpu && unbinding;
  }

  void owner() {
    ContainerInterleaving::wait_for(entered, 1);
    const auto result = controller->unregister_interrupt(irq);
    ut::expect(result.has_value() && __atomic_load_n(&finished, __ATOMIC_ACQUIRE) == 1 &&
               !controller->get_interrupt_info(irq) && affinity_valid());
  }
};

// Exercise the same fault transaction as native exception entry, on a real
// but inactive root. This checks software PTE/frame ownership, not remote TLBs.
struct FaultTransactions {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  static constexpr u8 resident_byte = 0x5a; // Nonzero so demand-zero cannot masquerade as resident data.
  // COW, demand-zero, fault/unmap, fault/fork, then fork/unmap. Each uses the
  // same milestones so the peer contends before the owner commits.
  static constexpr u32 scenarios = 5, milestones = 3;
  shared_ptr<process::AddressSpace> source, target, clone;
  PhysAddr original = 0, owner_page = 0, peer_page = 0;
  u64 contents = 0;
  // Independent monotonic clocks: phase = snapshot/commit/cleanup; arrived =
  // ready/contending/finished. The host's normal deadline bounds a stuck peer.
  u32 phase = 0, arrived = 0, snapshots = 0, contentions = 0;
  u32 round_base = 0;
  bool demand = false, unmap = false, fork = false, fork_unmap = false;
  bool peer_ok = false;

  void snapshot(PhysAddr root, VirtAddr fault_address) {
    if (!target || root != target->pgd_phys || fault_address != address) {
      return;
    }
    (void)__atomic_fetch_add(&snapshots, 1U, __ATOMIC_RELAXED);
    if (arch::get_current_cpu_id() == 0) {
      __atomic_store_n(&phase, round_base + 1, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(arrived, round_base + 2);
    } else {
      // Without serialization the peer reaches the same old-frame snapshot.
      // Force it to publish only after the owner has committed its replacement.
      __atomic_store_n(&arrived, round_base + 2, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, round_base + 2);
    }
  }

  void contended(PhysAddr root) {
    if (target && root == target->pgd_phys && arch::get_current_cpu_id() == 1) {
      (void)__atomic_fetch_add(&contentions, 1U, __ATOMIC_RELAXED);
      // With serialization this is the actual failed try_lock boundary. The
      // peer cannot snapshot the old PTE; let the owner complete before retry.
      __atomic_store_n(&arrived, round_base + 2, __ATOMIC_RELEASE);
    }
  }

  void committed(PhysAddr root, VirtAddr fault_address, PhysAddr page) {
    if (target && root == target->pgd_phys && fault_address == address && arch::get_current_cpu_id() == 0 && unmap) {
      // The peer may unmap immediately after resolve_fault releases vm_lock_.
      owner_page = page;
    }
  }

  bool peer() {
    peer_ok = arch::get_current_cpu_id() == 1;
    for (u32 round = 0; round < scenarios; ++round) {
      const auto base = round * milestones;
      __atomic_store_n(&arrived, base + 1, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, base + 1);
      if (fork) {
        auto transaction = target->lock_vm();
        peer_ok = static_cast<bool>(Tables::clone_user_page_tables(target->pgd_phys, clone->pgd_phys)) && peer_ok;
        const auto *pte = Tables::get_user_pte(clone->pgd_phys, address);
        peer_page = pte && pte->is_valid() ? pte->get_phys_addr() : 0;
        peer_ok = peer_page != 0 && peer_ok;
      } else if (unmap || fork_unmap) {
        auto transaction = target->lock_vm();
        const auto *pte = Tables::get_user_pte(target->pgd_phys, address);
        peer_page = pte && pte->is_valid() ? pte->get_phys_addr() : 0;
        Tables::unmap_user_page(target->pgd_phys, address);
        peer_ok = target->remove_vma(address, address + page_size) && peer_page != 0 && peer_ok;
      } else {
        peer_ok = target->resolve_fault(address, mm::UserFaultAccess::Write, !demand) && peer_ok;
        const auto *pte = Tables::get_user_pte(target->pgd_phys, address);
        peer_page = pte ? pte->get_phys_addr() : 0;
      }
      __atomic_store_n(&arrived, base + 3, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, base + 3);
    }
    return peer_ok;
  }

  void owner() {
    for (u32 round = 0; round < scenarios; ++round) {
      ContainerInterleaving::wait_for(arrived, round * milestones + 1);
      round_base = round * milestones;
      demand = round != 0;
      unmap = round == 2;
      fork = round == 3;
      fork_unmap = round == 4;
      snapshots = 0;
      contentions = 0;
      if (fork_unmap) {
        end_case();
        start_case("fork_unmap");
      } else if (fork) {
        end_case();
        start_case("fault_fork");
      } else if (unmap) {
        end_case();
        start_case("fault_unmap");
      } else if (demand) {
        end_case();
        start_case("demand_fault");
      }
      owner_round();
      __atomic_store_n(&phase, round_base + 3, __ATOMIC_RELEASE);
    }
  }

  void owner_round() {
    if (fork_unmap) {
      fork_unmap_round();
      return;
    }
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    const auto pages = Pfa::get_memory_stats().free_pages;
    auto original_space = process::user_space::create_user_address_space();
    auto copied_space = process::user_space::create_user_address_space();
    AddressSpaceReaders::require(original_space && copied_space);
    source = moss::move(*original_space);
    target = moss::move(*copied_space);
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
    AddressSpaceReaders::require(source->add_vma(address, address + page_size, flags));
    AddressSpaceReaders::require(target->add_vma(address, address + page_size, flags));
    if (fork) {
      auto cloned_space = process::user_space::create_user_address_space();
      AddressSpaceReaders::require(cloned_space.has_value());
      clone = moss::move(*cloned_space);
      AddressSpaceReaders::require(clone->add_vma(address, address + page_size, flags));
    }
    auto allocated = mm::allocate_pages(0);
    AddressSpaceReaders::require(allocated.has_value());
    original = *allocated;
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(original));
    // A nonzero pattern distinguishes the copied contents from demand-zero.
    for (usize index = 0; index < page_size; ++index) {
      bytes[index] = demand ? 0 : resident_byte;
    }
    contents = memory_hash(phys_to_virt(original), page_size);
    AddressSpaceReaders::require(Tables::map_user_page(source->pgd_phys, address, original, mm::page_perms::USER_RW));
    if (!demand) {
      AddressSpaceReaders::require(Tables::clone_user_page_tables(source->pgd_phys, target->pgd_phys));
    }
    // COW starts with one shared mapping in each root. Demand starts with an
    // empty target and a separate resident zero-page content oracle in source.
    AddressSpaceReaders::require(Pfa::page_ref_get(original) == (demand ? 1U : 2U));
    owner_page = 0;
    const bool owner_ok = target->resolve_fault(address, mm::UserFaultAccess::Write, !demand);
    if (!unmap) {
      const auto *owner_pte = Tables::get_user_pte(target->pgd_phys, address);
      owner_page = owner_pte ? owner_pte->get_phys_addr() : 0;
    }
    __atomic_store_n(&phase, round_base + 2, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, round_base + 3);
    ut::expect(owner_ok && peer_ok && affinity_valid());
    if (unmap) {
      const auto *pte = Tables::get_user_pte(target->pgd_phys, address);
      ut::expect(owner_page != 0 && peer_page == owner_page && (!pte || !pte->is_valid()) &&
                 !target->find_vma(address) && Pfa::page_ref_get(owner_page) == 0);
      ut::expect(Pfa::page_ref_get(original) == 1 && memory_hash(phys_to_virt(original), page_size) == contents);
      ut::expect(snapshots == 1 && contentions == 1);
      target.reset();
      source.reset();
      ut::expect(Pfa::get_memory_stats().free_pages == pages);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      return;
    }
    if (fork) {
      const auto *parent_pte = Tables::get_user_pte(target->pgd_phys, address);
      const auto *child_pte = Tables::get_user_pte(clone->pgd_phys, address);
      ut::expect(owner_page != 0 && owner_page != original && peer_page == owner_page && parent_pte && child_pte &&
                 parent_pte->get_phys_addr() == owner_page && child_pte->get_phys_addr() == owner_page &&
                 parent_pte->is_cow() && child_pte->is_cow() && !parent_pte->is_writable() &&
                 !child_pte->is_writable() && Pfa::page_ref_get(owner_page) == 2);
      ut::expect(Pfa::page_ref_get(original) == 1 && memory_hash(phys_to_virt(original), page_size) == contents &&
                 memory_hash(phys_to_virt(owner_page), page_size) == contents);
      ut::expect(snapshots == 1 && contentions == 1);
      clone.reset();
      target.reset();
      source.reset();
      ut::expect(Pfa::get_memory_stats().free_pages == pages);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      return;
    }
    ut::expect(owner_page != 0 && owner_page == peer_page && owner_page != original);
    const auto old_refs = Pfa::page_ref_get(original);
    const bool ownership = old_refs == 1 && peer_page != 0 && Pfa::page_ref_get(peer_page) == 1;
    if (!ut::expect(ownership)) {
      // A red run may already have freed a frame still mapped by source. Do
      // not dereference it or run destructors that would release it twice.
      end_case();
      finish("fault_ownership");
    }
    const auto *source_pte = Tables::get_user_pte(source->pgd_phys, address);
    const auto *target_pte = Tables::get_user_pte(target->pgd_phys, address);
    ut::expect(source_pte && source_pte->get_phys_addr() == original && source_pte->is_cow() == !demand &&
               source_pte->is_writable() == demand);
    ut::expect(target_pte && target_pte->get_phys_addr() == peer_page && !target_pte->is_cow() &&
               target_pte->is_writable());
    ut::expect(memory_hash(phys_to_virt(original), page_size) == contents);
    ut::expect(memory_hash(phys_to_virt(peer_page), page_size) == contents);
    ut::expect(snapshots == 1 && contentions == 1);
    // A resident retry must neither refill modified bytes nor relax protection.
    auto *private_bytes = reinterpret_cast<u8 *>(phys_to_virt(peer_page));
    private_bytes[0] ^= 0xff; // Flip every bit of one byte to distinguish a refill.
    const auto modified = memory_hash(phys_to_virt(peer_page), page_size);
    ut::expect(target->resolve_fault(address, mm::UserFaultAccess::Read, false));
    ut::expect(memory_hash(phys_to_virt(peer_page), page_size) == modified);
    ut::expect(!target->resolve_fault(address, mm::UserFaultAccess::Execute, false));
    if (!demand) {
      ut::expect(!source->resolve_fault(address, mm::UserFaultAccess::Write, false));
    }
    target.reset();
    source.reset();
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }

  void fork_unmap_round() {
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    const auto pages = Pfa::get_memory_stats().free_pages;
    auto parent = process::user_space::create_user_address_space();
    auto child = process::user_space::create_user_address_space();
    AddressSpaceReaders::require(parent && child);
    target = moss::move(*parent);
    clone = moss::move(*child);
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
    AddressSpaceReaders::require(target->add_vma(address, address + page_size, flags));
    auto allocated = mm::allocate_pages(0);
    AddressSpaceReaders::require(allocated.has_value());
    original = *allocated;
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(original));
    for (usize index = 0; index < page_size; ++index) {
      bytes[index] = resident_byte;
    }
    contents = memory_hash(phys_to_virt(original), page_size);
    AddressSpaceReaders::require(Tables::map_user_page(target->pgd_phys, address, original, mm::page_perms::USER_RW));

    bool cloned = false;
    bool metadata_copied = false;
    {
      auto transaction = target->lock_vm();
      __atomic_store_n(&phase, round_base + 1, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(arrived, round_base + 2);
      cloned = static_cast<bool>(Tables::clone_user_page_tables(target->pgd_phys, clone->pgd_phys));
      auto vma = target->find_vma(address);
      metadata_copied = vma && clone->add_vma(vma->start_addr, vma->end_addr, vma->flags);
      __atomic_store_n(&phase, round_base + 2, __ATOMIC_RELEASE);
    }
    ContainerInterleaving::wait_for(arrived, round_base + 3);
    const auto *parent_pte = Tables::get_user_pte(target->pgd_phys, address);
    const auto *child_pte = Tables::get_user_pte(clone->pgd_phys, address);
    const bool child_owned = child_pte && child_pte->is_valid() && child_pte->get_phys_addr() == original;
    ut::expect(cloned && metadata_copied && peer_ok && affinity_valid());
    ut::expect((!parent_pte || !parent_pte->is_valid()) && !target->find_vma(address) && peer_page == original);
    ut::expect(child_owned && child_pte->is_cow() && !child_pte->is_writable() && clone->find_vma(address) &&
               Pfa::page_ref_get(original) == 1);
    if (child_owned) {
      ut::expect(memory_hash(phys_to_virt(original), page_size) == contents);
    }
    ut::expect(snapshots == 0 && contentions == 1);
    clone.reset();
    target.reset();
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }
};
FaultTransactions *fault_transactions = nullptr;
InterruptUnbind *interrupt_unbind = nullptr;

// The target roots are real but never installed in hardware. This isolates
// software page ownership from the still-separate remote-TLB contract.
struct UserPageLeases {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  // Read versus unmap, then write versus fork. Each round publishes three
  // milestones: ready/borrowed, contending, and mutation/cleanup completed.
  static constexpr u32 scenarios = 2, milestones = 3;
  // Distinct arbitrary patterns identify original bytes and copied output.
  static constexpr u8 original_byte = 0x5a, payload_byte = 0x3c;
  shared_ptr<process::AddressSpace> target, clone;
  u8 buffer[page_size]{};
  PhysAddr original = 0;
  u64 expected = 0, cloned_contents = 0;
  u32 phase = 0, arrived = 0, round_base = 0, borrows = 0, contentions = 0;
  bool write = false, armed = false, peer_ok = false;

  void borrowed(PhysAddr root, VirtAddr cursor, PhysAddr page, bool writing) {
    if (!armed || !target || root != target->pgd_phys || cursor != address) {
      return;
    }
    armed = false;
    ++borrows;
    __atomic_store_n(&phase, round_base + 1, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, round_base + 2);
    const bool owned = page == original && Pfa::page_ref_get(page) == 1;
    if (!ut::expect(owned && writing == write && arch::get_current_cpu_id() == 0)) {
      // A missing lease can already have freed or shared this frame. Report
      // the red result before copying through a dangling or COW-bypassing alias.
      end_case();
      finish("user_page_ownership");
    }
  }

  void contended(PhysAddr root) {
    if (target && root == target->pgd_phys && arch::get_current_cpu_id() == 1) {
      ++contentions;
      // The real failed try_lock proves the competing mutation cannot enter
      // while copying owns the frame. Allow the owner to finish its copy.
      __atomic_store_n(&arrived, round_base + 2, __ATOMIC_RELEASE);
    }
  }

  bool peer() {
    peer_ok = arch::get_current_cpu_id() == 1;
    for (u32 round = 0; round < scenarios; ++round) {
      const auto base = round * milestones;
      __atomic_store_n(&arrived, base + 1, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, base + 1);
      {
        auto transaction = target->lock_vm();
        if (write) {
          peer_ok = static_cast<bool>(Tables::clone_user_page_tables(target->pgd_phys, clone->pgd_phys)) && peer_ok;
          const auto *pte = Tables::get_user_pte(clone->pgd_phys, address);
          peer_ok = pte && pte->is_valid() && peer_ok;
          if (peer_ok) {
            // Observe at clone completion, before any later owner write, so
            // the test distinguishes serialization from an eventual match.
            cloned_contents = memory_hash(phys_to_virt(pte->get_phys_addr()), page_size);
          }
        } else {
          Tables::unmap_user_page(target->pgd_phys, address);
          peer_ok = target->remove_vma(address, address + page_size) && peer_ok;
        }
        // Also release a lock-disabled red run that never saw contention.
        __atomic_store_n(&arrived, base + 2, __ATOMIC_RELEASE);
      }
      __atomic_store_n(&arrived, base + 3, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, base + 3);
    }
    return peer_ok;
  }

  void owner() {
    for (u32 round = 0; round < scenarios; ++round) {
      ContainerInterleaving::wait_for(arrived, round * milestones + 1);
      round_base = round * milestones;
      write = round != 0;
      borrows = 0;
      contentions = 0;
      if (write) {
        end_case();
        start_case("copy_fork");
      }
      owner_round();
      __atomic_store_n(&phase, round_base + 3, __ATOMIC_RELEASE);
    }
  }

  void owner_round() {
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    const auto pages = Pfa::get_memory_stats().free_pages;
    auto created = process::user_space::create_user_address_space();
    AddressSpaceReaders::require(created.has_value());
    target = moss::move(*created);
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
    AddressSpaceReaders::require(target->add_vma(address, address + page_size, flags));
    if (write) {
      auto child = process::user_space::create_user_address_space();
      AddressSpaceReaders::require(child.has_value());
      clone = moss::move(*child);
      AddressSpaceReaders::require(clone->add_vma(address, address + page_size, flags));
    }
    auto allocated = mm::allocate_pages(0);
    AddressSpaceReaders::require(allocated.has_value());
    original = *allocated;
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(original));
    for (usize index = 0; index < page_size; ++index) {
      bytes[index] = original_byte;
      buffer[index] = payload_byte;
    }
    expected = memory_hash(write ? reinterpret_cast<VirtAddr>(buffer) : phys_to_virt(original), page_size);
    AddressSpaceReaders::require(Tables::map_user_page(target->pgd_phys, address, original, mm::page_perms::USER_RW));
    armed = true;
    const auto remaining =
        write ? target->copy_to_user(address, buffer, page_size) : target->copy_from_user(buffer, address, page_size);
    ContainerInterleaving::wait_for(arrived, round_base + 3);
    ut::expect(remaining == 0 && peer_ok && affinity_valid());
    ut::expect(borrows == 1 && contentions == 1);
    if (write) {
      const auto *parent_pte = Tables::get_user_pte(target->pgd_phys, address);
      const auto *child_pte = Tables::get_user_pte(clone->pgd_phys, address);
      const bool shared = parent_pte && child_pte && parent_pte->get_phys_addr() == original &&
                          child_pte->get_phys_addr() == original && Pfa::page_ref_get(original) == 2;
      AddressSpaceReaders::require(shared);
      ut::expect(parent_pte->is_cow() && child_pte->is_cow() && !parent_pte->is_writable() &&
                 !child_pte->is_writable());
      ut::expect(cloned_contents == expected && memory_hash(phys_to_virt(original), page_size) == expected);
      // A subsequent write must split the shared frame, not modify the child.
      constexpr u8 private_byte = 0xc3; // Different from both initial patterns.
      ut::expect(target->copy_to_user(address, &private_byte, sizeof(private_byte)) == 0);
      parent_pte = Tables::get_user_pte(target->pgd_phys, address);
      const PhysAddr private_page = parent_pte ? parent_pte->get_phys_addr() : 0;
      AddressSpaceReaders::require(private_page && private_page != original && Pfa::page_ref_get(private_page) == 1 &&
                                   Pfa::page_ref_get(original) == 1);
      ut::expect(parent_pte->is_writable() && !parent_pte->is_cow());
      ut::expect(*reinterpret_cast<const u8 *>(phys_to_virt(private_page)) == private_byte);
      ut::expect(memory_hash(phys_to_virt(original), page_size) == expected);
    } else {
      const auto *pte = Tables::get_user_pte(target->pgd_phys, address);
      ut::expect((!pte || !pte->is_valid()) && !target->find_vma(address) && Pfa::page_ref_get(original) == 0);
      ut::expect(memory_hash(reinterpret_cast<VirtAddr>(buffer), page_size) == expected);
      ut::expect(target->copy_from_user(buffer, address, page_size) == page_size);
      bool zeroed = true;
      for (const u8 byte : buffer) {
        zeroed = byte == 0 && zeroed;
      }
      ut::expect(zeroed);
    }
    clone.reset();
    target.reset();
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }
};
UserPageLeases *user_page_leases = nullptr;

// Exercise synchronous cross-CPU invalidation with IRQs disabled. The
// peer really installs the owned root and primes its translation, so software
// PTE inspection alone cannot make the remote-remap assertion pass.
struct TlbBroadcast {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  // Local, IRQ-masked remote, VM-lock contention, interrupt-only delivery and
  // full-tree pruning, then concurrent remaps. Software publishers exercise
  // each CPU taking the lock first; ARM64 uses its native broadcasts. Every
  // round has prime/remap/root-retired milestones in the outer handshake.
  static constexpr u32 rounds = 7, milestones = 3;
  static constexpr u32 lock_round = 2, irq_round = 3, prune_round = 4;
  static constexpr u32 publisher_round = prune_round + 1;
  static constexpr const char *names[rounds] = {"local_remap",  "remote_remap",       "locked_remap",      "ipi_remap",
                                                "pruned_remap", "concurrent_remap_0", "concurrent_remap_1"};
  // Arbitrary, distinct values reveal whether a cached old frame was used.
  static constexpr u8 before = 0x5a, after = 0xa5;
  shared_ptr<process::AddressSpace> target;
  u32 phase = 0, arrived = 0, scenario = 0, contentions = 0;
  bool remote = false, peer_ok = false;
  u8 peer_before = 0, peer_after = 0;

  static bool active(PhysAddr physical, [[maybe_unused]] u16 asid) {
    u64 root;
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, ttbr0_el1" : "=r"(root));
    // TTBR0's ASID field starts at bit 48; require the actual nonzero tag,
    // not just a software AddressSpace whose root was never installed.
    return !arch::interrupts_enabled() && root == (physical | (static_cast<u64>(asid) << 48));
#elif defined(MOSS_ARCH_X64)
    asm volatile("mov %%cr3, %0" : "=r"(root));
    return !arch::interrupts_enabled() && root == physical;
#elif defined(MOSS_ARCH_RISCV64)
    asm volatile("csrr %0, satp" : "=r"(root));
    return !arch::interrupts_enabled() && root == hal::mmu::make_satp_value(physical, asid);
#endif
  }

  static bool active(const process::AddressSpace &space) { return active(space.pgd_phys, space.asid); }

  static bool read(u8 &value, VirtAddr user_address = address) {
    return moss::abi::uaccess::moss_raw_copy_from_user(&value, reinterpret_cast<const void *>(user_address),
                                                       sizeof(value)) == 0;
  }

  struct Publishers {
    // One publisher on each of the two pinned workers. The masks let either
    // worker finish first without overwriting the other's completion signal.
    static constexpr u32 count = 2, both = (1U << count) - 1;
    // Different bytes and VAs distinguish both old frames and both descriptors.
    static constexpr u8 old_bytes[count] = {0x35, 0x6a}, new_bytes[count] = {0xca, 0x95};
    shared_ptr<process::AddressSpace> spaces[count];
    PhysAddr original[count]{}, replacement[count]{};
    mm::PageTableEntry *leaves[count]{};
    u64 permissions[count]{};
    u32 ready = 0, completed = 0, publication = 0, contention = 0, first = 0;
    bool armed = false, ok[count]{};
    u8 primed[count]{}, observed[count]{};

    static VirtAddr location(u32 cpu) { return address + cpu * page_size; }

    void prepare(u32 first_cpu) {
      first = first_cpu;
      ready = completed = publication = contention = 0;
      for (u32 cpu = 0; cpu < count; ++cpu) {
        ok[cpu] = false;
        primed[cpu] = observed[cpu] = 0;
        auto created = process::user_space::create_user_address_space();
        AddressSpaceReaders::require(created);
        spaces[cpu] = moss::move(*created);
        constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO;
        AddressSpaceReaders::require(spaces[cpu]->add_vma(address, address + count * page_size, flags));
        // A resident sibling prevents pruning from masking a broken VA request
        // with a full-tree flush. The two writers hold different real VM locks.
        for (u32 page = 0; page < count; ++page) {
          AddressSpaceReaders::require(spaces[cpu]->copy_to_user(location(page), &old_bytes[cpu], sizeof(u8)) == 0);
        }
        leaves[cpu] = Tables::get_user_pte(spaces[cpu]->pgd_phys, location(cpu));
        AddressSpaceReaders::require(leaves[cpu] && spaces[cpu]->asid != 0);
        original[cpu] = leaves[cpu]->get_phys_addr();
        permissions[cpu] = leaves[cpu]->raw & ~hal::mmu::PTE_ADDR_MASK;
        Pfa::page_ref_inc(original[cpu]);
        auto frame = mm::allocate_pages(0);
        AddressSpaceReaders::require(frame && *frame != original[cpu] && Pfa::page_ref_get(original[cpu]) == 2);
        replacement[cpu] = *frame;
        *reinterpret_cast<u8 *>(phys_to_virt(*frame)) = new_bytes[cpu];
      }
      AddressSpaceReaders::require(spaces[0]->asid != spaces[1]->asid);
    }

    void publishing() {
      if (__atomic_load_n(&armed, __ATOMIC_ACQUIRE) && arch::get_current_cpu_id() == first &&
          __atomic_exchange_n(&publication, 1U, __ATOMIC_RELEASE) == 0) {
        // Hold the acquired production publisher lock before publishing any
        // request. The peer must actually fail its acquisition before we
        // proceed, so an early cooperative ACK cannot hide a broken lock wait.
        while ((__atomic_load_n(&contention, __ATOMIC_ACQUIRE) & (1U << (first ^ 1U))) == 0) {
          arch::cpu_yield();
        }
      }
    }

    void contended() {
      const auto cpu = arch::get_current_cpu_id();
      if (cpu < count && __atomic_load_n(&armed, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_or(&contention, 1U << cpu, __ATOMIC_RELEASE);
      }
    }

    void perform(u32 cpu) {
      auto process = process::current_process();
      auto saved = process ? process->address_space() : shared_ptr<process::AddressSpace>{};
      AddressSpaceReaders::require(saved && arch::get_current_cpu_id() == cpu);
      const bool interrupts = arch::interrupts_enabled();
      arch::disable_interrupts();
      const auto other = cpu ^ 1U;
      UserCopyVersion::activate(spaces[other]);
      ok[cpu] = active(*spaces[other]) && read(primed[cpu], location(other));
      __atomic_fetch_or(&ready, 1U << cpu, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(ready, both);
#if !defined(MOSS_ARCH_ARM64)
      if (cpu == first) {
        // Arm only after both workers have masked IRQs and primed their real
        // roots. A preempting task during preparation must not hit the barrier.
        __atomic_store_n(&armed, true, __ATOMIC_RELEASE);
      } else {
        ContainerInterleaving::wait_for(publication, 1);
      }
#endif
      {
        auto transaction = spaces[cpu]->lock_vm();
        Tables::unmap_user_page(spaces[cpu]->pgd_phys, location(cpu));
        AddressSpaceReaders::require(
            Tables::map_user_page(spaces[cpu]->pgd_phys, location(cpu), replacement[cpu], permissions[cpu]));
      }
      __atomic_fetch_or(&completed, 1U << cpu, __ATOMIC_RELEASE);
      // Service the peer's requests even after our own remap finished. IRQs
      // stay masked, so neither timer rescheduling nor a root reload can hide
      // a missed invalidation before the actual load from the peer's new page.
      ContainerInterleaving::wait_for(completed, both);
      // Both production remaps returned. Disarm before either worker restores
      // IRQs, so an unrelated task on these CPUs cannot enter our observers.
      __atomic_store_n(&armed, false, __ATOMIC_RELEASE);
      ok[cpu] = active(*spaces[other]) && read(observed[cpu], location(other)) && ok[cpu];
      UserCopyVersion::activate(saved);
      if (interrupts) {
        arch::enable_interrupts();
      }
    }

    void finish() {
#if !defined(MOSS_ARCH_ARM64)
      ut::expect(__atomic_load_n(&publication, __ATOMIC_ACQUIRE) == 1 &&
                 (__atomic_load_n(&contention, __ATOMIC_ACQUIRE) & (1U << (first ^ 1U))) != 0);
#endif
      for (u32 cpu = 0; cpu < count; ++cpu) {
        ut::expect(ok[cpu] && primed[cpu] == old_bytes[cpu ^ 1U] && observed[cpu] == new_bytes[cpu ^ 1U]);
        auto *leaf = Tables::get_user_pte(spaces[cpu]->pgd_phys, location(cpu));
        ut::expect(leaf == leaves[cpu] && leaf->get_phys_addr() == replacement[cpu]);
        ut::expect(Pfa::page_ref_get(original[cpu]) == 1 &&
                   *reinterpret_cast<const u8 *>(phys_to_virt(original[cpu])) == old_bytes[cpu]);
        spaces[cpu].reset();
        AddressSpaceReaders::require(Pfa::page_ref_dec(original[cpu]) == 0 && mm::free_pages(original[cpu], 0));
      }
    }
  } publishers;

  void contended(PhysAddr root) {
    if (scenario == lock_round && target && root == target->pgd_phys && arch::get_current_cpu_id() == 1) {
      ++contentions;
      __atomic_store_n(&arrived, scenario * milestones + 2, __ATOMIC_RELEASE);
    }
  }

  bool peer() {
    auto process = process::current_process();
    auto saved = process ? process->address_space() : shared_ptr<process::AddressSpace>{};
    peer_ok = saved && arch::get_current_cpu_id() == 1;
    for (u32 round = 0; round < rounds; ++round) {
      const auto base = round * milestones;
      __atomic_store_n(&arrived, base + 1, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, base + 1);
      if (round >= publisher_round) {
        publishers.perform(1);
        __atomic_store_n(&arrived, base + 3, __ATOMIC_RELEASE);
        ContainerInterleaving::wait_for(phase, base + 3);
        continue;
      }
      const bool interrupts = arch::interrupts_enabled();
      if (remote && peer_ok) {
        arch::disable_interrupts();
        UserCopyVersion::activate(target);
        peer_ok = active(*target) && read(peer_before) && peer_ok;
      }
      if (round == lock_round) {
        // Owner already holds this real VM lock. Its shootdown must complete
        // while we wait in the production IRQ-masked ticket-spin path.
        auto transaction = target->lock_vm();
      } else {
        if (round == irq_round) {
          // Prevent a timer context switch / CR3 or SATP reload from hiding a
          // missing TLB IPI. This wait deliberately does not service by polling.
          hal::timer::disable();
          arch::enable_interrupts();
        }
        __atomic_store_n(&arrived, base + 2, __ATOMIC_RELEASE);
      }
      if (round == irq_round) {
        while (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) < base + 2) {
          asm volatile("" ::: "memory");
        }
        arch::disable_interrupts();
      } else {
        ContainerInterleaving::wait_for(phase, base + 2);
      }
      if (remote && saved) {
        peer_ok = active(*target) && read(peer_after) && peer_ok;
        UserCopyVersion::activate(saved);
        if (round == irq_round) {
          hal::timer::enable();
        }
        if (interrupts) {
          arch::enable_interrupts();
        }
      }
      // No CPU may retain the target root when the owner releases its tables.
      __atomic_store_n(&arrived, base + 3, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, base + 3);
    }
    return peer_ok;
  }

  void owner() {
    for (u32 round = 0; round < rounds; ++round) {
      const auto base = round * milestones;
      ContainerInterleaving::wait_for(arrived, base + 1);
      scenario = round;
      contentions = 0;
      remote = round != 0;
      if (remote) {
        end_case();
        start_case(names[round]);
      }
      owner_round(base);
      __atomic_store_n(&phase, base + 3, __ATOMIC_RELEASE);
    }
  }

  void owner_round(u32 base) {
    const auto pages = Pfa::get_memory_stats().free_pages;
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    if (scenario >= publisher_round) {
      publishers.prepare(scenario - publisher_round);
      __atomic_store_n(&phase, base + 1, __ATOMIC_RELEASE);
      publishers.perform(0);
      ContainerInterleaving::wait_for(arrived, base + 3);
      publishers.finish();
      ut::expect(affinity_valid());
      ut::expect(Pfa::get_memory_stats().free_pages == pages);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      return;
    }
    auto created = process::user_space::create_user_address_space();
    AddressSpaceReaders::require(created.has_value());
    target = moss::move(*created);
    // Keep a resident sibling in the same leaf table: otherwise unmap's
    // separate full-tree pruning flush would mask a broken per-address TLBI.
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO;
    AddressSpaceReaders::require(target->add_vma(address, address + 2 * page_size, flags));
    AddressSpaceReaders::require(target->copy_to_user(address, &before, sizeof(before)) == 0);
    if (scenario != prune_round) {
      AddressSpaceReaders::require(target->copy_to_user(address + page_size, &before, sizeof(before)) == 0);
    }
    auto *leaf = Tables::get_user_pte(target->pgd_phys, address);
    AddressSpaceReaders::require(leaf && target->asid != 0);
#if defined(MOSS_ARCH_ARM64)
    AddressSpaceReaders::require((leaf->raw & mm::page_attr::NG) != 0);
#endif
    const auto original = leaf->get_phys_addr();
    const auto permissions = leaf->raw & ~hal::mmu::PTE_ADDR_MASK;
    // Keep the old frame owned even in a failing run: a stale translation
    // reports the old byte without accessing storage recycled for another use.
    Pfa::page_ref_inc(original);
    auto replacement = mm::allocate_pages(0);
    AddressSpaceReaders::require(replacement && *replacement != original && Pfa::page_ref_get(original) == 2);
    *reinterpret_cast<u8 *>(phys_to_virt(*replacement)) = after;
    auto process = process::current_process();
    auto saved = process ? process->address_space() : shared_ptr<process::AddressSpace>{};
    AddressSpaceReaders::require(saved && saved->asid != target->asid && affinity_valid());
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    u8 local_before = 0, local_after = 0;
    bool local_ok = true;
    if (!remote) {
      UserCopyVersion::activate(target);
      local_ok = active(*target) && read(local_before);
    }
    {
      auto transaction = target->lock_vm();
      __atomic_store_n(&phase, base + 1, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(arrived, base + 2);
      Tables::unmap_user_page(target->pgd_phys, address);
      AddressSpaceReaders::require(Tables::map_user_page(target->pgd_phys, address, *replacement, permissions));
    }
    if (!remote) {
      local_ok = read(local_after) && local_ok;
      UserCopyVersion::activate(saved);
    }
    __atomic_store_n(&phase, base + 2, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, base + 3);
    if (interrupts) {
      arch::enable_interrupts();
    }
    ut::expect(local_ok && peer_ok && affinity_valid());
    ut::expect((remote ? peer_before : local_before) == before);
    ut::expect((remote ? peer_after : local_after) == after);
    auto *observed = Tables::get_user_pte(target->pgd_phys, address);
    ut::expect(observed && observed->get_phys_addr() == *replacement && (scenario == prune_round || observed == leaf));
    if (scenario == lock_round) {
      ut::expect(contentions == 1);
    }
    ut::expect(*reinterpret_cast<const u8 *>(phys_to_virt(original)) == before && Pfa::page_ref_get(original) == 1);
    target.reset();
    AddressSpaceReaders::require(Pfa::page_ref_dec(original) == 0 && mm::free_pages(original, 0));
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }
};
TlbBroadcast *tlb_broadcast = nullptr;

struct HardwareRootLifetime {
  using Scheduler = process::CfsScheduler;
  using Pfa = mm::PageFrameAllocator;
  using Tables = mm::PageTableManager;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  // Cover both user->user replacement and detached user->kernel retirement.
  static constexpr u32 scenario_count = 2;
  static constexpr const char *names[scenario_count] = {"hardware_root", "kernel_root"};
  // A distinct nonzero byte distinguishes the resident old page from the
  // demand-zero neighbour and any replacement image's contents.
  static constexpr u8 payload = 0x69;
  unique_ptr<process::Process> target;
  PhysAddr root = 0, data = 0;
  u16 asid = 0;
  // CPU0 owns publication, CPU1 owns the temporary hardware root. Milestones
  // are 1=install prepared tree, 2=publication detached, 3=hardware retired.
  // Release/acquire handshakes make the final allocator checks quiescent.
  u32 phase = 0, arrived = 0, early = 0, retirements = 0;
  bool peer_ok = false;

  void retiring(PhysAddr retiring_root) {
    if (retiring_root != root) {
      return;
    }
    __atomic_fetch_add(&retirements, 1U, __ATOMIC_RELEASE);
    if (arch::get_current_cpu_id() == 1) {
      // Destruction must happen after the hardware write, not merely after
      // swapping the software owner. Stop before freeing an active tree.
      if (!ut::expect(!TlbBroadcast::active(root, asid))) {
        end_case();
        finish("active_root_retired_early");
      }
    } else if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) < 3) {
      // Make a missing CPU lease fail safely: the destructor has started but
      // has not freed anything. Ask the peer to switch away before continuing,
      // rather than probing a freed table through the MMU in the negative run.
      __atomic_store_n(&early, 1U, __ATOMIC_RELEASE);
      __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(arrived, 3);
    }
  }

  bool peer(bool kernel_retirement) {
    auto current = process::current_process();
    auto saved = current ? current->address_space() : shared_ptr<process::AddressSpace>{};
    AddressSpaceReaders::require(saved && arch::get_current_cpu_id() == 1);
    __atomic_store_n(&arrived, 1U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 1);
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    {
      auto incoming = target->address_space();
      AddressSpaceReaders::require(incoming && incoming->pgd_phys == root);
      Scheduler::use_address_space(moss::move(incoming));
    }
    // No task-stack or fixture shared_ptr owns the old tree now. Only the
    // Process publication and the CPU's installed-root lease may retain it.
    u8 byte = 0;
    peer_ok = TlbBroadcast::active(root, asid) && TlbBroadcast::read(byte, address) && byte == payload;
    __atomic_store_n(&arrived, 2U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 2);
    if (__atomic_load_n(&early, __ATOMIC_ACQUIRE) == 0) {
      peer_ok = TlbBroadcast::active(root, asid) && TlbBroadcast::read(byte, address) && byte == payload && peer_ok;
      // This page belongs only to the detached old image and is not resident.
      // A real raw-copy fault must resolve against the CPU's owned root, not
      // the replacement Process version or this worker's ordinary image.
      peer_ok = TlbBroadcast::read(byte, address + page_size) && byte == 0 && peer_ok;
    } else {
      peer_ok = false;
    }
    if (kernel_retirement) {
      Scheduler::use_kernel_address_space();
      peer_ok = !Scheduler::active_address_space() && peer_ok;
    }
    Scheduler::use_address_space(saved);
    __atomic_store_n(&arrived, 3U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 3);
    if (interrupts) {
      arch::enable_interrupts();
    }
    return peer_ok;
  }

  void owner(bool detach) {
    ContainerInterleaving::wait_for(arrived, 1);
    const auto pages = Pfa::get_memory_stats().free_pages;
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    target = make_unique<process::Process>(INVALID_PROCESS_ID);
    auto original = process::user_space::create_user_address_space();
    AddressSpaceReaders::require(original);
    root = (*original)->pgd_phys;
    asid = (*original)->asid;
    // One resident page plus one demand-only neighbour exercises both stable
    // hardware translations and a fault after Process ownership was replaced.
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO;
    AddressSpaceReaders::require((*original)->add_vma(address, address + 2 * page_size, flags));
    AddressSpaceReaders::require((*original)->copy_to_user(address, &payload, sizeof(payload)) == 0);
    auto *leaf = Tables::get_user_pte(root, address);
    AddressSpaceReaders::require(leaf);
    data = leaf->get_phys_addr();
    AddressSpaceReaders::require(target->set_address_space(moss::move(*original)));
    __atomic_store_n(&phase, 1U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 2);
    if (detach) {
      target->clear_address_space();
      target.reset(); // Process destruction must not remove a remote CPU's pin.
    } else {
      auto replacement = process::user_space::create_user_address_space();
      AddressSpaceReaders::require(replacement);
      AddressSpaceReaders::require(target->set_address_space(moss::move(*replacement)));
    }
    ut::expect(__atomic_load_n(&early, __ATOMIC_ACQUIRE) == 0);
    ut::expect(__atomic_load_n(&retirements, __ATOMIC_ACQUIRE) == 0);
    ut::expect(Pfa::page_ref_get(root) == 1 && Pfa::page_ref_get(data) == 1);
    {
      auto other = process::user_space::create_user_address_space();
      AddressSpaceReaders::require(other);
      ut::expect((*other)->asid != asid);
    }
    __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 3);
    ut::expect(peer_ok && affinity_valid());
    ut::expect(__atomic_load_n(&retirements, __ATOMIC_ACQUIRE) == 1);
    ut::expect(Pfa::page_ref_get(root) == 0 && Pfa::page_ref_get(data) == 0);
    // Stop observing this physical number before allocator reuse could make
    // a later, unrelated address space look like another retirement of it.
    root = 0;
    target.reset();
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 3U, __ATOMIC_RELEASE);
  }
};
HardwareRootLifetime *hardware_root_lifetime = nullptr;

#if !defined(MOSS_ARCH_ARM64)
// These probes run during the first secondary's actual boot registration, not
// by editing the live membership mask or re-registering an already online CPU.
// The BSP pauses after its firmware start request, before starting other CPUs.
struct TlbJoinObservation {
  // Release/acquire milestones coordinate boot CPUs without a scheduler.
  u32 prepared = 0, publishing = 0, contended = 0, joined = 0, writer_done = 0, finished = 0;
  u64 targets = 0;
  usize pages = 0, heap = 0;
  bool primed = false, refreshed = false, old_owned = false, released = false, restored = false;
};
TlbJoinObservation tlb_join_observed;

struct TlbJoin {
  using Pfa = mm::PageFrameAllocator;
  using Tables = mm::PageTableManager;
  using Scheduler = process::CfsScheduler;
  enum class Order { None, RequestFirst, CpuFirst };
  // BSP is the publisher; logical CPU1 is the first secondary on both boot paths.
  static constexpr u32 publisher_cpu = 0, joining_cpu = 1;
  static constexpr u32 joining_bit = 1U << joining_cpu;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  // Distinct nonzero bytes expose a retained translation to the old frame.
  static constexpr u8 old_byte = 0x39, new_byte = 0xc6;
  shared_ptr<process::AddressSpace> space;
  mm::PageTableEntry *leaf = nullptr;
  PhysAddr original = 0, replacement = 0;
  Order order;
  inline static TlbJoin *probe = nullptr;
#if defined(MOSS_ARCH_RISCV64)
  // sstatus.SUM (bit 18) permits S-mode loads from U pages. This boot callback
  // has not entered the exception path that normally enables it for raw copy.
  static constexpr u64 supervisor_user_access = u64{1} << 18;
  u64 saved_user_access = 0;
#endif

  static Order selected() {
    // Size derives from the longest accepted workload ID, including its NUL.
    char value[sizeof("mm.tlb_join.request_first")];
    if (!boot_option("moss.validation", value, sizeof(value))) {
      return Order::None;
    }
    if (ut::same_id(value, "mm.tlb_join.request_first")) {
      return Order::RequestFirst;
    }
    return ut::same_id(value, "mm.tlb_join.cpu_first") ? Order::CpuFirst : Order::None;
  }

  static void require(bool valid) {
    if (!valid) {
      // A setup failure must stop this guest, not publish fabricated evidence.
      // The host startup deadline and native failure capture still apply.
      arch::kernel_panic("TLB join fixture setup failed");
    }
  }

  explicit TlbJoin(Order selected_order) : order(selected_order) {
    auto created = process::user_space::create_user_address_space();
    require(static_cast<bool>(created));
    space = moss::move(*created);
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO;
    require(space->add_vma(address, address + page_size, flags));
    require(space->copy_to_user(address, &old_byte, sizeof(old_byte)) == 0);
    leaf = Tables::get_user_pte(space->pgd_phys, address);
    require(leaf && leaf->is_valid());
    original = leaf->get_phys_addr();
    // Retain the old frame independently so a deliberately missed flush reads
    // owned stale data, never a freed/reallocated physical page.
    Pfa::page_ref_inc(original);
    auto page = mm::allocate_pages(0);
    require(static_cast<bool>(page));
    replacement = *page;
    *reinterpret_cast<u8 *>(phys_to_virt(replacement)) = new_byte;
    Scheduler::use_address_space(space);
#if defined(MOSS_ARCH_RISCV64)
    // Keep SUM stable from priming through the final load: changing access
    // state between reads may discard cached translations and hide a missing
    // TLB invalidation. IRQs stay masked for this entire boot-only fixture.
    asm volatile("csrrs %0, sstatus, %1" : "=r"(saved_user_access) : "r"(supervisor_user_access) : "memory");
#endif
    prime();
  }

  ~TlbJoin() {
#if defined(MOSS_ARCH_RISCV64)
    // Restore only the access bit we changed, not unrelated status fields.
    if (!(saved_user_access & supervisor_user_access)) {
      asm volatile("csrc sstatus, %0" ::"r"(supervisor_user_access) : "memory");
    }
    u64 restored;
    asm volatile("csrr %0, sstatus" : "=r"(restored));
    require(((restored ^ saved_user_access) & supervisor_user_access) == 0);
#endif
  }

  void prime() const {
    u8 byte = 0;
    tlb_join_observed.primed = TlbBroadcast::active(*space) && TlbBroadcast::read(byte, address) && byte == old_byte;
    require(tlb_join_observed.primed);
  }

  static void registration(u32 stage) {
    if (arch::get_current_cpu_id() != joining_cpu || selected() == Order::None) {
      return;
    }
    auto &seen = tlb_join_observed;
    if (stage == 0) { // Before acquiring the real publisher lock.
      seen.pages = Pfa::get_memory_stats().free_pages;
      seen.heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      probe = new TlbJoin(selected());
      __atomic_store_n(&seen.prepared, 1U, __ATOMIC_RELEASE);
      if (probe->order == Order::RequestFirst) {
        ContainerInterleaving::wait_for(seen.publishing, 1);
      }
    } else if (stage == 1) { // Full local flush and membership publication done; lock still held.
      if (probe->order == Order::CpuFirst) {
        // Refill after the registration flush, before the BSP changes the PTE.
        // Only the later request can now remove this cached old translation.
        probe->prime();
        __atomic_store_n(&seen.joined, 1U, __ATOMIC_RELEASE);
        ContainerInterleaving::wait_for(seen.contended, 1U << publisher_cpu);
      }
    } else { // Lock released; remain IRQ-masked until the final hardware load.
      ContainerInterleaving::wait_for(seen.writer_done, 1);
      u8 byte = 0;
      seen.refreshed = TlbBroadcast::active(*probe->space) && TlbBroadcast::read(byte, address) && byte == new_byte;
      seen.old_owned = Pfa::page_ref_get(probe->original) == 1 &&
                       *reinterpret_cast<const u8 *>(phys_to_virt(probe->original)) == old_byte &&
                       probe->leaf->get_phys_addr() == probe->replacement;
      const auto original = probe->original, replacement = probe->replacement;
      Scheduler::use_kernel_address_space();
      delete probe;
      probe = nullptr;
      require(Pfa::page_ref_dec(original) == 0 && mm::free_pages(original, 0));
      seen.released = Pfa::page_ref_get(original) == 0 && Pfa::page_ref_get(replacement) == 0;
      seen.restored = !Scheduler::active_address_space() && Pfa::get_memory_stats().free_pages == seen.pages &&
                      mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == seen.heap;
      __atomic_store_n(&seen.finished, 1U, __ATOMIC_RELEASE);
    }
  }

  static void cpu_started(u32 cpu) {
    if (cpu != joining_cpu || selected() == Order::None) {
      return;
    }
    require(arch::get_current_cpu_id() == publisher_cpu);
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    auto &seen = tlb_join_observed;
    ContainerInterleaving::wait_for(seen.prepared, 1);
    if (probe->order == Order::CpuFirst) {
      ContainerInterleaving::wait_for(seen.joined, 1);
    }
    {
      auto transaction = probe->space->lock_vm();
      mm::PageTableEntry updated;
      updated.set_page(probe->replacement, probe->leaf->raw & ~hal::mmu::PTE_ADDR_MASK);
      // One atomic PTE replacement isolates the single production invalidation
      // request under test. An unmap/map pair could hide a missing join flush
      // with its second invalidation. Transfer the old mapping's frame reference
      // to the fixture's retained copy; the new allocation becomes the mapping.
      __atomic_store_n(&probe->leaf->raw, updated.raw, __ATOMIC_RELEASE);
      require(Pfa::page_ref_dec(probe->original) == 1);
      arch::flush_tlb_addr(address);
    }
    __atomic_store_n(&seen.writer_done, 1U, __ATOMIC_RELEASE);
    // CPU1 may reclaim tables while we wait. Cooperatively ACK that independent
    // cleanup, and do not touch its fixture again after publishing writer_done.
    ContainerInterleaving::wait_for(seen.finished, 1);
    if (interrupts) {
      arch::enable_interrupts();
    }
  }

  static bool recording() {
    return __atomic_load_n(&tlb_join_observed.prepared, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&tlb_join_observed.writer_done, __ATOMIC_ACQUIRE) == 0;
  }

  static void publishing() {
    if (recording() && arch::get_current_cpu_id() == publisher_cpu && probe->order == Order::RequestFirst) {
      __atomic_store_n(&tlb_join_observed.publishing, 1U, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(tlb_join_observed.contended, joining_bit);
    }
  }

  static void contended() {
    if (recording()) {
      __atomic_fetch_or(&tlb_join_observed.contended, 1U << arch::get_current_cpu_id(), __ATOMIC_RELEASE);
    }
  }

  static void targets(u64 mask) {
    if (recording() && arch::get_current_cpu_id() == publisher_cpu) {
      tlb_join_observed.targets = mask;
    }
  }

  static void verify() {
    const auto &seen = tlb_join_observed;
    ut::expect(__atomic_load_n(&seen.finished, __ATOMIC_ACQUIRE) == 1);
    ut::expect(seen.primed && seen.refreshed);
    ut::expect(seen.old_owned && seen.released && seen.restored);
    const bool request_first = selected() == Order::RequestFirst;
    ut::expect(seen.targets == (request_first ? 0 : joining_bit));
    ut::expect(seen.contended == (request_first ? joining_bit : 1U << publisher_cpu));
    ut::expect(probe == nullptr);
  }
};
#endif

// Independent fork-style tables share a real endpoint, not a synthetic counter.
// Barriers align contention and make ownership checks quiescent; they do not
// force a particular instruction-level interleaving inside ref()/release_file().


void wait_for_phase(const u32 &phase, u32 value) { ContainerInterleaving::wait_for(phase, value); }

static void empty_case() {}

void register_smp_cases() {
  ut::register_suite("containers.smp", [] { ut::register_test("interleaving", empty_case); });
  register_vfs_smp_cases();
  ut::register_suite("interrupts.smp", [] { ut::register_test("irq_context_retirement", empty_case); });
  ut::register_suite("mm.lifetime", [] {
    ut::register_test("held_readers", empty_case);
    for (const auto *name : HardwareRootLifetime::names) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("mm.concurrent", [] {
    ut::register_test("cow_fault", empty_case);
    ut::register_test("demand_fault", empty_case);
    ut::register_test("fault_unmap", empty_case);
    ut::register_test("fault_fork", empty_case);
    ut::register_test("fork_unmap", empty_case);
  });
  ut::register_suite("mm.uaccess", [] {
    ut::register_test("copy_unmap", empty_case);
    ut::register_test("copy_fork", empty_case);
  });
  ut::register_suite("mm.tlb_broadcast", [] {
    for (const auto *name : TlbBroadcast::names) {
      ut::register_test(name, empty_case);
    }
  });
#if !defined(MOSS_ARCH_ARM64)
  ut::register_suite("mm.tlb_join.request_first", [] { ut::register_test("registration", TlbJoin::verify); });
  ut::register_suite("mm.tlb_join.cpu_first", [] { ut::register_test("registration", TlbJoin::verify); });
#endif
}

long start_smp_suite() {
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
  if (const long mode = start_vfs_smp_suite()) {
    return mode;
  }
  if (ut::same_id(selection, "interrupts.smp")) {
    start_case("irq_context_retirement");
    AddressSpaceReaders::require(g_num_cpus >= 2);
    interrupt_unbind = new InterruptUnbind();
    interrupt_unbind->prepare();
    return 3;
  }
  if (ut::same_id(selection, "mm.lifetime")) {
    start_case("held_readers");
    AddressSpaceReaders::require(g_num_cpus >= 2);
    address_space_readers = new AddressSpaceReaders();
    hardware_root_lifetime = new HardwareRootLifetime[HardwareRootLifetime::scenario_count];
    return 3; // Existing mode pins a forked reader to CPU1 and its owner to CPU0.
  }
  if (ut::same_id(selection, "mm.concurrent")) {
    start_case("cow_fault");
    AddressSpaceReaders::require(g_num_cpus >= 2);
    fault_transactions = new FaultTransactions();
    return 3; // The same real fork/affinity/reap protocol as mm.lifetime.
  }
  if (ut::same_id(selection, "mm.uaccess")) {
    start_case("copy_unmap");
    AddressSpaceReaders::require(g_num_cpus >= 2);
    user_page_leases = new UserPageLeases();
    return 3; // Share the CPU0/CPU1 fork, affinity, control and reap protocol.
  }
  if (ut::same_id(selection, "mm.tlb_broadcast")) {
    start_case("local_remap");
    AddressSpaceReaders::require(g_num_cpus >= 2);
    tlb_broadcast = new TlbBroadcast();
    return 3; // Real CPU0/CPU1 workers, with temporary owned hardware roots.
  }
  return 0;
}

long smp_control(long op, long arg1, long arg2) {
  const char *selection = selected_suite();
  const char *active_case = running_case();
  if (ut::same_id(selection, "mm.tlb_broadcast") && active_case && tlb_broadcast) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return tlb_broadcast->peer() ? 1 : 0;
    }
    if (op == 8) {
      AddressSpaceReaders::require(arg1 && affinity_valid());
      arch::enable_interrupts();
      tlb_broadcast->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete tlb_broadcast;
      tlb_broadcast = nullptr;
      end_case();
      finish();
    }
  }
  if (ut::same_id(selection, "mm.uaccess") && active_case && user_page_leases) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return user_page_leases->peer() ? 1 : 0;
    }
    if (op == 8) {
      AddressSpaceReaders::require(arg1 && affinity_valid());
      arch::enable_interrupts();
      user_page_leases->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete user_page_leases;
      user_page_leases = nullptr;
      end_case();
      finish();
    }
  }
  if (ut::same_id(selection, "mm.concurrent") && active_case && fault_transactions) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return fault_transactions->peer() ? 1 : 0;
    }
    if (op == 8) {
      AddressSpaceReaders::require(arg1 && affinity_valid());
      arch::enable_interrupts();
      fault_transactions->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete fault_transactions;
      fault_transactions = nullptr;
      end_case();
      finish();
    }
  }
  if (ut::same_id(selection, "interrupts.smp") && active_case && interrupt_unbind) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return interrupt_unbind->peer() ? 1 : 0;
    }
    if (op == 8) {
      AddressSpaceReaders::require(arg1 && affinity_valid());
      arch::enable_interrupts();
      interrupt_unbind->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete interrupt_unbind;
      interrupt_unbind = nullptr;
      end_case();
      finish();
    }
  }
  if (ut::same_id(selection, "mm.lifetime") && active_case && address_space_readers) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      bool readers_ok = address_space_readers->peer();
      for (u32 index = 0; index < HardwareRootLifetime::scenario_count; ++index) {
        readers_ok = hardware_root_lifetime[index].peer(index != 0) && readers_ok;
      }
      return readers_ok ? 1 : 0;
    }
    if (op == 8) {
      AddressSpaceReaders::require(arg1 && affinity_valid());
      arch::enable_interrupts();
      address_space_readers->owner();
      for (u32 index = 0; index < HardwareRootLifetime::scenario_count; ++index) {
        end_case();
        start_case(HardwareRootLifetime::names[index]);
        hardware_root_lifetime[index].owner(index != 0);
      }
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete address_space_readers;
      address_space_readers = nullptr;
      delete[] hardware_root_lifetime;
      hardware_root_lifetime = nullptr;
      end_case();
      finish();
    }
  }
  if (ut::same_id(selection, "vfs.smp")) {
    return vfs_smp_control(op, arg1, arg2);
  }
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

extern "C" void moss_validation_user_copy(PhysAddr root, VirtAddr address) noexcept {
  if (user_copy_version) {
    user_copy_version->replace(root, address);
  }
}

extern "C" void moss_validation_user_page(PhysAddr root, VirtAddr address, PhysAddr page, bool write) noexcept {
  if (user_page_leases) {
    user_page_leases->borrowed(root, address, page, write);
  }
}

extern "C" void moss_validation_cow_snapshot(PhysAddr root, VirtAddr address) noexcept {
  if (fault_transactions) {
    fault_transactions->snapshot(root, address);
  }
}

extern "C" void moss_validation_demand_snapshot(PhysAddr root, VirtAddr address) noexcept {
  if (fault_transactions) {
    fault_transactions->snapshot(root, address);
  }
}

extern "C" void moss_validation_demand_committed(PhysAddr root, VirtAddr address, PhysAddr page) noexcept {
  if (fault_transactions) {
    fault_transactions->committed(root, address, page);
  }
}

extern "C" void moss_validation_vm_contended(PhysAddr root) noexcept {
  if (tlb_broadcast) {
    tlb_broadcast->contended(root);
  }
  if (fault_transactions) {
    fault_transactions->contended(root);
  }
  if (user_page_leases) {
    user_page_leases->contended(root);
  }
}

extern "C" void moss_validation_tlb_contended() noexcept {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::contended();
#endif
  if (tlb_broadcast) {
    tlb_broadcast->publishers.contended();
  }
}

extern "C" void moss_validation_tlb_publishing() noexcept {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::publishing();
#endif
  if (tlb_broadcast) {
    tlb_broadcast->publishers.publishing();
  }
}

extern "C" void moss_validation_tlb_registration([[maybe_unused]] unsigned stage) noexcept {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::registration(stage);
#endif
}

extern "C" void moss_validation_tlb_targets([[maybe_unused]] moss::u64 targets) noexcept {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::targets(targets);
#endif
}

extern "C" void moss_validation_cpu_started([[maybe_unused]] unsigned cpu) noexcept {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::cpu_started(cpu);
#endif
}

extern "C" void moss_validation_address_space_retiring(PhysAddr root) noexcept {
  if (hardware_root_lifetime) {
    for (u32 index = 0; index < HardwareRootLifetime::scenario_count; ++index) {
      hardware_root_lifetime[index].retiring(root);
    }
  }
}

} // namespace moss::test::validation
