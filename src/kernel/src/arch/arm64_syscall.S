// The exported entry uses the same full frame as the EL0 vector; there is
// no second, incompatible syscall prologue.
.section .text
.global syscall_entry_point
.type syscall_entry_point, @function
syscall_entry_point:
    b lower_el_sync_dispatch
.size syscall_entry_point, . - syscall_entry_point

.global syscall_return
.type syscall_return, @function
syscall_return:
    cbz x0, .Lreturn_error
    // Legacy opaque SyscallContext offset: byte 56 (slot 7). Its layout
    // is not defined here; this helper is not a native TrapFrame result accessor.
    ldr x0, [x0, #56]
    ret
.Lreturn_error:
    mov x0, #-1
    ret
.size syscall_return, . - syscall_return
.section .note.GNU-stack,"",%progbits
