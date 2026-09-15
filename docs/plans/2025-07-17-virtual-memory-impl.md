# Virtual Memory Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement Linux-style high-half kernel + per-process user address space with demand paging for MOSS on ARM64.

**Architecture:** Boot trampoline migrates kernel to `0xFFFF800000000000` via TTBR1. Per-process TTBR0 with ASID provides user-space isolation. Fully lazy demand paging via VMA-backed page fault handler. ELF loader registers VMAs without memcpy.

**Tech Stack:** C++26 modules, ARM64 4-level paging (48-bit VA), buddy allocator for page tables, QEMU virt platform.

---

## Task 1: Address Translation Helpers

**Files:**
- Modify: `src/core/src/types.cppm:39-42`

**Step 1: Add phys_to_virt / virt_to_phys helpers**

After line 42 (`constexpr VirtAddr USER_MAX = ...`), add:

```cpp
// Direct-map offset: physical RAM is mapped at KERNEL_BASE + phys_addr
constexpr VirtAddr KERNEL_DIRECT_MAP_BASE = KERNEL_BASE;  // 0xFFFF800000000000
constexpr PhysAddr PHYS_BASE = 0x40000000ULL;  // QEMU virt RAM start

// Convert between physical and virtual addresses (post-trampoline only)
inline VirtAddr phys_to_virt(PhysAddr pa) noexcept { return pa + KERNEL_DIRECT_MAP_BASE; }
inline PhysAddr virt_to_phys(VirtAddr va) noexcept { return va - KERNEL_DIRECT_MAP_BASE; }

// Check if an address is in the high-half kernel region
inline bool is_kernel_addr(VirtAddr va) noexcept { return va >= KERNEL_BASE; }
```

**Step 2: Build all 6 presets to verify no regressions**

Run: `uv run build.py`
Expected: All 6 presets compile without errors.

**Step 3: Commit**

```bash
git add src/core/src/types.cppm
git commit -m "[mm][types] add phys_to_virt/virt_to_phys address translation helpers"
```

---

## Task 2: Dynamic Page Table Allocator

**Files:**
- Modify: `src/mm/src/mm.cppm:553-564` (PageTableManager class)
- Modify: `src/mm/src/page_table.cpp:33-46` (allocate_page_table impl)

**Step 1: Add dynamic allocator method to PageTableManager**

In `mm.cppm`, add a new static method to PageTableManager (around line 583, after `get_kernel_pgd()`):

```cpp
// Dynamically allocate a page table from the buddy allocator (post-boot only).
// Returns a zeroed, page-aligned PageTable in the direct-mapped region.
[[nodiscard]] static KernelResult<PageTable*> allocate_page_table_dynamic();
```

Add a flag to track whether we've transitioned to dynamic allocation:

```cpp
static inline bool use_dynamic_alloc = false;
```

**Step 2: Implement in page_table.cpp**

Add after `allocate_page_table()` (line 46):

```cpp
KernelResult<PageTable*> PageTableManager::allocate_page_table_dynamic() {
    auto result = page_alloc::alloc_kernel_pages(0);  // order 0 = 1 page = 4KB
    if (!result) {
        return KernelResult<PageTable*>{ErrorCode::OutOfMemory};
    }
    PhysAddr pa = *result;
    auto* table = reinterpret_cast<PageTable*>(moss::kernel::phys_to_virt(pa));
    for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
        table->entries[i].clear();
    }
    return KernelResult<PageTable*>{table};
}
```

**Step 3: Update allocate_page_table to route dynamically when available**

Modify existing `allocate_page_table()` (line 33-46) to check the flag:

```cpp
KernelResult<PageTable*> PageTableManager::allocate_page_table() {
    if (use_dynamic_alloc) {
        return allocate_page_table_dynamic();
    }
    // Early boot path (bump allocator)
    if (next_table_index >= MAX_EARLY_TABLES) {
        return KernelResult<PageTable*>{ErrorCode::OutOfMemory};
    }
    PageTable* table = &early_tables[next_table_index++];
    for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
        table->entries[i].clear();
    }
    return KernelResult<PageTable*>{table};
}
```

**Step 4: Update get_physical_address to handle both identity and direct-map**

In `mm.cppm` line 562-564, change `get_physical_address`:

```cpp
[[nodiscard]] static PhysAddr get_physical_address(const PageTable* table) {
    auto va = reinterpret_cast<VirtAddr>(table);
    if (is_kernel_addr(va)) {
        return virt_to_phys(va);
    }
    return static_cast<PhysAddr>(va);  // identity-mapped (early boot)
}
```

Similarly update `get_table_from_physical` (line 565-567):

```cpp
[[nodiscard]] static PageTable* get_table_from_physical(PhysAddr pa) {
    if (use_dynamic_alloc) {
        return reinterpret_cast<PageTable*>(phys_to_virt(pa));
    }
    return reinterpret_cast<PageTable*>(pa);  // identity-mapped (early boot)
}
```

