"""Compile the production parser against firmware-style bindings, not QEMU's simplified tree."""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def without_modules(source):
    source = re.sub(r"^(?:export )?module[^;]*;|^import[^;]*;", "", source, flags=re.MULTILINE)
    return source.replace("export namespace", "namespace")


@pytest.fixture(scope="module")
def parser_binary(tmp_path_factory):
    clang, clangxx, dtc = (shutil.which(name) for name in ("clang", "clang++", "dtc"))
    if not all((clang, clangxx, dtc)):
        pytest.skip("LLVM and dtc are required for the production FDT parser harness")
    output = tmp_path_factory.mktemp("fdt-firmware")
    include = ROOT / "src/fdt/libfdt"
    objects = []
    # The parser uses readers only; fdt_addresses.c also references unvendored writer APIs.
    for name in ("fdt.c", "fdt_ro.c", "fdt_strerror.c"):
        source = include / name
        obj = output / (source.stem + ".o")
        subprocess.run([clang, "-I", str(include), "-c", str(source), "-o", str(obj)], check=True, capture_output=True)
        objects.append(str(obj))
    source = output / "parser.cpp"
    source.write_text(
        """
#include <cstdint>
#include <cstdio>
namespace moss {
using u8 = std::uint8_t; using u16 = std::uint16_t;
using u32 = std::uint32_t; using u64 = std::uint64_t;
using PhysAddr = u64; using VirtAddr = u64; using usize = std::size_t;
namespace kernel { using moss::PhysAddr; using moss::VirtAddr; using moss::usize; }
}
"""
        + without_modules((ROOT / "src/platform/src/platform.cppm").read_text())
        + without_modules((ROOT / "src/fdt/src/fdt.cppm").read_text())
        + r"""
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  auto *input = std::fopen(argv[1], "rb");
  if (!input) return 2;
  // Fixtures are small; a 64 KiB bound avoids depending on hosted C++ string
  // headers, whose libc declarations conflict with the kernel's freestanding ones on macOS.
  alignas(std::uint64_t) char tree[64 * 1024];
  auto size = std::fread(tree, 1, sizeof(tree), input);
  std::fclose(input);
  if (!size || size == sizeof(tree) || !moss::fdt::parse_dtb(tree)) return 3;
  const auto &p = moss::kernel::platform::hardware;
  std::printf("{\"memory_valid\":%s,\"timer\":%u,\"uart\":%u,\"intc_valid\":%s}\n",
              p.memory_map_valid ? "true" : "false", p.timer_interrupt, p.uart.irq, p.intc.valid ? "true" : "false");
}
"""
    )
    binary = output / "parser"
    result = subprocess.run(
        [clangxx, "-std=c++23", "-I", str(include), str(source), *objects, "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    return binary, dtc


@pytest.mark.parametrize("timer", ["arm,armv7-timer", "arm,armv8-timer"])
@pytest.mark.parametrize(
    ("pool", "memory_valid"),
    [
        ("size = <0x4000000>; reusable;", True),
        ("reg = <0x10000000 0x1000000>;", True),
        ("size = <0>; reusable;", False),
        ("size = <0x4000000>; reusable; no-map;", False),
        ("reusable;", False),
    ],
)
def test_real_binding_timer_uart_and_memory(parser_binary, tmp_path, timer, pool, memory_valid):
    binary, dtc = parser_binary
    source, tree = tmp_path / "firmware.dts", tmp_path / "firmware.dtb"
    source.write_text(
        """/dts-v1/;
/ {
  #address-cells = <1>; #size-cells = <1>; interrupt-parent = <&armctrl>;
  memory@0 { device_type = "memory"; reg = <0 0x3b400000>; };
  cpus { #address-cells = <1>; #size-cells = <0>;
    cpu@0 { device_type = "cpu"; reg = <0>; };
  };
  reserved-memory { #address-cells = <1>; #size-cells = <1>; ranges;
    pool { compatible = "shared-dma-pool"; POOL };
  };
  local: local_intc@40000000 { compatible = "brcm,bcm2836-l1-intc";
    reg = <0x40000000 0x100>; interrupt-controller; #interrupt-cells = <2>;
  };
  armctrl: interrupt-controller@3f00b200 { compatible = "brcm,bcm2836-armctrl-ic";
    reg = <0x3f00b200 0x200>; interrupt-controller; #interrupt-cells = <2>;
    interrupt-parent = <&local>; interrupts = <8 4>;
  };
  timer { compatible = "TIMER"; interrupt-parent = <&local>;
    interrupts = <0 4>, <1 4>, <3 4>, <2 4>;
  };
  serial@3f201000 { compatible = "arm,pl011"; reg = <0x3f201000 0x200>;
    interrupts = <2 25>;
  };
};
""".replace("POOL", pool).replace("TIMER", timer)
    )
    subprocess.run([dtc, "-q", "-O", "dtb", "-o", str(tree), str(source)], check=True, capture_output=True)
    result = subprocess.run([str(binary), str(tree)], check=True, capture_output=True, text=True)
    # BCM local source 3 maps to HAL IRQ 19; ARMCTRL bank 2 source 25 maps to IRQ 89.
    assert json.loads(result.stdout) == {"memory_valid": memory_valid, "timer": 19, "uart": 89, "intc_valid": True}
