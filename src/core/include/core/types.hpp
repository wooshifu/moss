#pragma once

// Thin bridge header - types now in moss.types module
import moss.std;
import moss.types;

// Macros do not cross module boundaries, so keep them here
#define ALIGNED(x) __attribute__((aligned(x)))
#define CACHE_ALIGNED ALIGNED(64)
#define PAGE_ALIGNED ALIGNED(4096)

#define NON_COPYABLE(ClassName)                                                \
  ClassName(const ClassName &) = delete;                                       \
  ClassName &operator=(const ClassName &) = delete;

#define NON_MOVABLE(ClassName)                                                 \
  ClassName(ClassName &&) = delete;                                            \
  ClassName &operator=(ClassName &&) = delete;

#define NON_COPYABLE_NON_MOVABLE(ClassName)                                    \
  NON_COPYABLE(ClassName)                                                      \
  NON_MOVABLE(ClassName)
