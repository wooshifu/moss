# Third-Party Source Provenance

This inventory accompanies [ADR-0007](adr/0007-vendor-third-party-sources-with-native-cmake.md). Sources are checked into `third_party/<dependency>/` and built through the [native userspace CMake integration](../src/userspace/mlibc/CMakeLists.txt). The original cached source archives were SHA-256 verified before import on 2026-09-16. Configuration and compilation do not download, extract, patch or rewrite source trees, or invoke Meson or BusyBox Make.

## Pinned Source Inputs

Complete upstream source trees are retained for BusyBox, mlibc and their small supporting dependencies, while compiling only the agreed Moss functionality. compiler-rt is limited to builtins and necessary supporting files, with the required LLVM CMake modules; this does not import the full LLVM project. Original license and attribution files are retained. Unused source does not expand the supported application or architecture profile.

| Dependency | Exact upstream version or commit | Original source | Pinned source archive |
| --- | --- | --- | --- |
| BusyBox | `1.37.0` | [Upstream project](https://busybox.net/) | [busybox-1.37.0.tar.bz2](https://busybox.net/downloads/busybox-1.37.0.tar.bz2) |
| mlibc | `7.0.0`; `7c2a178142625cc9852e59a1a090468c61a62d3b` | [Pinned tree](https://github.com/managarm/mlibc/tree/7c2a178142625cc9852e59a1a090468c61a62d3b) | [Commit archive](https://codeload.github.com/managarm/mlibc/tar.gz/7c2a178142625cc9852e59a1a090468c61a62d3b) |
| frigg | `b0dbea66bc19f7c5546f0039a3be842feb02678c` | [Pinned tree](https://github.com/managarm/frigg/tree/b0dbea66bc19f7c5546f0039a3be842feb02678c) | [Commit archive](https://codeload.github.com/managarm/frigg/tar.gz/b0dbea66bc19f7c5546f0039a3be842feb02678c) |
| libsmarter | `f7d061bc37d485418344452c7ceb28d5df3ba85d` | [Pinned tree](https://github.com/managarm/libsmarter/tree/f7d061bc37d485418344452c7ceb28d5df3ba85d) | [Commit archive](https://codeload.github.com/managarm/libsmarter/tar.gz/f7d061bc37d485418344452c7ceb28d5df3ba85d) |
| freestnd-c-hdrs | `d33711241b46ecb8f2ad33927fcefdcb3ac0162e` | [Pinned tree](https://github.com/osdev0/freestnd-c-hdrs/tree/d33711241b46ecb8f2ad33927fcefdcb3ac0162e) | [Commit archive](https://codeload.github.com/osdev0/freestnd-c-hdrs/tar.gz/d33711241b46ecb8f2ad33927fcefdcb3ac0162e) |
| freestnd-cxx-hdrs | `a6b351e0ab3e74e5789b01fa1447e4cd62373da7` | [Pinned tree](https://github.com/osdev0/freestnd-cxx-hdrs/tree/a6b351e0ab3e74e5789b01fa1447e4cd62373da7) | [Commit archive](https://codeload.github.com/osdev0/freestnd-cxx-hdrs/tar.gz/a6b351e0ab3e74e5789b01fa1447e4cd62373da7) |
| compiler-rt builtins | `20.1.8`; tag `llvmorg-20.1.8` | [Pinned builtins tree](https://github.com/llvm/llvm-project/tree/llvmorg-20.1.8/compiler-rt/lib/builtins) | [compiler-rt-20.1.8.src.tar.xz](https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/compiler-rt-20.1.8.src.tar.xz) |
| LLVM CMake modules | `20.1.8`; tag `llvmorg-20.1.8` | [Pinned CMake tree](https://github.com/llvm/llvm-project/tree/llvmorg-20.1.8/cmake) | [cmake-20.1.8.src.tar.xz](https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/cmake-20.1.8.src.tar.xz) |

The pinned mlibc revision declares `7.0.0` in its [upstream build metadata](https://raw.githubusercontent.com/managarm/mlibc/7c2a178142625cc9852e59a1a090468c61a62d3b/meson.build). The two LLVM archives belong to the [LLVM 20.1.8 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-20.1.8).

### Original Archive SHA-256

These verified hashes describe the original archives, not extracted directories or locally adapted source trees. Reverify input archives before an update; do not replace these values with hashes of modified Moss sources.

| Dependency | Recorded SHA-256 |
| --- | --- |
| BusyBox | `3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4` |
| mlibc | `22535f15a789b463bf19abb45b6d4aded20cf5f0b207eb43922b2f95e4507603` |
| frigg | `10cbee1dab6e7b0a1ca8d6d59d9eeffab1ed3cf6b0031551fd63a6861cc07a41` |
| libsmarter | `1426e24f3c3c2a08983fad71d461faa415f0ccc1ca432dd80bebe1dac9cc7bce` |
| freestnd-c-hdrs | `d3f0c1e0720dec9da97175eebdc51ac4b9bdd776b5687b2a1ccd97f6e861498e` |
| freestnd-cxx-hdrs | `dc4a44daef5d50a6f9aea0cb7bd2164c208f159d273c51f3ff8334b5bd65ef95` |
| compiler-rt builtins | `15277402f6fd63397c0917a5c7171cda82d16d226094b828c1ed0f58f73b9c69` |
| LLVM CMake modules | `3319203cfd1172bbac50f06fa68e318af84dcb5d65353310c0586354069d6634` |

## In-Repository Layout and Adaptations

| Location | Imported scope and direct Moss changes |
| --- | --- |
| [busybox](../third_party/busybox) | Complete release. `include/platform.h` and `libbb/appletlib.c` retain the Moss compatibility guards. Added native `CMakeLists.txt`, `moss-host-tools/CMakeLists.txt`, explicit `moss-sources.cmake` and resolved `moss.config`. |
| [mlibc](../third_party/mlibc) | Complete commit tree. Added native CMake sources/configuration and `sysdeps/moss` (moved from `src/userspace/mlibc/sysdeps`, with demo ABI headers). `options/ansi/generic/file-io.cpp` avoids seeking when there is no buffered offset to undo; RV64 `setjmp.S` guards floating-point saves/restores for the soft-float ABI. No upstream Meson rewrite is required. |
| [frigg](../third_party/frigg), [libsmarter](../third_party/libsmarter), [freestnd-c-hdrs](../third_party/freestnd-c-hdrs), [freestnd-cxx-hdrs](../third_party/freestnd-cxx-hdrs) | Complete, unmodified commit archives, including licenses, documentation and unused architecture headers. Used as header dependencies. |
| [compiler-rt](../third_party/compiler-rt) | `lib/builtins/`, `cmake/`, `include/`, `LICENSE.TXT`, `CREDITS.TXT`, `README.txt`, `Maintainers.md` from the compiler-rt archive; added a small native CMake entry point. Upstream builtins CMake targets run inside the same build graph. Sanitizers, tests and other runtimes are not imported. |
| [cmake](../third_party/cmake) | Complete LLVM CMake archive, including license; used by compiler-rt. This is not an LLVM compiler source checkout. |

Upstream Meson, Make/Kbuild, wrap files and download helpers remain in the complete source snapshots for reference. They are not the active Moss build path. There are no source submodules or generated-source caches to populate before compilation.

## Native Build Contract

The supported profile is ARM64, x64 and RV64, with static mlibc and 23 BusyBox applets: `ash`, `sh`, `ls`, `cat`, `mkdir`, `cp`, `mv`, `rm`, `grep`, `wc`, `head`, `cut`, `sort`, `uniq`, `tr`, `tee`, `cmp`, `basename`, `dirname`, `rmdir`, `uname`, `kill` and restricted `find`. `find` supports name/path/type/size filters, depth bounds, Boolean expressions and NUL-delimited output; time filters, execution and deletion are disabled. `sed` is not integrated. Uncompressed verbose command help is enabled.

The production initramfs contains `/init.elf`, `/file-service.elf`, `/moss-file.elf` and `/busybox.elf`. The embedded init trampoline starts `/init.elf`, which launches a capability-backed single-file service and supervises interactive ash with `PATH=/`. `/moss-file.elf` reads and writes that file through a delegated endpoint, independently of the kernel VFS. The validation initramfs instead provides `/validation.elf`, including the signal regression cases, which the same trampoline selects first. Standalone shell applet lookup re-executes `/busybox.elf`, so selected commands and `sh` work without applet symlinks or `/proc/self/exe`. Job control and a terminal line editor remain disabled. RV64 uses `rv64imac/lp64`; builtins supply compiler-generated arithmetic helpers, including the soft-float helpers needed by mlibc. There is no dynamic linker or Linux binary-compatibility promise.

The file service currently holds one volatile file of at most 255 bytes. A service restart loses its contents and creates a new endpoint; the supervisor restarts the shell to pass it the new capability. The kernel VFS still loads images and handles BusyBox file operations. Namespace lookup, larger shared-memory data transfers and POSIX file compatibility remain to be migrated.

The checked-in mlibc source/header lists were derived once from the pinned static profile; the BusyBox list and fully resolved configuration were taken from the same previously validated profile on all three architectures. Builds do not regenerate these lists from Meson or Kbuild. Enabling an additional libc option or applet requires deliberately updating the profile, selected sources, generated-header dependencies and runtime coverage together. Arbitrary BusyBox `.config` files are not supported.

mlibc headers and configuration are generated/copied into the build sysroot. Its math, regex and static-runtime objects feed `libc.a`. Two host-native BusyBox generators produce the applet and usage tables using the upstream generation code. BusyBox and the libc probe link real CMake libraries, then produce the existing stripped validation ELF paths. All generated files stay in the build tree; compiler dependency files drive rebuilds after direct edits to vendored source.

Host requirements remain external: Clang/Clang++ and LLVM binutils/LLD, CMake 3.31 or newer, Ninja, Python/uv and the repository's Python dependencies. BusyBox's host generators require a working host C compiler/libc, POSIX shell utilities and bzip2. QEMU is required only to run guest tests. The normal preset commands in the README are unchanged. With these tools installed, the target-source build works offline; `uv sync` itself still needs preinstalled packages or its package cache.

Repository-wide format, Ruff and clang-tidy runs exclude `third_party/` to preserve upstream source style. Git attributes also disable automatic line-ending conversion there, preserving archive bytes. Format new Moss CMake integration files explicitly when editing them. Original license files and embedded notices are authoritative; this inventory does not replace their redistribution requirements.

## Previously Vendored Code

The following records distinguish a verified matching upstream baseline from a historical Moss import. They do not claim an exhaustive inventory of all historically adapted code.

| Existing code | Available evidence | Provenance boundary |
| --- | --- | --- |
| [libfdt](../src/fdt/libfdt) | Seven upstream files match [dtc commit `8d15a63e84ff62fd31e8278088fa1176f2735eef`](https://git.kernel.org/pub/scm/utils/dtc/dtc.git/tree/libfdt?id=8d15a63e84ff62fd31e8278088fa1176f2735eef) byte-for-byte: `fdt.c`, `fdt.h`, `fdt_addresses.c`, `fdt_ro.c`, `fdt_strerror.c`, `libfdt.h`, `libfdt_internal.h`. Moss import: `48ec360a6c98d77c53085b959f73a16f1c703a3b`. | This establishes a matching upstream baseline, not proof of the original download revision. `libfdt_env.h` is the Moss freestanding replacement described by the import commit. This migration leaves the existing subset unchanged. |
| [ut_kernel](../src/test/framework/ut_kernel.hpp) | The header describes a simplified Boost.UT adaptation. Its [Moss introduction](https://github.com/wooshifu/moss/blob/eb817d677049a8c7f9d3893c9590d510b14decf1/src/test/framework/ut_kernel.hpp) is `eb817d677049a8c7f9d3893c9590d510b14decf1`. | No original Boost.UT revision or original archive was recorded. That historical provenance gap remains explicit; the Moss commit is not an upstream Boost.UT version. The framework is not replaced by this migration. |

Version updates must update upstream links, exact pins, archive digests and the documented adaptations together; do not silently substitute a moving branch or the latest release. Compare the complete imported source with the pinned archive, review the local adaptations, and rerun the [migration acceptance matrix](adr/0007-vendor-third-party-sources-with-native-cmake.md#migration-acceptance).