**Step 5: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets compile. No runtime changes yet (use_dynamic_alloc remains false).

**Step 6: Commit**

```bash
git add src/mm/src/mm.cppm src/mm/src/page_table.cpp
git commit -m "[mm][page_table] add dynamic buddy-backed page table allocator"
```

---

## Task 3: Linker Script High-Half VMA

**Files:**
- Modify: `linker.ld`

**Step 1: Rewrite linker script with dual VMA/LMA**

The key change: sections after `.text.boot` get high-half VMA (virtual memory address) but keep physical LMA via `AT()`. The `.text.boot` section stays at physical address for pre-trampoline execution.

```ld
/* ARM64内核链接器脚本 */
ENTRY(_start)

/* 定义内存布局 */
MEMORY
{
    /* QEMU virt平台的内存布局 */
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 256M
}

/* 内核虚拟地址基址 */
KERNEL_VMA = 0xFFFF800000000000;

SECTIONS
{
    . = ORIGIN(RAM);

    /* 启动代码段 - 必须在物理地址，pre-trampoline运行 */
    .text.boot : {
        _boot_start = .;
        *(.text.boot)
        _boot_end = .;
    } > RAM

    /* 记录物理地址位置，后续段将使用高半部分VMA */
    _phys_after_boot = .;

    /* === 高半部分内核段 (VMA = KERNEL_VMA + phys, LMA = phys) === */
    . = KERNEL_VMA + _phys_after_boot;

    /* 主代码段 */
    .text : AT(_phys_after_boot) ALIGN(4K) {
        _text_start = .;
        *(.text)
        *(.text.*)
        _text_end = .;
    }

    /* 只读数据段 */
    .rodata : AT(LOADADDR(.text) + SIZEOF(.text)) ALIGN(4K) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    }

    /* 数据段 */
    .data : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) ALIGN(4K) {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
    }

    /* BSS段（零初始化数据） */
    .bss : AT(LOADADDR(.data) + SIZEOF(.data)) ALIGN(4K) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* 栈空间 */
    .stack (NOLOAD) : ALIGN(4K) {
        _stack_bottom = .;
        . += 256K;
        _stack_top = .;
    }

    /* 内核堆空间标记 */
    .heap (NOLOAD) : ALIGN(4K) {
        _heap_start = .;
        . += 4K;
        _heap_end = .;
    }

    /* 页表空间（早期启动使用） */
    .pagetables (NOLOAD) : ALIGN(4K) {
        _pagetable_start = .;
        . += 64K;
        _pagetable_end = .;
    }

    /* 内核结束标记 */
    _kernel_end = .;

    /* Physical address of kernel end (for buddy allocator) */
    _kernel_phys_end = _kernel_end - KERNEL_VMA;

    /* Linux Image header uses this */
    _kernel_image_size = _kernel_phys_end - ORIGIN(RAM);

    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}

/* 导出符号 - 这些现在是高半部分虚拟地址 */
PROVIDE(_text_start_addr = _text_start);
PROVIDE(_text_end_addr = _text_end);
PROVIDE(_rodata_start_addr = _rodata_start);
PROVIDE(_rodata_end_addr = _rodata_end);
PROVIDE(_data_start_addr = _data_start);
PROVIDE(_data_end_addr = _data_end);
PROVIDE(_bss_start_addr = _bss_start);
PROVIDE(_bss_end_addr = _bss_end);
PROVIDE(_stack_bottom_addr = _stack_bottom);
PROVIDE(_stack_top_addr = _stack_top);
PROVIDE(_heap_start_addr = _heap_start);
PROVIDE(_heap_end_addr = _heap_end);
PROVIDE(_pagetable_start_addr = _pagetable_start);
PROVIDE(_pagetable_end_addr = _pagetable_end);
PROVIDE(_kernel_end_addr = _kernel_end);
/* Physical address exports for boot trampoline */
PROVIDE(_kernel_phys_end_addr = _kernel_phys_end);
PROVIDE(_boot_start_addr = _boot_start);
PROVIDE(_boot_end_addr = _boot_end);
```

**Important:** This is the highest-risk step. The `.text.boot` section stays at physical address while everything else moves to high-half VMA. The `AT()` directives ensure the loader still places code at physical addresses.

**Step 2: Build ARM64 debug preset to test**

Run: `cmake --workflow --preset arm64-qemu-debug`
Expected: Build succeeds. Kernel may not boot yet (trampoline not implemented).

**Step 3: Verify with objdump that symbols are at high-half addresses**

Run: `llvm-objdump -t build/arm64-qemu-debug/bin/moss.elf | grep _text_start`
Expected: `_text_start` at `0xFFFF8000_4000_xxxx`

**Step 4: Commit**

