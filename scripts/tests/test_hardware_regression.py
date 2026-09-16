"""Exercise production clock/IPI code with deterministic hardware boundaries."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def braced_definition(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 1
    position = opening + 1
    while depth:
        depth += (text[position] == "{") - (text[position] == "}")
        position += 1
    return text[start:position]


@pytest.fixture
def compiler():
    executable = shutil.which("clang++")
    if not executable:
        pytest.skip("Clang is needed to exercise the production C++ and ARM64 entry")
    return Path(executable)


def run_cpp(compiler, tmp_path, source):
    cpp = tmp_path / "regression.cpp"
    binary = tmp_path / "regression"
    cpp.write_text(source)
    subprocess.run([str(compiler), "-std=c++23", "-O2", str(cpp), "-o", str(binary)], check=True, capture_output=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


RESULT_STUB = r"""
#include <cstdint>
#include <cstdio>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
enum class ErrorCode { InvalidState, InvalidArgument, OutOfMemory };
struct VoidResult {
  bool ok = true;
  VoidResult() = default;
  VoidResult(ErrorCode) : ok(false) {}
  explicit operator bool() const { return ok; }
};
"""


def clocksource_source():
    text = (ROOT / "src/timer/src/timer.cppm").read_text()
    clock = braced_definition(text, "class Clocksource") + ";\n"
    start = text.index("VoidResult Clocksource::initialize()")
    end = text.index("// HrTimer implementation", start)
    # The fake controls the actual initialize() hardware inputs; the conversion
    # functions themselves are taken unchanged from the production module.
    return (
        RESULT_STUB
        + r"""
namespace hal::timer {
u64 injected_frequency;
u64 frequency() { return injected_frequency; }
u64 read_counter() { return 0; }
}
"""
        + clock
        + text[start:end]
    )


@pytest.mark.parametrize("frequency", [1 << 32, 100_000_000_000])
def test_high_frequency_clock_converts_subsecond_deadlines(compiler, tmp_path, frequency):
    # These are the old overflow boundary and the HAL's 100 GHz upper bound.
    source = (
        clocksource_source()
        + f"""
int main() {{
  hal::timer::injected_frequency = {frequency}ULL;
  Clocksource clock;
  if (!clock.initialize()) return 1;
  constexpr u64 ns = 1000000; // One millisecond: the old zero-inverse case loses it.
  const u64 expected = {frequency}ULL / 1000;
  const u64 actual = clock.ns_to_cycles(ns);
  if (actual > expected || expected - actual > 1) {{
    std::printf("frequency=%llu expected=%llu actual=%llu\\n",
      (unsigned long long){frequency}ULL, (unsigned long long)expected, (unsigned long long)actual);
    return 2;
  }}
}}
"""
    )
    result = run_cpp(compiler, tmp_path, source)
    assert result.returncode == 0, result.stdout + result.stderr


def test_kernel_clock_regression_helper(compiler, tmp_path):
    source = (
        clocksource_source()
        + r"""
namespace moss { using ::u64; }
namespace moss::kernel::timer { using ::Clocksource; }
"""
        + f'\n#include "{ROOT / "src/test/hardware_regression.hpp"}"\n'
        + "int main() { return moss::test::hardware::clocksource_high_frequency_regression() ? 0 : 1; }\n"
    )
    result = run_cpp(compiler, tmp_path, source)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("target", ["aarch64-none-elf", "riscv64-none-elf", "x86_64-none-elf"])
def test_clock_conversion_needs_no_freestanding_128_bit_runtime(compiler, tmp_path, target):
    nm = compiler.with_name("llvm-nm")
    if not nm.exists():
        pytest.skip("LLVM nm is needed to inspect freestanding runtime dependencies")
    # Replace only the host type/result scaffolding; compile the same production
    # Clocksource definitions for every kernel architecture without a sysroot.
    scaffold = r"""
using u8 = unsigned char;
using u32 = unsigned int;
using u64 = unsigned long long;
enum class ErrorCode { InvalidState };
struct VoidResult {
  VoidResult() = default;
  VoidResult(ErrorCode) {}
};
"""
    cpp = tmp_path / "clock.cpp"
    output = tmp_path / "clock.o"
    cpp.write_text(clocksource_source().replace(RESULT_STUB, scaffold))
    subprocess.run(
        [
            str(compiler),
            f"--target={target}",
            "-std=c++23",
            "-O2",
            "-ffreestanding",
            "-fno-exceptions",
            "-fno-rtti",
            "-c",
            str(cpp),
            "-o",
            str(output),
        ],
        check=True,
        capture_output=True,
    )
    undefined = subprocess.run([str(nm), "-u", str(output)], check=True, capture_output=True, text=True).stdout
    assert not re.search(r"__(?:u?div(?:mod)?|u?mod|mul)ti[34]", undefined), undefined


def test_arm64_upper_half_uses_4k_granule(compiler, tmp_path):
    text = (ROOT / "src/hal/mmu/src/mmu_hal.cppm").read_text()
    config = braced_definition(text, "struct AddressSpaceConfig") + ";"
    source = (
        RESULT_STUB
        + "\n#define MOSS_ARCH_ARM64\n"
        + config
        + r"""
