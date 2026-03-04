/*
 * =============================================================================
 * ARM64 Kernel Boot and Exception Handling
 * =============================================================================
 *
 * This is the primary assembly file for the ARM64 MOSS kernel.  It handles:
 *
 *   1. BOOT SEQUENCE
 *      - Linux ARM64 Image header (enables QEMU DTB auto-generation)
 *      - Exception level descent: EL3 → EL2 → EL1
 *      - BSS clearing, stack setup, DTB discovery
 *      - Primary CPU → early_main()
 *      - Secondary CPUs → secondary_cpu_entry()
 *
 *   2. EXCEPTION VECTOR TABLE
 *      - 4 exception sources × 4 exception types = 16 vectors
 *      - Routes IRQs to irq_trampoline (save/restore + C handler)
 *      - Routes synchronous from lower EL to lower_el_sync_dispatch
 *      - Routes other exceptions to exception_handler
 *
 *   3. IRQ HANDLING (irq_trampoline)
 *      - Saves 34-slot register frame (x0-x30 + ELR + SPSR + padding)
 *      - Calls irq_handler_c (C++)
 *      - Restores all registers and returns via eret
 *
 *   4. SYNCHRONOUS EXCEPTION DISPATCH
 *      - Same-EL: exception_handler routes Data Abort / Instruction Abort
 *        to kernel_page_fault_handler, others to unhandled_exception_handler
 *      - Lower-EL: lower_el_sync_dispatch routes:
 *          EC=0x15 (SVC) → system_call_handler
 *          EC=0x24 (Data Abort) / EC=0x20 (Instr Abort) → user_page_fault_handler
 *          Other EC → unhandled_user_exception_handler
 *
 *   5. MULTI-CORE SUPPORT
 *      - CPU 0 performs cold boot (BSS clear, MMU config)
 *      - Secondary CPUs skip cold boot, set up per-CPU stack, jump to C++
 *      - Early UART spinlock for safe multi-core boot messages
 *
 * == DTB (Device Tree Blob) Dual Discovery ==
 *
 * MOSS supports two DTB discovery methods, tried in priority order:
 *
 *   Method 1 (preferred): Linux boot protocol — x0 register
 *     Bootloader/QEMU passes DTB physical address via x0.  QEMU -kernel mode
 *     follows this protocol when it detects a Linux ARM64 Image (checks "MZ"
 *     and "ARM\x64" magic).  Validation: x0 non-zero AND DTB magic matches.
 *
 *   Method 2 (fallback): RAM linear scan
 *     When x0 is 0 or doesn't point to a valid DTB, scan from RAM top
 *     downward in 64KB steps looking for DTB magic (0xD00DFEED big-endian).
 *
 * Both methods complement each other: Linux Image mode uses Method 1,
 * bare ELF mode uses Method 2.  If both fail, C++ FDT parser has
 * hardcoded fallback values.
 *
 * == Register Frame Layout (34 slots × 8 bytes = 272 bytes) ==
 *
 *   Slot  Offset    Content
 *   ----  ------    -------
 *   0-1   0x00      x0, x1
 *   2-3   0x10      x2, x3
 *   ...   ...       ...
 *   28-29 0xE0      x28, x29 (FP)
 *   30    0xF0      x30 (LR)
 *   31    0xF8      ELR_EL1 (exception return address)
 *   32    0x100     SPSR_EL1 (saved processor state)
 *   33    0x108     (padding for 16-byte SP alignment)
 *
 * == Stack Layout ==
 *
 *   1MB total stack space, 32KB per CPU, supporting up to ASM_MAX_CPUS CPUs.
 *   CPU 0 stack: _stack_top → _stack_top - 32KB
 *   CPU N stack: _stack_top - N*32KB → _stack_top - (N+1)*32KB
 *
 * External C/C++ functions called:
 *   early_main(dtb_ptr)              — primary CPU entry (boot module)
 *   secondary_cpu_entry()            — secondary CPU entry (boot module)
 *   irq_handler_c()                  — interrupt handler (interrupts module)
 *   system_call_handler(nr, a0..a5)  — syscall dispatcher (kernel module)
 *   kernel_page_fault_handler(esr, far, elr) — same-EL page faults
 *   user_page_fault_handler(esr, far, elr)   — user-mode page faults
 *   unhandled_exception_handler(esr, far, elr) — fatal exceptions
 *   unhandled_user_exception_handler(esr, far, elr) — unknown user exceptions
 */

// Boot-time CPU limit (must match BOOT_MAX_CPUS in types.cppm).
// Can be overridden via CMake: -DASM_MAX_CPUS=N
#ifndef ASM_MAX_CPUS
#define ASM_MAX_CPUS 16
#endif

.section ".text.boot", "ax"
.global _start
.type _start, @function

