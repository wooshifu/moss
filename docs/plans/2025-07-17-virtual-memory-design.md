# Virtual Memory: High-Half Kernel + Per-Process User Space

**Goal:** Implement Linux-style kernel/user separation — high-half kernel via TTBR1, per-process user address space via TTBR0, demand paging, and ELF loader integration.

**Scope:** High-half kernel migration, dynamic page table allocation, per-process TTBR0 switching with ASID, VMA-based demand paging (fully lazy), ELF loader VMA registration. COW and mmap/munmap deferred to a future iteration.

**Architecture:** ARM64 4-level paging (48-bit VA), TTBR0/TTBR1 split. Kernel runs at EL1, user programs at EL0.

---

## 1. Address Space Layout

```
0xFFFF_FFFF_FFFF_FFFF  ┌──────────────────────────┐
                        │  Kernel Stack (per-CPU)   │
0xFFFF_8000_4xxx_xxxx  │  Kernel .text/.data/.bss  │  VMA = high-half, LMA = 0x40000000
0xFFFF_8000_4000_0000  │  Kernel Direct Map        │  phys RAM mapped at this offset
0xFFFF_8000_0000_0000  ├──────────────────────────┤  ← TTBR1 (kernel, shared)
                        │  [non-canonical hole]     │
0x0000_8000_0000_0000  ├──────────────────────────┤  ← TTBR0 (user, per-process)
0x0000_7FFF_0000_0000  │  User Stack (grows ↓)     │  8MB, guard page below
0x0000_0001_0000_0000  │  User Heap  (grows ↑)     │  demand-zero
0x0000_0000_0040_0000  │  ELF .text/.data          │  from 0x400000
0x0000_0000_0000_0000  └──────────────────────────┘
```

### Address Translation Helpers

```cpp
constexpr VirtAddr KERNEL_DIRECT_MAP_BASE = 0xFFFF800000000000ULL;
constexpr VirtAddr PHYS_BASE = 0x40000000ULL; // QEMU virt RAM start

inline VirtAddr phys_to_virt(PhysAddr pa) { return pa + KERNEL_DIRECT_MAP_BASE; }
inline PhysAddr virt_to_phys(VirtAddr va) { return va - KERNEL_DIRECT_MAP_BASE; }
```

## 2. Boot Trampoline

### Current State

- Kernel linked at `0x40000000`, identity-mapped via 4×1GB blocks
- Both TTBR0 and TTBR1 point to the same PGD
- Kernel runs entirely in the low 4GB

### Migration Sequence (inside `setup_memory_management()`)

1. Initialize PageFrameAllocator (already done)
2. Allocate kernel PGD from buddy allocator (for TTBR1)
3. Map high-half region: physical RAM → `KERNEL_DIRECT_MAP_BASE + phys`, using 2MB blocks
4. Keep identity mapping temporarily (trampoline needs it)
5. Write `ttbr1_el1 = kernel_pgd_phys` with ASID=0
6. ISB + full TLB invalidation
7. Compute high-half PC: `current_pc - PHYS_BASE + KERNEL_DIRECT_MAP_BASE`
8. Branch to high-half address
9. After landing in high-half: clear TTBR0 identity map (set to empty PGD)

### Linker Script Changes

- VMA starts at `0xFFFF800040000000` (high-half + physical offset)
- LMA stays at `0x40000000` via `AT()` directive
- Boot assembly section `.text.boot` remains at physical address (runs pre-trampoline)
- Linker symbols (`_kernel_start`, `_kernel_end`, `_stack_top`) become high-half addresses

### Constraints

- PageFrameAllocator must be initialized before trampoline (current boot order satisfies this)
- Assembly startup code (`start_arm64.S`) unchanged — runs in identity map, calls C code
- `early_main()` and `unified_boot_main()` execute in identity map until trampoline completes

## 3. Per-Process Page Tables

### Dynamic Page Table Allocator

Replace the 64-table bump allocator with buddy-backed allocation:

```cpp
KernelResult<PageTable*> allocate_page_table() {
    auto result = PageFrameAllocator::allocate_pages(0); // 1 page = 4KB
    PhysAddr pa = result.value();
    auto* table = reinterpret_cast<PageTable*>(phys_to_virt(pa));
    memzero(table, PAGE_SIZE);
    return table;
}
```

