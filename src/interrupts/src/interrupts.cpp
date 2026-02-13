// interrupts.cpp - Module implementation unit for moss.interrupts
// Defines global variables that need external linkage.

module moss.interrupts;

namespace moss::kernel::interrupts {

// Global GIC instance
GenericInterruptController *g_gic = nullptr;

} // namespace moss::kernel::interrupts
