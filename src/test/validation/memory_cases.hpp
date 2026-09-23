#pragma once

#include "validation_internal.hpp"

namespace moss::test::validation {
extern HeapPressure *address_space_control_pressure;
extern bool address_space_control_exhausted;

void mm_initialization_publication();
void mm_unsupported_contracts();
void table_permission_defaults();
void kernel_mapping_permissions();
void active_user_mapping_permissions();
void kernel_wx_permissions();
void address_space_ownership();
void cow_clone_permissions();
void vma_boundaries();
void pages();
void reuse_pages();
void page_release_contract();
void page_exhaustion();
void heap_alignment();
void heap_invalid_requests();
void heap_release_contract();
void heap_reuse();
void heap_exhaustion();

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