// =============================================================================
// ARM64 Linux Image Header (64 bytes)
// =============================================================================
//
// This header makes the kernel binary compatible with the Linux ARM64 boot
// protocol.  QEMU checks for the magic "ARM\x64" at offset 0x38 to identify
// the image format.  When detected, QEMU automatically generates a DTB and
// passes its address via x0, eliminating the need for manual DTB generation.
//
// The `b _start` instruction at offset 0 branches over the header to the
// actual kernel entry point.  For ELF boot (where this header is not at the
// file start), _start is reached directly via the ELF entry point.
// =============================================================================
_linux_header:
    b       _start                      // code0: branch to actual entry
    .word   0                           // code1: reserved
    .quad   0                           // text_offset: load at RAM base + 0
    .quad   _kernel_image_size          // image_size: filled by linker
    .quad   0                           // flags: little-endian, 4K pages
    .quad   0                           // res2
    .quad   0                           // res3
    .quad   0                           // res4
    .word   0x644d5241                  // magic: "ARM\x64"
    .word   0                           // res5 (PE offset)

// =============================================================================
// System Register Constants
// =============================================================================

// SCTLR_EL1 — System Control Register
#define SCTLR_EL1_RES1    (0x30C50838)  // Reserved bits that must be set to 1
#define SCTLR_EL1_RES0    (0xCE8181C1)  // Reserved bits that must be set to 0
#define SCTLR_EL1_UCI     (1 << 26)     // User Cache Instructions access enable
#define SCTLR_EL1_EE      (1 << 25)     // Exception Endianness (0=LE)
#define SCTLR_EL1_EOE     (1 << 24)     // EL0 Endianness (0=LE)
#define SCTLR_EL1_WXN     (1 << 19)     // Write implies Execute-Never
#define SCTLR_EL1_nTWE    (1 << 18)     // EL0 WFE not trapped
#define SCTLR_EL1_nTWI    (1 << 16)     // EL0 WFI not trapped
#define SCTLR_EL1_UCT     (1 << 15)     // User CTR_EL0 access enable
#define SCTLR_EL1_DZE     (1 << 14)     // EL0 DC ZVA instruction enable
#define SCTLR_EL1_I       (1 << 12)     // Instruction cache enable
#define SCTLR_EL1_UMA     (1 << 9)      // User Mask Access
#define SCTLR_EL1_SED     (1 << 8)      // SETEND instruction disable
#define SCTLR_EL1_ITD     (1 << 7)      // IT instruction disable
#define SCTLR_EL1_THEE    (1 << 6)      // ThumbEE enable (obsolete)
#define SCTLR_EL1_CP15BEN (1 << 5)      // CP15 barrier enable
#define SCTLR_EL1_SA0     (1 << 4)      // EL0 Stack Alignment check
#define SCTLR_EL1_SA      (1 << 3)      // EL1 Stack Alignment check
#define SCTLR_EL1_C       (1 << 2)      // Data/unified cache enable
#define SCTLR_EL1_A       (1 << 1)      // Alignment check enable
#define SCTLR_EL1_M       (1 << 0)      // MMU enable

// HCR_EL2 — Hypervisor Configuration Register
#define HCR_EL2_RW        (1 << 31)     // EL1 is AArch64 (not AArch32)

// CPACR_EL1 — Architectural Feature Access Control Register
#define CPACR_EL1_FPEN    (0x3 << 20)   // FP/NEON access: no trap at EL0/EL1

// SPSR_EL2 — Saved Program Status Register at EL2
#define SPSR_EL2_MASK     0x1F
#define SPSR_EL2_EL1h     0x05          // Target: EL1 with SP_EL1
#define SPSR_EL2_F        (1 << 6)      // FIQ mask
#define SPSR_EL2_I        (1 << 7)      // IRQ mask
#define SPSR_EL2_A        (1 << 8)      // SError mask
#define SPSR_EL2_D        (1 << 9)      // Debug mask

// =============================================================================
// _start — Kernel entry point
// =============================================================================
//
// On entry:
//   x0 = DTB physical address (from bootloader/QEMU, or 0 if not passed)
//   MPIDR_EL1.Aff0 = CPU ID (0 for boot CPU)
//   CurrentEL = EL3, EL2, or EL1 depending on firmware
// =============================================================================
_start:
    // Preserve x0 (potential DTB pointer) in callee-saved x20.
    // Linux boot protocol: bootloader passes DTB address via x0.
    // QEMU bare ELF mode: x0 = 0 (no DTB, will use RAM scan fallback).
    mov     x20, x0

    // Read CPU ID from MPIDR_EL1 (Multiprocessor Affinity Register)
    // Aff0 field [7:0] gives the core number within the cluster.
    mrs     x19, mpidr_el1
    and     x19, x19, #0xFF

    // Determine current Exception Level and route accordingly.
    // CurrentEL register bits [3:2] encode the EL: 1=EL1, 2=EL2, 3=EL3.
    mrs     x0, CurrentEL
    lsr     x0, x0, #2

    cmp     x0, #3
    b.eq    el3_entry                   // EL3 → configure security, drop to EL2

    cmp     x0, #2
    b.eq    el2_entry                   // EL2 → configure virtualization, drop to EL1

    b       el1_entry                   // EL1 → proceed with kernel init