```bash
git add linker.ld
git commit -m "[mm][linker] migrate kernel sections to high-half VMA with AT(LMA)"
```

---

## Task 4: Boot Trampoline

**Files:**
- Modify: `src/boot/src/arch/arm64/boot_impl.cpp:605-633` (setup_memory_management)
- Modify: `src/mm/src/page_table.cpp:59-109` (setup_kernel_page_tables)

This is the highest-risk task. The trampoline must:
1. Build high-half kernel page table for TTBR1
2. Keep identity mapping in TTBR0 (for trampoline code)
3. Switch to high-half
4. Clear TTBR0 identity map

**Step 1: Create high-half kernel page table builder**

Add new method to PageTableManager in `mm.cppm` and implement in `page_table.cpp`:

```cpp
// Build the TTBR1 kernel page table: map physical RAM at KERNEL_DIRECT_MAP_BASE
[[nodiscard]] static VoidResult setup_kernel_high_half_tables();
static inline PageTable* kernel_high_pgd = nullptr;
```

Implementation in `page_table.cpp`:

```cpp
VoidResult PageTableManager::setup_kernel_high_half_tables() {
    // 1. Allocate L0 (PGD) for TTBR1
    auto pgd_result = allocate_page_table();
    if (!pgd_result) return VoidResult{pgd_result.error()};
    kernel_high_pgd = *pgd_result;

    // 2. Determine RAM region
    const auto& plat = moss::fdt::get_platform_info();
    PhysAddr ram_start = (plat.dtb_valid && plat.total_memory_size > 0)
                             ? plat.total_memory_start
                             : moss::kernel::platform::ram_base();
    u64 ram_size = (plat.dtb_valid && plat.total_memory_size > 0)
                       ? plat.total_memory_size
                       : moss::kernel::platform::ram_size();

    // 3. Map physical RAM at KERNEL_DIRECT_MAP_BASE using 1GB blocks
    // KERNEL_DIRECT_MAP_BASE = 0xFFFF800000000000
    // PGD index for 0xFFFF800000000000 with T1SZ=16: bits[47:39] = 0x100 = 256
    // But with TTBR1, the top bit is ignored for indexing.
    // VA range for TTBR1: 0xFFFF000000000000 - 0xFFFFFFFFFFFFFFFF
    // With T1SZ=16: effective VA = 48 bits, index = (va >> 39) & 0x1FF
    // For 0xFFFF800000000000: PGD[256]

    constexpr u64 ONE_GB = 0x40000000ULL;
    u64 num_gb = (ram_size + ONE_GB - 1) / ONE_GB;

    // For QEMU virt with 256MB RAM at 0x40000000:
    // We need to map at least 0x00000000 - 0xFFFFFFFF (4GB) to cover
    // device MMIO + RAM, same as identity map.

    // Calculate PGD index for KERNEL_DIRECT_MAP_BASE in TTBR1 space
    // TTBR1 handles VA >= 0xFFFF000000000000 (with T1SZ=16)
    // PGD index = (va >> 39) & 0x1FF
    u16 pgd_idx = (KERNEL_DIRECT_MAP_BASE >> 39) & 0x1FF;

    // Allocate PUD table for this PGD entry
    auto pud_result = allocate_page_table();
    if (!pud_result) return VoidResult{pud_result.error()};
    PageTable* pud = *pud_result;

    PhysAddr pud_pa = get_physical_address(pud);
    kernel_high_pgd->entries[pgd_idx].set_table(pud_pa);

    // Fill PUD entries [0..3] with 1GB blocks covering 0-4GB
    PhysAddr ram_end = ram_start + ram_size;
    for (usize i = 0; i < 4; i++) {
        PhysAddr block_addr = static_cast<PhysAddr>(i * ONE_GB);
        PhysAddr block_end = block_addr + ONE_GB;
        bool overlaps_ram = (block_addr < ram_end) && (block_end > ram_start);
        u64 block_entry = overlaps_ram
            ? moss::kernel::hal::mmu::make_normal_block(block_addr)
            : moss::kernel::hal::mmu::make_device_block(block_addr);
        pud->entries[i].raw = block_entry;
    }

    return VoidResult{};
}
```

**Step 2: Implement trampoline in setup_memory_management**

Rewrite `setup_memory_management()` in `boot_impl.cpp` (line 605-633):

