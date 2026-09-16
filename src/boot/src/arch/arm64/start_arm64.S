#include "moss/trap_frame_offsets.h"

/*
 * =============================================================================
 * ARM64 Kernel Boot and Exception Handling
 * =============================================================================
 *
 * This is the primary assembly file for the ARM64 MOSS kernel.  It handles:
 *
 *   1. BOOT SEQUENCE
 *      - Linux ARM64 Image boot protocol
 *      - Exception level descent (EL3 path caveat documented at el3_entry)
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
 *      - Saves 34-slot register frame (x0-x30 + ELR + SPSR + interrupted SP)
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
 * DTB is supplied through x0 by the ARM64 Linux Image boot protocol.
 * No machine-specific memory scan or early MMIO is performed.
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
 *   33    0x108     interrupted SP (SP_EL0 for user-origin exceptions)
 *
 * == Stack Layout ==
 *
 *   Linker reserves 512KB: 16 CPUs * 32KB; ASM_MAX_CPUS must fit that reserve.
 *   CPU 0 stack: _stack_top → _stack_top - 32KB
 *   CPU N stack: _stack_top - N*32KB → _stack_top - (N+1)*32KB
 *
 * External C/C++ functions called:
 *   early_main(dtb_ptr)              — primary CPU entry (boot module)
 *   secondary_cpu_entry()            — secondary CPU entry (boot module)
 *   irq_handler_c()                  — interrupt handler (interrupts module)
 *   system_call_handler(frame)       — native TrapFrame syscall dispatcher
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
// protocol. The loader recognizes the Image magic and passes the runtime
// hardware description (DTB) in x0. Image placement belongs to the loader.
//
// The `b _start` instruction at offset 0 branches over the header to the
// actual kernel entry point.  For ELF boot (where this header is not at the
// file start), _start is reached directly via the ELF entry point.
// =============================================================================
_linux_header:
    b       _start                      // code0: branch to actual entry
    .word   0                           // code1: reserved
    .quad   0                           // text_offset: zero offset from loader-selected aligned base
    .quad   _kernel_image_size          // image_size: filled by linker
    .quad   10                          // flags: little-endian, 4K pages, arbitrary placement
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
//   MPIDR_EL1 = firmware hardware affinity (boot affinity need not be zero)
//   CurrentEL = EL3, EL2, or EL1 depending on firmware
// =============================================================================
_start:
    // Preserve x0 (potential DTB pointer) in callee-saved x20.
    // Linux boot protocol: bootloader passes DTB address via x0.
    mov     x20, x0

    mov     x19, #0                     // Logical boot CPU, irrespective of MPIDR.
    b       boot_exception_level

.global arm64_secondary_start
arm64_secondary_start:
    // PSCI and spin-table both enter here. Resolve affinity before using a stack.
    mrs     x2, mpidr_el1
    ubfx    x3, x2, #32, #8
    and     x2, x2, #0xFFFFFF
    bfi     x2, x3, #32, #8
    adrp    x1, boot_cpu_hardware_ids
    add     x1, x1, :lo12:boot_cpu_hardware_ids
    mov     x19, #1
1:
    cmp     x19, #ASM_MAX_CPUS
    b.hs    halt
    ldr     x3, [x1, x19, lsl #3]
    cmp     x2, x3
    b.eq    boot_exception_level
    add     x19, x19, #1
    b       1b

boot_exception_level:
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
// Legacy EL3 descent path
// =============================================================================
//
// Configure Secure Configuration Register (SCR_EL3):
//   0x431 = RW (bit 10) | NS (bit 0) | legacy RES1 bits [5:4].
// HCE (bit 8) and SMD (bit 7) are clear; this value does not enable HVC.
// The return mode below selects EL1h despite targeting the el2_entry label;
// entry from EL3 needs a separate mode/register-access correctness review.
// =============================================================================
el3_entry:
    mov     x0, #0x431
    msr     scr_el3, x0

    // SPSR.M=5 selects EL1h, with FIQ/IRQ/SError/debug masked; see caveat above.
    mov     x0, #(SPSR_EL2_F | SPSR_EL2_I | SPSR_EL2_A | SPSR_EL2_D | SPSR_EL2_EL1h)
    msr     spsr_el3, x0

    // Set EL3 return address to el2_entry
    adr     x0, el2_entry
    msr     elr_el3, x0

    eret                                // return mode follows SPSR_EL3, not label name

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

    // Only access GIC system registers when the CPU implements them.
    mrs     x0, id_aa64pfr0_el1
    ubfx    x0, x0, #24, #4
    cbz     x0, 1f
    // Enable GICv3 system register access before dropping to EL1.
    // ICC_SRE_EL2 = S3_4_C12_C9_5: SRE/DFB/DIB (bits 0..2) plus
    // Enable (bit 3) = 0xf, permitting EL1 access to its own SRE controls.
    // This allows EL1 to use ICC_*_EL1 system registers for GICv3 CPU interface.
    mov     x0, #0xF
    msr     S3_4_C12_C9_5, x0
    isb
1:

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
    // relocations, etc.). C++ activates their per-CPU MMU using shared tables.
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

    // The image is linked at zero. Patch only ELF64 RELATIVE relocations;
    // PC-relative code already runs at the firmware-selected physical address.
    // ELF64 Rela is three 8-byte fields (offset/info/addend): the 16+8 byte
    // loads advance one 24-byte entry. Only type 1027 with symbol zero is allowed.
    adr     x4, _linux_header
    adrp    x0, _rela_start
    add     x0, x0, :lo12:_rela_start
    adrp    x1, _rela_end
    add     x1, x1, :lo12:_rela_end
1:
    cmp     x0, x1
    b.hs    2f
    ldp     x2, x3, [x0], #16
    ldr     x5, [x0], #8
    cmp     x3, #1027                // R_AARCH64_RELATIVE, symbol index zero
    b.ne    halt
    add     x2, x2, x4
    add     x5, x5, x4
    str     x5, [x2]
    b       1b
2:
    dsb     sy
    isb

    // Zero BSS section (CPU 0 only — secondary CPUs skip this)
    ldr     x0, =_bss_start_addr
    ldr     x1, =_bss_end_addr
clear_bss:
    cmp     x0, x1
    b.hs    bss_cleared
    str     xzr, [x0], #8              // store 8 bytes of zeros, advance pointer
    b       clear_bss
bss_cleared:

    // CPU 0 uses the top 32 KiB slice of the linker's 512 KiB stack reserve.
    ldr     x0, =_stack_top_addr
    mov     sp, x0


    b       primary_cpu_continue

// =============================================================================
// Secondary CPU entry (firmware lands at arm64_secondary_start)
// =============================================================================
//
// Secondary CPUs use their own EL descent; the EL3 mode caveat above applies.
// They must NOT clear BSS. C++ configures their MMU after CPU 0 publishes tables.
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


    b       secondary_cpu_park

// =============================================================================
// Primary CPU continues — DTB discovery and jump to C++
// =============================================================================
primary_cpu_continue:


    // x20 is the DTB supplied by the boot protocol; C++ validates it.
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

    // Validate CPU ID is within supported range
    cmp     x19, #ASM_MAX_CPUS          // Configured maximum CPUs
    b.hs    invalid_cpu_id


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
// On AArch64 .align is a power of two: 8 reserves 256-byte alignment here.
// Startup records only require u64 alignment; the extra alignment's rationale
// is not recorded and must not be confused with an eight-byte stride.
.align 8
cpu_startup_flags:
    // Per-CPU startup control block: [entry_point: 8B] [startup_flag: 8B]
    // ASM_MAX_CPUS CPUs, each CPU gets 16 bytes
    .space ASM_MAX_CPUS * 16

.global boot_cpu_hardware_ids
boot_cpu_hardware_ids:
    .space ASM_MAX_CPUS * 8

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
    sub     sp, sp, #MOSS_TF_SIZE

    // Save all general-purpose registers
    stp     x0,  x1,  [sp, #MOSS_TF_X0]
    stp     x2,  x3,  [sp, #MOSS_TF_X2]
    stp     x4,  x5,  [sp, #MOSS_TF_X4]
    stp     x6,  x7,  [sp, #MOSS_TF_X6]
    stp     x8,  x9,  [sp, #MOSS_TF_X8]
    stp     x10, x11, [sp, #MOSS_TF_X10]
    stp     x12, x13, [sp, #MOSS_TF_X12]
    stp     x14, x15, [sp, #MOSS_TF_X14]
    stp     x16, x17, [sp, #MOSS_TF_X16]
    stp     x18, x19, [sp, #MOSS_TF_X18]
    stp     x20, x21, [sp, #MOSS_TF_X20]
    stp     x22, x23, [sp, #MOSS_TF_X22]
    stp     x24, x25, [sp, #MOSS_TF_X24]
    stp     x26, x27, [sp, #MOSS_TF_X26]
    stp     x28, x29, [sp, #MOSS_TF_X28]
    str     x30,       [sp, #MOSS_TF_X30]

    // Save exception return state + user stack pointer
    mrs     x0, elr_el1                 // exception return address
    mrs     x1, spsr_el1                // saved processor state
    stp     x0,  x1,  [sp, #MOSS_TF_PC]
    tst     x1, #15
    b.eq    .Lirq_user_sp
    add     x0, sp, #MOSS_TF_SIZE       // interrupted kernel SP
    b       .Lirq_save_sp
.Lirq_user_sp:
    mrs     x0, sp_el0
.Lirq_save_sp:
    str     x0,        [sp, #MOSS_TF_SP]

    // Call C++ interrupt handler
    bl      irq_handler_c
    msr     daifset, #0xf
    mov     x0, sp
    bl      user_return_handler

    // C handlers may schedule and resume with IRQs enabled. Keep return-state
    // publication atomic: an IRQ after MSR ELR/SPSR would overwrite that state.
    // ERET restores the interrupted context's original interrupt mask.
    msr     daifset, #2

    // Restore user stack pointer (before GP regs clobber temporaries)
    ldr     x1,        [sp, #MOSS_TF_STATUS]
    tst     x1, #15
    b.ne    .Lirq_restore_state
    ldr     x0,        [sp, #MOSS_TF_SP]
    msr     sp_el0, x0

    // Restore exception return state
.Lirq_restore_state:
    ldp     x0,  x1,  [sp, #MOSS_TF_PC]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    // Restore all general-purpose registers
    ldp     x0,  x1,  [sp, #MOSS_TF_X0]
    ldp     x2,  x3,  [sp, #MOSS_TF_X2]
    ldp     x4,  x5,  [sp, #MOSS_TF_X4]
    ldp     x6,  x7,  [sp, #MOSS_TF_X6]
    ldp     x8,  x9,  [sp, #MOSS_TF_X8]
    ldp     x10, x11, [sp, #MOSS_TF_X10]
    ldp     x12, x13, [sp, #MOSS_TF_X12]
    ldp     x14, x15, [sp, #MOSS_TF_X14]
    ldp     x16, x17, [sp, #MOSS_TF_X16]
    ldp     x18, x19, [sp, #MOSS_TF_X18]
    ldp     x20, x21, [sp, #MOSS_TF_X20]
    ldp     x22, x23, [sp, #MOSS_TF_X22]
    ldp     x24, x25, [sp, #MOSS_TF_X24]
    ldp     x26, x27, [sp, #MOSS_TF_X26]
    ldp     x28, x29, [sp, #MOSS_TF_X28]
    ldr     x30,       [sp, #MOSS_TF_X30]

    // Deallocate frame and return from exception
    add     sp, sp, #MOSS_TF_SIZE
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
    sub     sp, sp, #MOSS_TF_SIZE

    stp     x0,  x1,  [sp, #MOSS_TF_X0]
    stp     x2,  x3,  [sp, #MOSS_TF_X2]
    stp     x4,  x5,  [sp, #MOSS_TF_X4]
    stp     x6,  x7,  [sp, #MOSS_TF_X6]
    stp     x8,  x9,  [sp, #MOSS_TF_X8]
    stp     x10, x11, [sp, #MOSS_TF_X10]
    stp     x12, x13, [sp, #MOSS_TF_X12]
    stp     x14, x15, [sp, #MOSS_TF_X14]
    stp     x16, x17, [sp, #MOSS_TF_X16]
    stp     x18, x19, [sp, #MOSS_TF_X18]
    stp     x20, x21, [sp, #MOSS_TF_X20]
    stp     x22, x23, [sp, #MOSS_TF_X22]
    stp     x24, x25, [sp, #MOSS_TF_X24]
    stp     x26, x27, [sp, #MOSS_TF_X26]
    stp     x28, x29, [sp, #MOSS_TF_X28]
    str     x30,       [sp, #MOSS_TF_X30]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #MOSS_TF_PC]
    add     x0, sp, #MOSS_TF_SIZE
    str     x0, [sp, #MOSS_TF_SP]

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
    ldr     x3, [sp, #MOSS_TF_X30]        // arg3: saved x30 from faulting code
    mov     x4, sp                      // arg4: exception frame pointer
    bl      unhandled_exception_handler
    add     sp, sp, #MOSS_TF_SIZE
    b       halt

.Lcall_page_fault:
    // x0=esr, x1=far, x2=elr already set by mrs above
    mov     x3, sp                      // native frame for a contained uaccess fixup
    bl      kernel_page_fault_handler

    // If handler returns, the fault was resolved — restore and continue
    msr     daifset, #2                 // protect ELR/SPSR until ERET
    ldp     x0,  x1,  [sp, #MOSS_TF_PC]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    ldp     x0,  x1,  [sp, #MOSS_TF_X0]
    ldp     x2,  x3,  [sp, #MOSS_TF_X2]
    ldp     x4,  x5,  [sp, #MOSS_TF_X4]
    ldp     x6,  x7,  [sp, #MOSS_TF_X6]
    ldp     x8,  x9,  [sp, #MOSS_TF_X8]
    ldp     x10, x11, [sp, #MOSS_TF_X10]
    ldp     x12, x13, [sp, #MOSS_TF_X12]
    ldp     x14, x15, [sp, #MOSS_TF_X14]
    ldp     x16, x17, [sp, #MOSS_TF_X16]
    ldp     x18, x19, [sp, #MOSS_TF_X18]
    ldp     x20, x21, [sp, #MOSS_TF_X20]
    ldp     x22, x23, [sp, #MOSS_TF_X22]
    ldp     x24, x25, [sp, #MOSS_TF_X24]
    ldp     x26, x27, [sp, #MOSS_TF_X26]
    ldp     x28, x29, [sp, #MOSS_TF_X28]
    ldr     x30,       [sp, #MOSS_TF_X30]

    add     sp, sp, #MOSS_TF_SIZE
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
//     → system_call_handler(TrapFrame*) reads saved x8/x0-x5 and writes x0
//
//   EC=0x24: Data Abort from lower EL
//   EC=0x20: Instruction Abort from lower EL
//     → user_page_fault_handler(esr, far, elr)
//
//   Other EC: → unhandled_user_exception_handler(esr, far, elr)
// =============================================================================
.global lower_el_sync_dispatch
.type lower_el_sync_dispatch, @function
lower_el_sync_dispatch:
    // Save full register frame
    sub     sp, sp, #MOSS_TF_SIZE

    stp     x0,  x1,  [sp, #MOSS_TF_X0]
    stp     x2,  x3,  [sp, #MOSS_TF_X2]
    stp     x4,  x5,  [sp, #MOSS_TF_X4]
    stp     x6,  x7,  [sp, #MOSS_TF_X6]
    stp     x8,  x9,  [sp, #MOSS_TF_X8]
    stp     x10, x11, [sp, #MOSS_TF_X10]
    stp     x12, x13, [sp, #MOSS_TF_X12]
    stp     x14, x15, [sp, #MOSS_TF_X14]
    stp     x16, x17, [sp, #MOSS_TF_X16]
    stp     x18, x19, [sp, #MOSS_TF_X18]
    stp     x20, x21, [sp, #MOSS_TF_X20]
    stp     x22, x23, [sp, #MOSS_TF_X22]
    stp     x24, x25, [sp, #MOSS_TF_X24]
    stp     x26, x27, [sp, #MOSS_TF_X26]
    stp     x28, x29, [sp, #MOSS_TF_X28]
    str     x30,       [sp, #MOSS_TF_X30]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #MOSS_TF_PC]
    mrs     x0, sp_el0                  // save user-mode stack pointer
    str     x0,        [sp, #MOSS_TF_SP]

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
    mov     x0, sp
    bl      system_call_handler
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
    msr     daifset, #0xf
    mov     x0, sp
    bl      user_return_handler
    // ---- Common restore path for all lower-EL exceptions ----
    // A blocking syscall can resume with IRQs enabled. Mask before publishing
    // ELR/SPSR so nested IRQs cannot replace the user return with a kernel PC.
    msr     daifset, #2
    // Restore user SP_EL0 first (may have changed via context_switch)
    ldr     x0,        [sp, #MOSS_TF_SP]
    msr     sp_el0, x0

    ldp     x0,  x1,  [sp, #MOSS_TF_PC]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    ldp     x0,  x1,  [sp, #MOSS_TF_X0]
    ldp     x2,  x3,  [sp, #MOSS_TF_X2]
    ldp     x4,  x5,  [sp, #MOSS_TF_X4]
    ldp     x6,  x7,  [sp, #MOSS_TF_X6]
    ldp     x8,  x9,  [sp, #MOSS_TF_X8]
    ldp     x10, x11, [sp, #MOSS_TF_X10]
    ldp     x12, x13, [sp, #MOSS_TF_X12]
    ldp     x14, x15, [sp, #MOSS_TF_X14]
    ldp     x16, x17, [sp, #MOSS_TF_X16]
    ldp     x18, x19, [sp, #MOSS_TF_X18]
    ldp     x20, x21, [sp, #MOSS_TF_X20]
    ldp     x22, x23, [sp, #MOSS_TF_X22]
    ldp     x24, x25, [sp, #MOSS_TF_X24]
    ldp     x26, x27, [sp, #MOSS_TF_X26]
    ldp     x28, x29, [sp, #MOSS_TF_X28]
    ldr     x30,       [sp, #MOSS_TF_X30]

    add     sp, sp, #MOSS_TF_SIZE
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