// =============================================================================
// EL3 → EL2 transition
// =============================================================================
//
// Configure Secure Configuration Register (SCR_EL3):
//   0x431 = RW (EL2 is AArch64) | NS (non-secure) | SMD | HCE
// Then perform eret to EL2.
// =============================================================================
el3_entry:
    mov     x0, #0x431
    msr     scr_el3, x0

    // SPSR_EL3: mask all exceptions, target EL1h (will descend through EL2)
    mov     x0, #(SPSR_EL2_F | SPSR_EL2_I | SPSR_EL2_A | SPSR_EL2_D | SPSR_EL2_EL1h)
    msr     spsr_el3, x0

    // Set EL3 return address to el2_entry
    adr     x0, el2_entry
    msr     elr_el3, x0

    eret                                // → EL2

// =============================================================================
// EL2 → EL1 transition
// =============================================================================
//
// Configure HCR_EL2.RW = 1 so EL1 runs in AArch64 mode.
// Then perform eret to EL1.
// =============================================================================
el2_entry:
    mov     x0, #HCR_EL2_RW
    msr     hcr_el2, x0

    // Enable GICv3 system register access before dropping to EL1.
    // ICC_SRE_EL2 = S3_4_C12_C9_5: SRE (bit 0) | DFB (bit 1) | DIB (bit 2) = 0x7
    // This allows EL1 to use ICC_*_EL1 system registers for GICv3 CPU interface.
    // Safe no-op on GICv2 hardware (the register does not exist, write ignored).
    mov     x0, #0x7
    msr     S3_4_C12_C9_5, x0
    isb

    // SPSR_EL2: mask all exceptions, target EL1h
    mov     x0, #(SPSR_EL2_F | SPSR_EL2_I | SPSR_EL2_A | SPSR_EL2_D | 0x05)
    msr     spsr_el2, x0

    // Set EL2 return address to el1_entry
    adr     x0, el1_entry
    msr     elr_el2, x0

    eret                                // → EL1

// =============================================================================
// EL1 initialization — CPU 0 cold boot vs secondary CPU hot path
// =============================================================================
el1_entry:
    // Secondary CPUs must NOT re-run cold-boot initialization (BSS clear,
    // MMU disable, etc.) because CPU 0 has already set up the system.
    // They only need a stack and then jump to secondary_cpu_park.
    cbnz    x19, secondary_el1_entry

    // ==================== CPU 0 cold-boot path ====================

    // Disable MMU, data cache, instruction cache and alignment check
    // for clean startup.  We'll enable M/C/I later once page tables are
    // set up.  The A bit must be cleared because libfdt reads DTB fields
    // that may not be naturally aligned; QEMU ≥10 sets A=1 by default
    // on cortex-a57 which causes alignment faults during DTB parsing.
    mrs     x0, sctlr_el1
    bic     x0, x0, #SCTLR_EL1_M       // disable MMU
    bic     x0, x0, #SCTLR_EL1_C       // disable data cache
    bic     x0, x0, #SCTLR_EL1_I       // disable instruction cache
    bic     x0, x0, #SCTLR_EL1_A       // disable alignment check
    msr     sctlr_el1, x0

    // Install exception vector table (defined later in this file)
    adr     x0, exception_vectors
    msr     vbar_el1, x0

    // Enable FP/NEON access (otherwise any FP instruction traps to EL1)
    mov     x0, #CPACR_EL1_FPEN
    msr     cpacr_el1, x0

    // Zero BSS section (CPU 0 only — secondary CPUs skip this)
    ldr     x0, =_bss_start_addr
    ldr     x1, =_bss_end_addr
clear_bss:
    cmp     x0, x1
    b.hs    bss_cleared
    str     xzr, [x0], #8              // store 8 bytes of zeros, advance pointer
    b       clear_bss
bss_cleared:

    // Set CPU 0 stack pointer to top of the 256KB stack region
    ldr     x0, =_stack_top_addr
    mov     sp, x0

    // ---- Debug: print "[0]\n" to UART (with spinlock for multi-core safety) ----
    bl      early_uart_acquire
    mov     x25, #0x09000000            // QEMU virt PL011 UART base
    mov     x26, #'['
    str     w26, [x25]
    mov     x26, #'0'                   // CPU 0
    str     w26, [x25]
    mov     x26, #']'
    str     w26, [x25]
    mov     x26, #10                    // newline
    str     w26, [x25]
    bl      early_uart_release

    b       primary_cpu_continue

