// ipc.cpp - Module implementation unit for moss.ipc
// Provides definitions for global variables declared in the module interface

module moss.ipc;

namespace moss::kernel::ipc {

// Global shared memory manager instance
SharedMemoryManager *g_shared_memory_manager = nullptr;

// Global IPC manager instance
IpcManager *g_ipc_manager = nullptr;

} // namespace moss::kernel::ipc
