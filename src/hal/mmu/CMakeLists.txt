add_library(moss_hal_mmu OBJECT)

target_sources(
    moss_hal_mmu
    PUBLIC FILE_SET CXX_MODULES FILES src/mmu_hal.cppm
)

target_link_libraries(
    moss_hal_mmu
    PUBLIC moss_core moss_aal moss_platform
)