// =============================================================================
// Secondary CPU entry (PSCI CPU_ON lands here via _start)
// =============================================================================
//
// Secondary CPUs arrive here after going through EL3→EL2→EL1 descent.
// They must NOT touch BSS or MMU settings (CPU 0 already set up everything).
// Each CPU gets a 32KB stack slice below _stack_top.
// =============================================================================
secondary_el1_entry:
    // Clear alignment check on secondary CPUs too (SCTLR_EL1 resets on each
    // PSCI CPU_ON, so the A bit may be set by QEMU ≥10 default).
    mrs     x0, sctlr_el1
    bic     x0, x0, #SCTLR_EL1_A
    msr     sctlr_el1, x0
    isb

    // Set up per-CPU stack: SP = _stack_top - (CPU_ID * 32KB)
    ldr     x0, =_stack_top_addr
    mov     x1, #0x8000                 // 32KB per CPU
    mul     x1, x19, x1                // CPU_ID * stack_size
    sub     sp, x0, x1

    // ---- Debug: print "[N]\n" to UART (locked) ----
    bl      early_uart_acquire
    mov     x25, #0x09000000
    mov     x26, #'['
    str     w26, [x25]
    add     x26, x19, #'0'             // CPU ID → ASCII digit
    str     w26, [x25]
    mov     x26, #']'
    str     w26, [x25]
    mov     x26, #10
    str     w26, [x25]
    bl      early_uart_release

    b       secondary_cpu_park

// =============================================================================
// Primary CPU continues — DTB discovery and jump to C++
// =============================================================================
primary_cpu_continue:

    // ---- Debug: print "MAIN\n" (locked) ----
    bl      early_uart_acquire
    mov     x25, #0x09000000
    mov     x26, #'M'
    str     w26, [x25]
    mov     x26, #'A'
    str     w26, [x25]
    mov     x26, #'I'
    str     w26, [x25]
    mov     x26, #'N'
    str     w26, [x25]
    mov     x26, #10
    str     w26, [x25]
    bl      early_uart_release

    // ---- DTB dual discovery mechanism ----
    //
    // DTB magic bytes: big-endian 0xD00DFEED
    // In memory (little-endian ldr): reads as 0xEDFE0DD0
    //
    // Strategy:
    //   1. If x20 != 0 (bootloader passed DTB), verify its magic
    //   2. If x20 == 0 or magic mismatch, scan RAM from top downward
    //   3. If both fail, x20 = 0 → C++ layer uses hardcoded fallback
    //
    movz    w21, #0x0DD0                // DTB magic low 16 bits
    movk    w21, #0xEDFE, lsl #16      // complete: 0xEDFE0DD0

    // ---- Method 1: Validate bootloader-provided DTB pointer ----
    cbz     x20, .Ldtb_ram_scan         // x0 was 0 → skip to RAM scan

    ldr     w26, [x20]                  // read first 4 bytes at x20
    cmp     w26, w21                    // compare with DTB magic
    b.eq    .Ldtb_ready                 // match → use this DTB
    mov     x20, xzr                    // mismatch → clear, fall through to scan

    // ---- Method 2: RAM linear scan (fallback) ----
.Ldtb_ram_scan:
    // Scan from near RAM top downward, 64KB steps.
    // QEMU virt machine: RAM at 0x4000_0000, default 256MB → top ≈ 0x4FFF_0000
    movz    x24, #0x4FFF, lsl #16      // scan start: 0x4FFF_0000
    movz    x22, #0x4000, lsl #16      // scan end:   0x4000_0000

.Ldtb_scan_loop:
    cmp     x24, x22
    b.lo    .Ldtb_ready                 // exhausted scan → x20=0, C++ fallback

    ldr     w26, [x24]
    cmp     w26, w21                    // check DTB magic
    b.eq    .Ldtb_match

    sub     x24, x24, #0x10000         // step down 64KB
    b       .Ldtb_scan_loop

.Ldtb_match:
    mov     x20, x24                    // record found DTB physical address

.Ldtb_ready:
    // ---- Jump to C++ kernel entry ----
    mov     x0, x20                     // arg0: DTB physical address (or 0)
    bl      early_main

    // If early_main returns (should not happen), halt
    b       halt
.size _start, . - _start

