// =============================================================================
// x86_64 Kernel Boot Entry
// =============================================================================
//
// Bootstrap for x86_64 MOSS kernel in long mode (64-bit).
//
// Boot protocol: PVH (Xen PVH) via ELF note.
// QEMU's -kernel option detects the XEN_ELFNOTE_PHYS32_ENTRY note and
// enters the kernel directly in 32-bit protected mode with paging disabled.
// The PVH entry point (_pvh_start) then sets up 64-bit long mode and
// jumps to the 64-bit _start entry.
//
// This stub:
//   1. Transitions from 32-bit protected mode to 64-bit long mode (PVH entry)
//   2. Clears direction flag (CLD) for safe REP string operations
//   3. Sets up a temporary 8 KB kernel stack
//   4. Zeroes the BSS section
//   5. Loads a minimal GDT with kernel/user code+data segments
//   6. Calls early_main() to enter the unified boot path
//
// Entry point: _start (64-bit), _pvh_start (32-bit PVH entry)
// =============================================================================

// =============================================================================
// PVH ELF Note — tells QEMU to boot directly via PVH protocol
// =============================================================================
// XEN_ELFNOTE_PHYS32_ENTRY (type 18): 32-bit physical entry point address
// QEMU detects this note in the ELF and enters in 32-bit protected mode
// with paging disabled, A20 enabled, and BSS NOT zeroed.
// =============================================================================
.section ".note.Xen", "a"
.align 4
    .long 4                         // namesz = 4 ("Xen\0")
    .long 4                         // descsz = 4 (32-bit entry address)
    .long 18                        // type = XEN_ELFNOTE_PHYS32_ENTRY
    .asciz "Xen"                    // name (4 bytes with NUL)
    .long _pvh_start                // 32-bit physical entry point

// =============================================================================
// 32-bit PVH entry point
// =============================================================================
// CPU state on PVH entry:
//   - 32-bit protected mode, paging OFF
//   - CS = flat 32-bit code segment (base=0, limit=4GB)
//   - DS/ES/SS = flat 32-bit data segment
//   - EBX = pointer to hvm_start_info structure (PVH boot info)
//   - A20 gate enabled
// =============================================================================
.section ".text.boot", "ax"
.code32

.type _pvh_start, @function
_pvh_start:
    // Debug: Write 'P' to VGA to show PVH entry was called
    movb $0x50, 0xb8000        // 'P' character
    movb $0x0F, 0xb8001        // bright white on black

    // Mark that PVH entry was called by setting a sentinel value
    movl $0xDEADBEEF, pvh_temp_info_32

    // Save PVH boot info pointer (EBX) for later use
    mov %ebx, %esi

    // If EBX is non-zero, store it; otherwise keep sentinel
    test %ebx, %ebx
    jz .Lskip_pvh_save
    mov %esi, pvh_temp_info_32
.Lskip_pvh_save:

    // Disable interrupts
    cli

    // --- Set up identity-mapped page tables for first 4 GB ---
    // PML4[0] → PDPT → four 1GB pages (using 1GB huge pages if supported,
    // otherwise 2MB pages via PD)
    //
    // We use 2MB pages for compatibility: PML4 → PDPT → PD (2MB each)
    //
    // Page table layout (in .bss.pagetables):
    //   pvh_pml4:   4096 bytes (512 entries, only [0] used)
    //   pvh_pdpt:   4096 bytes (512 entries, only [0..3] used for 4GB)
    //   pvh_pd[4]:  4x4096 bytes (each maps 1GB via 512x2MB pages)

    // Zero page table area (6 pages = 24KB)
    mov $pvh_pml4, %edi
    xor %eax, %eax
    mov $(6 * 4096 / 4), %ecx
    rep stosl

    // PML4[0] = &pvh_pdpt | PRESENT | WRITABLE
    mov $pvh_pdpt, %eax
    or  $0x03, %eax
    mov %eax, pvh_pml4

    // PDPT[0..3] = &pvh_pd[0..3] | PRESENT | WRITABLE
    mov $pvh_pd0, %eax
    or  $0x03, %eax
    mov %eax, pvh_pdpt + 0*8
    mov $pvh_pd1, %eax
    or  $0x03, %eax
    mov %eax, pvh_pdpt + 1*8
    mov $pvh_pd2, %eax
    or  $0x03, %eax
    mov %eax, pvh_pdpt + 2*8
    mov $pvh_pd3, %eax
    or  $0x03, %eax
    mov %eax, pvh_pdpt + 3*8

    // Fill PD entries: each maps 2MB (PRESENT | WRITABLE | PAGE_SIZE)
    // PD0: 0x00000000 .. 0x3FFFFFFF (1st GB)
    // PD1: 0x40000000 .. 0x7FFFFFFF (2nd GB)
    // PD2: 0x80000000 .. 0xBFFFFFFF (3rd GB)
    // PD3: 0xC0000000 .. 0xFFFFFFFF (4th GB)
    mov $pvh_pd0, %edi
    xor %eax, %eax              // start physical address = 0
    mov $0x83, %ebx             // flags: PRESENT | WRITABLE | PAGE_SIZE(2MB)
    mov $(4 * 512), %ecx        // 4 PDs × 512 entries = 2048 entries total
