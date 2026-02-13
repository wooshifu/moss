// MOSS内核全局内存分配接口实现
// 实现C风格接口和内存管理系统集成

#include "mm/kernel_memory.hpp"

extern "C" {

// 初始化内核内存系统
bool moss_memory_init(void) noexcept {
    return moss::kernel::mm::initialize_kernel_memory();
}

// 关闭内存系统
void moss_memory_shutdown(void) noexcept {
    moss::kernel::mm::shutdown_kernel_memory();
}

// 标准内存分配接口
void* moss_kmalloc(moss::kernel::usize size) noexcept {
    return moss::kernel::mm::kmalloc(size);
}

void* moss_kzalloc(moss::kernel::usize size) noexcept {
    return moss::kernel::mm::kzalloc(size);
}

void* moss_kmalloc_aligned(moss::kernel::usize size, moss::kernel::usize alignment) noexcept {
    return moss::kernel::mm::kmalloc_aligned(size, alignment);
}

void* moss_kmalloc_atomic(moss::kernel::usize size) noexcept {
    return moss::kernel::mm::kmalloc_atomic(size);
}

void moss_kfree(void* ptr) noexcept {
    moss::kernel::mm::kfree(ptr);
}

void moss_kfree_sized(void* ptr, moss::kernel::usize size) noexcept {
    moss::kernel::mm::kfree_sized(ptr, size);
}

void* moss_krealloc(void* ptr, moss::kernel::usize old_size, moss::kernel::usize new_size) noexcept {
    return moss::kernel::mm::krealloc(ptr, old_size, new_size);
}

// 内存系统状态
int moss_memory_is_healthy(void) noexcept {
    return moss::kernel::mm::is_memory_system_healthy() ? 1 : 0;
}

int moss_memory_get_pressure(void) noexcept {
    return static_cast<int>(moss::kernel::mm::get_memory_pressure());
}

void moss_memory_gc(void) noexcept {
    moss::kernel::mm::trigger_memory_gc();
}

void moss_memory_check_leaks(void) noexcept {
    moss::kernel::mm::check_memory_leaks();
}

void moss_memory_print_stats(void) noexcept {
    moss::kernel::mm::print_memory_stats();
}

} // extern "C"
