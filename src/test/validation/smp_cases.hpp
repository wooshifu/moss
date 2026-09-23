#pragma once

#include "validation/runtime.hpp"

namespace moss::test::validation {
void wait_for_phase(const u32 &phase, u32 value);
template <typename Condition> void smp_require(Condition valid) {
  if (!boost::ut::expect(static_cast<Condition &&>(valid))) {
    end_case();
    finish("address_space_setup");
  }
}
bool tlb_active(PhysAddr root, u16 asid);
bool tlb_active(const process::AddressSpace &space);
bool tlb_read(u8 &value, VirtAddr address = process::user_layout::CODE_BASE);
void register_containers_smp();
void register_interrupt_smp();
void register_lifetime_smp();
void register_fault_smp();
void register_leases_smp();
void register_tlb_smp();
void register_tlb_join_smp();
long start_containers_smp();
long start_interrupt_smp();
long start_lifetime_smp();
long start_fault_smp();
long start_leases_smp();
long start_tlb_smp();
long control_containers_smp(long op, long arg1, long arg2);
long control_interrupt_smp(long op, long arg1, long arg2);
long control_lifetime_smp(long op, long arg1, long arg2);
long control_fault_smp(long op, long arg1, long arg2);
long control_leases_smp(long op, long arg1, long arg2);
long control_tlb_smp(long op, long arg1, long arg2);
void fault_contended(PhysAddr root);
void leases_contended(PhysAddr root);
void tlb_broadcast_contended(PhysAddr root);
void tlb_broadcast_tlb_contended();
void tlb_broadcast_tlb_publishing();
void tlb_join_contended();
void tlb_join_publishing();

void register_smp_cases();
void register_vfs_smp_cases();
long start_vfs_smp_suite();
long vfs_smp_control(long op, long arg1, long arg2);
long start_smp_suite();
long smp_control(long op, long arg1, long arg2);
void user_copy_version_binding();
void raw_user_copy_fixup();
} // namespace moss::test::validation