.Lfill_pd:
    mov %eax, (%edi)
    or  %ebx, (%edi)
    movl $0, 4(%edi)            // high 32 bits = 0 (for low 4GB)
    add $0x200000, %eax         // next 2MB page
    add $8, %edi
    dec %ecx
    jnz .Lfill_pd

    // --- Enable long mode ---

    // Load PML4 into CR3
    mov $pvh_pml4, %eax
    mov %eax, %cr3

    // Enable PAE in CR4
    mov %cr4, %eax
    or  $0x20, %eax             // CR4.PAE (bit 5)
    mov %eax, %cr4

    // Enable long mode in EFER MSR
    mov $0xC0000080, %ecx       // IA32_EFER
    rdmsr
    or  $0x100, %eax            // EFER.LME (bit 8)
    wrmsr

    // Enable paging in CR0 (activates long mode since EFER.LME is set)
    mov %cr0, %eax
    or  $0x80000001, %eax       // CR0.PG (bit 31) | CR0.PE (bit 0)
    mov %eax, %cr0

    // Load a temporary 64-bit GDT and far-jump to 64-bit code
    lgdt pvh_gdt_ptr

    ljmp $0x08, $_start         // jump to 64-bit entry with kernel CS

// Temporary GDT for the 32→64 transition (reloaded by _start with full GDT)
.align 16
pvh_gdt:
    .quad 0                             // [0] Null
    .quad 0x00AF9A000000FFFF            // [1] 64-bit Kernel Code
    .quad 0x00CF92000000FFFF            // [2] Kernel Data
pvh_gdt_end:

pvh_gdt_ptr:
    .word pvh_gdt_end - pvh_gdt - 1
    .long pvh_gdt

.size _pvh_start, . - _pvh_start

// =============================================================================
// 64-bit kernel entry (jumped to from PVH stub or called by bootloader)
// =============================================================================
.code64

.type _start, @function
.global _start
_start:
    // Clear direction flag — required for REP STOSB below to move forward
    cld

    // Set up temporary kernel stack (8 KB, defined in .bss.stack below)
    lea stack_top(%rip), %rsp

    // Zero BSS section: memset(BSS_START, 0, BSS_END - BSS_START)
    //   RDI = destination (BSS start)
    //   RCX = count in bytes (BSS end - BSS start)
    //   AL  = 0 (fill value)
    //   REP STOSB: store AL to [RDI], increment RDI, decrement RCX, repeat
    lea _bss_start(%rip), %rdi
    lea _bss_end(%rip), %rcx
    sub %rdi, %rcx                      // RCX = BSS size in bytes
    xor %eax, %eax                      // fill with zeros
    rep stosb                           // zero the entire BSS

    // Mark that _start was called (after BSS clearing)
    movq $0xCAFEBABE, %rax
    movq %rax, pvh_boot_info_ptr(%rip)

    // Debug: Write 'S' to VGA to show _start was called
    movb $0x53, 0xb8000        // 'S' character
    movb $0x0F, 0xb8001        // bright white on black

    // ---- Load GDT ----
    // Layout: [0] null | [1] kernel CS | [2] kernel DS | [3] user CS | [4] user DS
    // Selectors: 0x08=KCS, 0x10=KDS, 0x1B=UCS (RPL=3), 0x23=UDS (RPL=3)
    lea gdt_table(%rip), %rax
    lea gdt_ptr(%rip), %rbx
    movw $(gdt_end - gdt_table - 1), (%rbx) // limit
    movq %rax, 2(%rbx)                      // base
    lgdt (%rbx)

    // Reload CS via far return (lretq pops RIP then CS)
    pushq $0x08                              // kernel code segment selector
    lea .Lreload_cs(%rip), %rax
    pushq %rax
    lretq
