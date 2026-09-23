import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.timer;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;
import moss.ipc;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/benchmark.hpp"
#include "validation/core_cases.hpp"
#include "validation/exec_control.hpp"
#include "validation/isolation.hpp"
#include "validation/lifecycle.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/process_control.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/signal_control.hpp"
#include "validation/smp_cases.hpp"
#include "validation/timer_control.hpp"
#include "validation/uaccess.hpp"
#include "validation_internal.hpp"

#include "validation/registry.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
using moss::test::validation::active_case;
using moss::test::validation::address_space_control_rollback;
using moss::test::validation::address_space_heap_rollback;
using moss::test::validation::asid_leases;
using moss::test::validation::clone_allocation_rollback;
using moss::test::validation::clone_preserves_destination;
using moss::test::validation::Event;
using moss::test::validation::kernel_isolation_cases;
using moss::test::validation::map_allocation_rollback;
using moss::test::validation::map_preserves_existing;
using moss::test::validation::map_rejects_blocks;
using moss::test::validation::process_heap_rollback;
using moss::test::validation::raw_user_copy_fixup;
using moss::test::validation::resources;
using moss::test::validation::timer_capacity;
using moss::test::validation::timer_contracts;
using moss::test::validation::timer_dispatch;
using moss::test::validation::uaccess_cases;
using moss::test::validation::unmap_reclaims_tables;
using moss::test::validation::user_copy_version_binding;
using moss::test::validation::vma_heap_rollback;