```cpp
::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_memory_management(BootContext &ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);
    early_print("=== ARM64 Memory Management Setup ===\n");

    // Phase 1: Setup identity-mapped page tables (existing code)
    auto mmu_result = ::moss::kernel::mm::setup_mmu();
    if (!mmu_result) {
        early_print("MMU setup failed\n");
        return ::moss::kernel::VoidResult{mmu_result.error()};
    }

    // Phase 2: Initialize PageFrameAllocator
    auto pfa_result = ::moss::kernel::mm::PageFrameAllocator::initialize();
    if (!pfa_result) {
        early_print("Physical page allocator init failed\n");
        return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
    }

    // Phase 3: Build high-half kernel page table
    early_print("Building high-half kernel page table...\n");
    auto high_result = ::moss::kernel::mm::PageTableManager::setup_kernel_high_half_tables();
    if (!high_result) {
        early_print("High-half page table setup failed\n");
        return ::moss::kernel::VoidResult{high_result.error()};
    }

    // Phase 4: Boot trampoline — switch to high-half
    early_print("Boot trampoline: switching to high-half kernel...\n");
    PhysAddr high_pgd_pa = ::moss::kernel::mm::PageTableManager::get_physical_address(
        ::moss::kernel::mm::PageTableManager::get_kernel_high_pgd());

    // Write TTBR1 with kernel high-half PGD (ASID=0 for kernel)
    asm volatile("msr ttbr1_el1, %0" :: "r"(high_pgd_pa));
    asm volatile("isb" ::: "memory");

    // Full TLB invalidation
    asm volatile("tlbi vmalle1" ::: "memory");
    asm volatile("dsb sy" ::: "memory");
    asm volatile("isb" ::: "memory");

    // At this point, TTBR0 still has identity map (for our current PC),
    // and TTBR1 has high-half map. Both map the same physical memory,
    // so we can access data via either mapping.

    // Phase 5: Mark dynamic allocation as active
    ::moss::kernel::mm::PageTableManager::enable_dynamic_alloc();

    // Phase 6: Initialize runtime heap (using high-half addresses now)
    // Note: _heap_start_addr is now a high-half address from the linker
    VirtAddr heap_start = reinterpret_cast<VirtAddr>(_heap_start_addr);
    ::moss::kernel::usize initial_heap_size = 256 * 1024;
    auto heap_result = ::moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(
        heap_start, initial_heap_size);
    if (!heap_result) {
        early_print("Runtime heap allocator init failed\n");
        return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
    }

    early_print("ARM64 memory management setup complete (high-half active)\n\n");
    return ::moss::kernel::VoidResult{};
}
```

**Step 3: Add accessor and enable method to PageTableManager**

In `mm.cppm`, add to PageTableManager:

```cpp
[[nodiscard]] static PageTable* get_kernel_high_pgd() { return kernel_high_pgd; }
static void enable_dynamic_alloc() { use_dynamic_alloc = true; }
```

**Step 4: Build ARM64 debug preset**

Run: `cmake --workflow --preset arm64-qemu-debug`
Expected: Compiles. May need iteration on linker symbol access patterns.

**Step 5: Test with QEMU**

Run: `./build/arm64-qemu-debug/run_qemu.sh`
Expected: Kernel boots and prints high-half addresses in logs.

**Step 6: Commit**

```bash
git add src/boot/src/arch/arm64/boot_impl.cpp src/mm/src/mm.cppm src/mm/src/page_table.cpp
git commit -m "[mm][boot] implement boot trampoline for high-half kernel migration"
```

---

## Task 5: Kernel Adaptation to High-Half

**Files:**
- Modify: `src/mm/src/page_table.cpp` (page table walk uses reinterpret_cast on PA)
- Modify: `src/boot/src/arch/arm64/boot_impl.cpp` (hardcoded UART `0x9000000`)
- Potentially others using raw physical addresses

**Step 1: Fix page table walk to use get_table_from_physical()**

In `page_table.cpp`, the `map_page()` method (line 136-197) uses:
```cpp
current_table = reinterpret_cast<PageTable*>(pud_pa);  // WRONG after high-half
```

Change all such lines to:
```cpp
current_table = PageTableManager::get_table_from_physical(pud_pa);
```

Apply this to `map_page()` (3 places: lines 160, 175, 190), `unmap_page()` (lines 404, 414, 424), `query_page()` (lines 464, 479, 494), and `print_pgd_entries()` (line 305).

**Step 2: Fix UART base addresses in boot_impl.cpp**

Hardcoded `0x9000000` UART addresses in early boot code will still work via identity-mapped TTBR0. After clearing TTBR0, they need to use `phys_to_virt(0x09000000)`. However, since UART is accessed before and during the trampoline, we keep identity map accessible until all CPUs are booted.

For `secondary_cpu_entry()` and other post-trampoline code, update UART accesses:
```cpp
volatile u8 *uart_out = reinterpret_cast<volatile u8 *>(
    moss::kernel::phys_to_virt(0x09000000));
```

**Step 3: Build all 6 presets**

Run: `uv run build.py`
Expected: All compile. x64 and RISC-V 64 unchanged (phys_to_virt/virt_to_phys are no-ops for now).

**Step 4: QEMU test**

Run: `./build/arm64-qemu-debug/run_qemu.sh`
Expected: Kernel boots with high-half addresses, all CPUs online, scheduler running.

**Step 5: Commit**

```bash
git add -A
git commit -m "[mm][kernel] adapt page table walks and MMIO to high-half addressing"
```

