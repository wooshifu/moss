# Minimal Kernel-Mode Page Fault Handling — Design

## Goal

Add page fault handling to MOSS kernel: rewrite ARM64 exception vector dispatch to read ESR_EL1/FAR_EL1, route Data Abort / Instruction Abort to a C++ handler, implement demand-zero page allocation, and add `unmap_page` / `query_page` / TLB invalidation to the MM subsystem. All unhandled exceptions print detailed diagnostic info before halting.

## Scope

- ✅ ARM64 exception vector table rework (read ESR_EL1, FAR_EL1, dispatch by EC)
- ✅ Unhandled exception diagnostic printing (EC name, ISS, FAR, ELR)
- ✅ Kernel-mode Data Abort (EC=0x25) and Instruction Abort (EC=0x21) handlers
- ✅ Demand-zero page mapping (VMA_DEMAND_ZERO flag, allocate + zero + map on fault)
- ✅ `unmap_page()`, `query_page()`, `tlb_invalidate()` additions to MM
- ✅ VMA flags extension (VMA_DEMAND_ZERO)
- ✅ Page fault statistics (Process::record_page_fault)
- ❌ User-mode page faults (TTBR0/TTBR1 split — future)
- ❌ COW (copy-on-write — future)
- ❌ Swap / page-out (future)

## Architecture

```
Exception Vector Table (start_arm64.S)
  │
  ├─ EC=0x25 (Data Abort, same EL) ──→ kernel_page_fault_handler() [C++]
  ├─ EC=0x21 (Instr Abort, same EL) ─→ kernel_page_fault_handler() [C++]
  └─ Other EC ─────────────────────→ unhandled_exception_handler() [C++] → halt
                                        (prints EC, ISS, FAR, ELR)
```

```
kernel_page_fault_handler(esr, far, elr)
  │
  ├─ Translation fault (DFSC 0x04-0x07)
  │   ├─ VMA found + DEMAND_ZERO → handle_demand_zero() → eret (retry)
  │   └─ No VMA → panic
  │
  ├─ Permission fault (DFSC 0x0C-0x0F) → panic (COW future)
  └─ Other DFSC → panic
```

## Components

### 1. Exception Vector Table (start_arm64.S)

Modify `exception_handler` label to:
- Save x0-x30, ELR_EL1, SPSR_EL1 to stack
- Read ESR_EL1 → x0, FAR_EL1 → x1, ELR_EL1 → x2
- Extract EC = ESR[31:26]
- Branch to `kernel_page_fault_handler` for EC=0x25/0x21
- Branch to `unhandled_exception_handler` + halt for others
- Restore registers + `eret` on successful page fault handling

### 2. Unhandled Exception Handler (page_fault.cpp)

`extern "C" void unhandled_exception_handler(u64 esr, u64 far, u64 elr)`
- Decode EC field to human-readable string (15+ exception types)
- Print: EC name, raw ESR, ISS, FAR, ELR via klog::panic
- Returns to asm which then halts

### 3. Page Fault Handler (page_fault.cpp)

`extern "C" void kernel_page_fault_handler(u64 esr, u64 far, u64 elr)`
- Extract DFSC (esr & 0x3F) and WnR bit (esr >> 6 & 1)
- Translation fault → look up VMA → demand zero or panic
- Permission fault → panic (future: COW)
- Log all faults via klog

### 4. Demand Zero (page_fault.cpp)

`void handle_demand_zero(u64 fault_addr, const VmaRegion* vma)`
- Align fault_addr to 4K page boundary
- Allocate physical page from buddy allocator
- Zero the page via memset
- Call map_page with VMA permissions
- Log mapping via klog::debug

### 5. Page Table Extensions (mm.cppm)

- `unmap_page(VirtAddr)` — 4-level walk, clear PTE, TLB invalidate
- `query_page(VirtAddr) → PageInfo` — 4-level walk, return phys + attrs + level
- `tlb_invalidate(VirtAddr)` — TLBI vale1is + barriers
- `tlb_invalidate_all()` — TLBI vmalle1is + barriers

### 6. VMA Flags Extension (process.cppm)

- Add `VMA_DEMAND_ZERO = 1 << 3` to VmaFlags
- Add `is_demand_zero()` helper
- Add `find_vma_for_address()` lookup function

## Files

| File | Action | Content |
|------|--------|---------|
| `src/boot/src/arch/arm64/start_arm64.S` | Modify | Exception vector rework (~60 lines) |
| `src/mm/src/page_fault.cpp` | Create | Fault handlers + demand zero (~180 lines) |
| `src/mm/src/mm.cppm` | Modify | unmap_page, query_page, TLB ops (~130 lines) |
| `src/process/src/process.cppm` | Modify | VMA flags, find_vma helper (~15 lines) |
| `src/mm/CMakeLists.txt` | Modify | Add page_fault.cpp |

Estimated total: ~385 new/modified lines.
