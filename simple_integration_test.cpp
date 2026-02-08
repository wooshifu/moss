#include "types.hpp"
#include "result.hpp"
#include "smart_ptr.hpp"
#include "concepts/kernel_concepts.hpp"
#include "concepts/container_concepts.hpp"
#include "concepts/memory_concepts.hpp"

using namespace moss::kernel;
using namespace moss::concepts;

int main() {
    // 测试基本类型
    u32 test_val = 42;
    u64 test_val2 = 100;
    
    // 测试concepts约束
    static_assert(KernelSafe<u32>);
    static_assert(AtomicCompatible<u64>);
    static_assert(PowerOfTwo<256>);
    
    // 测试容器concepts
    static_assert(LockFreeElement<u32>);
    static_assert(ValidQueueCapacity<512>);
    
    // 测试内存concepts  
    static_assert(UniquePtrCompatible<int>);
    
    // 测试Result类型
    using TestResult = Result<u32, ErrorCode>;
    TestResult result{test_val};
    
    if (result.has_value()) {
        return 0;  // 成功
    }
    return 1;
}
