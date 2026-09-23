// MOSS ABI Module — single source of truth for all extern "C" symbols.
//
// Eliminates ~80 duplicated extern "C" declarations scattered across 30+ files.
// Consumer modules: `import moss.abi;` then use moss::abi::linker::*, etc.

module;

// ============================================================================
// Linker script symbols (all architectures)
// ============================================================================
extern "C" {
extern char _text_start_addr[];
extern char _text_end_addr[];
extern char _rodata_start_addr[];
extern char _rodata_end_addr[];
extern char _data_start_addr[];
extern char _data_end_addr[];
extern char _bss_start_addr[];
extern char _bss_end_addr[];
extern char _stack_bottom_addr[];
extern char _stack_top_addr[];
extern char _heap_start_addr[];
extern char _heap_end_addr[];
extern char _pagetable_start_addr[];
extern char _pagetable_end_addr[];
extern char _kernel_end_addr[];
// C++ global constructor array (populated by linker from .init_array section)
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
}

// ============================================================================
// Assembly-defined functions (all architectures)
// ============================================================================
extern "C" {
void _start();
void context_switch(void *prev_context, void *next_context);
void switch_to_user(void *context, unsigned long long user_stack);
void kernel_thread_entry();
void syscall_entry_point() noexcept;
void syscall_return(void *context) noexcept;
extern const unsigned char moss_sigreturn_start[];
extern const unsigned char moss_sigreturn_end[];
unsigned long long moss_raw_copy_from_user(void *dst, const void *src, unsigned long long size) noexcept;
unsigned long long moss_raw_copy_to_user(void *dst, const void *src, unsigned long long size) noexcept;
extern const int moss_uaccess_table_start[];
extern const int moss_uaccess_table_end[];
}

// ============================================================================
// Architecture-specific assembly symbols
// ============================================================================
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
extern "C" {
void user_eret_trampoline();
void syscall_fast_path();
void flush_tlb_single(unsigned long long va);
void flush_tlb_all();
extern volatile unsigned long long cpu_startup_flags[][2];
extern unsigned int early_uart_lock;
extern char exception_vectors[];
extern char _user_program_start[];
extern char _user_program_end[];
}
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X64)
extern "C" {
void user_iret_trampoline();
extern char _user_program_start[];
extern char _user_program_end[];
// TSS (104 bytes, defined in start_x64.S .bss.tss)
extern unsigned char g_tss[];
// Per-task kernel stack pointer for SYSCALL entry (defined in x64_syscall.S)
extern unsigned long long g_kernel_rsp;
void x64_set_kernel_stack(unsigned long long top) noexcept;
}
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV64)
extern "C" {
void user_sret_trampoline();
extern char _user_program_start[];
extern char _user_program_end[];
}
#endif

// ============================================================================
// C++ functions called FROM assembly (need C linkage at definition site)
// ============================================================================
extern "C" {
[[noreturn]] void early_main(void *device_tree_ptr);
[[noreturn]] void kernel_main(void) noexcept;
void early_debug_print(const char *message) noexcept;
void system_call_handler(void *raw_frame) noexcept;
void user_return_handler(void *raw_frame) noexcept;
void irq_handler_c(void) noexcept;
void kernel_page_fault_handler(unsigned long long esr, unsigned long long far_addr, unsigned long long elr,
                               void *raw_frame) noexcept;
void riscv64_page_fault_handler(unsigned long long cause, unsigned long long address, unsigned long long pc,
                                void *trap_frame) noexcept;
void x64_page_fault_handler(unsigned long long error, unsigned long long address, unsigned long long pc,
                            void *trap_frame) noexcept;
void user_page_fault_handler(unsigned long long esr, unsigned long long far_addr, unsigned long long elr) noexcept;
void unhandled_exception_handler(unsigned long long esr, unsigned long long far_addr, unsigned long long elr,
                                 unsigned long long saved_x30, unsigned long long frame_sp) noexcept;
[[noreturn]] void unhandled_user_exception_handler(unsigned long long esr, unsigned long long far_addr,
                                                   unsigned long long elr) noexcept;
void mark_runtime_heap_ready() noexcept;
}

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
extern "C" [[noreturn]] void secondary_cpu_entry() noexcept;
#endif