namespace moss::test::validation {
static void empty_case() {}
void failing_case() { ut::expect(false); }
[[noreturn]] void panic_case() {
  Event("fatal").str("case", active_case).str("kind", "panic").send();
  logging::klog::panic("validation intentional panic");
  // Exercise the emergency path with TX already locked by this CPU. It must
  // still reach the real panic/halt; peers cannot interleave its diagnostic.
  hal::uart::TransmitGuard guard;
  arch::kernel_panic("validation intentional panic");
}
[[noreturn]] void timeout_case() {
  Event("fatal").str("case", active_case).str("kind", "timeout").send();
  for (;;) {
    arch::cpu_yield();
  }
}

void declare_cases() {
  moss::test::validation::register_driver_cases();
  ut::register_suite("resources", [] { ut::register_test("cpu_memory", resources); });
  moss::test::validation::register_mm_cases();
  moss::test::validation::register_pfa_cases();
  moss::test::validation::register_heap_cases();
  moss::test::validation::register_containers_cases();
  moss::test::validation::register_smp_cases();
  moss::test::validation::register_vfs_cases();
  ut::register_suite("timers", [] {
    ut::register_test("clocksource_high_frequency",
                      [] { ut::expect(moss::test::hardware::clocksource_high_frequency_regression()); });
    ut::register_test("contracts", timer_contracts);
    ut::register_test("dispatch", timer_dispatch);
    ut::register_test("capacity", timer_capacity);
  });
  moss::test::validation::register_scheduler_cases();
  ut::register_suite("process", [] { ut::register_test("heap_rollback", process_heap_rollback); });
  ut::register_suite("users", [] {
    ut::register_test("syscall_values", empty_case);
    ut::register_test("user_ranges", empty_case);
    ut::register_test("fork_exec_exit_reap", empty_case);
    ut::register_test("pipe_output_rollback", empty_case);
    ut::register_test("yield_reuse", empty_case);
    ut::register_test("wait_status_rollback", empty_case);
    ut::register_test("pipe_waits_for_writer", empty_case);
    ut::register_test("pipe_cross_cpu_roundtrip", empty_case);
    ut::register_test("pipe_waits_for_reader", empty_case);
    ut::register_test("cross_cpu_exit_reap", empty_case);
    ut::register_test("fork_fd_allocation_rollback", empty_case);
    ut::register_test("mmap_heap_rollback", empty_case);
    ut::register_test("fork_process_allocation_rollback", empty_case);
    ut::register_test("fork_metadata_allocation_rollback", empty_case);
  });
  ut::register_suite("users.frame", [] {
    ut::register_test("native_frame", empty_case);
    ut::register_test("fork_registers", empty_case);
    ut::register_test("signal_return", empty_case);
  });
  ut::register_suite("users.uaccess", [] {
    for (const auto *name : uaccess_cases) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("users.vm", [] {
    ut::register_test("private_cow", empty_case);
    ut::register_test("readonly_cow", empty_case);
    ut::register_test("access_permissions", empty_case);
    ut::register_test("brk_lifecycle", empty_case);
    for (const auto *name : kernel_isolation_cases) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("users.lifecycle", [] { ut::register_test("core_paths_recovery", empty_case); });
  ut::register_suite("users.applications", [] { ut::register_test("core_application_recovery", empty_case); });
  ut::register_suite("users.timers", [] {
    ut::register_test("relative_sleep", empty_case);
    ut::register_test("absolute_sleep", empty_case);
    ut::register_test("invalid_arguments", empty_case);
    ut::register_test("short_reuse", empty_case);
    ut::register_test("cancel_in_flight", empty_case);
    ut::register_test("early_wakeup", empty_case);
    ut::register_test("arm_failure_recovery", empty_case);
    ut::register_test("relative_interrupted", empty_case);
    ut::register_test("clock_relative_interrupted", empty_case);
    ut::register_test("clock_absolute_interrupted", empty_case);
  });
  ut::register_suite("users.ipc", [] {
    ut::register_test("roundtrip", empty_case);
    ut::register_test("deadline", empty_case);
    ut::register_test("peer_death", empty_case);
    ut::register_test("signal_cancel", empty_case);
    ut::register_test("capability_transfer", empty_case);
    ut::register_test("delivery_rollback", empty_case);
    ut::register_test("memory_object", empty_case);
    ut::register_test("badged_sender", empty_case);
    ut::register_test("nested_roundtrip", empty_case);
  });
  ut::register_suite("users.libc", [] {
    ut::register_test("static_runtime", empty_case);
    ut::register_test("filesystem_permissions", empty_case);
  });
  ut::register_suite("users.exec", [] {
    constexpr const char *names[] = {
        "rejects_invalid_entry",
        "rejects_phentsize",
        "rejects_load_size",
        "rejects_truncated_header",
        "rejects_truncated_phdr",
        "rejects_file_range",
        "rejects_user_range",
        "rejects_address_overflow",
        "rejects_page_offset",
        "rejects_alignment",
        "rejects_reserved_range",
        "rejects_page_overlap",
        "rejects_program_header_limit",
        "rejects_wx",
        "rejects_dynamic",
        "rejects_interp",
        "rejects_orphan_tls_file",
        "bad_env_vector",
        "bad_env_string",
        "argument_count_limit",
        "combined_count_limit",
        "string_byte_limit",
        "exact_combined_count",
        "exact_string_bytes",
        "empty_vectors",
        "allocation_rollback",
        "mutable_snapshot_rollback",
        "boundary_load_plan",
        "source_version",
        "registration_gate",
        "shared_thread_gate",
    };
    for (const auto *name : names) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("users.busybox", [] {
    ut::register_test("ash_exit", empty_case);
    ut::register_test("ash_substitution", empty_case);
    ut::register_test("ash_exec_environment", empty_case);
    ut::register_test("text_pipeline", empty_case);
    ut::register_test("directory_lifecycle", empty_case);
    ut::register_test("file_redirection", empty_case);
    ut::register_test("file_copy", empty_case);
    ut::register_test("file_rename", empty_case);
    ut::register_test("application_workflow", empty_case);
    ut::register_test("head", empty_case);
    ut::register_test("cut", empty_case);
    ut::register_test("sort", empty_case);
    ut::register_test("uniq", empty_case);
    ut::register_test("tr", empty_case);
    ut::register_test("tee", empty_case);
    ut::register_test("cmp", empty_case);
    ut::register_test("basename", empty_case);
    ut::register_test("dirname", empty_case);
    ut::register_test("rmdir", empty_case);
    ut::register_test("uname", empty_case);
    ut::register_test("kill", empty_case);
    ut::register_test("find", empty_case);
    ut::register_test("find_rejects_unsupported", empty_case);
  });
  ut::register_suite("users.signals", [] {
    ut::register_test("basic_handler", empty_case);
    ut::register_test("nested_signals", empty_case);
    ut::register_test("sigchld", empty_case);
    ut::register_test("wait_registration", empty_case);
    ut::register_test("wait_interrupted", empty_case);
    ut::register_test("wait_restarted", empty_case);
    ut::register_test("cpu_bound_irq", empty_case);
    ut::register_test("stop_continue", empty_case);
    ut::register_test("wait_job_status", empty_case);
    ut::register_test("no_cldstop", empty_case);
    ut::register_test("wait_process_group", empty_case);
    ut::register_test("wait_group_change", empty_case);
    ut::register_test("no_cldwait", empty_case);
    ut::register_test("sigaction_race", empty_case);
    ut::register_test("sigaction_discard", empty_case);
    ut::register_test("signal_exit_status", empty_case);
    ut::register_test("sigprocmask", empty_case);
    ut::register_test("sigaltstack", empty_case);
    ut::register_test("sig_ign", empty_case);
    ut::register_test("invalid_arguments", empty_case);
    ut::register_test("frame_validation", empty_case);
    ut::register_test("altstack_overflow", empty_case);
    ut::register_test("altstack_boundaries", empty_case);
    ut::register_test("inheritance", empty_case);
    ut::register_test("pid_lifecycle", empty_case);
    ut::register_test("pipe_sigpipe", empty_case);
    ut::register_test("pipe_interrupted", empty_case);
    ut::register_test("pipe_restarted", empty_case);
    ut::register_test("pipe_noninterrupting_signals", empty_case);
    ut::register_test("pipe_partial_interrupt", empty_case);
    ut::register_test("signal_wakeup_affinity", empty_case);
    ut::register_test("console_interrupted", empty_case);
    ut::register_test("console_partial_interrupt", empty_case);
    ut::register_test("console_restarted", empty_case);
    ut::register_test("console_multi_reader", empty_case);
  });
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
  ut::register_suite("users.console_irq", [] { ut::register_test("irq_before_registration", empty_case); });
#endif
#if defined(MOSS_ARCH_X64)
  ut::register_suite("users.simd_fault", [] { ut::register_test("isolation", empty_case); });
#endif
  moss::test::validation::register_mm_permissions_cases();
  ut::register_suite("mm.transactions", [] {
    ut::register_test("user_copy_version", user_copy_version_binding);
    ut::register_test("raw_copy_fixup", raw_user_copy_fixup);
    ut::register_test("map_preserves_existing", map_preserves_existing);
    ut::register_test("map_allocation_rollback", map_allocation_rollback);
    ut::register_test("map_rejects_blocks", map_rejects_blocks);
    ut::register_test("clone_preserves_destination", clone_preserves_destination);
    ut::register_test("clone_allocation_rollback", clone_allocation_rollback);
    ut::register_test("address_space_heap_rollback", address_space_heap_rollback);
    ut::register_test("address_space_control_rollback", address_space_control_rollback);
    ut::register_test("vma_heap_rollback", vma_heap_rollback);
    ut::register_test("asid_leases", asid_leases);
    ut::register_test("unmap_reclaims_tables", unmap_reclaims_tables);
  });
  moss::test::validation::register_self_cases();
  ut::register_suite("self.fail", [] {
    ut::register_test("intentional_assertion", failing_case);
    ut::register_test("not_run", empty_case);
  });
  ut::register_suite("self.panic", [] { ut::register_test("intentional_panic", panic_case); });
  ut::register_suite("self.timeout", [] { ut::register_test("intentional_timeout", timeout_case); });
}

} // namespace moss::test::validation
