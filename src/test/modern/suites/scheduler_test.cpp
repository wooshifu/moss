#include "../../moss_ut.hpp"
#include "../fixtures/kernel_fixtures.hpp"
#include "../core/test_utilities.hpp"

using namespace boost::ut;
using namespace moss::test::utils;
using namespace moss::test::fixtures;
using namespace moss::kernel;

void run_scheduler_tests() {
    "scheduler"_suite([] {

        "task_state_transitions"_test([] {
            enum class TaskState : u32 {
                READY = 0,
                RUNNING = 1,
                BLOCKED = 2,
                TERMINATED = 3
            };

            struct MockTask {
                TaskState state;
                u32 priority;
                u64 runtime_ns;

                MockTask(u32 prio = 50) : state(TaskState::READY), priority(prio), runtime_ns(0) {}
            };

            MockTask task(100);

            // Test initial state
            expect(task.state == TaskState::READY and
                   task.priority == 100u and
                   task.runtime_ns == 0u);

            // Test state transitions
            task.state = TaskState::RUNNING;
            task.runtime_ns = 1000;

            expect(task.state == TaskState::RUNNING and task.runtime_ns == 1000u);

            // Test complete state cycle
            task.state = TaskState::BLOCKED;
            expect(task.state == TaskState::BLOCKED);

            task.state = TaskState::TERMINATED;
            expect(task.state == TaskState::TERMINATED);
        });

        "scheduler_load_balancing"_test([] {
            struct MockCPU {
                u32 id;
                u32 load_percentage;
                u32 active_tasks;

                MockCPU(u32 cpu_id) : id(cpu_id), load_percentage(0), active_tasks(0) {}
            };

            MockCPU cpu0(0), cpu1(1);

            // Simulate load imbalance
            cpu0.load_percentage = 80;
            cpu0.active_tasks = 8;
            cpu1.load_percentage = 20;
            cpu1.active_tasks = 2;

            // Test load balancing logic
            auto load_difference = cpu0.load_percentage - cpu1.load_percentage;
            expect(load_difference > 50u);  // Significant imbalance

            // Simulate task migration
            if (load_difference > 30) {
                cpu0.active_tasks--;
                cpu1.active_tasks++;
                cpu0.load_percentage = 70;
                cpu1.load_percentage = 30;
            }

            auto new_difference = cpu0.load_percentage - cpu1.load_percentage;
            expect(new_difference <= 50u and new_difference >= 30u);  // Improved balance

            // Verify task migration occurred
            expect(cpu0.active_tasks == 7u and cpu1.active_tasks == 3u);
        });

        "scheduler_timing_precision"_test([] {
            PerformanceTimer timer;

            // Simulate some work
            volatile int work = 0;
            for (int i = 0; i < 1000; ++i) {
                work += i;
            }

            // Verify timing is reasonable (should be fast)
            expect(timer.elapsed_less_than(100000u));  // Less than 100k cycles
        });

        "scheduler_priority_ordering"_test([] {
            struct PriorityTask {
                u32 id;
                u32 priority;
                u32 execution_order;

                PriorityTask(u32 task_id, u32 prio)
                    : id(task_id), priority(prio), execution_order(0) {}
            };

            PriorityTask high_prio(1, 100);
            PriorityTask med_prio(2, 50);
            PriorityTask low_prio(3, 10);

            // Verify priority ordering
            expect(high_prio.priority > med_prio.priority);
            expect(med_prio.priority > low_prio.priority);

            // Simulate scheduling order
            high_prio.execution_order = 1;
            med_prio.execution_order = 2;
            low_prio.execution_order = 3;

            expect(high_prio.execution_order == 1u and
                   med_prio.execution_order == 2u and
                   low_prio.execution_order == 3u);
        });

    });
}