---

## Task 6: Per-Process PGD + AddressSpace

**Files:**
- Modify: `src/process/src/process.cppm:180-222` (VmaRegion, AddressSpace)
- Modify: `src/process/src/process.cpp:257-287` (create_user_address_space, etc.)

**Step 1: Enhance VmaRegion with backing data support**

In `process.cppm` (around line 189), replace VmaRegion:

```cpp
enum class VmaType : u8 {
    CODE = 0, DATA = 1, STACK = 2, HEAP = 3, BSS = 4
};

struct VmaRegion {
    moss::kernel::VirtAddr start_addr;
    moss::kernel::VirtAddr end_addr;
    moss::kernel::u32 flags;
    VmaType type;

    // Lazy backing: ELF segment data source
    const u8* backing_data;     // ELF data in kernel memory (nullptr = zero page)
    usize backing_offset;       // Offset into backing_data for VMA start
    usize backing_size;         // Valid backing data length

    VmaRegion(moss::kernel::VirtAddr start, moss::kernel::VirtAddr end,
              moss::kernel::u32 region_flags, VmaType vma_type = VmaType::DATA,
              const u8* data = nullptr, usize offset = 0, usize size = 0) noexcept
        : start_addr(start), end_addr(end), flags(region_flags), type(vma_type),
          backing_data(data), backing_offset(offset), backing_size(size) {}

    [[nodiscard]] bool is_demand_zero() const noexcept {
        return backing_data == nullptr;
    }
    [[nodiscard]] bool contains(VirtAddr addr) const noexcept {
        return addr >= start_addr && addr < end_addr;
    }
};
```

**Step 2: Enhance AddressSpace with real PGD allocation**

```cpp
struct AddressSpace {
    PhysAddr pgd_phys;
    u16 asid;
    containers::RcuList<VmaRegion> vma_list;
    containers::AtomicSize total_pages;
    containers::AtomicSize resident_pages;

    AddressSpace(PhysAddr pgd, u16 asid_val) noexcept
        : pgd_phys(pgd), asid(asid_val), total_pages(0), resident_pages(0) {}

    // VMA operations
    VoidResult add_vma(VirtAddr start, VirtAddr end, u32 flags,
                       VmaType type = VmaType::DATA,
                       const u8* backing = nullptr,
                       usize backing_off = 0, usize backing_sz = 0) noexcept;
    VmaRegion* find_vma(VirtAddr addr) noexcept;
};
```

**Step 3: Implement real create_user_address_space in process.cpp**

Replace the stub (line 257-270):

```cpp
// ASID counter
static u16 next_asid = 1;

KernelResult<unique_ptr<AddressSpace>> create_user_address_space() noexcept {
    // Allocate PGD for user space (all entries zeroed = no mappings)
    auto pgd_result = mm::PageTableManager::allocate_page_table();
    if (!pgd_result) {
        return KernelResult<unique_ptr<AddressSpace>>{ErrorCode::OutOfMemory};
    }
    PhysAddr pgd_pa = mm::PageTableManager::get_physical_address(*pgd_result);

    // Assign ASID (wrap with TLB flush at 256)
    u16 asid = next_asid++;
    if (next_asid > 255) {
        moss::kernel::arch::flush_tlb();
        next_asid = 1;
    }

    auto as = make_unique<AddressSpace>(pgd_pa, asid);
    if (!as) {
        return KernelResult<unique_ptr<AddressSpace>>{ErrorCode::OutOfMemory};
    }
    return KernelResult<unique_ptr<AddressSpace>>{moss::move(as)};
}
```

**Step 4: Implement add_vma and find_vma**

```cpp
VoidResult AddressSpace::add_vma(VirtAddr start, VirtAddr end, u32 flags,
                                 VmaType type, const u8* backing,
                                 usize backing_off, usize backing_sz) noexcept {
    VmaRegion region(start, end, flags, type, backing, backing_off, backing_sz);
    vma_list.push_back(region);
    return VoidResult{};
}

VmaRegion* AddressSpace::find_vma(VirtAddr addr) noexcept {
    for (auto& vma : vma_list) {
        if (vma.contains(addr)) return &vma;
    }
    return nullptr;
}
```

**Step 5: Build all 6 presets**

Run: `uv run build.py`

**Step 6: Commit**

```bash
git add src/process/src/process.cppm src/process/src/process.cpp
git commit -m "[process][mm] implement real per-process PGD allocation with ASID"
```

---

## Task 7: TTBR0 Switching in Context Switch

**Files:**
- Modify: `src/process/src/process.cppm` (context_switch_to_task, ~line 1807)

**Step 1: Add TTBR0 write before context_switch**

In `context_switch_to_task()`, before the actual `context_switch()` call, add:

