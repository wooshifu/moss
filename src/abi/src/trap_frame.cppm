module;
#include "moss/trap_frame_offsets.h"

export module moss.abi:trap_frame;
import moss.types;

export namespace moss::abi {
using moss::kernel::u32;
using moss::kernel::u64;
using moss::kernel::u8;

// Owned by the exception entry's kernel stack. Valid only until that entry
// returns. GP numbering is native: ARM x0-x30, RV x1-x31, x86 as CpuContext.
// The 16-byte alignment matches the native call-stack contract used by entry
// assembly; the assertions below prevent a C++ layout change from corrupting it.
#if defined(MOSS_ARCH_ARM64)
struct alignas(16) TrapFrame {
  u64 x0;
  u64 x1;
  u64 x2;
  u64 x3;
  u64 x4;
  u64 x5;
  u64 x6;
  u64 x7;
  u64 x8;
  u64 x9;
  u64 x10;
  u64 x11;
  u64 x12;
  u64 x13;
  u64 x14;
  u64 x15;
  u64 x16;
  u64 x17;
  u64 x18;
  u64 x19;
  u64 x20;
  u64 x21;
  u64 x22;
  u64 x23;
  u64 x24;
  u64 x25;
  u64 x26;
  u64 x27;
  u64 x28;
  u64 x29;
  u64 x30;
  u64 pc;
  u64 status;
  u64 sp;
  static constexpr u32 GPR_COUNT = 31;
  // SPSR.M[3:0]=0 identifies EL0t; EL1t/EL1h are privileged origins.
  [[nodiscard]] bool from_user() const noexcept { return (status & 15) == 0; }
  [[nodiscard]] u64 syscall_number() const noexcept { return x8; }
  u64 &result() noexcept { return x0; }
  [[nodiscard]] u64 user_status() const noexcept { return (status & MOSS_USER_STATUS_MASK) | MOSS_USER_STATUS_FIXED; }
  u64 &argument(u32 index) noexcept {
    constexpr u32 offsets[] = {MOSS_TF_X0, MOSS_TF_X1, MOSS_TF_X2, MOSS_TF_X3, MOSS_TF_X4, MOSS_TF_X5};
    return *reinterpret_cast<u64 *>(reinterpret_cast<u8 *>(this) + offsets[index]);
  }
  u64 &gpr(u32 index) noexcept {
    constexpr u32 offsets[] = {
        MOSS_TF_X0,  MOSS_TF_X1,  MOSS_TF_X2,  MOSS_TF_X3,  MOSS_TF_X4,  MOSS_TF_X5,  MOSS_TF_X6,  MOSS_TF_X7,
        MOSS_TF_X8,  MOSS_TF_X9,  MOSS_TF_X10, MOSS_TF_X11, MOSS_TF_X12, MOSS_TF_X13, MOSS_TF_X14, MOSS_TF_X15,
        MOSS_TF_X16, MOSS_TF_X17, MOSS_TF_X18, MOSS_TF_X19, MOSS_TF_X20, MOSS_TF_X21, MOSS_TF_X22, MOSS_TF_X23,
        MOSS_TF_X24, MOSS_TF_X25, MOSS_TF_X26, MOSS_TF_X27, MOSS_TF_X28, MOSS_TF_X29, MOSS_TF_X30};
    return *reinterpret_cast<u64 *>(reinterpret_cast<u8 *>(this) + offsets[index]);
  }
};
static_assert(__builtin_offsetof(TrapFrame, x0) == MOSS_TF_X0);
static_assert(__builtin_offsetof(TrapFrame, x1) == MOSS_TF_X1);
static_assert(__builtin_offsetof(TrapFrame, x2) == MOSS_TF_X2);
static_assert(__builtin_offsetof(TrapFrame, x3) == MOSS_TF_X3);
static_assert(__builtin_offsetof(TrapFrame, x4) == MOSS_TF_X4);
static_assert(__builtin_offsetof(TrapFrame, x5) == MOSS_TF_X5);
static_assert(__builtin_offsetof(TrapFrame, x6) == MOSS_TF_X6);
static_assert(__builtin_offsetof(TrapFrame, x7) == MOSS_TF_X7);
static_assert(__builtin_offsetof(TrapFrame, x8) == MOSS_TF_X8);
static_assert(__builtin_offsetof(TrapFrame, x9) == MOSS_TF_X9);
static_assert(__builtin_offsetof(TrapFrame, x10) == MOSS_TF_X10);
static_assert(__builtin_offsetof(TrapFrame, x11) == MOSS_TF_X11);
static_assert(__builtin_offsetof(TrapFrame, x12) == MOSS_TF_X12);
static_assert(__builtin_offsetof(TrapFrame, x13) == MOSS_TF_X13);
static_assert(__builtin_offsetof(TrapFrame, x14) == MOSS_TF_X14);
static_assert(__builtin_offsetof(TrapFrame, x15) == MOSS_TF_X15);
static_assert(__builtin_offsetof(TrapFrame, x16) == MOSS_TF_X16);
static_assert(__builtin_offsetof(TrapFrame, x17) == MOSS_TF_X17);
static_assert(__builtin_offsetof(TrapFrame, x18) == MOSS_TF_X18);
static_assert(__builtin_offsetof(TrapFrame, x19) == MOSS_TF_X19);
static_assert(__builtin_offsetof(TrapFrame, x20) == MOSS_TF_X20);
static_assert(__builtin_offsetof(TrapFrame, x21) == MOSS_TF_X21);
static_assert(__builtin_offsetof(TrapFrame, x22) == MOSS_TF_X22);
static_assert(__builtin_offsetof(TrapFrame, x23) == MOSS_TF_X23);
static_assert(__builtin_offsetof(TrapFrame, x24) == MOSS_TF_X24);
static_assert(__builtin_offsetof(TrapFrame, x25) == MOSS_TF_X25);
static_assert(__builtin_offsetof(TrapFrame, x26) == MOSS_TF_X26);
static_assert(__builtin_offsetof(TrapFrame, x27) == MOSS_TF_X27);
static_assert(__builtin_offsetof(TrapFrame, x28) == MOSS_TF_X28);
static_assert(__builtin_offsetof(TrapFrame, x29) == MOSS_TF_X29);
static_assert(__builtin_offsetof(TrapFrame, x30) == MOSS_TF_X30);
static_assert(__builtin_offsetof(TrapFrame, pc) == MOSS_TF_PC);
static_assert(__builtin_offsetof(TrapFrame, status) == MOSS_TF_STATUS);
static_assert(__builtin_offsetof(TrapFrame, sp) == MOSS_TF_SP);
#elif defined(MOSS_ARCH_RISCV64)
struct alignas(16) TrapFrame {
  u64 ra;
  u64 t0;
  u64 t1;
  u64 t2;
  u64 t3;
  u64 t4;
  u64 t5;
  u64 t6;
  u64 a0;
  u64 a1;
  u64 a2;
  u64 a3;
  u64 a4;
  u64 a5;
  u64 a6;
  u64 a7;
  u64 s0;
  u64 s1;
  u64 s2;
  u64 s3;
  u64 s4;
  u64 s5;
  u64 s6;
  u64 s7;
  u64 s8;
  u64 s9;
  u64 s10;
  u64 s11;
  u64 pc;
  u64 status;
  u64 sp;
  u64 cause;
  u64 tp;
  u64 gp;
  u64 reserved0;
  u64 reserved1;
  static constexpr u32 GPR_COUNT = 31;
  // sstatus.SPP (bit 8) records the privilege before the supervisor trap.
  [[nodiscard]] bool from_user() const noexcept { return (status & (1ULL << 8)) == 0; }
  [[nodiscard]] u64 syscall_number() const noexcept { return a7; }
  u64 &result() noexcept { return a0; }
  [[nodiscard]] u64 user_status() const noexcept { return (status & MOSS_USER_STATUS_MASK) | MOSS_USER_STATUS_FIXED; }
  u64 &argument(u32 index) noexcept {
    constexpr u32 offsets[] = {MOSS_TF_A0, MOSS_TF_A1, MOSS_TF_A2, MOSS_TF_A3, MOSS_TF_A4, MOSS_TF_A5};
    return *reinterpret_cast<u64 *>(reinterpret_cast<u8 *>(this) + offsets[index]);
  }
  u64 &gpr(u32 index) noexcept {
    constexpr u32 offsets[] = {MOSS_TF_RA, MOSS_TF_SP, MOSS_TF_GP, MOSS_TF_TP, MOSS_TF_T0,  MOSS_TF_T1,  MOSS_TF_T2,
                               MOSS_TF_S0, MOSS_TF_S1, MOSS_TF_A0, MOSS_TF_A1, MOSS_TF_A2,  MOSS_TF_A3,  MOSS_TF_A4,
                               MOSS_TF_A5, MOSS_TF_A6, MOSS_TF_A7, MOSS_TF_S2, MOSS_TF_S3,  MOSS_TF_S4,  MOSS_TF_S5,
                               MOSS_TF_S6, MOSS_TF_S7, MOSS_TF_S8, MOSS_TF_S9, MOSS_TF_S10, MOSS_TF_S11, MOSS_TF_T3,
                               MOSS_TF_T4, MOSS_TF_T5, MOSS_TF_T6};
    return *reinterpret_cast<u64 *>(reinterpret_cast<u8 *>(this) + offsets[index]);
  }
};
static_assert(__builtin_offsetof(TrapFrame, ra) == MOSS_TF_RA);
static_assert(__builtin_offsetof(TrapFrame, t0) == MOSS_TF_T0);
static_assert(__builtin_offsetof(TrapFrame, t1) == MOSS_TF_T1);
static_assert(__builtin_offsetof(TrapFrame, t2) == MOSS_TF_T2);
static_assert(__builtin_offsetof(TrapFrame, t3) == MOSS_TF_T3);
static_assert(__builtin_offsetof(TrapFrame, t4) == MOSS_TF_T4);
static_assert(__builtin_offsetof(TrapFrame, t5) == MOSS_TF_T5);
static_assert(__builtin_offsetof(TrapFrame, t6) == MOSS_TF_T6);
static_assert(__builtin_offsetof(TrapFrame, a0) == MOSS_TF_A0);
static_assert(__builtin_offsetof(TrapFrame, a1) == MOSS_TF_A1);
static_assert(__builtin_offsetof(TrapFrame, a2) == MOSS_TF_A2);
static_assert(__builtin_offsetof(TrapFrame, a3) == MOSS_TF_A3);
static_assert(__builtin_offsetof(TrapFrame, a4) == MOSS_TF_A4);
static_assert(__builtin_offsetof(TrapFrame, a5) == MOSS_TF_A5);
static_assert(__builtin_offsetof(TrapFrame, a6) == MOSS_TF_A6);
static_assert(__builtin_offsetof(TrapFrame, a7) == MOSS_TF_A7);
static_assert(__builtin_offsetof(TrapFrame, s0) == MOSS_TF_S0);
static_assert(__builtin_offsetof(TrapFrame, s1) == MOSS_TF_S1);
static_assert(__builtin_offsetof(TrapFrame, s2) == MOSS_TF_S2);
static_assert(__builtin_offsetof(TrapFrame, s3) == MOSS_TF_S3);
static_assert(__builtin_offsetof(TrapFrame, s4) == MOSS_TF_S4);
static_assert(__builtin_offsetof(TrapFrame, s5) == MOSS_TF_S5);
static_assert(__builtin_offsetof(TrapFrame, s6) == MOSS_TF_S6);
static_assert(__builtin_offsetof(TrapFrame, s7) == MOSS_TF_S7);
static_assert(__builtin_offsetof(TrapFrame, s8) == MOSS_TF_S8);
static_assert(__builtin_offsetof(TrapFrame, s9) == MOSS_TF_S9);
static_assert(__builtin_offsetof(TrapFrame, s10) == MOSS_TF_S10);
static_assert(__builtin_offsetof(TrapFrame, s11) == MOSS_TF_S11);
static_assert(__builtin_offsetof(TrapFrame, pc) == MOSS_TF_PC);
static_assert(__builtin_offsetof(TrapFrame, status) == MOSS_TF_STATUS);
static_assert(__builtin_offsetof(TrapFrame, sp) == MOSS_TF_SP);
static_assert(__builtin_offsetof(TrapFrame, cause) == MOSS_TF_CAUSE);
static_assert(__builtin_offsetof(TrapFrame, tp) == MOSS_TF_TP);
static_assert(__builtin_offsetof(TrapFrame, gp) == MOSS_TF_GP);
static_assert(__builtin_offsetof(TrapFrame, reserved0) == MOSS_TF_RESERVED0);
static_assert(__builtin_offsetof(TrapFrame, reserved1) == MOSS_TF_RESERVED1);
#elif defined(MOSS_ARCH_X64)
struct alignas(16) TrapFrame {
  u64 r15;
  u64 r14;
  u64 r13;
  u64 r12;
  u64 r11;
  u64 r10;
  u64 r9;
  u64 r8;
  u64 rbp;
  u64 rdi;
  u64 rsi;
  u64 rdx;
  u64 rcx;
  u64 rbx;
  u64 rax;
  u64 vector;
  u64 error;
  u64 pc;
  u64 cs;
  u64 status;
  u64 sp;
  u64 ss;
  static constexpr u32 GPR_COUNT = 15;
  [[nodiscard]] bool from_user() const noexcept { return (cs & 3) == 3; }
  [[nodiscard]] u64 syscall_number() const noexcept { return rax; }
  u64 &result() noexcept { return rax; }
  [[nodiscard]] u64 user_status() const noexcept { return (status & MOSS_USER_STATUS_MASK) | MOSS_USER_STATUS_FIXED; }
  u64 &argument(u32 index) noexcept {
    constexpr u32 offsets[] = {MOSS_TF_RDI, MOSS_TF_RSI, MOSS_TF_RDX, MOSS_TF_R10, MOSS_TF_R8, MOSS_TF_R9};
    return *reinterpret_cast<u64 *>(reinterpret_cast<u8 *>(this) + offsets[index]);
  }
  u64 &gpr(u32 index) noexcept {
    constexpr u32 offsets[] = {MOSS_TF_RAX, MOSS_TF_RBX, MOSS_TF_RCX, MOSS_TF_RDX, MOSS_TF_RSI,
                               MOSS_TF_RDI, MOSS_TF_RBP, MOSS_TF_R8,  MOSS_TF_R9,  MOSS_TF_R10,
                               MOSS_TF_R11, MOSS_TF_R12, MOSS_TF_R13, MOSS_TF_R14, MOSS_TF_R15};
    return *reinterpret_cast<u64 *>(reinterpret_cast<u8 *>(this) + offsets[index]);
  }
};
static_assert(__builtin_offsetof(TrapFrame, r15) == MOSS_TF_R15);
static_assert(__builtin_offsetof(TrapFrame, r14) == MOSS_TF_R14);
static_assert(__builtin_offsetof(TrapFrame, r13) == MOSS_TF_R13);
static_assert(__builtin_offsetof(TrapFrame, r12) == MOSS_TF_R12);
static_assert(__builtin_offsetof(TrapFrame, r11) == MOSS_TF_R11);
static_assert(__builtin_offsetof(TrapFrame, r10) == MOSS_TF_R10);
static_assert(__builtin_offsetof(TrapFrame, r9) == MOSS_TF_R9);
static_assert(__builtin_offsetof(TrapFrame, r8) == MOSS_TF_R8);
static_assert(__builtin_offsetof(TrapFrame, rbp) == MOSS_TF_RBP);
static_assert(__builtin_offsetof(TrapFrame, rdi) == MOSS_TF_RDI);
static_assert(__builtin_offsetof(TrapFrame, rsi) == MOSS_TF_RSI);
static_assert(__builtin_offsetof(TrapFrame, rdx) == MOSS_TF_RDX);
static_assert(__builtin_offsetof(TrapFrame, rcx) == MOSS_TF_RCX);
static_assert(__builtin_offsetof(TrapFrame, rbx) == MOSS_TF_RBX);
static_assert(__builtin_offsetof(TrapFrame, rax) == MOSS_TF_RAX);
static_assert(__builtin_offsetof(TrapFrame, vector) == MOSS_TF_VECTOR);
static_assert(__builtin_offsetof(TrapFrame, error) == MOSS_TF_ERROR);
static_assert(__builtin_offsetof(TrapFrame, pc) == MOSS_TF_PC);
static_assert(__builtin_offsetof(TrapFrame, cs) == MOSS_TF_CS);
static_assert(__builtin_offsetof(TrapFrame, status) == MOSS_TF_STATUS);
static_assert(__builtin_offsetof(TrapFrame, sp) == MOSS_TF_SP);
static_assert(__builtin_offsetof(TrapFrame, ss) == MOSS_TF_SS);
#endif
static_assert(sizeof(TrapFrame) == MOSS_TF_SIZE && alignof(TrapFrame) == 16);
} // namespace moss::abi
