# src/abi/CMakeLists.txt — ABI Module
#
# Single source of truth for all extern "C" symbols (assembly, linker script,
# cross-module bridges).  Consumer modules `import moss.abi;` instead of
# duplicating extern "C" declarations in their own GMFs.

add_library(moss_abi OBJECT src/sigreturn.S src/uaccess.S)

target_sources(
    moss_abi
    PUBLIC FILE_SET
           CXX_MODULES
           FILES
           src/abi.cppm
           src/trap_frame.cppm
)
target_include_directories(moss_abi PUBLIC include)

# moss.abi depends only on moss_core (for moss.types → VirtAddr, usize)
target_link_libraries(
    moss_abi
    PUBLIC moss_core
)

message(STATUS "ABI module configured")