```cpp
// Switch user address space: write TTBR0 with new process's PGD + ASID
auto* next_proc = /* get process owning task */;
if (next_proc && next_proc->get_address_space()) {
    auto* as = next_proc->get_address_space();
    u64 ttbr0_val = as->pgd_phys | (static_cast<u64>(as->asid) << 48);
    asm volatile("msr ttbr0_el1, %0; isb" :: "r"(ttbr0_val) : "memory");
}
// Kernel threads: lazy TTBR0 — keep previous TTBR0 (harmless)
```

**Step 2: Build and test**

Run: `uv run build.py && ./build/arm64-qemu-debug/run_qemu.sh`

**Step 3: Commit**

```bash
git add src/process/src/process.cppm
git commit -m "[process][mm] add TTBR0 switching with ASID on context switch"
```

---

## Task 8: map_user_page with User PTE Permissions

**Files:**
- Modify: `src/mm/src/mm.cppm` (add map_user_page to PageTableManager)
- Modify: `src/mm/src/page_table.cpp` (implement)
- Modify: `src/hal/mmu/src/mmu_hal.cppm` (add user page PTE presets)

**Step 1: Add user PTE permission presets in mmu_hal.cppm**

After existing PagePerms (line 127-135), enhance user permissions:

```cpp
// User page permissions with proper nG, PXN, AF, SH bits
inline constexpr u64 USER_RO_PAGE = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                     PageAttr::READONLY | PageAttr::ATTR_NORMAL |
                                     PageAttr::NG | PageAttr::PXN | (3ULL << 8);
inline constexpr u64 USER_RW_PAGE = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                     PageAttr::ATTR_NORMAL | PageAttr::NG |
                                     PageAttr::PXN | PageAttr::XN | (3ULL << 8);
inline constexpr u64 USER_RX_PAGE = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                     PageAttr::READONLY | PageAttr::ATTR_NORMAL |
                                     PageAttr::NG | PageAttr::PXN | (3ULL << 8);
```

**Step 2: Implement map_user_page in page_table.cpp**

```cpp
VoidResult PageTableManager::map_user_page(PhysAddr pgd_phys, VirtAddr va,
                                            PhysAddr pa, u64 perms) {
    auto* pgd = get_table_from_physical(pgd_phys);
    auto bd = break_virtual_address(va);

    // 4-level walk with on-demand intermediate table allocation
    // PGD → PUD
    if (!pgd->entries[bd.pgd_index].is_valid()) {
        auto result = allocate_page_table();
        if (!result) return VoidResult{ErrorCode::OutOfMemory};
        pgd->entries[bd.pgd_index].set_table(get_physical_address(*result));
    }
    auto* pud = get_table_from_physical(pgd->entries[bd.pgd_index].get_phys_addr());

    // PUD → PMD
    if (!pud->entries[bd.pud_index].is_valid()) {
        auto result = allocate_page_table();
        if (!result) return VoidResult{ErrorCode::OutOfMemory};
        pud->entries[bd.pud_index].set_table(get_physical_address(*result));
    }
    auto* pmd = get_table_from_physical(pud->entries[bd.pud_index].get_phys_addr());

    // PMD → PTE table
    if (!pmd->entries[bd.pmd_index].is_valid()) {
        auto result = allocate_page_table();
        if (!result) return VoidResult{ErrorCode::OutOfMemory};
        pmd->entries[bd.pmd_index].set_table(get_physical_address(*result));
    }
    auto* pte = get_table_from_physical(pmd->entries[bd.pmd_index].get_phys_addr());

    // Set the final page entry
    pte->entries[bd.pte_index].set_block(pa, perms);
    return VoidResult{};
}
```

**Step 3: Build all 6 presets**

**Step 4: Commit**

```bash
git add src/mm/src/mm.cppm src/mm/src/page_table.cpp src/hal/mmu/src/mmu_hal.cppm
git commit -m "[mm][page_table] implement map_user_page with ARM64 user PTE permissions"
```

---

## Task 9: VMA Management

Already implemented in Task 6 (`add_vma`, `find_vma`). This step adds the permission-to-PTE conversion:

**Files:**
- Modify: `src/process/src/process.cppm` or `process.cpp`

**Step 1: Add VMA flags to PTE permission converter**

```cpp
u64 vma_flags_to_pte_perms(u32 vma_flags) noexcept {
    using namespace moss::kernel::hal::mmu;
    if ((vma_flags & VmaFlags::EXEC) && !(vma_flags & VmaFlags::WRITE))
        return PagePerms::USER_RX_PAGE;
    if (vma_flags & VmaFlags::WRITE)
        return PagePerms::USER_RW_PAGE;
    return PagePerms::USER_RO_PAGE;
}
```

**Step 2: Commit**

```bash
git add src/process/src/process.cppm
git commit -m "[process][mm] add VMA flags to PTE permission converter"
```

---

## Task 10: Page Fault Handler — Demand Paging

