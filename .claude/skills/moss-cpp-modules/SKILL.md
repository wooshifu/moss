---
name: moss-cpp-modules
description: This skill should be used when creating a new C++ module, adding a partition to an existing module, modifying module declarations (.cppm), writing module implementation files (.cpp), registering modules in CMakeLists.txt, or encountering module-related compilation errors. Also use when the user mentions "module", "partition", "cppm", "export module", "import", or "Global Module Fragment".
---

# C++ Modules — Moss Kernel

```
Every module file must be one of exactly three roles:
primary interface (.cppm), partition interface (-part.cppm), or implementation unit (.cpp).
Pick the right one BEFORE writing any code.
```

Moss uses C++26 named modules with Clang 23 or newer and CMake. Production kernel C++ has no headers of its own. The only headers are:

- the C ABI headers shared with userspace, in `src/abi/include/moss/` (for example `syscall_numbers.def` and `domain_spawn.h`), which kernel code includes through a Global Module Fragment;
- test-only `.hpp` helpers under `src/test/`;
- the C userspace headers under `src/userspace/`.

The environment is freestanding, so there are no standard library headers; use `import moss.std`.

## Step 1 — Choose Module Strategy

The module's total line count determines its structure:

| Total Lines | Strategy | Files |
|-------------|----------|-------|
| < 500 | **Single file** — interface + implementation in one `.cppm` | 1 `.cppm` |
| > 1000 | **Partitions** — primary interface is a re-export hub, logic split into partition `.cppm` files | N `.cppm` + M `.cpp` |
| Arch-specific | **Per-arch impl** — single `.cppm` interface + one `.cpp`/`.S` per architecture, selected by `MOSS_TARGET_ARCH` | 1 `.cppm` + N `.cpp`/`.S` |

Between 500 and 1000 lines, follow the module's existing shape. Some modules predate these thresholds: `moss.containers` is a single ~2000-line `.cppm`. Small architecture differences inside one file use the `MOSS_ARCH_ARM64` / `MOSS_ARCH_X64` / `MOSS_ARCH_RISCV64` compile definitions instead of per-arch files; `moss.arch` in `src/aal` works this way.

## Step 2 — Write the Module Files

### Small module (single file)

Place interface and implementation in one `.cppm`. Non-exported code goes after the `export namespace` block.

```cpp
// src/timer/src/timer.cppm
export module moss.timer;      // ← primary interface declaration

import moss.std;               // ← imports immediately after declaration
import moss.types;
import moss.result;
import moss.arch;
import moss.hal.timer;
import moss.containers;

export namespace moss::kernel::timer {
  // ... exported API ...
}

namespace moss::kernel::timer {
  // ... non-exported implementation ...
}
```

### Large module (partitions)

The primary interface re-exports every partition. Each partition owns a logical slice of the API. Keep the primary small: imports, `export import` lines and, at most, glue that must live in the primary. For example, `moss.mm` defines its `extern "C"` allocator shim there.

```cpp
// src/mm/src/mm.cppm — re-export hub
export module moss.mm;

import moss.std;
import moss.types;
import moss.result;
import moss.fdt;
import moss.containers;
import moss.arch;
import moss.platform;
import moss.hal.mmu;
import moss.logging;

export import :core;           // ← re-exports from mm-core.cppm
export import :page_table;
export import :policy;
export import :reclaim;
export import :interface;
```

Partition files use colon syntax (`:name`, not `.name`):

```cpp
// src/mm/src/mm-core.cppm
export module moss.mm:core;    // ← partition declaration (colon, not dot)

import moss.std;
import moss.types;

export inline constexpr moss::kernel::usize PAGE_SIZE = 4096;
```

**Naming convention:** partition files are named `{module}-{partition}.cppm` (e.g., `mm-core.cppm`, `kernel-elf.cppm`).

### Implementation unit (.cpp)

A `.cpp` that belongs to a module. It sees all partitions automatically — no need to specify which.

