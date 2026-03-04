# Signal Mechanism Design — User-Space Handler Delivery

## Overview

Implement POSIX-compatible signal delivery with user-space handler execution via the classic sigframe approach. This completes the P1 signal mechanism that is currently half-implemented (send/mask/kill work, but user handlers are never invoked).

## Current State

**Working:**
- `send_signal()` — sets pending bit in `thread->pending_signals`
- `dequeue_signal()` — lowest-numbered-first dequeue
- `kill()` — full implementation (pid > 0, == 0, == -1, < -1)
- `sigaction()` — registers user handler in `SignalState::actions[]`
- `sigprocmask()` — modifies `thread->signal_mask`
- `do_signal_checkpoint()` — called on syscall return path, processes SIG_DFL/SIG_IGN
- Signal constants, default actions, blocked signal check

**Missing:**
- User handler invocation (sigframe construction on user stack)
- `sigreturn` syscall (restore context from sigframe)
- `sigaltstack` syscall (alternate signal stack)
- Assembly changes to pass frame pointer to C code
- User-space test program

## Approach: Classic Sigframe

When a pending signal has a user-space handler:
1. Save the interrupted user context (GP regs + NEON + PC/SPSR/SP) into a `SignalFrame` on the user stack
2. Redirect execution to the handler by modifying the exception return registers
3. Handler returns via a trampoline that invokes `sigreturn`
4. `sigreturn` restores the original context from the sigframe

This approach supports nested signals naturally (each nesting level pushes another sigframe).

## SignalFrame Layout (ARM64)

Total size: 848 bytes (16-byte aligned).

```
Offset  Size   Field
──────  ─────  ──────────────────────────────────
0x000   8      magic (0xDEAD_5164_5346_524D)
0x008   248    x0-x30 (31 × 8 bytes GP registers)
0x100   8      saved ELR_EL1 (interrupted PC)
0x108   8      saved SPSR_EL1
0x110   8      saved SP_EL0 (interrupted user SP)
0x118   8      FPSR
0x120   8      FPCR
0x128   512    Q0-Q31 (32 × 16 bytes NEON registers)
0x328   8      signo
0x330   8      saved signal_mask
0x338   8      sigreturn trampoline: mov x8, #17; svc #0
──────  ─────
0x340   =832   Total → pad to 848 (0x350) for 16-byte alignment
```

Note: The trampoline is embedded at the top of the frame. When the handler returns (BL → LR), LR points to the trampoline which triggers `sigreturn`.

## Signal Delivery Flow

```
system_call_handler() returns
        │
        ▼
  signal_pending(thread)?  ──no──→  .Llower_restore_eret (normal return)
        │yes
        ▼
  dequeue_signal() → signo
        │
  lookup sigaction[signo]
        │
  handler == SIG_DFL → do_signal_default()
  handler == SIG_IGN → skip
  handler == user fn  → setup_sigframe()
        │
        ▼
  setup_sigframe(thread, signo, handler, trap_frame_ptr):
    1. Read interrupted context from assembly trap frame (34-slot on kernel stack)
    2. Choose signal stack:
       - If SA_ONSTACK && altstack configured → use altstack
       - Otherwise → use interrupted SP_EL0
    3. Allocate sigframe on chosen stack: new_sp = stack_top - SIGFRAME_SIZE (aligned)
    4. Build SignalFrame in kernel buffer:
       - Write MAGIC
       - Copy x0-x30, ELR, SPSR, SP_EL0, FPSR, FPCR, Q0-Q31
       - Write signo, thread->signal_mask
       - Write trampoline: 0xD2800228 (mov x8, #17), 0xD4000001 (svc #0)
    5. copy_to_user(new_sp, &sigframe, sizeof(sigframe))
    6. Modify assembly trap frame for handler dispatch:
       - trap_frame[31] (ELR slot) = handler address
       - trap_frame[33] (SP_EL0 slot) = new_sp
       - trap_frame[0] (x0 slot) = signo
       - trap_frame[30] (x30/LR slot) = &trampoline in sigframe
    7. Block signals: thread->signal_mask |= sa.mask | sigmask(signo)
        │
        ▼
  .Llower_restore_eret → eret to handler with signo in x0
```

## sigreturn Syscall (Number 17)

```
sys_sigreturn(trap_frame_ptr):
    1. Read current user SP_EL0 from trap frame
    2. copy_from_user: read SignalFrame from user SP
    3. Validate MAGIC field → -EFAULT if invalid
    4. Restore to assembly trap frame:
       - x0-x30 from sigframe
       - ELR, SPSR, SP_EL0 from sigframe
       - FPSR, FPCR, Q0-Q31 from sigframe (via abi bridge to restore NEON)
    5. Restore thread->signal_mask from sigframe.saved_signal_mask
    6. Return (trap frame now has original interrupted context)
       → .Llower_restore_eret → eret to original code point
```

Special: `sigreturn` does NOT store its return value into trap_frame[0] — the x0 in the sigframe IS the correct return value (it was the x0 at the time of interruption).

## Assembly Changes

### lower_el_sync_dispatch

Pass kernel stack frame pointer as 8th argument to `system_call_handler()`:

