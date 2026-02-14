// MOSS Page Fault Handler
//
// Handles kernel-mode page faults (Data Abort EC=0x25, Instruction Abort EC=0x21)
// and prints diagnostic information for all other unhandled exceptions.
//
// Called from the ARM64 exception vector table in start_arm64.S.
// x86_64 and RISC-V stubs are provided for link compatibility.

module;

#include "arch_detect.h"

// extern "C" handler symbols called from assembly
extern "C" void kernel_page_fault_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept;

extern "C" void unhandled_exception_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept;

// User-mode exception handlers (called from lower_el_sync_dispatch)
extern "C" [[noreturn]] void user_page_fault_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept;

extern "C" [[noreturn]] void unhandled_user_exception_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept;

module moss.mm;

namespace moss::kernel::mm {

// ============================================================================
// EC (Exception Class) to human-readable string
// ============================================================================
static auto ec_to_string(u64 ec) noexcept -> const char* {
    switch (ec) {
    case 0x00: return "Unknown reason";
    case 0x01: return "WFI/WFE trapped";
    case 0x03: return "MCR/MRC (cp15)";
    case 0x04: return "MCRR/MRRC (cp15)";
    case 0x05: return "MCR/MRC (cp14)";
    case 0x06: return "LDC/STC access (cp14)";
    case 0x07: return "SVE/SIMD/FP trapped";
    case 0x0C: return "MRRC (cp14)";
    case 0x0E: return "Illegal execution state";
    case 0x11: return "SVC from AArch32";
    case 0x15: return "SVC from AArch64";
    case 0x18: return "MSR/MRS/system instr trapped";
    case 0x19: return "SVE access trapped";
    case 0x20: return "Instruction Abort (lower EL)";
    case 0x21: return "Instruction Abort (same EL)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data Abort (lower EL)";
    case 0x25: return "Data Abort (same EL)";
    case 0x26: return "SP alignment fault";
    case 0x28: return "FP exception (AArch32)";
    case 0x2C: return "FP exception (AArch64)";
    case 0x2F: return "SError interrupt";
    case 0x30: return "Breakpoint (lower EL)";
    case 0x31: return "Breakpoint (same EL)";
    case 0x32: return "Software Step (lower EL)";
    case 0x33: return "Software Step (same EL)";
    case 0x34: return "Watchpoint (lower EL)";
    case 0x35: return "Watchpoint (same EL)";
    case 0x38: return "BKPT (AArch32)";
    case 0x3C: return "BRK (AArch64)";
    default:   return "Other/reserved";
    }
}

// ============================================================================
// DFSC/IFSC (Data/Instruction Fault Status Code) to string
// ============================================================================
static auto dfsc_to_string(u64 dfsc) noexcept -> const char* {
    switch (dfsc & 0x3F) {
    case 0x00: return "Address size fault (L0)";
    case 0x01: return "Address size fault (L1)";
    case 0x02: return "Address size fault (L2)";
    case 0x03: return "Address size fault (L3)";
    case 0x04: return "Translation fault (L0)";
    case 0x05: return "Translation fault (L1)";
    case 0x06: return "Translation fault (L2)";
    case 0x07: return "Translation fault (L3)";
    case 0x08: return "Access flag fault (L0)";
    case 0x09: return "Access flag fault (L1)";
    case 0x0A: return "Access flag fault (L2)";
    case 0x0B: return "Access flag fault (L3)";
    case 0x0C: return "Permission fault (L0)";
    case 0x0D: return "Permission fault (L1)";
    case 0x0E: return "Permission fault (L2)";
    case 0x0F: return "Permission fault (L3)";
    case 0x10: return "Synchronous external abort";
    case 0x14: return "Synchronous external abort (L0)";
    case 0x15: return "Synchronous external abort (L1)";
    case 0x16: return "Synchronous external abort (L2)";
    case 0x17: return "Synchronous external abort (L3)";
    case 0x21: return "Alignment fault";
    default:   return "Other/reserved DFSC";
    }
}

} // namespace moss::kernel::mm