**Files:**
- Modify: `src/mm/src/page_fault.cpp:223-264` (user_page_fault_handler)

**Step 1: Replace stub with demand paging implementation**

```cpp
extern "C" [[noreturn]] void user_page_fault_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept {
    namespace log = moss::kernel::logging;
    namespace mm  = moss::kernel::mm;
    namespace proc = moss::kernel::process;
    using moss::u64;

    u64 ec   = (esr >> 26) & 0x3F;
    u64 dfsc = esr & 0x3F;
    bool is_write = ((esr >> 6) & 1) != 0;
    bool is_translation_fault = (dfsc >= 0x04 && dfsc <= 0x07);

    log::klog::debug("user page fault: addr={:#x} pc={:#x} write={} dfsc={:#x}",
                     far_addr, elr, is_write, dfsc);

    if (!is_translation_fault) {
        // Permission fault or other — kill process
        log::klog::error("USER PERMISSION FAULT: addr={:#x} — killing process", far_addr);
        goto kill_process;
    }

    {
        // 1. Get current process's AddressSpace
        auto* thread = proc::CfsScheduler::get_current_thread();
        if (!thread) goto kill_process;
        auto* process = proc::ProcessManager::find_process(thread->owner_pid);
        if (!process || !process->get_address_space()) goto kill_process;
        auto* as = process->get_address_space();

        // 2. Find VMA containing faulting address
        moss::kernel::VirtAddr fault_page = far_addr & ~(0xFFFULL);
        auto* vma = as->find_vma(fault_page);
        if (!vma) {
            log::klog::error("USER SEGFAULT: addr={:#x} not in any VMA", far_addr);
            goto kill_process;
        }

        // 3. Permission check: write to read-only VMA
        if (is_write && !(vma->flags & proc::VmaFlags::WRITE)) {
            log::klog::error("USER WRITE VIOLATION: addr={:#x}", far_addr);
            goto kill_process;
        }

        // 4. Allocate physical page
        auto page_result = mm::page_alloc::alloc_user_pages(0);
        if (!page_result) {
            log::klog::error("OOM during demand page: addr={:#x}", far_addr);
            goto kill_process;
        }
        moss::kernel::PhysAddr new_page_pa = *page_result;
        auto* page_ptr = reinterpret_cast<moss::u8*>(
            moss::kernel::phys_to_virt(new_page_pa));

        // 5. Fill page content
        usize page_offset = fault_page - vma->start_addr;
        if (vma->backing_data != nullptr && page_offset < vma->backing_size) {
            usize copy_size = vma->backing_size - page_offset;
            if (copy_size > moss::kernel::PAGE_SIZE)
                copy_size = moss::kernel::PAGE_SIZE;
            const moss::u8* src = vma->backing_data + vma->backing_offset + page_offset;
            for (usize i = 0; i < copy_size; i++) page_ptr[i] = src[i];
            for (usize i = copy_size; i < moss::kernel::PAGE_SIZE; i++) page_ptr[i] = 0;
        } else {
            for (usize i = 0; i < moss::kernel::PAGE_SIZE; i++) page_ptr[i] = 0;
        }

        // 6. Map the page into user page table
        u64 perms = proc::vma_flags_to_pte_perms(vma->flags);
        auto map_result = mm::PageTableManager::map_user_page(
            as->pgd_phys, fault_page, new_page_pa, perms);
        if (!map_result) {
            log::klog::error("Failed to map user page: addr={:#x}", fault_page);
            goto kill_process;
        }

        as->resident_pages.fetch_add(1);
        log::klog::debug("demand page: mapped va={:#x} -> pa={:#x}", fault_page, new_page_pa);

        // 7. Return — CPU retries faulting instruction
        // The [[noreturn]] attribute must be removed for this to work,
        // or we must use eret explicitly
        return;  // eret via exception return path in asm
    }

kill_process:
    log::klog::error("Terminating user process (SIGSEGV)");
    // TODO: proper process termination via scheduler
    while (true) {
#if defined(MOSS_ARCH_ARM64)
        asm volatile("wfi");
#endif
    }
}
```

