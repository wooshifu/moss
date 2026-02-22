// MOSS Kernel Module - Primary Interface (re-export hub)
// All declarations live in partition files; this module re-exports them
// and provides the global module fragment needed by implementation .cpp files.

export module moss.kernel;

// All external module imports (available to implementation units)
import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.platform;
import moss.hal.uart;
import moss.hal.intc;
import moss.hal.timer;
import moss.containers;
import moss.mm;
import moss.interrupts;
import moss.drivers;
import moss.fdt;
import moss.initramfs;
import moss.ipc;
import moss.process;
import moss.timer;
import moss.logging;
import moss.boot;

// Re-export all partitions
export import :elf;
export import :syscall_table;
export import :syscall_arch;
export import :main;
