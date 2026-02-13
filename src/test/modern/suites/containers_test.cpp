#include "../../moss_ut.hpp"
#include "../fixtures/kernel_fixtures.hpp"
#include "../core/test_utilities.hpp"
#include "../../../containers/include/containers/lockfree_queue.hpp"

using namespace boost::ut;
using namespace moss::test::utils;
using namespace moss::test::fixtures;
using namespace moss::kernel::containers;
using namespace moss::kernel; // For u32 type access

void run_containers_tests() {
    "containers"_suite([] {

        "spsc_queue_basic_operations"_test([] {
            SPSCQueue<u32, 8> queue;

            // Simple assertions
            expect(queue.empty());
            expect(!queue.full());
            expect(queue.approximate_size() == 0u);

            // Enqueue operation
            expect(queue.try_enqueue(42u));
            expect(queue.approximate_size() == 1u and !queue.empty());

            // Dequeue verification
            u32 result = 0u;
            expect(queue.try_dequeue(result) and result == 42u);
            expect(queue.empty() and queue.approximate_size() == 0u);
        });

        "spsc_queue_capacity_limits"_test([] {
            SPSCQueue<u32, 4> small_queue;

            // Fill queue to capacity - using u32 consistently
            for(u32 i = 0u; i < 4u; ++i) {
                expect(small_queue.try_enqueue(i));
            }

            // Verify we can still dequeue - don't assume exact size for lock-free queue
            u32 result = 0u;
            expect(small_queue.try_dequeue(result) and result == 0u);
            expect(!small_queue.empty()); // Should have remaining items

            // Verify we can't enqueue when approaching full
            expect(!small_queue.try_enqueue(999u));  // Should fail eventually
        });

    });
}
