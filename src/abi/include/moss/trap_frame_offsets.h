#pragma once

// Native exception-frame ABI: byte offsets consumed by preprocessed assembly.
// No platform, bootloader or emulator information belongs here.
// Every saved slot is one 8-byte u64, in TrapFrame member order. Frame sizes
// include any reserved slots and are multiples of the 16-byte stack alignment;
// changing the layout requires updating assembly and the C++ offset assertions.
#if defined(MOSS_ARCH_ARM64)
// Restore only NZCV (bits 31:28); fixed zero selects EL0t with IRQs unmasked.
// User-provided status must never choose a privileged return mode.
#define MOSS_USER_STATUS_MASK 0xf0000000
#define MOSS_USER_STATUS_FIXED 0
#define MOSS_TF_X0 0
#define MOSS_TF_X1 8
#define MOSS_TF_X2 16
#define MOSS_TF_X3 24
#define MOSS_TF_X4 32
#define MOSS_TF_X5 40
#define MOSS_TF_X6 48
#define MOSS_TF_X7 56
#define MOSS_TF_X8 64
#define MOSS_TF_X9 72
#define MOSS_TF_X10 80
#define MOSS_TF_X11 88
#define MOSS_TF_X12 96
#define MOSS_TF_X13 104
#define MOSS_TF_X14 112
#define MOSS_TF_X15 120
#define MOSS_TF_X16 128
#define MOSS_TF_X17 136
#define MOSS_TF_X18 144
#define MOSS_TF_X19 152
#define MOSS_TF_X20 160
#define MOSS_TF_X21 168
#define MOSS_TF_X22 176
#define MOSS_TF_X23 184
#define MOSS_TF_X24 192
#define MOSS_TF_X25 200
#define MOSS_TF_X26 208
#define MOSS_TF_X27 216
#define MOSS_TF_X28 224
#define MOSS_TF_X29 232
#define MOSS_TF_X30 240
#define MOSS_TF_PC 248
#define MOSS_TF_STATUS 256
#define MOSS_TF_SP 264
#define MOSS_TF_SIZE 272
#elif defined(MOSS_ARCH_RISCV64)
// Ignore user-supplied sstatus. UXL=2 (bits 33:32) selects RV64 and SPIE=1
// (bit 5) restores supervisor interrupt delivery after SRET; SPP remains zero.
#define MOSS_USER_STATUS_MASK 0
#define MOSS_USER_STATUS_FIXED 0x200000020
#define MOSS_TF_RA 0
#define MOSS_TF_T0 8
#define MOSS_TF_T1 16
#define MOSS_TF_T2 24
#define MOSS_TF_T3 32
#define MOSS_TF_T4 40
#define MOSS_TF_T5 48
#define MOSS_TF_T6 56
#define MOSS_TF_A0 64
#define MOSS_TF_A1 72
#define MOSS_TF_A2 80
#define MOSS_TF_A3 88
#define MOSS_TF_A4 96
#define MOSS_TF_A5 104
#define MOSS_TF_A6 112
#define MOSS_TF_A7 120
#define MOSS_TF_S0 128
#define MOSS_TF_S1 136
#define MOSS_TF_S2 144
#define MOSS_TF_S3 152
#define MOSS_TF_S4 160
#define MOSS_TF_S5 168
#define MOSS_TF_S6 176
#define MOSS_TF_S7 184
#define MOSS_TF_S8 192
#define MOSS_TF_S9 200
#define MOSS_TF_S10 208
#define MOSS_TF_S11 216
#define MOSS_TF_PC 224
#define MOSS_TF_STATUS 232
#define MOSS_TF_SP 240
#define MOSS_TF_CAUSE 248
#define MOSS_TF_TP 256
#define MOSS_TF_GP 264
#define MOSS_TF_RESERVED0 272
#define MOSS_TF_RESERVED1 280
#define MOSS_TF_SIZE 288
#elif defined(MOSS_ARCH_X64)
// 0xcd5 preserves CF/PF/AF/ZF/SF/DF/OF only; 0x202 forces reserved bit 1
// and IF while excluding IOPL and other privileged RFLAGS controls.
#define MOSS_USER_STATUS_MASK 0xcd5
#define MOSS_USER_STATUS_FIXED 0x202
#define MOSS_TF_R15 0
#define MOSS_TF_R14 8
#define MOSS_TF_R13 16
#define MOSS_TF_R12 24
#define MOSS_TF_R11 32
#define MOSS_TF_R10 40
#define MOSS_TF_R9 48
#define MOSS_TF_R8 56
#define MOSS_TF_RBP 64
#define MOSS_TF_RDI 72
#define MOSS_TF_RSI 80
#define MOSS_TF_RDX 88
#define MOSS_TF_RCX 96
#define MOSS_TF_RBX 104
#define MOSS_TF_RAX 112
#define MOSS_TF_VECTOR 120
#define MOSS_TF_ERROR 128
#define MOSS_TF_PC 136
#define MOSS_TF_CS 144
#define MOSS_TF_STATUS 152
#define MOSS_TF_SP 160
#define MOSS_TF_SS 168
#define MOSS_TF_SIZE 176
#else
#error Unsupported native trap frame
#endif
