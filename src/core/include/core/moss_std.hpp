#pragma once

// Thin bridge: imports moss.std module and re-exports types into global namespace
// for backward compatibility with existing code that uses unqualified names.

import moss.std;

// Re-export basic types from moss:: into global namespace
using moss::size_t;
using moss::ptrdiff_t;
using moss::intptr_t;
using moss::uintptr_t;
using moss::nullptr_t;
using moss::max_align_t;

using moss::u8;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::i8;
using moss::i16;
using moss::i32;
using moss::i64;
using moss::f32;
using moss::f64;

using moss::PhysAddr;
using moss::VirtAddr;
using moss::PageFrame;

// Re-export constants
using moss::UINT8_MAX;
using moss::UINT16_MAX;
using moss::UINT32_MAX;
using moss::UINT64_MAX;