// ============================================================================
// Cross-module bridge functions (extern "C" to break circular dependencies)
// ============================================================================
extern "C" {
// mm <-> kernel bridge
int resolve_current_user_fault(unsigned long long fault_addr, unsigned int access, bool cow_only) noexcept;
unsigned long long get_current_pgd_phys() noexcept;
[[noreturn]] void terminate_current_user_process(int signal_number) noexcept;

// Console and event wait bridges keep VFS and drivers independent of process.
extern void (*g_x64_uart_rx_handler)() noexcept;
void console_rx_init() noexcept;
int console_getc_blocking() noexcept;
int console_try_getc() noexcept;
// Event owners register waiters while locked, then unlock before commit.
void *moss_prepare_io_wait() noexcept;
bool moss_io_wait_interrupted() noexcept;
void moss_commit_io_wait() noexcept;
void moss_wake_io_waiter(void *opaque) noexcept;
void moss_signal_broken_pipe() noexcept;

// containers <-> mm bridge
void *moss_heap_allocate(unsigned long long size, unsigned long long alignment) noexcept;
unsigned long long moss_slab_alloc_pages(unsigned long long order) noexcept;
int moss_slab_free_pages(unsigned long long addr, unsigned long long order) noexcept;

// C string function (runtime_support.cpp)
int strcmp(const char *s1, const char *s2) noexcept;
}

export module moss.abi;
export import :trap_frame;

import moss.types;

export namespace moss::abi::uaccess {
// Raw primitives return bytes not copied and access the active hardware root;
// only the user operand's instruction has a fixup. They do not retain an
// AddressSpace or protect its frames. Ordinary callers use process::copy_*_user,
// which binds the selected version and copies through leased physical aliases.
using ::moss_raw_copy_from_user;
using ::moss_raw_copy_to_user;

inline bool fixup(TrapFrame &frame) noexcept {
  using moss::kernel::i64;
  using moss::kernel::u64;
  if (frame.from_user()) {
    return false;
  }
  static_assert(sizeof(int) == 4);
  // Each pair is two 4-byte signed displacements emitted by uaccess.S:
  // faulting instruction, then recovery PC, each relative to its own field.
  // Boot relocation therefore needs no writable pointers or fixed load address.
  for (const auto *entry = ::moss_uaccess_table_start; entry < ::moss_uaccess_table_end; entry += 2) {
    const u64 instruction = reinterpret_cast<u64>(entry) + static_cast<u64>(static_cast<i64>(entry[0]));
    if (frame.pc == instruction) {
      frame.pc = reinterpret_cast<u64>(entry + 1) + static_cast<u64>(static_cast<i64>(entry[1]));
      return true;
    }
  }
  return false;
}
} // namespace moss::abi::uaccess

export namespace moss::abi::signal {
inline const unsigned char *trampoline() noexcept { return ::moss_sigreturn_start; }
inline moss::kernel::usize trampoline_size() noexcept {
  return static_cast<moss::kernel::usize>(::moss_sigreturn_end - ::moss_sigreturn_start);
}
} // namespace moss::abi::signal

// ============================================================================
// Typed linker symbol accessors
// ============================================================================
export namespace moss::abi::linker {

using moss::kernel::usize;
using moss::kernel::VirtAddr;

inline auto text_start() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_text_start_addr); }
inline auto text_end() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_text_end_addr); }
inline auto rodata_start() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_rodata_start_addr); }
inline auto rodata_end() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_rodata_end_addr); }
inline auto data_start() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_data_start_addr); }
inline auto data_end() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_data_end_addr); }
inline auto bss_start() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_bss_start_addr); }
inline auto bss_end() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_bss_end_addr); }
inline auto stack_bottom() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_stack_bottom_addr); }
inline auto stack_top() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_stack_top_addr); }
inline auto heap_start() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_heap_start_addr); }
inline auto heap_end() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_heap_end_addr); }
inline auto pagetable_start() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_pagetable_start_addr); }
inline auto pagetable_end() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_pagetable_end_addr); }
inline auto kernel_end() noexcept -> VirtAddr { return reinterpret_cast<VirtAddr>(_kernel_end_addr); }

// Computed helpers
inline auto text_size() noexcept -> usize { return text_end() - text_start(); }
inline auto rodata_size() noexcept -> usize { return rodata_end() - rodata_start(); }
inline auto data_size() noexcept -> usize { return data_end() - data_start(); }
inline auto bss_size() noexcept -> usize { return bss_end() - bss_start(); }
inline auto stack_size() noexcept -> usize { return stack_top() - stack_bottom(); }
inline auto heap_size() noexcept -> usize { return heap_end() - heap_start(); }
inline auto pagetable_size() noexcept -> usize { return pagetable_end() - pagetable_start(); }

