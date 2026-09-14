// MOSS VFS Module — Primary interface (re-export hub)
//
// Provides a complete POSIX-like virtual filesystem layer including:
//   - Core types and constants
//   - Inode / SuperBlock / FileOps abstraction
//   - Dentry cache and path resolution
//   - File descriptors and per-process FdTable
//   - Mount table
//   - devfs (/dev/console, /dev/null, /dev/zero)
//   - ramfs (initramfs-backed read-only filesystem)
//   - pipefs (anonymous pipes)
//   - Syscall handler layer

export module moss.vfs;

import moss.std;
import moss.types;
import moss.result;
import moss.containers;
import moss.logging;

export import :types;
export import :buffer;
export import :inode;
export import :dcache;
export import :file;
export import :mount;
export import :devfs;
export import :ramfs;
export import :pipefs;
export import :syscall;
