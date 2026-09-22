#pragma once

/* The BusyBox metadata generators only need this small POSIX subset. */
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stddef.h>

#define STDOUT_FILENO 1
#define getpid _getpid
#define open _open

static inline int moss_host_dup2(int source, int target)
{
	int result = _dup2(source, target);
	if (result == 0) {
		_setmode(target, _O_BINARY);
		/* POSIX permits renaming an open file; the Windows CRT does not. */
		_close(source);
	}
	return result;
}

static inline int moss_host_write(int descriptor, const void *buffer, size_t count)
{
	_setmode(descriptor, _O_BINARY);
	return _write(descriptor, buffer, (unsigned int) count);
}

#define dup2 moss_host_dup2
#define write moss_host_write
