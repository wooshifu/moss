#pragma once

// 0x4d4f5353 spells MOSS in ASCII; this private auxv key carries the selected
// process-local capability handle retained by SYS_EXECVE_CAP.
enum { MOSS_AT_STARTUP_CAP = 0x4d4f5353 };