**Note:** The `[[noreturn]]` attribute on `user_page_fault_handler` must be removed in the declaration (both in page_fault.cpp's global fragment at line 25 and in start_arm64.S dispatch). After demand paging, the handler CAN return (eret back to user space).

**Step 2: Build and test**

**Step 3: Commit**

```bash
git add src/mm/src/page_fault.cpp
git commit -m "[mm][page_fault] implement demand paging in user page fault handler"
```

---

## Task 11: ELF Loader VMA Registration

**Files:**
- Modify: `src/kernel/src/elf_loader.cpp` (complete rewrite of load flow)

**Step 1: Rewrite load_elf_from_memory for VMA-based lazy loading**

Replace the current `allocate_user_address_space` / `map_elf_segments` / `setup_user_stack` flow with:

```cpp
Result<LoadedProgram> ElfLoader::load_elf_from_memory(
    const u8* elf_data, usize elf_size) noexcept {
    // ... validate header, check arch (same as before) ...

    // Create real user address space with PGD + ASID
    auto as_result = process::user_space::create_user_address_space();
    if (!as_result) return Result<LoadedProgram>{ErrorCode::OutOfMemory};
    auto address_space = moss::move(*as_result);

    // Register VMAs for each PT_LOAD segment (NO memcpy!)
    const auto* header = reinterpret_cast<const ElfHeader*>(elf_data);
    const auto* phdrs = reinterpret_cast<const ProgramHeader*>(
        elf_data + header->e_phoff);

    for (u16 i = 0; i < header->e_phnum; ++i) {
        const auto& phdr = phdrs[i];
        if (phdr.p_type != PT_LOAD) continue;

        u32 flags = elf_flags_to_memory_flags(phdr.p_flags);
        VmaType type = (flags & 0x4) ? VmaType::CODE : VmaType::DATA;
        VirtAddr seg_start = phdr.p_vaddr & ~(PAGE_SIZE - 1);
        VirtAddr seg_end = (phdr.p_vaddr + phdr.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        address_space->add_vma(seg_start, seg_end, flags, type,
                               elf_data, phdr.p_offset, phdr.p_filesz);
    }

    // Register stack VMA (8MB, demand-zero)
    constexpr VirtAddr USER_STACK_TOP = 0x00007FFF00000000ULL;
    constexpr usize STACK_SIZE = 8 * 1024 * 1024;
    address_space->add_vma(USER_STACK_TOP - STACK_SIZE, USER_STACK_TOP,
                           VmaFlags::READ | VmaFlags::WRITE | VmaFlags::DEMAND_ZERO,
                           VmaType::STACK);

    // Register heap VMA (initial 4KB, demand-zero)
    constexpr VirtAddr HEAP_START = 0x0000000100000000ULL;
    address_space->add_vma(HEAP_START, HEAP_START + PAGE_SIZE,
                           VmaFlags::READ | VmaFlags::WRITE | VmaFlags::DEMAND_ZERO,
                           VmaType::HEAP);

    LoadedProgram program = {
        .entry_point = header->e_entry,
        .base_address = 0,
        .stack_top = USER_STACK_TOP,
        .heap_start = HEAP_START,
        .total_size = elf_size,
        .load_segments = header->e_phnum
    };
    // Store address_space into the process (via caller)

    return Result<LoadedProgram>{program};
}
```

**Step 2: Build and test**

**Step 3: Commit**

```bash
git add src/kernel/src/elf_loader.cpp
git commit -m "[kernel][elf] rewrite ELF loader to register VMAs without memcpy"
```

---

## Task 12: End-to-End Integration + QEMU Verification

**Step 1: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets compile without errors.

**Step 2: ARM64 QEMU boot verification**

Run: `./build/arm64-qemu-debug/run_qemu.sh`

Check for:
- Kernel boot logs show high-half addresses (`kernel_main` at `0xFFFF8000_4xxx_xxxx`)
- TTBR1 ≠ TTBR0 in MMU register dump
- SMP scheduling unaffected (8 CPUs, context switches)
- ELF user program runs ("Hello from userspace!")
- Page faults resolve (demand paging logs visible)
- Invalid access → process killed (kernel continues)

**Step 3: Fix any issues found during verification**

This step is inherently iterative. Common issues:
- TLB invalidation missed somewhere
- Page table walk hitting stale identity-mapped addresses
- ELF backing_data pointer lifetime issue
- Stack/heap VMA size too small

**Step 4: Final commit**

```bash
git add -A
git commit -m "[mm][vm] complete high-half kernel + per-process virtual memory integration"
```

---

## Verification Criteria

| Signal | Expected |
|--------|----------|
| Kernel boot at high-half | `kernel_main` at `0xFFFF8000_4xxx_xxxx` |
| TTBR1 ≠ TTBR0 | Different PGD physical addresses |
| SMP scheduling works | 8 CPUs, test tasks running |
| ELF program runs | "Hello from userspace!" |
| Demand paging works | `demand page: mapped va=...` logs |
| Process isolation | Two processes have different TTBR0 |
| Invalid access kills process | No VMA → process terminated, kernel continues |
| All 6 presets compile | ARM64/x64/RISC-V 64 × debug/release |

## Risk Summary

| Risk | Mitigation |
|------|------------|
| Linker VMA change breaks boot asm | `.text.boot` stays at physical address |
| Trampoline crash | Keep identity map in TTBR0 until confirmed |
| phys_to_virt scope | Incremental per-module conversion |
| Page fault infinite loop | Recursion detection in handler |
| Dynamic page table OOM | Use UNMOVABLE migration type |
| ELF backing_data lifetime | Embedded ELF stays in kernel memory |
