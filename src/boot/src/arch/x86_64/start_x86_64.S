// =============================================================================
// x86_64 Kernel Boot Entry
// =============================================================================
//
// Bootstrap for x86_64 MOSS kernel in long mode (64-bit).
//
// Assumes the bootloader already placed us in 64-bit long mode with a
// flat identity map.  This stub:
//   1. Clears direction flag (CLD) for safe REP string operations
//   2. Sets up a temporary 8 KB kernel stack
//   3. Zeroes the BSS section
//   4. Loads a minimal GDT with kernel/user code+data segments
//   5. Calls early_main() to enter the unified boot path
//
// Entry point: _start (set by the linker via ENTRY(_start) in linker.ld)
// =============================================================================

.section ".text.boot", "ax"
.global _start
.code64

.type _start, @function
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
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx                      // RCX = BSS size in bytes
    xor %eax, %eax                      // fill with zeros
    rep stosb                           // zero the entire BSS

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

    // ---- Call unified boot entry ----
    // x86_64 has no DTB; pass NULL as device_tree_ptr
    xorq %rdi, %rdi
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

// =============================================================================
// GDT (Global Descriptor Table)
// =============================================================================
//
// 5-entry GDT for 64-bit long mode:
//   [0] Null descriptor           (required by x86 architecture)
//   [1] Kernel Code (0x08)        64-bit, DPL=0, Execute/Read
//   [2] Kernel Data (0x10)        DPL=0, Read/Write
//   [3] User Code   (0x18+3=0x1B) 64-bit, DPL=3, Execute/Read
//   [4] User Data   (0x20+3=0x23) DPL=3, Read/Write
//
// Descriptor format (8 bytes each):
//   Bits [15:0]  = Limit [15:0]     (ignored in 64-bit mode for code/data)
//   Bits [31:16] = Base [15:0]      (ignored in 64-bit mode for code/data)
//   Bits [39:32] = Base [23:16]
//   Bits [47:40] = Access byte      (P=1, DPL, S=1, Type)
//   Bits [51:48] = Limit [19:16]    (+ flags in [55:52])
//   Bits [55:52] = Flags            (G, D/B, L, AVL)
//   Bits [63:56] = Base [31:24]
// =============================================================================
.section ".rodata", "a"
.align 16
gdt_table:
    .quad 0                             // [0] Null descriptor
    .quad 0x00AF9A000000FFFF            // [1] Kernel Code: L=1(64-bit), DPL=0, P=1, type=0xA(E/R)
    .quad 0x00CF92000000FFFF            // [2] Kernel Data: DPL=0, P=1, type=0x2(R/W)
    .quad 0x00AFFA000000FFFF            // [3] User Code:   L=1(64-bit), DPL=3, P=1, type=0xA(E/R)
    .quad 0x00CFF2000000FFFF            // [4] User Data:   DPL=3, P=1, type=0x2(R/W)
gdt_end:

.align 16
gdt_ptr:
    .word 0                             // limit (filled at runtime above)
    .quad 0                             // base  (filled at runtime above)

// =============================================================================
// Weak BSS boundary symbols
// =============================================================================
//
// These are defined as weak so the linker script can override them.
// If no linker script is used, they default to 0 (no BSS to clear).
// =============================================================================
.weak __bss_start
.weak __bss_end
__bss_start = 0
__bss_end = 0