### Per-Process PGD

`create_user_address_space()` implementation:

1. Allocate 4KB page as user PGD (L0 table)
2. Zero all 512 entries (all invalid)
3. Allocate ASID (8-bit, monotonic increment, flush TLB on wrap)
4. Return `AddressSpace{pgd_phys, asid}`

User PGD does NOT contain kernel mappings — TTBR1 handles kernel space.

### TTBR0 Switching

In `context_switch_to_task()`, before calling `context_switch()`:

```cpp
u64 ttbr0_val = next_as->pgd_phys | ((u64)next_as->asid << 48);
asm volatile("msr ttbr0_el1, %0; isb" :: "r"(ttbr0_val));
```

Kernel threads (no address_space_): lazy TTBR0 — keep previous process's TTBR0. Kernel threads never access user addresses, so stale TTBR0 is harmless.

### ASID Management

- 8-bit ASID (256 values), controlled by TCR_EL1.AS=0
- Monotonic counter in `create_user_address_space()`
- On overflow (>255): global TLB flush + reset counter to 1
- ASID 0 reserved for kernel

## 4. VMA Management and Demand Paging

### VmaRegion (enhanced)

```cpp
struct VmaRegion {
    VirtAddr start_addr;
    VirtAddr end_addr;
    u32 flags;              // READ | WRITE | EXEC | DEMAND_ZERO
    u32 type;               // VMA_CODE, VMA_DATA, VMA_STACK, VMA_HEAP, VMA_BSS

    // Lazy backing: ELF segment data source
    const u8* backing_data; // Pointer to ELF data in kernel memory (nullptr = zero page)
    usize backing_offset;   // Offset into backing_data for this VMA's start
    usize backing_size;     // Valid backing data length (remainder is zero-filled)
};
```

### VMA Operations

On `AddressSpace`:

- `add_vma(start, end, flags, type, backing_data, offset, size)` — add region, check overlap
- `find_vma(addr)` — find VMA containing addr (page fault hot path)
- `remove_vma(start, end)` — remove region (future: munmap)

Data structure: `RcuList<VmaRegion>` (linked list). Process VMA count < 20, linear scan sufficient.

### Demand Paging Flow

User page fault handler (`user_page_fault_handler`):

```
1. far_addr = faulting address (page-aligned)
2. Get current process's AddressSpace
3. vma = address_space->find_vma(far_addr)
4. if (!vma) → terminate process, return to scheduler
5. Permission check: write to read-only VMA → terminate
6. Allocate physical page: PageFrameAllocator::allocate_pages(0)
7. page_offset = far_addr_aligned - vma->start_addr
8. if (backing_data != nullptr && page_offset < backing_size):
     copy_size = min(PAGE_SIZE, backing_size - page_offset)
     memcpy(page, backing_data + backing_offset + page_offset, copy_size)
     memzero(page + copy_size, PAGE_SIZE - copy_size)
   else:
     memzero(page, PAGE_SIZE)
9. map_user_page(address_space, far_addr_aligned, phys, permissions)
10. return → CPU retries faulting instruction
```

### map_user_page

Operates on user PGD (not kernel PGD):

```cpp
VoidResult map_user_page(AddressSpace* as, VirtAddr va, PhysAddr pa, u64 perms) {
    PageTable* pgd = phys_to_virt(as->pgd_phys);
    // 4-level walk: PGD → PUD → PMD → PTE
    // Allocate intermediate tables from buddy allocator as needed
    // Final PTE: pa | perms | USER(AP[1]=1) | AF | nG | PXN | VALID
}
```

### User PTE Permission Bits (ARM64)

| Bit | Meaning | Setting |
|-----|---------|---------|
| AP[2:1] | Access Permission | `01`=EL0+EL1 RW, `11`=EL0+EL1 RO |
| UXN | User Execute Never | Code=0, Data/Stack/Heap=1 |
| PXN | Privileged Execute Never | Always 1 (kernel must not execute user pages) |
| AF | Access Flag | Always 1 (avoid access flag faults) |
| nG | non-Global | Always 1 (per-process, ASID-tagged) |
| SH | Shareability | `11`=Inner Shareable (SMP) |

