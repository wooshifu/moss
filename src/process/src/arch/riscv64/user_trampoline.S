// S2 points to the complete user context. The scheduler has set sscratch
// to the reserved CPU-identity slot at the top of this thread's kernel stack.
.section .text
.global user_sret_trampoline
.type user_sret_trampoline, @function
user_sret_trampoline:
    // s3 addresses the reserved 16-byte top slot. Store the 8-byte kernel tp
    // identity before user tp is restored, so trap entry can recover it.
    sd tp, 0(s3)
    csrw sscratch, s3
    mv a0, s2
    ld a1, 272(s2) // CpuContext::sp follows 32*8 GP bytes and two 8-byte fields.
    tail switch_to_user
.size user_sret_trampoline, .-user_sret_trampoline
.section .note.GNU-stack,"",@progbits
