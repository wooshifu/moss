// drivers.cpp - Module implementation unit for moss.drivers
// Defines global variables and static data members.

module moss.drivers;

namespace moss::kernel::drivers {

// Global device manager instance
DeviceManager *g_device_manager = nullptr;

// Global UART driver instance
UartDriver *g_uart_driver = nullptr;

// UartDriver compatible device list (static data member)
const char *UartDriver::compatible_devices[] = {"arm,pl011", "arm,primecell"};

} // namespace moss::kernel::drivers