.Lreload_cs:
    // Reload data segment registers with kernel data selector
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    // ---- Retrieve PVH boot info from 32-bit temp location ----
    // Zero-extend the 32-bit PVH pointer to 64 bits and store it properly
    movl pvh_temp_info_32(%rip), %eax  // Load 32-bit value
    movq %rax, pvh_boot_info_ptr(%rip) // Store as 64-bit (zero-extended)

    // ---- Set up TSS for ring 3 → ring 0 transitions ----
    call x86_64_setup_tss

    // ---- Call unified boot entry ----
    // Pass PVH boot info pointer as device_tree_ptr for x86_64
    movq pvh_boot_info_ptr(%rip), %rdi

    call early_main

    // Should not return — halt loop as safety net
    cli
1:  hlt
    jmp 1b
.size _start, . - _start

// =============================================================================
// Temporary kernel stack (8 KB, page-aligned, in BSS)
// =============================================================================
.section ".bss.stack", "aw", @nobits
.align 16
stack_bottom:
    .skip 8192                          // 8 KB stack
stack_top:

// PVH boot info (saved from 32-bit mode)
.align 8
pvh_boot_info_ptr:
    .quad 0                             // 64-bit pointer to hvm_start_info structure

.align 4
pvh_temp_info_32:
    .long 0                             // 32-bit temporary storage for PVH pointer

// =============================================================================
// PVH page tables (24 KB — zeroed by PVH stub before enabling paging)
// Placed in a dedicated section (.pvh.pagetables) to avoid BSS clearing in
// _start from overwriting these live page tables.
// =============================================================================
.section ".pvh.pagetables", "aw", @nobits
.align 4096
pvh_pml4:    .skip 4096
pvh_pdpt:    .skip 4096
pvh_pd0:     .skip 4096
pvh_pd1:     .skip 4096
pvh_pd2:     .skip 4096
pvh_pd3:     .skip 4096

// =============================================================================
// GDT (Global Descriptor Table)
// =============================================================================
//
// 7-entry GDT for 64-bit long mode with TSS:
//   [0] Null descriptor           (required by x86 architecture)
//   [1] Kernel Code (0x08)        64-bit, DPL=0, Execute/Read
//   [2] Kernel Data (0x10)        DPL=0, Read/Write
//   [3] User Data   (0x18+3=0x1B) DPL=3, Read/Write      (SYSRET: SS = STAR[63:48]+8)
//   [4] User Code   (0x20+3=0x23) 64-bit, DPL=3, Exec/Read (SYSRET: CS = STAR[63:48]+16)
//   [5-6] TSS descriptor (0x28)   16-byte system descriptor, filled at runtime
//
// NOTE: User Data MUST precede User Code for SYSRETQ compatibility.
// SYSRETQ computes: SS = STAR[63:48]+8, CS = STAR[63:48]+16 (both OR'd with RPL=3).
// With STAR[63:48]=0x0010: SS=0x0018|3=0x1B (User Data), CS=0x0020|3=0x23 (User Code).
//
// Placed in .data (writable) so the TSS descriptor can be written at runtime.
// =============================================================================
.section ".data", "aw"
.align 16
.global gdt_table
gdt_table:
    .quad 0                             // [0] Null descriptor
    .quad 0x00AF9A000000FFFF            // [1] Kernel Code (0x08): L=1, DPL=0, P=1, type=0xA(E/R)
    .quad 0x00CF92000000FFFF            // [2] Kernel Data (0x10): DPL=0, P=1, type=0x2(R/W)
    .quad 0x00CFF2000000FFFF            // [3] User Data   (0x18): DPL=3, P=1, type=0x2(R/W)
    .quad 0x00AFFA000000FFFF            // [4] User Code   (0x20): L=1, DPL=3, P=1, type=0xA(E/R)
    .quad 0                             // [5] TSS low  (0x28) — filled by x86_64_setup_tss
    .quad 0                             // [6] TSS high (0x30) — filled by x86_64_setup_tss
gdt_end:

.align 16
gdt_ptr:
    .word 0                             // limit (filled at runtime above)
    .quad 0                             // base  (filled at runtime above)

// =============================================================================
// TSS (Task State Segment) — 104 bytes, zeroed in BSS, filled at runtime
// =============================================================================
.section ".bss.tss", "aw", @nobits
.align 16
.global g_tss
g_tss:
    .skip 104                           // x86_64 TSS is 104 bytes

// Prevent executable stack warning
.section .note.GNU-stack,"",@progbits