## 5. ELF Loader Integration

### Revised Load Flow

```
load_elf_from_memory(elf_data, elf_size):
  1. validate_elf_header()
  2. create_user_address_space() → real PGD + ASID
  3. For each PT_LOAD segment:
       flags = segment_permissions_to_vma_flags(p_flags)
       add_vma(p_vaddr, p_vaddr + p_memsz, flags,
               backing_data = elf_data + p_offset,
               backing_size = p_filesz)
       // BSS: p_memsz > p_filesz handled by backing_size < VMA size
  4. add_vma(STACK_TOP - 8MB, STACK_TOP, RW | DEMAND_ZERO)
  5. add_vma(HEAP_START, HEAP_START + INITIAL, RW | DEMAND_ZERO)
  6. Create Process, set address_space
  7. create_thread(e_entry, stack_top, stack_size)
  // No physical pages allocated, no memcpy — all demand-paged
```

### End-to-End Execution

1. Thread enters scheduler → context_switch
2. TTBR0 written with process PGD + ASID
3. CPU jumps to `e_entry` (user VA, e.g. `0x400000`)
4. First instruction fetch → translation fault (no PTE)
5. Page fault handler: find code VMA → alloc page → copy ELF data → map → return
6. Instruction re-executes successfully
7. Stack access → fault → alloc zero page → map → continue
8. Program runs normally, each new page demand-faulted

## 6. Implementation Steps

| Step | Description | Files | Risk |
|------|-------------|-------|------|
| 1 | `phys_to_virt()` / `virt_to_phys()` helpers | mm.cppm, types.cppm | Low |
| 2 | Dynamic page table allocator (buddy-backed) | page_table.cpp, mm.cppm | Low |
| 3 | Linker script high-half VMA + AT(LMA) | linker.ld | Medium |
| 4 | Boot trampoline: TTBR1 kernel page table + jump | boot_impl.cpp | High |
| 5 | Kernel adaptation to high-half (pointer conversions) | page_table.cpp, mm.cppm, multiple | High |
| 6 | Per-process PGD allocation + AddressSpace real impl | process.cppm, process.cpp | Medium |
| 7 | TTBR0 switching in context_switch_to_task | process.cppm | Medium |
| 8 | `map_user_page()` with user PTE permissions | page_table.cpp, mmu_hal.cppm | Medium |
| 9 | VMA management (add_vma / find_vma) | process.cppm | Low |
| 10 | Page fault handler: VMA lookup + demand paging | page_fault.cpp | High |
| 11 | ELF loader: VMA registration (no memcpy) | elf_loader.cpp | Medium |
| 12 | End-to-end integration + 8-core QEMU verification | all | High |

## 7. Verification Criteria

| Signal | Expected |
|--------|----------|
| Kernel boot logs show high-half addresses | `kernel_main` at `0xFFFF8000_4xxx_xxxx` |
| TTBR1 ≠ TTBR0 | Different PGD physical addresses |
| SMP scheduling unaffected | 8 CPUs, test tasks running, context switches |
| ELF user program runs | "Hello from userspace!" output |
| Page faults resolve | Code/stack/heap faults logged and resolved |
| Process isolation | Two processes have different TTBR0 values |
| Invalid access → process killed | No VMA → process terminated, kernel continues |
| All 6 presets compile | ARM64/x86_64/RISC-V × debug/release |

## 8. Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Linker VMA change breaks boot assembly | `.text.boot` section linked at physical address, separate from high-half |
| Trampoline crash during jump | Keep identity map until confirmed high-half works |
| phys_to_virt refactoring scope | Incremental per-module conversion, compile after each |
| Page fault handler infinite loop | Recursion detection: fault-in-fault → panic |
| Dynamic page table OOM | Use `alloc_kernel_pages()` (UNMOVABLE), monitor free page count |
| ELF backing_data lifetime | Embedded ELF stays in kernel memory; future file-loaded ELF needs pinning |

## 9. Non-Goals (This Iteration)

- COW (Copy-on-Write) for fork
- mmap / munmap / mprotect syscalls
- Swap / page reclaim
- Huge pages (2MB/1GB) for user space
- NUMA-aware allocation
- High-half kernel for x86_64 / RISC-V (ARM64 only)
