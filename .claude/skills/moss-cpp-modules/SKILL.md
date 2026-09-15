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

Moss uses C++26 named modules with Clang 21 and CMake. Modules replace headers entirely — there are no `.h` files in this project. The freestanding environment means no standard library headers; everything flows through `import moss.std`.

## Step 1 — Choose Module Strategy

The module's total line count determines its structure:

| Total Lines | Strategy | Files |
|-------------|----------|-------|
| < 500 | **Single file** — interface + implementation in one `.cppm` | 1 `.cppm` |
| > 1000 | **Partitions** — primary interface is a re-export hub, logic split into partition `.cppm` files | N `.cppm` + M `.cpp` |
| Arch-specific | **Per-arch impl** — single `.cppm` interface + one `.cpp` per architecture | 1 `.cppm` + N `.cpp` |

## Step 2 — Write the Module Files

### Small module (single file)

Place interface and implementation in one `.cppm`. Non-exported code goes after the `export namespace` block.

```cpp
// src/timer/src/timer.cppm
export module moss.timer;      // ← primary interface declaration

import moss.std;               // ← imports immediately after declaration
import moss.types;
import moss.containers;

export namespace moss::kernel::timer {
  // ... exported API ...
}

namespace moss::kernel::timer {
  // ... non-exported implementation ...
}
```

### Large module (partitions)

The primary interface becomes a thin re-export hub (~20 lines). Each partition owns a logical slice of the API.

```cpp
// src/mm/src/mm.cppm — re-export hub
export module moss.mm;

import moss.std;
import moss.types;

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

Single `.cppm` interface, one `.cpp` per architecture. No partitions needed.

```cpp
// src/boot/src/boot.cppm — shared interface
export module moss.boot;

import moss.std;
import moss.mm;
// ... arch-specific .cpp files implement the same functions
```

## Step 3 — Handle Global Module Fragment (GMF)

The GMF is the region between `module;` and `export module`. Only three things belong there — everything else goes in the module purview:

| Content | Example |
|---------|---------|
| Vendored C headers | `#include "libfdt.h"` inside `extern "C" {}` |
| Linker script symbols | `extern "C" { extern char _text_start_addr[]; }` |
| Macro definitions | `#define` needed before module purview |

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

Every `.cppm` file (including partitions) must appear under `PUBLIC FILE_SET CXX_MODULES`. Implementation `.cpp` files go under `PRIVATE`.

```cmake
# src/mm/CMakeLists.txt
add_library(moss_mm OBJECT)

target_sources(moss_mm
    PUBLIC FILE_SET CXX_MODULES FILES
        src/mm.cppm              # primary interface
        src/mm-core.cppm         # partitions
        src/mm-page_table.cppm
        src/mm-policy.cppm
        src/mm-reclaim.cppm
        src/mm-interface.cppm
    PRIVATE
        src/page_table.cpp       # implementation units
        src/page_frame_allocator.cpp)

target_link_libraries(moss_mm PRIVATE moss_core moss_abi moss_containers)
```

For arch-specific modules, select `.cpp` files with `if(MOSS_TARGET_ARCH)`:

```cmake
# src/boot/CMakeLists.txt
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND BOOT_SOURCES src/arch/arm64/boot_impl.cpp)
elseif(MOSS_TARGET_ARCH STREQUAL "X64")
    list(APPEND BOOT_SOURCES src/arch/x64/boot_impl.cpp)
endif()

target_sources(moss_boot PUBLIC FILE_SET CXX_MODULES FILES src/boot.cppm)
```

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
| `moss.timer` | Single file | 1 `.cppm` |
| `moss.fdt` | Single file with GMF | 1 `.cppm` |
| `moss.containers` | Single file | 1 `.cppm` |
| `moss.mm` | 5 partitions (core, page_table, policy, reclaim, interface) | 6 `.cppm` + 6 `.cpp` |
| `moss.kernel` | 4 partitions (elf, syscall_table, syscall_arch, main) | 5 `.cppm` + 4 `.cpp` |
| `moss.process` | 3 partitions (types, scheduler, load_balancer) | 4 `.cppm` + 2 `.cpp` |
| `moss.boot` | Arch-specific (per-arch `.cpp` + `.S`) | 1 `.cppm` + 4 `.cpp` |
| core modules | Single file each | 1 `.cppm` each |