```cpp
// src/mm/src/page_table.cpp
module moss.mm;                // ← no "export", no partition — just the module name

import moss.logging;           // ← additional imports are allowed here

namespace moss::kernel::mm {
  KernelResult<PageTable *> PageTableManager::allocate_page_table() {
    // ...
  }
}
```

### Arch-specific module (boot pattern)

A single `.cppm` interface and a shared `boot.cpp`, plus one `src/arch/<arch>/boot_impl.cpp` and one `start_<arch>.S` per architecture. No partitions are needed. Each per-arch `.cpp` is an ordinary implementation unit, `module moss.boot;`, and only the current architecture's files are compiled. `moss.process` and `moss.kernel` select their per-arch assembly (`context_switch.S`, `*_syscall.S`, …) the same way.

```cpp
// src/boot/src/boot.cppm — shared interface
export module moss.boot;

import moss.std;
import moss.mm;
// ... src/arch/<arch>/boot_impl.cpp implements the arch-specific functions
```

## Step 3 — Handle Global Module Fragment (GMF)

The GMF is the region between `module;` and the module declaration (`export module …` or, in an implementation unit, `module …`). Only these belong there; everything else goes in the module purview:

| Content | Example |
|---------|---------|
| Vendored C headers | `#include "libfdt.h"` inside `extern "C" {}` (`fdt.cppm`) |
| Moss C ABI headers | `#include <moss/domain_spawn.h>` (`syscall_table.cpp`), `#include "moss/trap_frame_offsets.h"` (`trap_frame.cppm`) |
| Linker script symbols | `extern "C" { extern char _text_start_addr[]; }` (`abi.cppm`) |
| C-linkage entry points shared with assembly | `extern "C" void kernel_page_fault_handler(...) noexcept;` (`page_fault.cpp`) |
| Macro definitions | `#define` needed before the module purview (`boot.cpp`) |

Each file that needs a GMF must have its own — C++ modules do not share GMF content across files.

```cpp
// src/fdt/src/fdt.cppm — GMF with vendored C header
module;

extern "C" {
#include "libfdt.h"
}

export module moss.fdt;
import moss.std;
```

```cpp
// src/abi/src/abi.cppm — GMF with linker symbols
module;

extern "C" {
extern char _text_start_addr[];
extern char _bss_end_addr[];
}

export module moss.abi;
```

## Step 4 — Register in CMake

Every `.cppm` file (including partitions) must appear under `PUBLIC FILE_SET CXX_MODULES`. Implementation `.cpp` files go under `PRIVATE`. Each area is an **OBJECT** library; a STATIC archive would drop symbols referenced only from assembly or vector tables.

```cmake
# src/mm/CMakeLists.txt (cmake-format style)
add_library(moss_mm OBJECT)

target_sources(
    moss_mm
    PUBLIC FILE_SET
           CXX_MODULES
           FILES
           src/mm.cppm
           src/mm-core.cppm
           src/mm-page_table.cppm
           src/mm-policy.cppm
           src/mm-reclaim.cppm
           src/mm-interface.cppm
    PRIVATE src/page_table.cpp
            src/page_fault.cpp
            src/page_frame_allocator.cpp
            src/runtime_heap_allocator.cpp
            src/mm_interface_impl.cpp
)

target_link_libraries(
    moss_mm
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_platform
            moss_hal_mmu
            moss_fdt
            moss_containers
            moss_logging
            # ...
)
```

For arch-specific sources, build a source list from `MOSS_TARGET_ARCH` (`ARM64`, `X64` or `RISCV64`):

```cmake
# src/boot/CMakeLists.txt
set(BOOT_SOURCES src/boot.cpp)
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND BOOT_SOURCES src/arch/arm64/boot_impl.cpp src/arch/arm64/start_arm64.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X64")
    list(APPEND BOOT_SOURCES src/arch/x64/start_x64.S src/arch/x64/isr_x64.S src/arch/x64/boot_impl.cpp)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV64")
    list(APPEND BOOT_SOURCES src/arch/riscv64/start_riscv64.S src/arch/riscv64/boot_impl.cpp)
endif()
add_library(moss_boot OBJECT ${BOOT_SOURCES})

target_sources(moss_boot PUBLIC FILE_SET CXX_MODULES FILES src/boot.cppm)
```

