#pragma once

#include "validation_internal.hpp"

namespace moss::test::validation {
extern HeapPressure *address_space_control_pressure;
extern bool address_space_control_exhausted;

void register_mm_cases();
void register_pfa_cases();
void register_heap_cases();
void register_mm_permissions_cases();

void map_preserves_existing();
void map_allocation_rollback();
void map_rejects_blocks();
void clone_preserves_destination();
void clone_allocation_rollback();
void address_space_heap_rollback();
void address_space_control_rollback();
void vma_heap_rollback();
void asid_leases();
void unmap_reclaims_tables();
} // namespace moss::test::validation