// Call all C++ global constructors from the .init_array section.
// Must be called once during early boot, before kernel_main().
inline void call_global_constructors() noexcept {
  using CtorFn = void (*)();
  CtorFn *start = __init_array_start;
  CtorFn *end = __init_array_end;
  for (CtorFn *fn = start; fn < end; ++fn) {
    if (*fn) {
      (*fn)();
    }
  }
}

} // namespace moss::abi::linker

// ============================================================================
// Assembly function re-exports
// ============================================================================
export namespace moss::abi {

using ::_start;
using ::context_switch;
using ::kernel_thread_entry;
using ::switch_to_user;
using ::syscall_entry_point;
using ::syscall_return;

} // namespace moss::abi

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
export namespace moss::abi::arm64 {

using ::cpu_startup_flags;
using ::early_uart_lock;
using ::exception_vectors;
using ::flush_tlb_all;
using ::flush_tlb_single;
using ::syscall_fast_path;
using ::user_eret_trampoline;

inline auto user_program_start() noexcept -> const unsigned char * {
  return reinterpret_cast<const unsigned char *>(::_user_program_start);
}
inline auto user_program_end() noexcept -> const unsigned char * {
  return reinterpret_cast<const unsigned char *>(::_user_program_end);
}
inline auto user_program_size() noexcept -> moss::kernel::usize {
  return static_cast<moss::kernel::usize>(user_program_end() - user_program_start());
}
inline auto exception_vectors_addr() noexcept -> moss::kernel::VirtAddr {
  return reinterpret_cast<moss::kernel::VirtAddr>(::exception_vectors);
}

} // namespace moss::abi::arm64

#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X64)
export namespace moss::abi::x64 {

using ::g_kernel_rsp;
using ::g_tss;
inline void set_kernel_stack(unsigned long long top) noexcept { ::x64_set_kernel_stack(top); }
using ::user_iret_trampoline;

inline auto user_program_start() noexcept -> const unsigned char * {
  return reinterpret_cast<const unsigned char *>(::_user_program_start);
}
inline auto user_program_end() noexcept -> const unsigned char * {
  return reinterpret_cast<const unsigned char *>(::_user_program_end);
}
inline auto user_program_size() noexcept -> moss::kernel::usize {
  return static_cast<moss::kernel::usize>(user_program_end() - user_program_start());
}

} // namespace moss::abi::x64

#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV64)
export namespace moss::abi::riscv64 {

using ::user_sret_trampoline;

inline auto user_program_start() noexcept -> const unsigned char * {
  return reinterpret_cast<const unsigned char *>(::_user_program_start);
}
inline auto user_program_end() noexcept -> const unsigned char * {
  return reinterpret_cast<const unsigned char *>(::_user_program_end);
}
inline auto user_program_size() noexcept -> moss::kernel::usize {
  return static_cast<moss::kernel::usize>(user_program_end() - user_program_start());
}

} // namespace moss::abi::riscv64
#endif

// ============================================================================
// Cross-module bridge re-exports
// ============================================================================
export namespace moss::abi::bridge {

using ::console_getc_blocking;
using ::console_rx_init;
using ::console_try_getc;
using ::g_x64_uart_rx_handler;
using ::get_current_pgd_phys;
using ::moss_commit_io_wait;
using ::moss_heap_allocate;
using ::moss_io_wait_interrupted;
using ::moss_prepare_io_wait;
using ::moss_signal_broken_pipe;
using ::moss_slab_alloc_pages;
using ::moss_slab_free_pages;
using ::moss_wake_io_waiter;
using ::resolve_current_user_fault;
using ::strcmp;
using ::terminate_current_user_process;

} // namespace moss::abi::bridge

// ============================================================================
// Kernel entry points (C++ functions called from assembly)
// ============================================================================
export namespace moss::abi::entry {

using ::early_debug_print;
using ::early_main;
using ::irq_handler_c;
using ::kernel_main;
using ::kernel_page_fault_handler;
using ::mark_runtime_heap_ready;
using ::riscv64_page_fault_handler;
using ::system_call_handler;
using ::unhandled_exception_handler;
using ::unhandled_user_exception_handler;
using ::user_page_fault_handler;
using ::user_return_handler;
using ::x64_page_fault_handler;

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
using ::secondary_cpu_entry;
#endif

} // namespace moss::abi::entry
