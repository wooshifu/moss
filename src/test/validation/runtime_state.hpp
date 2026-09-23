#pragma once

#include "validation/runtime.hpp"

namespace moss::test::validation {
extern char selection[81];
extern const char *active_case;
extern bool failed;
extern unsigned completed;
extern unsigned selected_count;
extern unsigned sample_index;
extern unsigned warmup_count;
extern unsigned sample_count;
extern usize fixed_iterations;
extern bool stability;
} // namespace moss::test::validation