// =============================================================================
// Secondary CPU park — wait then jump to C++ secondary_cpu_entry
// =============================================================================
//
// After printing a debug message, secondary CPUs validate their CPU ID
// (must be < ASM_MAX_CPUS) and jump directly to the C++ secondary_cpu_entry()
// function.  The Linux-style deferred activation mechanism in C++ handles
// synchronization — no polling loop needed here.
// =============================================================================
.type secondary_cpu_park, @function
secondary_cpu_park:
    // ---- Debug: print "CPUN:S\n" (locked) ----
    bl      early_uart_acquire
    mov     x25, #0x09000000
    mov     x26, #'C'
    str     w26, [x25]
    mov     x26, #'P'
    str     w26, [x25]
    mov     x26, #'U'
    str     w26, [x25]
    add     x26, x19, #'0'             // CPU ID → ASCII
    str     w26, [x25]
    mov     x26, #':'
    str     w26, [x25]
    mov     x26, #'S'                   // S = Started
    str     w26, [x25]
    mov     x26, #10
    str     w26, [x25]
    bl      early_uart_release

    // Validate CPU ID is within supported range
    cmp     x19, #ASM_MAX_CPUS          // Configured maximum CPUs
    b.hs    invalid_cpu_id

    // ---- Debug: print "DN\n" (D=Direct jump, N=CPU ID) (locked) ----
    bl      early_uart_acquire
    mov     x25, #0x09000000
    mov     x26, #'D'
    str     w26, [x25]
    add     x26, x19, #'0'
    str     w26, [x25]
    mov     x26, #10
    str     w26, [x25]
    bl      early_uart_release

    // Jump directly to C++ secondary_cpu_entry (no polling needed)
    adr     x23, secondary_cpu_entry_wrapper

    // Memory barriers to ensure all previous stores are visible
    dsb     sy
    isb

    br      x23

secondary_cpu_entry_wrapper:
    // Call C++ secondary_cpu_entry()
    bl      secondary_cpu_entry

    // If it returns (should not happen), halt
    b       halt

invalid_cpu_id:
    // CPU ID out of range — enter permanent WFI loop
    wfi
    b       invalid_cpu_id
.size secondary_cpu_park, . - secondary_cpu_park

// =============================================================================
// CPU startup flags — inter-processor communication during boot
// =============================================================================
.global cpu_startup_flags

.section ".bss", "aw", @nobits
.align 8
cpu_startup_flags:
    // Per-CPU startup control block: [entry_point: 8B] [startup_flag: 8B]
    // ASM_MAX_CPUS CPUs, each CPU gets 16 bytes
    .space ASM_MAX_CPUS * 16

// =============================================================================
// Early UART spinlock — shared between assembly and C++ boot code
// =============================================================================
//
// Simple test-and-set spinlock using LDXR/STXR exclusive monitor.
// 0 = unlocked, 1 = locked.
// Used to serialize UART output during early boot when multiple CPUs
// are printing simultaneously.
// =============================================================================
.global early_uart_lock
.align 4
early_uart_lock:
    .word 0

// Switch back to .text.boot for executable code
.section ".text.boot", "ax"

// =============================================================================
// early_uart_acquire / early_uart_release — spinlock subroutines
// =============================================================================
//
// Uses the ARMv8 exclusive monitor (LDXR/STXR) for atomic test-and-set.
// DMB SY provides acquire/release memory ordering semantics.
//
// Clobbers: x24, x27, x28 (chosen to not conflict with UART print sequences
//           which use x25=base, x26=char)
// =============================================================================
.type early_uart_acquire, @function
early_uart_acquire:
    adr     x27, early_uart_lock
1:
    ldxr    w28, [x27]                  // load-exclusive: read lock value
    cbnz    w28, 1b                     // spin while locked (value != 0)
    mov     w28, #1
    stxr    w24, w28, [x27]             // store-exclusive: try to set lock=1
    cbnz    w24, 1b                     // retry if exclusive store failed
    dmb     sy                          // acquire barrier
    ret
.size early_uart_acquire, . - early_uart_acquire

.type early_uart_release, @function
early_uart_release:
    adr     x27, early_uart_lock
    dmb     sy                          // release barrier
    str     wzr, [x27]                  // unlock: write 0
    ret
.size early_uart_release, . - early_uart_release

// =============================================================================
// halt — infinite WFI loop (system halt)
// =============================================================================
.type halt, @function
halt:
    wfi                                 // wait for interrupt (power-saving halt)
    b       halt
.size halt, . - halt

