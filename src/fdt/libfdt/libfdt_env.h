/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */
#ifndef LIBFDT_ENV_H
#define LIBFDT_ENV_H
/*
 * libfdt - Flat Device Tree manipulation
 * Custom freestanding environment header for MOSS kernel
 *
 * Replaces the upstream libfdt_env.h which depends on hosted C headers
 * (stdint.h, string.h, etc.). In freestanding mode we provide types via
 * compiler builtins and declare C string functions as extern (implemented
 * in runtime_support.cpp).
 */

/* === Fixed-width integer types via compiler builtins === */
typedef __UINT8_TYPE__   uint8_t;
typedef __UINT16_TYPE__  uint16_t;
typedef __UINT32_TYPE__  uint32_t;
typedef __UINT64_TYPE__  uint64_t;
typedef __INT32_TYPE__   int32_t;
typedef __SIZE_TYPE__    size_t;
typedef __UINTPTR_TYPE__ uintptr_t;

/* === Boolean type === */
#ifndef __cplusplus
typedef _Bool bool;
#define true  1
#define false 0
#endif

/* === NULL === */
#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void *)0)
#endif
#endif

/* === Limits === */
#ifndef INT_MAX
#define INT_MAX __INT_MAX__
#endif
#ifndef UINT_MAX
#define UINT_MAX __UINT32_MAX__
#endif
#ifndef UINT32_MAX
#define UINT32_MAX __UINT32_MAX__
#endif
#ifndef INT32_MAX
#define INT32_MAX __INT32_MAX__
#endif

/* === C string/memory functions (implemented in runtime_support.cpp) === */
/* C++ 编译时需要 noexcept 与定义保持一致，C 编译器忽略 */
#ifdef __cplusplus
#define LIBFDT_NOEXCEPT noexcept
#else
#define LIBFDT_NOEXCEPT
#endif

extern size_t strlen(const char *s) LIBFDT_NOEXCEPT;
extern size_t strnlen(const char *s, size_t maxlen) LIBFDT_NOEXCEPT;
extern int strcmp(const char *s1, const char *s2) LIBFDT_NOEXCEPT;
extern int strncmp(const char *s1, const char *s2, size_t n) LIBFDT_NOEXCEPT;
extern char *strchr(const char *s, int c) LIBFDT_NOEXCEPT;
extern char *strrchr(const char *s, int c) LIBFDT_NOEXCEPT;
extern void *memchr(const void *s, int c, size_t n) LIBFDT_NOEXCEPT;
extern void *memset(void *dst, int c, size_t n) LIBFDT_NOEXCEPT;
extern void *memcpy(void *dst, const void *src, size_t n) LIBFDT_NOEXCEPT;
extern void *memmove(void *dst, const void *src, size_t n) LIBFDT_NOEXCEPT;
extern int memcmp(const void *s1, const void *s2, size_t n) LIBFDT_NOEXCEPT;

/* === Sparse/checker annotations (unused in kernel build) === */
#ifdef __CHECKER__
#define FDT_FORCE __attribute__((force))
#define FDT_BITWISE __attribute__((bitwise))
#else
#define FDT_FORCE
#define FDT_BITWISE
#endif

/* === FDT byte-order types === */
typedef uint16_t FDT_BITWISE fdt16_t;
typedef uint32_t FDT_BITWISE fdt32_t;
typedef uint64_t FDT_BITWISE fdt64_t;

/* === Byte-swap (DTB is big-endian, all MOSS targets are little-endian) ===
 *
 * C++ 路径使用 __builtin_bswap 避免 old-style cast 警告；
 * C 路径保留原始 EXTRACT_BYTE 宏供 libfdt .c 文件使用。
 */

#ifdef __cplusplus

/* C++ 路径：使用 __builtin_bswap，避免 -Wold-style-cast */
static inline uint16_t fdt16_to_cpu(fdt16_t x)
{
	return __builtin_bswap16(x);
}
static inline fdt16_t cpu_to_fdt16(uint16_t x)
{
	return __builtin_bswap16(x);
}

static inline uint32_t fdt32_to_cpu(fdt32_t x)
{
	return __builtin_bswap32(x);
}
static inline fdt32_t cpu_to_fdt32(uint32_t x)
{
	return __builtin_bswap32(x);
}

static inline uint64_t fdt64_to_cpu(fdt64_t x)
{
	return __builtin_bswap64(x);
}
static inline fdt64_t cpu_to_fdt64(uint64_t x)
{
	return __builtin_bswap64(x);
}

#else /* C */

#define EXTRACT_BYTE(x, n) ((unsigned long long)((uint8_t *)&x)[n])

#define CPU_TO_FDT16(x) ((EXTRACT_BYTE(x, 0) << 8) | EXTRACT_BYTE(x, 1))

#define CPU_TO_FDT32(x) ((EXTRACT_BYTE(x, 0) << 24) | \
                          (EXTRACT_BYTE(x, 1) << 16) | \
                          (EXTRACT_BYTE(x, 2) << 8)  | \
                          EXTRACT_BYTE(x, 3))

#define CPU_TO_FDT64(x) ((EXTRACT_BYTE(x, 0) << 56) | \
                          (EXTRACT_BYTE(x, 1) << 48) | \
                          (EXTRACT_BYTE(x, 2) << 40) | \
                          (EXTRACT_BYTE(x, 3) << 32) | \
                          (EXTRACT_BYTE(x, 4) << 24) | \
                          (EXTRACT_BYTE(x, 5) << 16) | \
                          (EXTRACT_BYTE(x, 6) << 8)  | \
                          EXTRACT_BYTE(x, 7))

static inline uint16_t fdt16_to_cpu(fdt16_t x)
{
	return (FDT_FORCE uint16_t)CPU_TO_FDT16(x);
}
static inline fdt16_t cpu_to_fdt16(uint16_t x)
{
	return (FDT_FORCE fdt16_t)CPU_TO_FDT16(x);
}

static inline uint32_t fdt32_to_cpu(fdt32_t x)
{
	return (FDT_FORCE uint32_t)CPU_TO_FDT32(x);
}
static inline fdt32_t cpu_to_fdt32(uint32_t x)
{
	return (FDT_FORCE fdt32_t)CPU_TO_FDT32(x);
}

static inline uint64_t fdt64_to_cpu(fdt64_t x)
{
	return (FDT_FORCE uint64_t)CPU_TO_FDT64(x);
}
static inline fdt64_t cpu_to_fdt64(uint64_t x)
{
	return (FDT_FORCE fdt64_t)CPU_TO_FDT64(x);
}

#undef CPU_TO_FDT64
#undef CPU_TO_FDT32
#undef CPU_TO_FDT16
#undef EXTRACT_BYTE

#endif /* __cplusplus */

#endif /* LIBFDT_ENV_H */
