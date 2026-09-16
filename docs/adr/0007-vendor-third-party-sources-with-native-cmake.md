---
status: accepted
date: 2026-09-16
---

# Vendor Third-Party Sources with Native CMake

The existing userspace build uses CMake to download and adapt sources, then invokes Meson for mlibc and Make for BusyBox. The user selected checked-in sources with direct Moss adaptations and native CMake compilation targets, not another CMake wrapper around those upstream build systems. Building mlibc and BusyBox must no longer invoke Meson or BusyBox Make.

The CMake port covers Moss's three supported architectures and the application profile in [ADR-0006](0006-validate-general-purpose-kernel-capabilities.md), preserving BusyBox 1.37.0 and the static mlibc/Moss runtime choice. This means maintaining the selected source lists, generated headers and build dependencies locally; it does not promise CMake support for every upstream platform or BusyBox applet.

## Source Dependency Boundary

Vendor BusyBox, mlibc, frigg, libsmarter, freestnd-c-hdrs, freestnd-cxx-hdrs, compiler-rt builtins and the LLVM CMake modules needed to build them. Existing checked-in third-party code, such as libfdt, remains included. This boundary covers the source dependencies of the current target programs, not the host tools themselves: Clang/LLVM compilers, QEMU, CMake, Ninja, Python/uv and their tooling dependencies remain externally provided.

## Source Retention and Provenance

Preserve complete upstream source trees for BusyBox, mlibc and the small supporting dependencies, including upstream documentation and license files; select the supported functionality in CMake rather than deleting unused source files. The previously agreed compiler-rt boundary remains builtins and its necessary supporting files, not the entire LLVM project. Apply Moss adaptations directly to the checked-in sources.

Keep original upstream source links and exact release versions or full commit IDs in repository documentation. The [source provenance inventory](../third-party-sources.md) records the current pins and archive digests; these identify the original inputs, not the subsequently adapted Moss trees. Preserve provenance when sources are updated and distinguish upstream revisions from Moss import commits.

## Migration Acceptance

The user confirmed the following acceptance boundary:

- Run clean builds, production-boot checks and the existing mlibc/BusyBox end-to-end workloads on ARM64, x64 and RISC-V 64 in both Debug and Release. Run the host test suite as well. Report actual results for every required configuration; building alone does not establish runtime acceptance.
- With the declared host tools and their dependencies already installed, configuration and compilation must use checked-in third-party sources without downloading source archives or invoking Meson or BusyBox Make. Verify this from fresh build directories rather than relying on previously populated source caches.
- Verify incremental builds, including an unchanged rebuild and rebuilding affected outputs after a direct source change. Configuration and compilation must not rewrite the source tree; generated files belong in the build tree.
- Preserve and honestly report the existing `scheduler/migration_current_owner` failure. Do not remove, disable or relabel it as passing to obtain a green migration report. Its kernel repair is outside this migration; the overall functional matrix must not be described as passing while the regression remains. Other failures require investigation rather than being attributed to this known defect without evidence.

This migration does not resume the paused kernel acceptance goal, long-run stability gates or performance acceptance. Networking and persistent storage remain excluded by ADR-0006. The user authorized implementation on 2026-09-16 and subsequently requested committing and pushing the completed migration. Implementation details and verification evidence are recorded in the [source inventory](../third-party-sources.md) and [validation log](../kernel-validation.md#native-cmake-source-migration-2026-09-16).