// =============================================================================
// Exception Vector Table
// =============================================================================
//
// ARMv8 exception vector table layout:
//   - 4 exception sources × 4 exception types = 16 entries
//   - Each entry is 128 bytes (2^7), aligned to 2048 bytes (2^11)
//
// Exception sources:
//   1. Current EL with SP_EL0  (not used by MOSS — kernel always uses SP_ELx)
//   2. Current EL with SP_ELx  (kernel-to-kernel exceptions)
//   3. Lower EL using AArch64  (user-space exceptions — primary path)
//   4. Lower EL using AArch32  (user-space AArch32 — fallback)
//
// Exception types per source:
//   Synchronous: SVC, data abort, instruction abort, etc.
//   IRQ:         Hardware interrupts (timer, UART, etc.)
//   FIQ:         Fast interrupts (not used by MOSS)
//   SError:      System errors (not used by MOSS)
//
// For user-space exceptions (source 3), SP_EL1 is pre-set to the current
// thread's per-thread kernel stack before entering EL0 (done by switch_to_user
// and after irq_trampoline cleanup).  So when exceptions from EL0 arrive,
// SP is already correct.
// =============================================================================
.align 11
exception_vectors:
    // ---- Source 1: Current EL with SP_EL0 (unused) ----
    .align 7
    b       exception_handler           // Synchronous
    .align 7
    b       irq_trampoline              // IRQ
    .align 7
    b       exception_handler           // FIQ
    .align 7
    b       exception_handler           // SError

    // ---- Source 2: Current EL with SP_ELx (kernel mode) ----
    .align 7
    b       exception_handler           // Synchronous (kernel page fault, etc.)
    .align 7
    b       irq_trampoline              // IRQ (timer tick, device interrupts)
    .align 7
    b       exception_handler           // FIQ
    .align 7
    b       exception_handler           // SError

    // ---- Source 3: Lower EL using AArch64 (user mode — primary path) ----
    .align 7
    b       lower_el_sync_dispatch      // Synchronous (SVC / Data Abort / Instr Abort)
    .align 7
    b       irq_trampoline              // IRQ (SP_EL1 pre-set to per-thread kernel stack)
    .align 7
    b       exception_handler           // FIQ
    .align 7
    b       exception_handler           // SError

    // ---- Source 4: Lower EL using AArch32 (user mode — fallback) ----
    .align 7
    b       exception_handler           // Synchronous
    .align 7
    b       irq_trampoline              // IRQ (SP_EL1 pre-set to per-thread kernel stack)
    .align 7
    b       exception_handler           // FIQ
    .align 7
    b       exception_handler           // SError