int main() {
  const u64 tg1 = (AddressSpaceConfig::TCR_VALUE >> 30) & 3;
  // Arm TCR_EL1.TG1[31:30]=2 selects 4 KiB; zero is reserved, unlike TG0.
  if (tg1 != 2) { std::printf("TG1=%llu, expected=2\n", (unsigned long long)tg1); return 1; }
}
"""
    )
    result = run_cpp(compiler, tmp_path, source)
    assert result.returncode == 0, result.stdout + result.stderr


def test_arm64_el3_returns_to_el2_with_hvc_enabled(compiler, tmp_path):
    objdump = compiler.with_name("llvm-objdump")
    if not objdump.exists():
        pytest.skip("LLVM objdump is needed to inspect ARM64 entry instructions")
    output = tmp_path / "entry.o"
    subprocess.run(
        [
            str(compiler),
            "--target=aarch64-none-elf",
            "-DMOSS_ARCH_ARM64",
            "-I",
            str(ROOT / "src/abi/include"),
            "-c",
            str(ROOT / "src/boot/src/arch/arm64/start_arm64.S"),
            "-o",
            str(output),
        ],
        check=True,
        capture_output=True,
    )
    decoded = subprocess.run(
        [str(objdump), "-d", "--disassemble-symbols=el3_entry", str(output)], check=True, capture_output=True, text=True
    ).stdout
    immediates = [int(value, 0) for value in re.findall(r"\bmov\s+x0,\s*#(0x[\da-f]+|\d+)", decoded)]
    assert len(immediates) == 2, decoded
    scr, status = immediates
    # SCR.NS/RW select non-secure AArch64; RES1[5:4] must survive. HCE[8]
    # permits the HVC firmware transport used after the descent to EL1.
    required_scr = (1 << 0) | (3 << 4) | (1 << 8) | (1 << 10)
    assert scr & required_scr == required_scr, decoded
    assert not scr & (1 << 7), decoded  # SCR.SMD must keep the SMC transport legal.
    # SPSR.M[4:0]=9 selects EL2h, allowing el2_entry to access HCR_EL2.
    assert status & 31 == 9, decoded
    assert status & (15 << 6) == 15 << 6, decoded  # Mask FIQ/IRQ/SError/debug before vector setup.
    assert "<el2_entry>" in decoded, decoded
    assert decoded.index("SCR_EL3") < decoded.index("SPSR_EL3") < decoded.index("ELR_EL3") < decoded.index("eret")


def ipi_source():
    interface = (ROOT / "src/interrupts/src/interrupts.cppm").read_text()
    types = braced_definition(interface, "namespace simple {")
    implementation = (ROOT / "src/interrupts/src/ipi_simple.cpp").read_text().replace("module moss.interrupts;", "")
    stub = (
        RESULT_STUB
        + r"""
namespace moss::kernel {
inline constexpr u32 BOOT_MAX_CPUS = 16;
namespace logging::klog { void info(const char*) {} void error(const char*) {} }
namespace arch { u32 get_current_cpu_id() { return 2; } }
namespace platform { struct { u32 cpu_count = 3; } hardware; }
namespace containers {
enum class MemoryOrder { Relaxed };
struct AtomicU64 {
  u64 value = 0;
  void store(u64 n, MemoryOrder) { value = n; }
  u64 fetch_add(u64 n, MemoryOrder) { auto old = value; value += n; return old; }
  u64 load(MemoryOrder) const { return value; }
};
}
namespace interrupts::hw_simple {
enum class IpiResult { Success, InvalidCpu, InvalidType, NotInitialized, HardwareError };
struct SimpleHardwareIpi {
  IpiResult verdict = IpiResult::Success;
  u32 calls = 0;
  u32 targets = 0;
  IpiResult ping_cpu(u32 cpu) { ++calls; targets |= 1U << cpu; return verdict; }
};
SimpleHardwareIpi* g_simple_hardware_ipi = nullptr;
}
namespace interrupts {
"""
    )
    return stub + types + "\n}\n}\n" + implementation


def test_simple_ipi_does_not_report_success_without_delivery(compiler, tmp_path):
    source = (
        ipi_source()
        + r"""
int main() {
  using namespace moss::kernel::interrupts;
  simple::SimpleInterProcessorInterrupt empty;
  if (empty.initialize(0)) return 6;
  simple::SimpleInterProcessorInterrupt manager;
  if (manager.ping_cpu(1) != simple::IpiResult::NotInitialized) return 7;
  if (!manager.initialize(3)) return 1;
  if (manager.ping_cpu(1) == simple::IpiResult::Success || manager.get_system_info().total_pings_sent) {
    std::puts("simple IPI reported success and counted a send without a hardware backend"); return 2;
  }
  hw_simple::SimpleHardwareIpi backend;
  hw_simple::g_simple_hardware_ipi = &backend;
  if (manager.ping_cpu(3) == simple::IpiResult::Success ||
      manager.send_ipi(1, static_cast<simple::IpiType>(0)) == simple::IpiResult::Success || backend.calls) return 8;
  backend.verdict = hw_simple::IpiResult::NotInitialized;
  if (manager.ping_cpu(1) != simple::IpiResult::NotInitialized || manager.get_system_info().total_pings_sent) return 9;
  backend.calls = 0;
  backend.verdict = hw_simple::IpiResult::HardwareError;
  if (manager.ping_cpu(1) == simple::IpiResult::Success || manager.get_system_info().total_pings_sent) return 3;
  backend.verdict = hw_simple::IpiResult::Success;
  if (manager.ping_cpu(1) != simple::IpiResult::Success || backend.calls != 2 ||
      manager.get_system_info().total_pings_sent != 1) return 4;
  backend.calls = 0; backend.targets = 0;
  // Source CPU is logical index 2, so self-test must notify 0 and 1, not 2.
  if (!manager.self_test() || backend.calls != 2 || backend.targets != 3) return 5;
}
"""
    )
    result = run_cpp(compiler, tmp_path, source)
    assert result.returncode == 0, result.stdout + result.stderr