// ============================================================================
// Unhandled exception handler — print diagnostics, then return to asm (halt)
// ============================================================================
extern "C" void unhandled_exception_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept {
    namespace log = moss::kernel::logging;
    using moss::u64;

    u64 ec  = (esr >> 26) & 0x3F;
    u64 iss = esr & 0x1FFFFFF;

    log::klog::panic("=== UNHANDLED EXCEPTION ===");
    log::klog::panic("EC:  {:#x} ({})", ec, moss::kernel::mm::ec_to_string(ec));
    log::klog::panic("ISS: {:#x}", iss);
    log::klog::panic("ESR: {:#x}", esr);
    log::klog::panic("FAR: {:#x}", far_addr);
    log::klog::panic("ELR: {:#x}", elr);
    // Returns to asm which executes `b halt`
}

// ============================================================================
// Kernel page fault handler — handle translation faults with demand zero
// ============================================================================
extern "C" void kernel_page_fault_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept {
    namespace log = moss::kernel::logging;
    namespace mm  = moss::kernel::mm;
    using moss::u64;

    u64 ec   = (esr >> 26) & 0x3F;
    u64 dfsc = esr & 0x3F;
    [[maybe_unused]] bool is_write = ((esr >> 6) & 1) != 0;

    log::klog::warn("page fault: addr={:#x} pc={:#x} write={:#b} dfsc={:#x} ({})",
                    far_addr, elr, is_write,
                    dfsc, mm::dfsc_to_string(dfsc));

    // Translation faults: DFSC 0x04-0x07 (L0-L3 translation miss)
    bool is_translation_fault = (dfsc >= 0x04 && dfsc <= 0x07);

    // Permission faults: DFSC 0x0C-0x0F (page exists, permission denied)
    bool is_permission_fault  = (dfsc >= 0x0C && dfsc <= 0x0F);

    if (is_translation_fault) {
        // Check if the faulting address has a valid page table mapping already.
        // For the boot identity mapping (1GB blocks), query_page will return
        // mapped=true, so we won't reach here for normal kernel addresses.
        auto info = mm::PageTableManager::query_page(far_addr);
        if (info.mapped) {
            // Address is mapped but we got a translation fault — this shouldn't
            // happen. Could be a stale TLB entry; try invalidating and retrying.
            mm::PageTableManager::invalidate_tlb_addr(far_addr);
            log::klog::warn("page fault: addr={:#x} was mapped (level={}), TLB invalidated — retrying",
                            far_addr, static_cast<moss::u32>(info.level));
            return; // eret will retry the faulting instruction
        }

        // Address is truly unmapped — for now, no demand-zero VMA lookup
        // because the kernel runs on boot identity mapping (1GB blocks).
        // This is a genuine fault in kernel code.
        log::klog::panic("KERNEL PAGE FAULT: no mapping for addr={:#x}", far_addr);
        log::klog::panic("  EC:   {:#x} ({})", ec, mm::ec_to_string(ec));
        log::klog::panic("  DFSC: {:#x} ({})", dfsc, mm::dfsc_to_string(dfsc));
        log::klog::panic("  ELR:  {:#x}", elr);
        // Return to asm; since we used panic, the log is flushed.
        // The asm side will halt after this returns if we don't eret.
        // For safety, we loop here (asm will also halt).
        while (true) {
#if defined(MOSS_ARCH_ARM64)
            asm volatile("wfi");
#endif
        }
    }

    if (is_permission_fault) {
        // Permission fault — future: COW handling
        log::klog::panic("KERNEL PERMISSION FAULT: addr={:#x} pc={:#x} write={:#b}",
                         far_addr, elr, is_write);
        log::klog::panic("  DFSC: {:#x} ({})", dfsc, mm::dfsc_to_string(dfsc));
        while (true) {
#if defined(MOSS_ARCH_ARM64)
            asm volatile("wfi");
#endif
        }
    }

    // Other DFSC values (alignment, external abort, etc.)
    log::klog::panic("KERNEL FAULT: unhandled dfsc={:#x} ({}) addr={:#x} pc={:#x}",
                     dfsc, mm::dfsc_to_string(dfsc), far_addr, elr);
    while (true) {
#if defined(MOSS_ARCH_ARM64)
        asm volatile("wfi");
#endif
    }
}

