#include "types.hpp"
#include "result.hpp"
#include "smart_ptr.hpp"
#include "concepts/kernel_concepts.hpp"
#include "concepts/container_concepts.hpp"
#include "concepts/memory_concepts.hpp"
#include "../containers/lockfree_queue.hpp"
#include "../containers/atomic_types.hpp"
#include "kernel_std.hpp"

using namespace moss::kernel;
using namespace moss::concepts;

int main() {
    // 测试concepts约束
    static_assert(KernelSafe<u32>);
    static_assert(AtomicCompatible<u64>);
    static_assert(PowerOfTwo<256>);
    
    // 测试容器concepts
    static_assert(LockFreeElement<u32>);
    static_assert(ValidQueueCapacity<512>);
    
    // 测试内存concepts  
    static_assert(UniquePtrCompatible<int>);
    
    return 0;
}