```asm
.Llower_syscall:
    ldr     x0, [sp, #(8  * 8)]   // nr  ← saved x8
    ldr     x1, [sp, #(0  * 8)]   // a0  ← saved x0
    ldr     x2, [sp, #(1  * 8)]   // a1  ← saved x1
    ldr     x3, [sp, #(2  * 8)]   // a2  ← saved x2
    ldr     x4, [sp, #(3  * 8)]   // a3  ← saved x3
    ldr     x5, [sp, #(4  * 8)]   // a4  ← saved x4
    ldr     x6, [sp, #(5  * 8)]   // a5  ← saved x5
    mov     x7, sp                 // ★ NEW: trap frame pointer
    bl      system_call_handler
```

### system_call_handler signature change

```c++
extern "C" long system_call_handler(long nr, long a0, long a1, long a2,
                                     long a3, long a4, long a5,
                                     long trap_frame_ptr) noexcept;
```

The `trap_frame_ptr` is passed through to:
- `setup_sigframe()` in signal delivery
- `sys_sigreturn()` to restore context

### NEON state in trap frame

Current trap frame only saves x0-x30 + ELR + SPSR + SP_EL0 (34 slots). For signal delivery with NEON, two options:

**Option chosen: Read/write NEON via dedicated functions.** `setup_sigframe()` uses inline assembly to read current NEON state (which is still the interrupted user's NEON state since kernel code does not use NEON). `sys_sigreturn()` restores NEON via inline assembly. This avoids expanding the trap frame.

## sigaltstack Syscall

```c++
// New syscall: sigaltstack(ss, old_ss)
// Syscall number: to be assigned (use an unused slot)

struct UserStack {
    unsigned long ss_sp;    // stack base
    unsigned long ss_size;  // stack size
    unsigned long ss_flags; // SS_DISABLE=2, SS_ONSTACK=1
};

// Thread struct addition:
struct Thread {
    // ... existing fields ...
    VirtAddr alt_stack_sp{0};
    usize alt_stack_size{0};
    u32 alt_stack_flags{2}; // SS_DISABLE by default
    bool on_alt_stack{false}; // true when currently executing on altstack
};
```

Stack selection during signal delivery:
1. If `sigaction.flags & SA_ONSTACK` and `alt_stack_flags != SS_DISABLE` and not already `on_alt_stack`:
   - Use altstack: `new_sp = alt_stack_sp + alt_stack_size`
   - Set `on_alt_stack = true`
2. Otherwise: use current user SP

## Sigaction Flags

Extend `UserSigaction` and `Sigaction` to support:
- `SA_ONSTACK (0x1)` — use alternate signal stack
- `SA_RESTART (0x2)` — restart interrupted syscalls (future, not this PR)
- `SA_SIGINFO (0x4)` — reserved for future `siginfo_t` support

## Thread Struct Changes

Add to `struct Thread` in `process-types.cppm`:

```c++
// Signal alternate stack
VirtAddr alt_stack_sp{0};
usize alt_stack_size{0};
u32 alt_stack_flags{2}; // SS_DISABLE

// True when handler is executing on the alternate signal stack
bool on_alt_stack{false};
```

## Testing: signal_test.c

New user-space program added to initramfs:

1. **test_basic_handler** — Register SIGUSR1 handler, `kill(getpid(), SIGUSR1)`, verify handler ran and normal execution resumed
2. **test_nested_signals** — In SIGUSR1 handler, send SIGUSR2 to self → both handlers execute and unwind correctly
3. **test_sigchld** — Fork child that exits, parent gets SIGCHLD
4. **test_sigprocmask** — Block SIGUSR1, send it, verify not delivered, unblock, verify delivered
5. **test_sigaltstack** — Set altstack, register handler with SA_ONSTACK, verify handler SP is on altstack
6. **test_sig_ign** — Set SIGCHLD to SIG_IGN, verify no zombie

## File Change Summary

| File | Change |
|------|--------|
| `src/process/src/process-types.cppm` | Add altstack fields to Thread |
| `src/process/src/process-signal.cppm` | Add SignalFrame struct, setup_sigframe(), SA_* constants |
| `src/process/src/signal.cpp` | Implement setup_sigframe(), update do_signal_checkpoint() |
| `src/kernel/src/syscall_table.cpp` | Implement sys_sigreturn, sys_sigaltstack, update system_call_handler signature, pass trap_frame |
| `src/boot/src/arch/arm64/start_arm64.S` | Add `mov x7, sp` in .Llower_syscall |
| `src/abi/src/abi.cppm` | Update system_call_handler declaration |
| `src/userspace/syscall.h` | Add signal syscall wrappers (kill, sigaction, sigreturn, sigaltstack) |
| `src/userspace/signal_test.c` | New: 6 test scenarios |
| `CMakeLists.txt` (userspace) | Add signal_test.elf to initramfs |

## Risks and Mitigations

- **Stack overflow during sigframe push**: Validate that the target SP (user stack or altstack) is within a valid VMA before writing. If not, deliver SIGSEGV with default action (terminate).
- **Forged sigreturn**: MAGIC validation prevents random `sigreturn` calls from corrupting state.
- **NEON state corruption**: Since the kernel runs freestanding without NEON, the user's NEON state is preserved across the exception until we explicitly read it in `setup_sigframe()`.
