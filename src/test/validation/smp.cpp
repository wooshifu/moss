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
// Monotonic release/acquire fixture phases may advance before a waiter observes them.
void wait_for_phase(const u32 &phase, u32 value) {
  while (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) < value) {
    arch::cpu_yield();
  }
}

void register_smp_cases() {
  register_containers_smp();
  register_vfs_smp_cases();
  register_interrupt_smp();
  register_lifetime_smp();
  register_fault_smp();
  register_leases_smp();
  register_tlb_smp();
  register_tlb_join_smp();
}

long start_smp_suite() {
  if (const long mode = start_containers_smp()) {
    return mode;
  }
  if (const long mode = start_vfs_smp_suite()) {
    return mode;
  }
  if (const long mode = start_interrupt_smp()) {
    return mode;
  }
  if (const long mode = start_lifetime_smp()) {
    return mode;
  }
  if (const long mode = start_fault_smp()) {
    return mode;
  }
  if (const long mode = start_leases_smp()) {
    return mode;
  }
  if (const long mode = start_tlb_smp()) {
    return mode;
  }
  return 0;
}

long smp_control(long op, long arg1, long arg2) {
  const char *selection = selected_suite();
  if (ut::same_id(selection, "mm.tlb_broadcast")) {
    return control_tlb_smp(op, arg1, arg2);
  }
  if (ut::same_id(selection, "mm.uaccess")) {
    return control_leases_smp(op, arg1, arg2);
  }
  if (ut::same_id(selection, "mm.concurrent")) {
    return control_fault_smp(op, arg1, arg2);
  }
  if (ut::same_id(selection, "interrupts.smp")) {
    return control_interrupt_smp(op, arg1, arg2);
  }
  if (ut::same_id(selection, "mm.lifetime")) {
    return control_lifetime_smp(op, arg1, arg2);
  }
  if (ut::same_id(selection, "vfs.smp")) {
    return vfs_smp_control(op, arg1, arg2);
  }
  if (ut::same_id(selection, "containers.smp")) {
    return control_containers_smp(op, arg1, arg2);
  }
  invalid_control();
}

extern "C" void moss_validation_vm_contended(PhysAddr root) noexcept {
  tlb_broadcast_contended(root);
  fault_contended(root);
  leases_contended(root);
}
extern "C" void moss_validation_tlb_contended() noexcept {
  tlb_join_contended();
  tlb_broadcast_tlb_contended();
}
extern "C" void moss_validation_tlb_publishing() noexcept {
  tlb_join_publishing();
  tlb_broadcast_tlb_publishing();
}
} // namespace moss::test::validation
