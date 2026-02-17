// MOSS Process Module - Process Management, CFS Scheduler, Load Balancer, Idle Task
// Primary interface: re-exports all partitions.

module;

// Architecture detection
#include "arch_detect.h"

// Assembly interop declarations (global module fragment)
extern "C" void switch_to_user(void* context, unsigned long long user_stack);
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
extern "C" void context_switch(void* prev_context, void* next_context);
#endif

export module moss.process;

// All external module imports
import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.containers;
import moss.mm;
import moss.ipc;
import moss.interrupts;
import moss.platform;
import moss.hal.intc;
import moss.hal.timer;
import moss.timer;
import moss.logging;

// Re-export all partitions
export import :types;
export import :scheduler;
export import :load_balancer;