A **new area** also needs two edits in the top-level `CMakeLists.txt`:

1. `add_subdirectory(src/<area>)` in dependency order (`intrinsics` → `core` → `abi` → … → `kernel` → `boot`). OBJECT libraries cannot form dependency cycles; break a cycle with an `extern "C"` shim, as `moss_heap_allocate` does in `mm.cppm`.
2. Add `moss_<area>` to the `target_link_libraries(moss.elf ...)` list.

`src/test/CMakeLists.txt` copies `moss.elf`'s `LINK_LIBRARIES` into `moss.test.elf`, so the validation image picks up the new library automatically. Run `uv run format` afterwards, because cmake-format re-wraps long source lists.

## Red Flags — Common Errors

When a module-related build fails, check this table before attempting fixes:

| Error | Cause | Fix |
|-------|-------|-----|
| `import` after non-import statement | `import` placed in the middle of a file | Move all `import` statements immediately after the `export module` declaration |
| `static` function in `export namespace` | Internal-linkage function inside an exported block | Move `static` functions outside `export namespace`, into a non-exported namespace |
| Attribute mismatch on declaration/definition | `[[noreturn]]` on declaration but missing on definition (or vice versa) | Keep attributes identical on both declaration and definition |
| `.cppm` not found by other modules | Partition `.cppm` missing from CMake registration | Add every `.cppm` to `PUBLIC FILE_SET CXX_MODULES FILES` |
| Undefined symbol from linker script | `extern char _symbol[]` in module purview | Move `extern "C"` declarations into the Global Module Fragment |

## Existing Modules

Use this table to find examples matching the pattern needed:

| Module | Strategy | Files |
|--------|----------|-------|
| `moss.timer` | Single file (~600 lines) | 1 `.cppm` |
| `moss.fdt` | Single file with GMF (vendored libfdt) | 1 `.cppm` |
| `moss.containers` | Single file (~2000 lines, predates the partition rule) | 1 `.cppm` |
| `moss.abi` | GMF with linker symbols; `trap_frame` partition includes a Moss C ABI header | 2 `.cppm` + 2 `.S` |
| `moss.intrinsics` | 7 partitions (atomic, bitops, bswap, control, memory, source, traits) | 8 `.cppm` |
| `moss.mm` | 5 partitions (core, page_table, policy, reclaim, interface) | 6 `.cppm` + 5 `.cpp` |
| `moss.kernel` | 4 partitions (elf, syscall_table, syscall_arch, main) + per-arch `.S` | 5 `.cppm` + 4 `.cpp` |
| `moss.process` | 5 partitions (types, scheduler, load_balancer, signal, uaccess) + per-arch `.S` | 6 `.cppm` + 4 `.cpp` |
| `moss.vfs` | 10 partitions (types, inode, dcache, file, mount, buffer, ramfs, devfs, pipefs, syscall) | 11 `.cppm` + 3 `.cpp` |
| `moss.boot` | Arch-specific (shared `boot.cpp` + per-arch `boot_impl.cpp` and `.S`) | 1 `.cppm` + 4 `.cpp` |
| `moss.arch` | Single interface; architecture selected with `MOSS_ARCH_*` | 1 `.cppm` + 1 `.cpp` |
| core modules (`moss.std`, `moss.types`, `moss.result`, `moss.concepts`, `moss.smart_ptr`) | Single file each | 1 `.cppm` each |

Kernel validation files in `src/test/validation/` are **not** module units. They are plain global-module translation units that `import` production modules and `#include` local `.hpp` helpers. Do not add `module moss.kernel;` to them, because that changes their linkage.
