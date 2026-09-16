# Included after the static mlibc/compiler-rt targets and ABI flags are defined.
find_program(BUSYBOX_MAKE make REQUIRED)
fetchcontent_declare(
    busybox_source
    URL https://busybox.net/downloads/busybox-1.37.0.tar.bz2
    URL_HASH SHA256=3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4
    SOURCE_SUBDIR moss-no-cmake-subproject
)
fetchcontent_makeavailable(busybox_source)

function(busybox_patch file before after)
    set(path "${busybox_source_SOURCE_DIR}/${file}")
    file(READ "${path}" source)
    string(FIND "${source}" "${after}" applied)
    if(applied EQUAL -1)
        string(FIND "${source}" "${before}" anchor)
        if(anchor EQUAL -1)
            message(FATAL_ERROR "Pinned BusyBox patch did not match ${file}")
        endif()
        string(REPLACE "${before}" "${after}" source "${source}")
        file(WRITE "${path}" "${source}")
    endif()
endfunction()

busybox_patch(
    include/platform.h
    "#define DEV_FD_PREFIX \"/dev/fd/\""
    "#define DEV_FD_PREFIX \"/dev/fd/\"\n#if defined(__moss__)\n# undef HAVE_MNTENT_H\n# undef HAVE_SYS_STATFS_H\n#endif"
)
busybox_patch(libbb/appletlib.c "    || defined(__APPLE__) \\\n" "    || defined(__APPLE__) || defined(__moss__) \\\n")
# This helper is used only by taskset/nproc, neither in the selected profile.
busybox_patch(
    libbb/Kbuild.src "lib-y += alloc_affinity.o"
    "lib-$(CONFIG_TASKSET) += alloc_affinity.o\nlib-$(CONFIG_NPROC) += alloc_affinity.o"
)

string(JOIN " " BUSYBOX_ABI_FLAGS ${MLIBC_ARCH_FLAGS})
get_filename_component(BUSYBOX_BUILTINS_DIR "${MLIBC_BUILTINS}" DIRECTORY)
set(BUSYBOX_BUILD "${CMAKE_CURRENT_BINARY_DIR}/busybox")
add_custom_target(
    busybox-validation
    COMMAND ${CMAKE_COMMAND} -DMAKE=${BUSYBOX_MAKE} -DSOURCE=${busybox_source_SOURCE_DIR} -DBUILD=${BUSYBOX_BUILD} -P
            ${CMAKE_CURRENT_LIST_DIR}/configure_busybox.cmake
    COMMAND
        ${BUSYBOX_MAKE} -C ${busybox_source_SOURCE_DIR} O=${BUSYBOX_BUILD} CC=${CLANG}
        "LD=${CLANG} ${BUSYBOX_ABI_FLAGS} -nostdlib -fuse-ld=lld"
        "CONFIG_EXTRA_CFLAGS=${BUSYBOX_ABI_FLAGS} -D__moss__ -ffreestanding -isystem ${MLIBC_SYSROOT}/usr/include"
        "EXTRA_LDFLAGS=-nostdlib -static -fuse-ld=lld -Wl,--image-base=0x200000000,-z,max-page-size=4096 -L${MLIBC_SYSROOT}/usr/lib -L${BUSYBOX_BUILTINS_DIR}"
        "CONFIG_EXTRA_LDLIBS=-Wl,${MLIBC_SYSROOT}/usr/lib/crt1.o c clang_rt.builtins-${MLIBC_CPU}" AR=${LLVM_AR}
        NM=llvm-nm STRIP=${LLVM_STRIP} -j4
    DEPENDS mlibc compiler-rt-builtins
    BYPRODUCTS ${BUSYBOX_BUILD}/busybox
    USES_TERMINAL VERBATIM
)