// ============================================================================
// User-mode page fault handler
//
// Called from lower_el_sync_dispatch when EC=0x24 (Data Abort, lower EL) or
// EC=0x20 (Instruction Abort, lower EL).
//
// MVP behaviour: log diagnostics and terminate the faulting user process.
// The kernel does NOT panic — it simply kills the offending process and
// lets the scheduler pick the next runnable task.
//
// Future: demand paging, COW, stack growth, mmap fault-in.
// ============================================================================
extern "C" [[noreturn]] void user_page_fault_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept {
    namespace log = moss::kernel::logging;
    namespace mm  = moss::kernel::mm;
    using moss::u64;

    u64 ec   = (esr >> 26) & 0x3F;
    u64 dfsc = esr & 0x3F;
    bool is_write = ((esr >> 6) & 1) != 0;

    log::klog::error("USER PAGE FAULT: addr={:#x} pc={:#x} write={} ec={:#x} ({})",
                     far_addr, elr, is_write,
                     ec, mm::ec_to_string(ec));
    log::klog::error("  DFSC: {:#x} ({})", dfsc, mm::dfsc_to_string(dfsc));

    // TODO: Once VMA regions are tracked per-process, look up the faulting
    // address in the process VMA list and handle demand paging / COW here.
    // For now, any user page fault is fatal to the process.

    log::klog::error("  Terminating user process (SIGSEGV equivalent)");

    // TODO: Properly terminate the current user process via the scheduler.
    // For MVP, we just log and return — the eret will re-execute the
    // faulting instruction, which will fault again. To prevent an infinite
    // loop, we advance ELR past the faulting instruction (skip 4 bytes on
    // ARM64) and set x0 to an error indicator. In practice, the user
    // program will crash on the next instruction, but the kernel stays alive.
    //
    // A proper implementation would: mark process as killed, switch to
    // scheduler, never return to this user context.

    // For now: halt the CPU to prevent infinite fault loop.
    // This is temporary until we have proper process termination.
    log::klog::error("  Halting CPU (user process killed)");
    while (true) {
#if defined(MOSS_ARCH_ARM64)
        asm volatile("wfi");
#endif
    }
}

// ============================================================================
// Unhandled user-mode exception handler
//
// Called for any Lower EL synchronous exception that is NOT an SVC, Data
// Abort, or Instruction Abort (e.g., SP alignment fault, illegal execution
// state, FP trap, etc.).
// ============================================================================
extern "C" [[noreturn]] void unhandled_user_exception_handler(
    unsigned long long esr,
    unsigned long long far_addr,
    unsigned long long elr) noexcept {
    namespace log = moss::kernel::logging;
    using moss::u64;

    u64 ec  = (esr >> 26) & 0x3F;
    u64 iss = esr & 0x1FFFFFF;

    log::klog::error("=== UNHANDLED USER EXCEPTION ===");
    log::klog::error("EC:  {:#x} ({})", ec, moss::kernel::mm::ec_to_string(ec));
    log::klog::error("ISS: {:#x}", iss);
    log::klog::error("ESR: {:#x}", esr);
    log::klog::error("FAR: {:#x}", far_addr);
    log::klog::error("ELR: {:#x}", elr);
    log::klog::error("Terminating user process");

    // Same temporary halt as user_page_fault_handler.
    // Proper implementation: kill process, return to scheduler.
    while (true) {
#if defined(MOSS_ARCH_ARM64)
        asm volatile("wfi");
#endif
    }
}