// =============================================================================
// irq_trampoline — save context, call C handler, restore, eret
// =============================================================================
//
// This is the common IRQ entry point for ALL exception sources.
// It saves a 34-slot register frame, calls the C++ irq_handler_c(),
// then restores all registers and returns via eret.
//
// For exceptions from EL0 (user mode):
//   SP_EL1 was pre-set to the per-thread kernel stack before entering EL0.
//   After the `add sp, #(34*8)` cleanup, SP_EL1 is back at stack top.
//
// For exceptions from same EL (kernel mode):
//   SP was already the kernel stack.
// =============================================================================
.type irq_trampoline, @function
irq_trampoline:
    // Allocate 34-slot frame: x0-x30, ELR, SPSR, SP_EL0
    sub     sp, sp, #(34 * 8)

    // Save all general-purpose registers
    stp     x0,  x1,  [sp, #(0  * 8)]
    stp     x2,  x3,  [sp, #(2  * 8)]
    stp     x4,  x5,  [sp, #(4  * 8)]
    stp     x6,  x7,  [sp, #(6  * 8)]
    stp     x8,  x9,  [sp, #(8  * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(18 * 8)]
    stp     x20, x21, [sp, #(20 * 8)]
    stp     x22, x23, [sp, #(22 * 8)]
    stp     x24, x25, [sp, #(24 * 8)]
    stp     x26, x27, [sp, #(26 * 8)]
    stp     x28, x29, [sp, #(28 * 8)]
    str     x30,       [sp, #(30 * 8)]

    // Save exception return state + user stack pointer
    mrs     x0, elr_el1                 // exception return address
    mrs     x1, spsr_el1                // saved processor state
    stp     x0,  x1,  [sp, #(31 * 8)]
    mrs     x0, sp_el0                  // user-mode stack pointer
    str     x0,        [sp, #(33 * 8)]

    // Call C++ interrupt handler
    bl      irq_handler_c

    // Restore user stack pointer (before GP regs clobber temporaries)
    ldr     x0,        [sp, #(33 * 8)]
    msr     sp_el0, x0

    // Restore exception return state
    ldp     x0,  x1,  [sp, #(31 * 8)]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    // Restore all general-purpose registers
    ldp     x0,  x1,  [sp, #(0  * 8)]
    ldp     x2,  x3,  [sp, #(2  * 8)]
    ldp     x4,  x5,  [sp, #(4  * 8)]
    ldp     x6,  x7,  [sp, #(6  * 8)]
    ldp     x8,  x9,  [sp, #(8  * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(18 * 8)]
    ldp     x20, x21, [sp, #(20 * 8)]
    ldp     x22, x23, [sp, #(22 * 8)]
    ldp     x24, x25, [sp, #(24 * 8)]
    ldp     x26, x27, [sp, #(26 * 8)]
    ldp     x28, x29, [sp, #(28 * 8)]
    ldr     x30,       [sp, #(30 * 8)]

    // Deallocate frame and return from exception
    add     sp, sp, #(34 * 8)
    eret
.size irq_trampoline, . - irq_trampoline

// =============================================================================
// exception_handler — same-EL synchronous exception dispatch
// =============================================================================
//
// Handles synchronous exceptions from the current EL (kernel mode).
// Reads ESR_EL1 to determine the Exception Class (EC):
//
//   EC=0x25: Data Abort from same EL     → kernel_page_fault_handler(esr, far, elr)
//   EC=0x21: Instruction Abort, same EL  → kernel_page_fault_handler(esr, far, elr)
//   Other:   Fatal — print diagnostics   → unhandled_exception_handler → halt
//
// Uses the same 34-slot frame layout as irq_trampoline.
// =============================================================================
.type exception_handler, @function
exception_handler:
    // Save all registers (same frame as irq_trampoline)
    sub     sp, sp, #(34 * 8)

    stp     x0,  x1,  [sp, #(0  * 8)]
    stp     x2,  x3,  [sp, #(2  * 8)]
    stp     x4,  x5,  [sp, #(4  * 8)]
    stp     x6,  x7,  [sp, #(6  * 8)]
    stp     x8,  x9,  [sp, #(8  * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(18 * 8)]
    stp     x20, x21, [sp, #(20 * 8)]
    stp     x22, x23, [sp, #(22 * 8)]
    stp     x24, x25, [sp, #(24 * 8)]
    stp     x26, x27, [sp, #(26 * 8)]
    stp     x28, x29, [sp, #(28 * 8)]
    str     x30,       [sp, #(30 * 8)]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #(31 * 8)]

    // Read exception syndrome registers for C handler arguments
    mrs     x0, esr_el1                 // arg0: Exception Syndrome Register
    mrs     x1, far_el1                 // arg1: Fault Address Register
    mrs     x2, elr_el1                 // arg2: Exception Link Register

    // Extract Exception Class: EC = ESR_EL1[31:26]
    lsr     x3, x0, #26
    and     x3, x3, #0x3F

    // Route by EC value
    cmp     x3, #0x25                   // Data Abort from same EL
    b.eq    .Lcall_page_fault

    cmp     x3, #0x21                   // Instruction Abort from same EL
    b.eq    .Lcall_page_fault

    // All other exceptions: fatal → print diagnostics and halt
    // Pass saved x30 (frame offset 30*8=0xF0) as arg3 for diagnostics:
    // unhandled_exception_handler(esr, far, elr, saved_x30, frame_sp)
    ldr     x3, [sp, #(30 * 8)]        // arg3: saved x30 from faulting code
    mov     x4, sp                      // arg4: exception frame pointer
    bl      unhandled_exception_handler
    add     sp, sp, #(34 * 8)
    b       halt

.Lcall_page_fault:
    // x0=esr, x1=far, x2=elr already set by mrs above
    bl      kernel_page_fault_handler

    // If handler returns, the fault was resolved — restore and continue
    ldp     x0,  x1,  [sp, #(31 * 8)]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    ldp     x0,  x1,  [sp, #(0  * 8)]
    ldp     x2,  x3,  [sp, #(2  * 8)]
    ldp     x4,  x5,  [sp, #(4  * 8)]
    ldp     x6,  x7,  [sp, #(6  * 8)]
    ldp     x8,  x9,  [sp, #(8  * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(18 * 8)]
    ldp     x20, x21, [sp, #(20 * 8)]
    ldp     x22, x23, [sp, #(22 * 8)]
    ldp     x24, x25, [sp, #(24 * 8)]
    ldp     x26, x27, [sp, #(26 * 8)]
    ldp     x28, x29, [sp, #(28 * 8)]
    ldr     x30,       [sp, #(30 * 8)]

    add     sp, sp, #(34 * 8)
    eret
.size exception_handler, . - exception_handler

// =============================================================================
// lower_el_sync_dispatch — user-mode synchronous exception dispatch
// =============================================================================
//
// ALL synchronous exceptions from EL0 (user-space) arrive here.
// Saves the full 34-slot frame, reads ESR_EL1, extracts EC, and routes:
//
//   EC=0x15: SVC from AArch64 (system call)
//     → Remap x8/x0-x5 to C calling convention
//     → system_call_handler(nr, a0, a1, a2, a3, a4, a5)
//     → Store return value in saved x0 slot
//
//   EC=0x24: Data Abort from lower EL
//   EC=0x20: Instruction Abort from lower EL
//     → user_page_fault_handler(esr, far, elr)
//
//   Other EC: → unhandled_user_exception_handler(esr, far, elr)
// =============================================================================
.type lower_el_sync_dispatch, @function
lower_el_sync_dispatch:
    // Save full register frame
    sub     sp, sp, #(34 * 8)

    stp     x0,  x1,  [sp, #(0  * 8)]
    stp     x2,  x3,  [sp, #(2  * 8)]
    stp     x4,  x5,  [sp, #(4  * 8)]
    stp     x6,  x7,  [sp, #(6  * 8)]
    stp     x8,  x9,  [sp, #(8  * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(18 * 8)]
    stp     x20, x21, [sp, #(20 * 8)]
    stp     x22, x23, [sp, #(22 * 8)]
    stp     x24, x25, [sp, #(24 * 8)]
    stp     x26, x27, [sp, #(26 * 8)]
    stp     x28, x29, [sp, #(28 * 8)]
    str     x30,       [sp, #(30 * 8)]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #(31 * 8)]
    mrs     x0, sp_el0                  // save user-mode stack pointer
    str     x0,        [sp, #(33 * 8)]

    // Read ESR_EL1 and extract Exception Class
    mrs     x9, esr_el1                 // save ESR in callee-saved-safe x9
    lsr     x10, x9, #26               // EC = bits[31:26]
    and     x10, x10, #0x3F

    // ---- Route by Exception Class ----

    cmp     x10, #0x15                  // EC=0x15: SVC from AArch64
    b.eq    .Llower_syscall

    cmp     x10, #0x24                  // EC=0x24: Data Abort from lower EL
    b.eq    .Llower_page_fault

    cmp     x10, #0x20                  // EC=0x20: Instruction Abort from lower EL
    b.eq    .Llower_page_fault

    // ---- Unhandled user exception ----
    mov     x0, x9                      // arg0: ESR_EL1
    mrs     x1, far_el1                 // arg1: FAR_EL1
    mrs     x2, elr_el1                 // arg2: ELR_EL1
    bl      unhandled_user_exception_handler
    b       .Llower_restore_eret

.Llower_syscall:
    // ---- System call dispatch ----
    //
    // User registers are saved on the stack.  Remap to C calling convention:
    //   system_call_handler(nr, a0, a1, a2, a3, a4, a5, trap_frame)
    //
    // Saved register frame offsets:
    //   x0 at SP+(0*8), x1 at SP+(1*8), ..., x8 at SP+(8*8)
    //
    ldr     x0, [sp, #(8  * 8)]        // arg0 (nr)  ← saved x8
    ldr     x1, [sp, #(0  * 8)]        // arg1 (a0)  ← saved x0
    ldr     x2, [sp, #(1  * 8)]        // arg2 (a1)  ← saved x1
    ldr     x3, [sp, #(2  * 8)]        // arg3 (a2)  ← saved x2
    ldr     x4, [sp, #(3  * 8)]        // arg4 (a3)  ← saved x3
    ldr     x5, [sp, #(4  * 8)]        // arg5 (a4)  ← saved x4
    ldr     x6, [sp, #(5  * 8)]        // arg6 (a5)  ← saved x5
    mov     x7, sp                      // arg7: trap frame pointer for signal delivery
    bl      system_call_handler

    // Store return value into the saved x0 slot.
    // When we restore registers and eret, user-space will see this in x0.
    str     x0, [sp, #(0  * 8)]
    b       .Llower_restore_eret

.Llower_page_fault:
    // ---- User page fault dispatch ----
    mov     x0, x9                      // arg0: ESR_EL1
    mrs     x1, far_el1                 // arg1: FAR_EL1 (faulting address)
    mrs     x2, elr_el1                 // arg2: ELR_EL1 (faulting instruction)
    bl      user_page_fault_handler
    // Fall through — handler may have resolved the fault (mapped a page),
    // or terminated the process (scheduler picks next task on eret).
    b       .Llower_restore_eret

.Llower_restore_eret:
    // ---- Common restore path for all lower-EL exceptions ----
    // Restore user SP_EL0 first (may have changed via context_switch)
    ldr     x0,        [sp, #(33 * 8)]
    msr     sp_el0, x0

    ldp     x0,  x1,  [sp, #(31 * 8)]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    ldp     x0,  x1,  [sp, #(0  * 8)]
    ldp     x2,  x3,  [sp, #(2  * 8)]
    ldp     x4,  x5,  [sp, #(4  * 8)]
    ldp     x6,  x7,  [sp, #(6  * 8)]
    ldp     x8,  x9,  [sp, #(8  * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(18 * 8)]
    ldp     x20, x21, [sp, #(20 * 8)]
    ldp     x22, x23, [sp, #(22 * 8)]
    ldp     x24, x25, [sp, #(24 * 8)]
    ldp     x26, x27, [sp, #(26 * 8)]
    ldp     x28, x29, [sp, #(28 * 8)]
    ldr     x30,       [sp, #(30 * 8)]

    add     sp, sp, #(34 * 8)
    eret
.size lower_el_sync_dispatch, . - lower_el_sync_dispatch

// =============================================================================
// Exported linker symbols and external function declarations
// =============================================================================
.global _bss_start_addr, _bss_end_addr, _stack_top_addr
.global exception_vectors

.extern system_call_handler
.extern irq_handler_c
.extern kernel_page_fault_handler
.extern unhandled_exception_handler
.extern user_page_fault_handler
.extern unhandled_user_exception_handler
