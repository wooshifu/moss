import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.boot;
import moss.containers;
import moss.logging;
import moss.mm;
import moss.kernel;
import moss.capability;
import moss.process;
import moss.vfs;
import moss.timer;
import moss.platform;
import moss.result;
import moss.initramfs;
import moss.hal.uart;

#include "framework/ut_kernel.hpp"
#include "validation/core_cases.hpp"

using namespace moss::kernel;
using moss::abi::TrapFrame;
namespace ut = boost::ut;

extern "C" {
unsigned long strnlen(const char *s, unsigned long maxlen) noexcept;
char *strchr(const char *s, int c) noexcept;
char *strrchr(const char *s, int c) noexcept;
void system_call_handler(void *raw_frame) noexcept;
void user_return_handler(void *raw_frame) noexcept;
void *_ZnamSt11align_val_t(unsigned long size, unsigned long alignment);
void _ZdlPv(void *ptr);
void _ZdaPv(void *ptr);
void _ZdlPvmSt11align_val_t(void *ptr, unsigned long size, unsigned long alignment);
void _ZdaPvmSt11align_val_t(void *ptr, unsigned long size, unsigned long alignment);
}

namespace moss::test::validation {
namespace {
void arch_primitives() {
  arch::memory_barrier();
  arch::read_barrier();
  arch::write_barrier();
  arch::data_sync_barrier();
  arch::instruction_barrier();
  arch::io_barrier();
  arch::cpu_yield();

  const bool was_enabled = arch::interrupts_enabled();
  arch::disable_interrupts();
  ut::expect(!arch::interrupts_enabled());
  arch::enable_interrupts();
  ut::expect(arch::interrupts_enabled());
  arch::disable_all_interrupts();
  if (was_enabled) {
    arch::enable_interrupts();
  }

  alignas(64) u8 code[128]{};
  arch::flush_cache_line(reinterpret_cast<VirtAddr>(code));
#if defined(MOSS_ARCH_ARM64)
  arch::publish_instruction_range(reinterpret_cast<VirtAddr>(code), sizeof(code));
#endif
  arch::arch_init();
  arch::early_arch_init();
  ut::expect(arch::get_current_cpu_id() == 0);
  void *stack = arch::get_current_stack_pointer();
  ut::expect(stack != nullptr);
  (void)arch::get_frame_pointer();
  const u64 first = arch::get_timestamp_counter();
  arch::cpu_yield();
  const u64 second = arch::get_timestamp_counter();
  ut::expect(second >= first);
  arch::cpu_idle_once();
  arch::flush_tlb();
  const VirtAddr address = reinterpret_cast<VirtAddr>(code);
  arch::flush_tlb_addr(address);
  arch::synchronize_tlb(0, true);
  arch::synchronize_tlb(address, false);
}

void containers_extended() {
  using namespace containers;
  const MemoryOrder orders[] = {MemoryOrder::Relaxed, MemoryOrder::Consume, MemoryOrder::Acquire, MemoryOrder::Release,
                                MemoryOrder::AcqRel, MemoryOrder::SeqCst};
  for (const auto order : orders) {
    (void)to_intrinsics_order(order);
  }
  Optional<u32> empty;
  ut::expect(!empty.has_value());
  Optional<u32> value(7u);
  ut::expect(value.has_value() && *value == 7u);
  Optional<u32> copied(value);
  ut::expect(copied.has_value() && *copied == 7u);
  Optional<u32> moved(moss::move(value));
  ut::expect(moved.has_value() && *moved == 7u);
  ut::expect(!value.has_value());
  empty = copied;
  ut::expect(empty.has_value() && *empty == 7u);
  empty = moss::move(copied);
  ut::expect(empty.has_value() && *empty == 7u && !copied.has_value());
  empty.reset();
  ut::expect(!empty.has_value());
  empty.emplace(9u);
  ut::expect(empty.has_value() && *empty == 9u);
  const auto &aliased = empty;
  ut::expect(aliased.value() == 9u && aliased.operator->() != nullptr);
  ut::expect(moss::move(empty).value() == 9u);

  ObjectPool<u32, 4> pool;
  auto *node = pool.allocate();
  ut::expect(node != nullptr && pool.has_available());
  pool.deallocate(node);
  auto *reused = pool.allocate();
  ut::expect(reused != nullptr);
  pool.deallocate(reused);
  pool.deallocate(nullptr);

  MPMCQueue<u32, 2, 4> queue;
  ut::expect(queue.empty() && queue.approximate_total_size() == 0U);
  for (u32 i = 0; i < 6; ++i) {
    ut::expect(queue.try_enqueue(i));
  }
  ut::expect(!queue.try_enqueue(99u));
  ut::expect(queue.approximate_total_size() == 6);
  u32 result = 0;
  ut::expect(!queue.try_dequeue(5, result));
  const bool first_dequeue = queue.try_dequeue_any(result) || queue.try_dequeue(0, result);
  ut::expect(first_dequeue);
  ut::expect(queue.empty() || queue.approximate_total_size() < 6);

  LockedHashMap<u32, u32, 8> map;
  map.insert_or_update(1u, 10u);
  map.insert_or_update(1u, 11u);
  map.insert_or_update(2u, 20u);
  ut::expect(map.find(1u).has_value() && *map.find(1u) == 11u);
  ut::expect(!map.find(99u).has_value());
  auto value_slot = map.get_or_insert(3u, [] { return 30u; });
  ut::expect(value_slot == 30u && map.size() == 3U && !map.empty());
  auto existing = map.get_or_insert(3u, [] { return 31u; });
  ut::expect(existing == 30u);
  auto extracted = map.extract(2u);
  ut::expect(extracted.has_value() && *extracted == 20u);
  ut::expect(map.remove(99u) == false);
  ut::expect(map.remove(1u));
  map.for_each([](const auto &entry) { (void)entry; });
  map.for_each_snapshot([](const auto &entry) { (void)entry; });
  map.clear();
  ut::expect(map.empty() && map.size() == 0U);

  LockedList<u32> list;
  list.push_front(1u);
  list.push_front(2u);
  ut::expect(list.update_if([](u32 item) { return item == 1u; }, [](u32 &item) { item = 3u; }));
  ut::expect(!list.update_if([](u32 item) { return item == 9u; }, [](u32 &item) { item = 0u; }));
  ut::expect(!list.update_if_unless([](u32 item) { return item == 9u; }, [](u32) { return false; },
                                   [](u32 &item) { item = 0u; }));
  ut::expect(list.update_if_unless([](u32 item) { return item == 3u; }, [](u32 item) { return item == 9u; },
                                   [](u32 &item) { item = 4u; }));
  ut::expect(!list.update_if_unless([](u32 item) { return item == 4u; }, [](u32 item) { return item == 2u; },
                                    [](u32 &item) { item = 5u; }));
  ut::expect(list.find(4u).has_value() && !list.find(99u).has_value());
  TicketSpinLock lock;
  ut::expect(!lock.is_locked() && lock.try_lock());
  ut::expect(lock.is_locked());
  lock.unlock();

  WaitQueue wait_queue;
  int waiter = 0;
  ut::expect(!wait_queue.has_waiters() && wait_queue.try_add_waiter(&waiter));
  ut::expect(wait_queue.has_waiters());
  wait_queue.clear();
  ut::expect(!wait_queue.has_waiters());

  SlabAllocator empty_slab;
  empty_slab.get_statistics();
  auto small = empty_slab.allocate(8);
  if (ut::expect(small.has_value())) {
    empty_slab.get_statistics();
    ut::expect(empty_slab.deallocate(*small, 8).has_value());
  }
}

void logging_formats() {
  using namespace logging;
  LogBuffer buffer;
  buffer.append_char('x');
  buffer.append_str("yz");
  buffer.append_dec(123);
  buffer.append_signed(-45);
  buffer.append_hex(0xabc);
  buffer.append_bool(true);
  buffer.append_source_loc("internals.cpp", 7, "logging_formats");
  ut::expect(buffer.pos() > 0 && buffer.data() != nullptr);
  buffer.flush_line_direct();
  LogBuffer levels;
  levels.append_level_tag(LogLevel::Debug);
  levels.append_level_tag(LogLevel::Info);
  levels.append_level_tag(LogLevel::Warn);
  levels.append_level_tag(LogLevel::Error);
  levels.append_level_tag(LogLevel::Panic);
  levels.append_level_tag(static_cast<LogLevel>(99));
  ut::expect(levels.pos() > 0);
  const char *format = "{:x}";
  const auto spec = parse_fmt_spec(format);
  LogBuffer formatted;
  format_arg(formatted, spec, 255u);
  format_arg(formatted, FmtSpec::Bool, true);
  format_arg(formatted, FmtSpec::Auto, -7);
  format_arg(formatted, FmtSpec::Auto, static_cast<char>('q'));
  format_arg(formatted, FmtSpec::Auto, "text");
  format_into(formatted, "{} {} {} {} {} {}", 1, 2u, 3ULL, -4, "five", true);
  klog::info("format probe {} {} {} {} {}", 1, 2u, 3ULL, -4, "five");
  klog::warn("format values {} {}", 255, 7);
  klog::debug("debug probe");
  klog::debug_chain("debug chain");
  set_log_level(LogLevel::Warn);
  klog::info("filtered info");
  set_log_level(LogLevel::Debug);
  buffer.append_source_loc(nullptr, 0, nullptr);
  buffer.append_source_loc("dir/file.cpp", 0, nullptr);
  buffer.append_source_loc("file.cpp", 1, "");
  LogBuffer direct;
  direct.append_char('x');
  direct.flush_line_direct();
  ut::expect(direct.pos() == 0);
  LogBuffer full;
  for (u32 i = 0; i < LogBuffer::BUFFER_SIZE - 1; ++i) {
    full.append_char('f');
  }
  const bool uart_valid = platform::hardware.uart.valid;
  platform::hardware.uart.valid = false;
  full.flush_line_direct();
  platform::hardware.uart.valid = uart_valid;
  const char *specs[] = {"}", "{:x}", "{:b}", "{:#x}", "{:z}tail", "{:x", "{:b"};
  for (const char *spec_text : specs) {
    const char *cursor = spec_text;
    (void)parse_fmt_spec(cursor);
  }
  LogBuffer braces;
  const char *formats[] = {"{{", "}}", "{", "}", "{{{", "{:x}", "{missing}"};
  for (const char *format_text : formats) {
    format_into(braces, format_text);
    format_into(braces, format_text, 7u);
  }
  format_arg(braces, FmtSpec::Bool, 0ULL);
  format_arg(braces, FmtSpec::Hex, static_cast<const void *>(nullptr));
  format_arg(braces, FmtSpec::Auto, static_cast<long>(-1));
  format_arg(braces, FmtSpec::Auto, static_cast<unsigned long>(1));
  format_arg(braces, FmtSpec::Auto, static_cast<i32>(-2));
  format_arg(braces, FmtSpec::Auto, static_cast<u32>(2));
  ut::expect(braces.pos() > 0);
}

void logging_ring_and_chains() {
  using namespace logging;

  LogBuffer empty;
  empty.flush_line_direct();
  ut::expect(empty.pos() == 0);
  empty.append_str(nullptr);
  empty.append_dec(0);
  empty.append_signed(-9223372036854775807LL - 1);
  empty.append_hex(0);
  empty.append_bool(false);
  ut::expect(empty.pos() > 0);

  LogBuffer truncated;
  for (u32 i = 0; i < LogBuffer::BUFFER_SIZE + 16; ++i) {
    truncated.append_char('q');
  }
  ut::expect(truncated.pos() == LogBuffer::BUFFER_SIZE - 1);

  LogBuffer braces;
  format_into(braces, "{{}} {} {");
  format_into(braces, "{{}}", 1);
  format_into(braces, "suffix }");
  ut::expect(braces.pos() > 0);

  const char *hex_spec = "{:#x}";
  const char *bool_spec = "{:b}";
  const char *plain_spec = "{}";
  const char *hex_cursor = hex_spec + 1;
  const char *bool_cursor = bool_spec + 1;
  const char *plain_cursor = plain_spec + 1;
  ut::expect(parse_fmt_spec(hex_cursor) == FmtSpec::Hex);
  ut::expect(parse_fmt_spec(bool_cursor) == FmtSpec::Bool);
  ut::expect(parse_fmt_spec(plain_cursor) == FmtSpec::Auto);

  LogBuffer args;
  format_arg(args, FmtSpec::Hex, static_cast<i64>(-1));
  format_arg(args, FmtSpec::Bool, false);
  format_arg(args, FmtSpec::Auto, static_cast<const void *>(nullptr));
  format_arg(args, FmtSpec::Auto, static_cast<const char *>(nullptr));
  format_arg(args, FmtSpec::Auto, 'z');
  format_arg(args, FmtSpec::Auto, 7L);
  format_arg(args, FmtSpec::Auto, 7UL);
  format_arg(args, FmtSpec::Auto, 7u);
  format_arg(args, FmtSpec::Auto, 7);
  ut::expect(args.pos() > 0);

  set_log_level(LogLevel::Error);
  auto filtered = klog::debug_chain("d");
  filtered.str("x").chr('y').u64(1).i64(-1).u32(2).i32(-2).hex(3).ptr(nullptr).boolean(false).endl();
  auto filtered_info = klog::info_chain();
  filtered_info.str("x");
  auto filtered_warn = klog::warn_chain();
  filtered_warn.str("x");
  klog::debug("filtered debug");
  klog::info("filtered info");
  klog::warn("filtered warn");
  set_log_level(LogLevel::Debug);

  auto active = klog::debug_chain("active ");
  active.str("str ").chr('c').u64(1).i64(-1).u32(2).i32(-2).hex(3).ptr(nullptr).boolean(true).endl();
  auto moved = moss::move(active);
  moved.str(" moved");
  auto info_entry = klog::info_chain("info ");
  info_entry.str("entry");
  auto warn_entry = klog::warn_chain("warn ");
  warn_entry.str("entry");
  auto error_entry = klog::error_chain("error ");
  error_entry.str("entry");

  // The 32 KiB ring exceeds the 16 KiB kernel stack; keep this fixture in
  // static storage rather than overflowing the caller's frame.
  static PrintkRingBuffer ring;
  ring.try_drain();
  char text[608]{};
  for (u32 i = 0; i < sizeof(text); ++i) {
    text[i] = 'r';
  }
  ring.emit(LogLevel::Info, text, sizeof(text));
  for (u32 i = 0; i < 64; ++i) {
    ring.emit(LogLevel::Debug, text, 496);
  }
  ring.try_drain();
  ut::expect(!ring.is_emergency());
  ring.set_emergency();
  ut::expect(ring.is_emergency());
}

void types_errors() {
  ut::expect(ErrorCode::Success == ErrorCode::Success);
  ut::expect(ErrorCode::InvalidParameter != ErrorCode::OutOfMemory);
  ut::expect(ErrorCode::InvalidParameter != ErrorCode::InvalidState);
  ut::expect(ErrorCode::OutOfMemory != ErrorCode::InvalidState);
  ut::expect(ErrorCode::PermissionDenied != ErrorCode::NotFound);
  ut::expect(ErrorCode::AlreadyExists != ErrorCode::Busy);
  ut::expect(ErrorCode::NotSupported != ErrorCode::InvalidArgument);
  ut::expect(ErrorCode::Timeout != ErrorCode::Interrupted);
  ut::expect(ErrorCode::InvalidParameter != ErrorCode::ResourceExhausted);
  ut::expect(ErrorCode::NotFound != ErrorCode::ResourceBusy);
  ut::expect(ErrorCode::Busy != ErrorCode::DeviceBusy);
  ut::expect(ErrorCode::NotSupported != ErrorCode::InternalError);
  ut::expect(ErrorCode::Timeout != ErrorCode::DeviceError);
  ut::expect(ErrorCode::InvalidParameter != ErrorCode::IoError);
  ut::expect(ErrorCode::Unknown != ErrorCode::NetworkError);
  ut::expect(ErrorCode::FileSystemError != ErrorCode::TooManyFiles);
  ut::expect(ErrorCode::NoSpace != ErrorCode::ReadOnly);

  alignas(16) u8 bytes[16]{};
  u8 source[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  ut::expect(moss::memset(bytes, 0x5a, sizeof(bytes)) == bytes);
  ut::expect(moss::memcpy(bytes, source, sizeof(source)) == bytes);
  ut::expect(moss::memmove(bytes + 2, bytes, 4) == bytes + 2);
  ut::expect(moss::memcmp(bytes, bytes, sizeof(bytes)) == 0);
  ut::expect(moss::memcmp(source, source + 1, 1) != 0);
  ut::expect(moss::abs(-7) == 7 && moss::abs(7) == 7);
  ut::expect(moss::min(3, 5) == 3 && moss::max(3, 5) == 5);
  ut::expect(moss::clamp(1, 3, 5) == 3 && moss::clamp(4, 3, 5) == 4 && moss::clamp(9, 3, 5) == 5);
  u32 first = 1;
  u32 second = 2;
  moss::swap(first, second);
  ut::expect(first == 2 && second == 1);
  ut::expect(moss::exchange(first, 9u) == 2 && first == 9);
  ut::expect(moss::memory_order_to_int(moss::MemoryOrder::AcqRel) == 4);
  ut::expect(moss::memory_order_to_int(moss::memory_order::seq_cst) == 5);
  moss::atomic_thread_fence(moss::MemoryOrder::SeqCst);
  moss::atomic_thread_fence(moss::memory_order::acquire);
  moss::atomic_thread_fence(0);

  moss::atomic<u32> atomic_value(1);
  ut::expect(atomic_value.load(moss::memory_order_relaxed) == 1);
  atomic_value.store(2, moss::memory_order_release);
  ut::expect(atomic_value.exchange(3, moss::memory_order_acq_rel) == 2);
  u32 expected = 3;
  ut::expect(atomic_value.compare_exchange_weak(expected, 4));
  expected = 4;
  ut::expect(atomic_value.compare_exchange_strong(expected, 5));
  ut::expect(atomic_value.fetch_add(1) == 5 && atomic_value.load() == 6);
  ut::expect(atomic_value.fetch_sub(1) == 6 && atomic_value.load() == 5);
  ut::expect(atomic_value.fetch_and(3) == 5 && atomic_value.load() == 1);
  ut::expect(atomic_value.fetch_or(6) == 1 && atomic_value.load() == 7);
  ut::expect(atomic_value.fetch_xor(1) == 7 && atomic_value.load() == 6);
  ut::expect(++atomic_value == 7);
  ut::expect(atomic_value++ == 7 && atomic_value.load() == 8);
  ut::expect(--atomic_value == 7);
  ut::expect(atomic_value-- == 7 && atomic_value.load() == 6);
  ut::expect((atomic_value += 2) == 8);
  ut::expect((atomic_value -= 2) == 6);
  ut::expect((atomic_value &= 3) == 2);
  ut::expect((atomic_value |= 4) == 6);
  ut::expect((atomic_value ^= 1) == 7);
  ut::expect(moss::atomic_load(&atomic_value) == 7);
  moss::atomic_store(&atomic_value, 9u);
  ut::expect(moss::atomic_exchange(&atomic_value, 10u) == 9 && atomic_value.load() == 10);

  CpuBitmap empty_bitmap;
  ut::expect(empty_bitmap.empty() && empty_bitmap.low_byte() == 0 && empty_bitmap.low_word() == 0);
  ut::expect(!empty_bitmap.test(0));
  empty_bitmap.set(3);
  empty_bitmap.set(3);
  ut::expect(empty_bitmap.test(3) && empty_bitmap.low_byte() == 8);
  empty_bitmap.clear(3);
  ut::expect(empty_bitmap.empty());
  empty_bitmap.set_from_u32(0x80000001U);
  ut::expect(empty_bitmap.test(0) && empty_bitmap.test(31) && !empty_bitmap.test(1));
  auto all_cpus = CpuBitmap::all();
  ut::expect(!all_cpus.empty());
  auto one_cpu = CpuBitmap::single(2);
  ut::expect(one_cpu.test(2) && !one_cpu.test(1));
  CpuBitmap copied(one_cpu);
  ut::expect(copied.test(2));
  CpuBitmap moved(moss::move(copied));
  ut::expect(moved.test(2) && copied.empty());
  CpuBitmap wide(130, true);
  ut::expect(wide.test(0) && wide.test(64) && wide.test(129) && !wide.test(130));
  CpuBitmap wide_copy(wide);
  CpuBitmap assigned;
  assigned = wide_copy;
  ut::expect(assigned.test(129));
  CpuBitmap moved_assigned;
  moved_assigned = moss::move(assigned);
  ut::expect(moved_assigned.test(129) && assigned.empty());
  wide = CpuBitmap::single(1);
  ut::expect(wide.test(1) && !wide.test(0));

  ut::expect(moss::align_up<16>(17) == 32 && moss::align_down<16>(17) == 16);
  ut::expect(moss::is_aligned<16>(static_cast<const void *>(bytes)));
  ut::expect(!moss::is_aligned<16>(static_cast<const void *>(bytes + 1)));
  ut::expect(align_up<32>(33) == 64 && align_down<32>(33) == 32 && moss::kernel::is_aligned(64, 32));
  ut::expect(moss::min(5, 3) == 3 && moss::max(5, 3) == 5);
  ut::expect(moss::clamp(1, 3, 5) == 3 && moss::clamp(4, 3, 5) == 4 && moss::clamp(9, 3, 5) == 5);
  ut::expect(moss::abs(static_cast<i32>(-1)) == 1 && moss::abs(static_cast<i32>(1)) == 1);

  alignas(alignof(u32)) u8 placement[sizeof(u32)]{};
  auto *placed = new (placement) u32(42);
  ut::expect(*placed == 42);
  placed->~u32();
  ::operator delete(placed, placement);

  using moss::kernel::Err;
  using moss::kernel::Ok;
  using moss::kernel::Result;
  Result<i32> value = Ok<i32>(7);
  Result<i32> error = Err<ErrorCode>(ErrorCode::Busy);
  Result<i32> copied_value(value);
  Result<i32> copied_error(error);
  Result<i32> moved_value(moss::move(copied_value));
  Result<i32> moved_error(moss::move(copied_error));
  ut::expect(value.has_value() && *value == 7 && error.is_err() && error.error() == ErrorCode::Busy);
  ut::expect(moved_value.value_or(0) == 7 && moved_value.value_or(0) == 7);
  ut::expect(moved_error.value_or(11) == 11 && moss::move(moved_error).value_or(12) == 12);
  value = error;
  ut::expect(value.is_err() && value.error() == ErrorCode::Busy);
  value = Ok<i32>(9);
  ut::expect(value.has_value() && *value == 9);
  value = Result<i32>(ErrorCode::Timeout);
  ut::expect(value.is_err() && value.error() == ErrorCode::Timeout);
  value = Err<ErrorCode>(ErrorCode::NotFound);
  ut::expect(value.is_err());
  Result<i32> assigned_error;
  assigned_error = error;
  ut::expect(assigned_error.is_err());
  assigned_error = moss::move(error);
  ut::expect(assigned_error.is_err());
  Result<i32> assigned_ok;
  assigned_ok = Ok<i32>(3);
  ut::expect(assigned_ok.has_value());
  assigned_ok = moss::move(value);
  ut::expect(assigned_ok.is_err());
  ut::expect((moss::move(assigned_ok).value_or(4)) == 4);
  auto made_ok = moss::kernel::make_result_ok(5);
  auto made_err = moss::kernel::make_result_err(ErrorCode::Busy);
  ut::expect(made_ok.has_value() && *made_ok == 5 && made_err.is_err());
  auto void_ok = Ok<void>();
  auto global_ok = moss::kernel::make_result_ok();
  Result<void> void_copied(void_ok);
  Result<void> void_moved(moss::move(void_copied));
  void_moved = void_ok;
  void_moved = moss::move(void_copied);
  ut::expect(void_moved.has_value() && global_ok.has_value());
  Result<void> void_error = Err<ErrorCode>(ErrorCode::Busy);
  Result<void> void_error_copy(void_error);
  Result<void> void_error_move(moss::move(void_error_copy));
  void_error_move = void_error;
  void_error_move = moss::move(void_error);
  void_error_move = Err<ErrorCode>(ErrorCode::Timeout);
  ut::expect(void_error_move.is_err() && void_error_move.error() == ErrorCode::Timeout);

  TrapFrame user_frame{};
  TrapFrame kernel_frame{};
#if defined(MOSS_ARCH_X64)
  user_frame.cs = 3;
  kernel_frame.cs = 0;
#elif defined(MOSS_ARCH_RISCV64)
  user_frame.status = 0;
  kernel_frame.status = 1ULL << 8;
#else
  user_frame.status = 0;
  kernel_frame.status = 15;
#endif
  ut::expect(user_frame.from_user() && !kernel_frame.from_user());
  ut::expect(moss::abi::linker::text_size() > 0 && moss::abi::linker::rodata_size() > 0 &&
             moss::abi::linker::data_size() > 0 && moss::abi::linker::bss_size() > 0 &&
             moss::abi::linker::stack_size() > 0 && moss::abi::linker::heap_size() > 0 &&
             moss::abi::linker::pagetable_size() > 0);
  ut::expect(moss::abi::linker::bss_start() <= moss::abi::linker::bss_end() &&
             moss::abi::linker::stack_bottom() < moss::abi::linker::stack_top());
#if defined(MOSS_ARCH_ARM64)
  ut::expect(moss::abi::arm64::exception_vectors_addr() != 0 && moss::abi::arm64::user_program_size() > 0);
#endif

  CpuBitmap zero_bitmap(0, true);
  ut::expect(zero_bitmap.empty());
  CpuBitmap heap_bitmap(130, true);
  CpuBitmap inline_bitmap;
  inline_bitmap = moss::move(heap_bitmap);
  ut::expect(inline_bitmap.test(129) && !inline_bitmap.test(1000));
  CpuBitmap heap_target(130, false);
  heap_target = inline_bitmap;
  ut::expect(heap_target.test(129));

  const char text[] = "runtime";
  ut::expect(strnlen(text, 0) == 0 && strnlen(text, 3) == 3 && strnlen(text, 99) == 7);
  ut::expect(strchr(text, 't') == text + 3 && strchr(text, 'x') == nullptr);
  ut::expect(strchr(text, '\0') == text + 7);
  ut::expect(strrchr(text, 't') == text + 3 && strrchr(text, 'x') == nullptr);
  ut::expect(strrchr(text, '\0') == text + 7);
  auto *array_storage = static_cast<char *>(_ZnamSt11align_val_t(32, 16));
  ut::expect(array_storage != nullptr);
  _ZdaPvmSt11align_val_t(array_storage, 32, 16);
  auto *plain_array = static_cast<char *>(_ZnamSt11align_val_t(16, 8));
  ut::expect(plain_array != nullptr);
  _ZdaPv(plain_array);
  auto *plain_storage = static_cast<char *>(_ZnamSt11align_val_t(16, 8));
  ut::expect(plain_storage != nullptr);
  _ZdlPvmSt11align_val_t(plain_storage, 16, 8);
  auto *ordinary = static_cast<char *>(_ZnamSt11align_val_t(8, 8));
  ut::expect(ordinary != nullptr);
  _ZdlPv(ordinary);

  {
    auto *saved_manager = process::g_process_manager;
    auto *saved_scheduler = process::g_scheduler;
    process::ProcessManager manager;
    process::g_process_manager = &manager;
    process::g_scheduler = nullptr;
    auto created = manager.create_process();
    if (ut::expect(static_cast<bool>(created))) {
      auto proc = *created;
      auto space_result = process::user_space::create_user_address_space();
      if (ut::expect(static_cast<bool>(space_result))) {
        auto space = *space_result;
        ut::expect(space->add_vma(process::user_layout::CODE_BASE,
                                  process::user_layout::CODE_BASE + 4 * moss::kernel::PAGE_SIZE,
                                  process::vma_flags::READ | process::vma_flags::EXEC,
                                  process::VmaType::CODE));
        ut::expect(space->add_vma(process::user_layout::STACK_TOP - process::user_layout::STACK_SIZE,
                                  process::user_layout::STACK_TOP,
                                  process::vma_flags::READ | process::vma_flags::WRITE,
                                  process::VmaType::STACK));
        ut::expect(static_cast<bool>(proc->set_address_space(space)));
        auto *thread = new process::Thread(process::Process::allocate_thread_id(), proc->pid());
        if (ut::expect(proc->register_thread(thread).has_value())) {
          TrapFrame frame{};
#if defined(MOSS_ARCH_ARM64)
          frame.status = 0;
          frame.pc = process::user_layout::CODE_BASE;
          frame.sp = process::user_layout::STACK_TOP - 16;
#elif defined(MOSS_ARCH_X64)
          frame.cs = 3;
          frame.pc = process::user_layout::CODE_BASE;
          frame.sp = process::user_layout::STACK_TOP - 16;
#else
          frame.status = 0;
          frame.pc = process::user_layout::CODE_BASE;
          frame.sp = process::user_layout::STACK_TOP - 16;
#endif
          thread->trap_frame = &frame;
          process::Sigaction action{};
          action.handler = process::user_layout::CODE_BASE;
          ut::expect(!process::setup_sigframe(nullptr, process::sig::SIGUSR1, action));
          thread->trap_frame = nullptr;
          ut::expect(!process::setup_sigframe(thread, process::sig::SIGUSR1, action));
          thread->trap_frame = &frame;
#if defined(MOSS_ARCH_ARM64)
          frame.status = 15;
#elif defined(MOSS_ARCH_X64)
          frame.cs = 0;
#else
          frame.status = 1ULL << 8;
#endif
          ut::expect(!process::setup_sigframe(thread, process::sig::SIGUSR1, action));
#if defined(MOSS_ARCH_ARM64)
          frame.status = 0;
#elif defined(MOSS_ARCH_X64)
          frame.cs = 3;
#else
          frame.status = 0;
#endif
          const auto saved_sp = frame.sp;
          frame.sp = 32;
          ut::expect(!process::setup_sigframe(thread, process::sig::SIGUSR1, action));
          frame.sp = saved_sp;
          action.handler = process::user_layout::HEAP_START;
          ut::expect(!process::setup_sigframe(thread, process::sig::SIGUSR1, action));
          action.handler = process::user_layout::CODE_BASE;
          thread->alt_stack_flags = 0;
          thread->alt_stack_sp = ~u64{0} - 1;
          thread->alt_stack_size = 4;
          action.flags = process::sa_flags::SA_ONSTACK;
          ut::expect(!process::setup_sigframe(thread, process::sig::SIGUSR1, action));
          thread->alt_stack_sp = process::user_layout::STACK_TOP - moss::kernel::PAGE_SIZE;
          thread->alt_stack_size = moss::kernel::PAGE_SIZE;
          thread->on_alt_stack = true;
          frame.sp = process::user_layout::STACK_TOP + 16;
          ut::expect(!process::setup_sigframe(thread, process::sig::SIGUSR1, action));
          frame.sp = process::user_layout::STACK_TOP - 16;
          thread->on_alt_stack = false;
          action.flags = 0;
          thread->restart_syscall_pending = true;
          action.flags = process::sa_flags::SA_RESTART;
#if defined(MOSS_ARCH_ARM64)
          frame.pc = process::user_layout::CODE_BASE + 8;
#elif defined(MOSS_ARCH_X64)
          frame.pc = process::user_layout::CODE_BASE + 2;
#else
          frame.pc = process::user_layout::CODE_BASE + 8;
#endif
          ut::expect(process::setup_sigframe(thread, process::sig::SIGUSR1, action));
          thread->restart_syscall_pending = false;
#if defined(MOSS_ARCH_X64)
          // sigreturn runs after RET consumes the 8-byte handler link slot, so
          // the user RSP is the active frame address, not the handler entry SP.
          frame.sp = thread->active_signal_frame;
#endif
          ut::expect(process::do_sigreturn(thread) >= 0);
          thread->trap_frame = nullptr;
          ut::expect(process::do_sigreturn(thread) < 0);
          thread->trap_frame = &frame;
          thread->active_signal_frame = 0;
          ut::expect(process::do_sigreturn(thread) < 0);

          auto set_action = [&](u32 signo, VirtAddr handler, u32 flags = 0) {
            const auto current = proc->signal_action(signo);
            process::Sigaction desired{};
            desired.handler = handler;
            desired.flags = flags;
            return proc->try_replace_signal_action(signo, current, desired);
          };
          process::CfsScheduler::set_current_task(thread);
          ut::expect(process::do_signal_checkpoint(nullptr) == 0);
          ut::expect(process::do_signal_checkpoint(thread) == 0);
          thread->pending_signals.store(process::sig::sigmask(process::sig::SIGTERM));
          ut::expect(process::do_signal_checkpoint(thread) == process::sig::SIGTERM);
          ut::expect(set_action(process::sig::SIGUSR1, process::SIG_IGN));
          thread->pending_signals.store(process::sig::sigmask(process::sig::SIGUSR1));
          ut::expect(process::do_signal_checkpoint(thread) == 0);
          ut::expect(set_action(process::sig::SIGUSR2, process::user_layout::CODE_BASE));
          thread->pending_signals.store(process::sig::sigmask(process::sig::SIGUSR2));
          ut::expect(process::do_signal_checkpoint(thread) == 0);
          thread->pending_signals.store(process::sig::sigmask(process::sig::SIGSTOP));
          ut::expect(process::do_signal_checkpoint(thread) == process::sig::SIGSTOP);
          thread->pending_signals.store(process::sig::sigmask(process::sig::SIGCONT));
          ut::expect(process::do_signal_checkpoint(thread) == 0);

          ut::expect(!process::send_signal(nullptr, process::sig::SIGUSR1));
          ut::expect(!process::send_signal(thread, 0));
          ut::expect(!process::send_signal(thread, process::sig::NSIG));
          thread->state = process::ProcessState::Sleeping;
          ut::expect(process::send_signal(thread, process::sig::SIGUSR1));
          thread->pending_signals.store(0);
          thread->state = process::ProcessState::Stopped;
          thread->job_stopped = true;
          ut::expect(process::send_signal(thread, process::sig::SIGCONT));
          ut::expect(!thread->job_stopped);
          process::CfsScheduler::set_current_task(nullptr);
        }
      }
    }
    process::g_process_manager = saved_manager;
    process::g_scheduler = saved_scheduler;
  }
}

void memory_paths() {
  using namespace mm;
  ut::expect(PageFrameAllocator::initialize().has_value());
  const auto pfa_page = PageFrameAllocator::allocate_pages(0);
  if (ut::expect(static_cast<bool>(pfa_page))) {
    ut::expect(PageFrameAllocator::page_ref_get(*pfa_page) == 1);
    PageFrameAllocator::page_ref_inc(*pfa_page);
    ut::expect(PageFrameAllocator::page_ref_get(*pfa_page) == 2);
    PageFrameAllocator::page_ref_set(*pfa_page, 1);
    ut::expect(PageFrameAllocator::page_ref_get(*pfa_page) == 1);
    ut::expect(!PageFrameAllocator::free_pages(*pfa_page + 1, 0).has_value());
    ut::expect(!PageFrameAllocator::free_pages(*pfa_page, MAX_ORDER + 1).has_value());
    ut::expect(PageFrameAllocator::free_pages(*pfa_page, 0).has_value());
    ut::expect(!PageFrameAllocator::free_pages(*pfa_page, 0).has_value());
  }
  ut::expect(PageFrameAllocator::page_ref_get(0) == 0 && PageFrameAllocator::page_ref_dec(0) == 0);
  PageFrameAllocator::page_ref_inc(0);
  PageFrameAllocator::page_ref_set(0, 9);
  auto stats = PageFrameAllocator::get_memory_stats();
  ut::expect(stats.total_pages > 0 && stats.free_pages <= stats.total_pages && stats.metadata_size > 0);
  NUMADistance distance;
  ut::expect(distance.get_distance(0, 0) == NUMADistance::LOCAL_DISTANCE);
  ut::expect(distance.get_distance(0, 1) == NUMADistance::UNREACHABLE_DISTANCE);
  ut::expect(distance.get_distance(MAX_NUMA_NODES, 0) == NUMADistance::UNREACHABLE_DISTANCE);
  distance.set_distance(0, 1, NUMADistance::REMOTE_DISTANCE);
  ut::expect(distance.get_distance(0, 1) == NUMADistance::REMOTE_DISTANCE);
  ut::expect(distance.get_distance(1, 0) == NUMADistance::REMOTE_DISTANCE);
  distance.set_distance(MAX_NUMA_NODES, 0, 1);

  NUMANode offline;
  NUMANode online(1, 0x10000000, 0x200000);
  ut::expect(offline.node_id == NUMA_NO_NODE && offline.state == NodeState::OFFLINE);
  ut::expect(online.node_id == 1 && online.state == NodeState::ONLINE && online.memory_size == 0x200000);
  NUMAAllocator allocator(nullptr);
  const NUMAAllocator::AllocationRequest request(4096, NUMAPolicy::BIND, 1);
  ut::expect(request.size == 4096 && request.preferred_node == 1);
  (void)allocator.get_stats();
  NUMABalancer::BalancerConfig balance_config{};
  balance_config.balance_interval_ms = 1;
  NUMABalancer::BalancerStats balance_stats{};
  balance_stats.migrations_performed = 1;
  NUMAPolicyManager::NUMAConfig numa_config{};
  numa_config.default_policy = NUMAPolicy::PREFERRED;
  (void)balance_config;
  (void)balance_stats;
  (void)numa_config;

  VmallocRequest vmalloc_request(4096);
  ut::expect(vmalloc_request.size == 4096 && vmalloc_request.type == VmAreaType::VMALLOC);
  VmArea area;
  VmArea mapped(0x1000, 0x2000, VmAreaType::IOREMAP, 1);
  ut::expect(!area.contains(0) && mapped.contains(0x1000) && !mapped.contains(0x3000));
  ut::expect(mapped.overlaps(0x2000, 0x4000) && !mapped.overlaps(0x3000, 0x4000));

  ReclaimPageInfo page_info;
  LRUList::LRUEntry lru_entry;
  CMARegion cma_region;
  CMARegion cma_active(0x10000000, 0x200000, 1);
  PageMigration migration;
  HugePageInfo huge_info;
  ut::expect(page_info.phys_addr == 0 && !page_info.is_dirty);
  ut::expect(lru_entry.next == nullptr && lru_entry.scan_count == 0);
  ut::expect(cma_region.base_addr == 0 && !cma_region.is_active);
  ut::expect(cma_active.base_addr == 0x10000000 && cma_active.is_active);
  ut::expect(migration.source == 0 && !migration.completed);
  ut::expect(huge_info.phys_addr == 0 && huge_info.size == HugePageSize::SIZE_2MB);
  MemoryReclaimEngine::ReclaimConfig reclaim_config{};
  MemoryCompactionEngine::CompactionConfig compaction_config{};
  CMAAllocator::CMAConfig cma_config{};
  HugePagesManager::HugePagesConfig huge_config{};
  HugePagePool::PoolConfig pool_config{};
  THPManager::THPConfig thp_config{};
  HugeTLBManager::HugeTLBConfig hugetlb_config{};
  (void)reclaim_config;
  (void)compaction_config;
  (void)cma_config;
  (void)huge_config;
  (void)pool_config;
  (void)thp_config;
  (void)hugetlb_config;

  const auto before = RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  auto allocated = mm::alloc(64);
  if (ut::expect(static_cast<bool>(allocated))) {
    ut::expect(static_cast<bool>(mm::free(*allocated)));
  }
  void *heap = kmalloc(32);
  ut::expect(heap != nullptr);
  kfree(heap);
  void *zero = kzalloc(32);
  ut::expect(zero != nullptr);
  kfree_sized(zero, 32);
  void *aligned = kmalloc_aligned(64, 64);
  ut::expect(aligned != nullptr);
  kfree(aligned);
  void *atomic = kmalloc_atomic(32);
  ut::expect(atomic != nullptr);
  kfree(atomic);
  void *local = kmalloc_numa(32, 0);
  ut::expect(local != nullptr);
  kfree(local);
  void *huge = kmalloc_huge(32);
  ut::expect(huge != nullptr);
  kfree(huge);
  ut::expect(kmalloc(0) == nullptr);
  ut::expect(kzalloc(0) == nullptr);
  ut::expect(kmalloc_aligned(0, 64) == nullptr);
  ut::expect(kmalloc_atomic(0) == nullptr);
  ut::expect(kmalloc_numa(0, 0) == nullptr);
  ut::expect(kmalloc_huge(0) == nullptr);
  void *fresh = krealloc(nullptr, 0, 32);
  ut::expect(fresh != nullptr);
  ut::expect(krealloc(fresh, 32, 0) == nullptr);
  void *grown = kmalloc(32);
  ut::expect(grown != nullptr && krealloc(grown, 32, 64) == nullptr);
  kfree_sized(grown, 32);
  ut::expect(!mm::get_pressure());
  ut::expect(mm::is_healthy());
  ut::expect(!mm::gc());
  ut::expect(!mm::optimize(0, 0));
  ut::expect(!mm::compact());
  ut::expect(!trigger_memory_gc());
  ut::expect(!get_memory_pressure());
  ut::expect(!get_memory_stats());
  print_memory_stats();
  ut::expect(!check_memory_leaks());
  ut::expect(RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);

  MemoryRequest request_default(64);
  MemoryRequest request_aligned(64, 128, AllocFlags::ZERO_MEMORY);
  MemoryInfo memory_info;
  MemoryLeakDetector::LeakReport leak_report;
  ut::expect(request_default.size == 64 && request_aligned.alignment == 128);
  ut::expect(memory_info.virtual_address == 0 && memory_info.size == 0);
  ut::expect(leak_report.total_leaked_bytes == 0 && leak_report.leak_count == 0);
}

void memory_manager_surfaces() {
  using namespace mm;

  auto scan_config = PageScanner::ScanConfig{};
  scan_config.scan_batch_size = 16;
  scan_config.max_scan_pages = 64;
  scan_config.scan_priority = 1;
  scan_config.policy = ReclaimPolicy::BALANCED;
  scan_config.scan_anon_pages = true;
  scan_config.scan_file_pages = false;
  scan_config.allow_writeback = true;
  PageScanner::ScanResult scan_result{};
  scan_result.scanned_pages = 1;
  WorkingSetDetector::WorkingSetInfo working_set{};
  working_set.hot_pages = 1;
  MemoryPressureMonitor::PressureConfig pressure_config{};
  pressure_config.low_threshold_pages = 1;
  pressure_config.medium_threshold_pages = 2;
  pressure_config.high_threshold_pages = 3;
  pressure_config.critical_threshold_pages = 4;
  pressure_config.check_interval_ms = 5;
  MemoryPressureMonitor::PressureInfo pressure_info{};
  pressure_info.available_pages = 1;
  MemoryReclaimEngine::ReclaimStats reclaim_stats{};
  reclaim_stats.total_pages_reclaimed = 1;

  CompactionScanner::ScanConfig compaction_scan{};
  compaction_scan.scan_batch_size = 1;
  compaction_scan.max_migrate_pages = 2;
  compaction_scan.strategy = CompactionStrategy::LIGHT;
  compaction_scan.scan_whole_zone = true;
  compaction_scan.scan_priority = 3;
  CompactionScanner::ScanResult compaction_result{};
  compaction_result.pages_scanned = 1;
  PageMigrator::MigrationConfig migration_config{};
  migration_config.default_mode = MigrationMode::ASYNC;
  migration_config.max_batch_size = 2;
  migration_config.max_retries = 3;
  migration_config.timeout_us = 4;
  PageMigrator::MigrationStats migration_stats{};
  migration_stats.total_migrations = 1;
  CMAAllocator::CMAConfig cma_config{};
  cma_config.default_region_size = 2 * PAGE_SIZE;
  cma_config.default_alignment_order = 1;
  cma_config.enable_migration = true;
  CMAAllocator::CMAStats cma_stats{};
  cma_stats.total_regions = 1;
  MemoryCompactionEngine::CompactionConfig compaction_config{};
  compaction_config.default_strategy = CompactionStrategy::HEAVY;
  compaction_config.enable_background_compaction = false;
  compaction_config.compaction_interval_ms = 1;
  compaction_config.fragmentation_threshold = 2;
  MemoryCompactionEngine::CompactionStats compaction_stats{};
  compaction_stats.pages_migrated = 1;

  HugePagePool::PoolConfig pool_config{};
  pool_config.initial_2mb_pages = 1;
  pool_config.initial_1gb_pages = 0;
  pool_config.max_2mb_pages = 2;
  pool_config.max_1gb_pages = 0;
  pool_config.reserve_2mb_pages = 0;
  pool_config.reserve_1gb_pages = 0;
  HugePagePool::PoolStats pool_stats{};
  pool_stats.total_2mb_pages = 1;
  THPManager::THPConfig thp_config{};
  thp_config.policy = THPPolicy::ALWAYS;
  thp_config.split_policy = SplitPolicy::LAZY;
  thp_config.defrag_policy = DefragPolicy::DEFER;
  thp_config.max_thp_pages = 1;
  thp_config.enable_khugepaged = false;
  thp_config.scan_interval_ms = 1;
  thp_config.pages_to_scan = 1;
  THPManager::THPStats thp_stats{};
  thp_stats.current_thp_count = 1;
  HugeTLBManager::HugeTLBConfig hugetlb_config{};
  hugetlb_config.default_pool_2mb = 1;
  hugetlb_config.default_pool_1gb = 0;
  hugetlb_config.overcommit_allowed = false;
  hugetlb_config.overcommit_ratio = 0;
  HugePagesManager::HugePagesConfig huge_config{};
  huge_config.pool_config = pool_config;
  huge_config.thp_config = thp_config;
  huge_config.hugetlb_config = hugetlb_config;
  huge_config.enable_huge_pages = false;
  huge_config.enable_thp = false;
  huge_config.enable_hugetlb = false;
  HugePagesManager::HugePagesSystemStats huge_stats{};
  huge_stats.total_huge_memory = 1;

  ut::expect(scan_config.scan_batch_size == 16 && scan_result.scanned_pages == 1 && working_set.hot_pages == 1);
  ut::expect(pressure_config.high_threshold_pages == 3 && pressure_info.available_pages == 1);
  ut::expect(reclaim_stats.total_pages_reclaimed == 1 && compaction_scan.scan_priority == 3);
  ut::expect(compaction_result.pages_scanned == 1 && migration_config.timeout_us == 4);
  ut::expect(migration_stats.total_migrations == 1 && cma_config.default_alignment_order == 1);
  ut::expect(cma_stats.total_regions == 1 && compaction_config.fragmentation_threshold == 2);
  ut::expect(compaction_stats.pages_migrated == 1 && pool_config.max_2mb_pages == 2);
  ut::expect(pool_stats.total_2mb_pages == 1 && thp_config.max_thp_pages == 1);
  ut::expect(thp_stats.current_thp_count == 1 && hugetlb_config.default_pool_2mb == 1);
  ut::expect(huge_config.enable_huge_pages == false && huge_stats.total_huge_memory == 1);

  UnifiedMemoryManager::SystemConfig system_config{};
  system_config.min_free_memory = PAGE_SIZE;
  ut::expect(system_config.min_free_memory == PAGE_SIZE);
  ut::expect(UnifiedMemoryManager::is_system_initialized());
  ut::expect(UnifiedMemoryManager::is_system_healthy());
  ut::expect(!UnifiedMemoryManager::query_memory_info(0));
  ut::expect(!UnifiedMemoryManager::get_allocated_size(0));
  VirtAddr address = 0;
  ut::expect(!UnifiedMemoryManager::reallocate(address, 1, 2));
  ut::expect(!UnifiedMemoryManager::prefault_memory(0, 1));
  ut::expect(!UnifiedMemoryManager::advise_usage_pattern(0, 1, UsagePattern::RANDOM));
  ut::expect(!UnifiedMemoryManager::trigger_memory_reclaim());
  ut::expect(!UnifiedMemoryManager::trigger_memory_compaction());
  ut::expect(!UnifiedMemoryManager::get_memory_pressure());
  ut::expect(!UnifiedMemoryManager::optimize_numa_placement(0, 1));
  ut::expect(!UnifiedMemoryManager::promote_to_huge_pages(0, 1));
  ut::expect(!UnifiedMemoryManager::compact_memory_region(0, 1));
  ut::expect(!UnifiedMemoryManager::get_performance_stats());
  ut::expect(!UnifiedMemoryManager::reset_performance_counters());
  ut::expect(!UnifiedMemoryManager::dump_allocation_history());
  ut::expect(!UnifiedMemoryManager::generate_leak_report());
  UnifiedMemoryManager::dump_memory_layout();

  auto direct = mm::alloc(128);
  if (ut::expect(static_cast<bool>(direct))) {
    ut::expect(static_cast<bool>(UnifiedMemoryManager::free(*direct)));
  }
  auto sized = mm::alloc(128);
  if (ut::expect(static_cast<bool>(sized))) {
    ut::expect(static_cast<bool>(UnifiedMemoryManager::free(*sized, 128)));
  }
  ut::expect(static_cast<bool>(UnifiedMemoryManager::free(0)));
  ut::expect(static_cast<bool>(UnifiedMemoryManager::free(0, 1)));

  // Tear down and rebuild the publication shell to exercise both state-machine
  // transitions rather than only the already-ready fast path.
  UnifiedMemoryManager::shutdown_system();
  ut::expect(!UnifiedMemoryManager::is_system_initialized());
  UnifiedMemoryManager::shutdown_system();
  UnifiedMemoryManager::SystemConfig config{};
  config.min_free_memory = PAGE_SIZE;
  ut::expect(static_cast<bool>(UnifiedMemoryManager::initialize_system(config)));
  ut::expect(UnifiedMemoryManager::is_system_initialized());
  ut::expect(static_cast<bool>(UnifiedMemoryManager::initialize_system(config)));
  shutdown_kernel_memory();
  ut::expect(!is_memory_system_healthy());
  ut::expect(static_cast<bool>(UnifiedMemoryManager::initialize_system(config)));
  ut::expect(is_memory_system_healthy());
}

namespace cpio_fixture {
// Backed by direct-mapped pages: virt_to_phys() is only valid for the direct
// map, and the initramfs parser consumes a physical address.
static u8 *archive = nullptr;

void hex_field(char *field, u32 value) noexcept {
  constexpr char digits[] = "0123456789ABCDEF";
  for (i32 i = 7; i >= 0; --i) {
    field[i] = digits[value & 15U];
    value >>= 4;
  }
}

usize append_entry(usize offset, const char *name, const u8 *data, u32 size) noexcept {
  auto *header = reinterpret_cast<initramfs::CpioNewcHeader *>(archive + offset);
  for (usize i = 0; i < sizeof(*header); ++i) {
    reinterpret_cast<u8 *>(header)[i] = '0';
  }
  const char *magic = "070701";
  for (usize i = 0; i < 6; ++i) {
    header->c_magic[i] = magic[i];
  }
  u32 name_size = 0;
  while (name[name_size]) {
    ++name_size;
  }
  ++name_size;
  hex_field(header->c_mode, 0100644);
  hex_field(header->c_uid, 0);
  hex_field(header->c_gid, 0);
  hex_field(header->c_nlink, 1);
  hex_field(header->c_mtime, 0);
  hex_field(header->c_filesize, size);
  hex_field(header->c_devmajor, 0);
  hex_field(header->c_devminor, 0);
  hex_field(header->c_rdevmajor, 0);
  hex_field(header->c_rdevminor, 0);
  hex_field(header->c_namesize, name_size);
  hex_field(header->c_check, 0);
  usize at = offset + sizeof(initramfs::CpioNewcHeader);
  for (u32 i = 0; i < name_size; ++i) {
    archive[at++] = static_cast<u8>(name[i]);
  }
  while (at % 4) {
    archive[at++] = 0;
  }
  for (u32 i = 0; i < size; ++i) {
    archive[at++] = data ? data[i] : 0;
  }
  while (at % 4) {
    archive[at++] = 0;
  }
  return at;
}

usize build() noexcept {
  usize at = append_entry(0, ".", nullptr, 0);
  const u8 first[] = {1, 2, 3};
  at = append_entry(at, "./a", first, sizeof(first));
  at = append_entry(at, "/b", nullptr, 0);
  const u8 third[] = {9, 8};
  at = append_entry(at, "c", third, sizeof(third));
  const char trailer[] = "TRAILER!!!";
  at = append_entry(at, trailer, nullptr, 0);
  return at;
}
} // namespace cpio_fixture

void kernel_elf_edges() {
  namespace elf = moss::kernel::elf;
  alignas(16) static u8 image[512]{};
  auto reset = [&] {
    for (usize i = 0; i < sizeof(image); ++i) {
      image[i] = 0;
    }
    auto *header = reinterpret_cast<elf::ElfHeader *>(image);
    header->e_ident[0] = 0x7f;
    header->e_ident[1] = 'E';
    header->e_ident[2] = 'L';
    header->e_ident[3] = 'F';
    header->e_ident[4] = elf::ELF_CLASS_64;
    header->e_ident[5] = elf::ELF_DATA_LSB;
    header->e_ident[6] = elf::ELF_VERSION;
    header->e_type = elf::ET_EXEC;
#if defined(MOSS_ARCH_ARM64)
    header->e_machine = elf::EM_AARCH64;
#elif defined(MOSS_ARCH_X64)
    header->e_machine = elf::EM_X64;
#else
    header->e_machine = elf::EM_RISCV64;
#endif
    header->e_version = elf::ELF_VERSION;
    header->e_entry = 0x1000;
    header->e_phoff = sizeof(elf::ElfHeader);
    header->e_ehsize = sizeof(elf::ElfHeader);
    header->e_phentsize = sizeof(elf::ProgramHeader);
    header->e_phnum = 1;
    auto *program = reinterpret_cast<elf::ProgramHeader *>(image + sizeof(elf::ElfHeader));
    program->p_type = elf::PT_LOAD;
    program->p_flags = elf::PF_R | elf::PF_X;
    program->p_offset = 0;
    program->p_vaddr = 0x1000;
    program->p_filesz = 0x10;
    program->p_memsz = 0x100;
    program->p_align = 0x1000;
  };
  const elf::LoadPlanPolicy policy{.page_size = 0x1000,
                                   .user_begin = 0x1000,
                                   .user_end = 0x100000,
                                   .reserved = nullptr,
                                   .reserved_count = 0};
  reset();
  auto *header = reinterpret_cast<elf::ElfHeader *>(image);
  ut::expect(elf::validate_elf_header(header, sizeof(image)));
  elf::LoadPlan plan{};
  ut::expect(elf::build_load_plan(image, sizeof(image), policy, plan));
  ut::expect(plan.segment_count == 1 && plan.entry == 0x1000);

  auto *program = reinterpret_cast<elf::ProgramHeader *>(image + sizeof(elf::ElfHeader));
  auto expect_invalid = [&] {
    elf::LoadPlan rejected{};
    return !elf::build_load_plan(image, sizeof(image), policy, rejected);
  };

  reset();
  ut::expect(!elf::validate_elf_header(header, sizeof(elf::ElfHeader) - 1));
  reset();
  header->e_ident[0] = 0;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_ident[4] = 1;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_ident[5] = 2;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_type = elf::ET_DYN;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_machine = elf::EM_NONE;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_ident[6] = 2;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_version = 2;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_ehsize = 0;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_phentsize = 0;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_phoff = 0;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_phnum = 0;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_phnum = 65;
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));
  reset();
  header->e_phoff = sizeof(image);
  ut::expect(!elf::validate_elf_header(header, sizeof(image)));

  elf::LoadPlanPolicy bad_policy = policy;
  bad_policy.page_size = 0;
  ut::expect(!elf::build_load_plan(image, sizeof(image), bad_policy, plan));
  bad_policy = policy;
  bad_policy.page_size = 3;
  ut::expect(!elf::build_load_plan(image, sizeof(image), bad_policy, plan));
  bad_policy = policy;
  bad_policy.user_begin = bad_policy.user_end;
  ut::expect(!elf::build_load_plan(image, sizeof(image), bad_policy, plan));
  bad_policy = policy;
  bad_policy.reserved_count = 1;
  ut::expect(!elf::build_load_plan(image, sizeof(image), bad_policy, plan));
  ut::expect(!elf::build_load_plan(nullptr, sizeof(image), policy, plan));

  reset();
  program->p_type = elf::PT_INTERP;
  ut::expect(expect_invalid());
  reset();
  program->p_type = elf::PT_DYNAMIC;
  ut::expect(expect_invalid());
  reset();
  program->p_type = elf::PT_NOTE;
  ut::expect(expect_invalid());
  reset();
  program->p_filesz = 0x200;
  program->p_memsz = 0x100;
  ut::expect(expect_invalid());
  reset();
  program->p_offset = sizeof(image) + 1;
  ut::expect(expect_invalid());
  reset();
  program->p_vaddr = 0x800;
  ut::expect(expect_invalid());
  reset();
  program->p_vaddr = policy.user_end;
  ut::expect(expect_invalid());
  reset();
  program->p_memsz = policy.user_end;
  ut::expect(expect_invalid());
  reset();
  program->p_flags = elf::PF_W | elf::PF_X;
  ut::expect(expect_invalid());
  reset();
  program->p_flags = 0x8;
  ut::expect(expect_invalid());
  reset();
  program->p_memsz = 0;
  ut::expect(expect_invalid());
  reset();
  program->p_align = 3;
  ut::expect(expect_invalid());
  reset();
  program->p_offset = 1;
  ut::expect(expect_invalid());
  reset();
  program->p_vaddr = 0xf000000000000000ULL;
  program->p_memsz = 0x2000;
  ut::expect(expect_invalid());
  reset();
  header->e_entry = 0x9000;
  ut::expect(expect_invalid());

  // A conflicting reserved page and a second overlapping LOAD are rejected.
  reset();
  const elf::LoadRange reserved{.start = 0x1000, .end = 0x2000};
  elf::LoadPlanPolicy reserved_policy = policy;
  reserved_policy.reserved = &reserved;
  reserved_policy.reserved_count = 1;
  ut::expect(!elf::build_load_plan(image, sizeof(image), reserved_policy, plan));
  reset();
  header->e_phnum = 2;
  auto *second = program + 1;
  *second = *program;
  second->p_vaddr = 0x1000;
  second->p_offset = 0;
  ut::expect(expect_invalid());

  // TLS must be fully covered by a LOAD with matching file translation.
  reset();
  header->e_phnum = 2;
  *second = *program;
  second->p_type = elf::PT_TLS;
  second->p_flags = elf::PF_R;
  second->p_vaddr = 0x1000;
  second->p_offset = 0;
  second->p_filesz = 0x10;
  second->p_memsz = 0x10;
  second->p_align = 1;
  elf::LoadPlan tls{};
  ut::expect(elf::build_load_plan(image, sizeof(image), policy, tls));
  second->p_offset = 1;
  ut::expect(expect_invalid());
  second->p_offset = 0;
  second->p_flags = elf::PF_W | elf::PF_X;
  ut::expect(expect_invalid());
  second->p_flags = elf::PF_R;
  second->p_align = 3;
  ut::expect(expect_invalid());
}

void boot_edges() {
  boot::BootContext ctx{};
  ut::expect(boot::ArchBoot::detect_memory_layout(ctx).has_value());
  (void)boot::ArchBoot::get_current_cpu_id();
  ut::expect(boot::ArchBoot::finalize_arch_init(ctx).has_value());

  const auto saved_platform = platform::hardware;
  platform::hardware = {};
  ut::expect(!platform::order_cpus(0));
  platform::hardware.cpu_count = 17;
  ut::expect(!platform::order_cpus(0));
  platform::hardware.cpu_count = 2;
  platform::hardware.cpus[0].hardware_id = 1;
  platform::hardware.cpus[1].hardware_id = 1;
  ut::expect(!platform::order_cpus(1));
  platform::hardware.cpus[1].hardware_id = 2;
  ut::expect(!platform::order_cpus(3));
  ut::expect(platform::order_cpus(2));
  ut::expect(platform::hardware.cpus[0].hardware_id == 2);
  ut::expect(platform::logical_cpu(2) == 0 && platform::logical_cpu(9) == 16);
  platform::hardware.cpu_count = 0;
  ut::expect(platform::logical_cpu(9) == 0);
  (void)platform::uart_base();
  (void)platform::intc_dist_base();
  (void)platform::intc_cpu_base();
  (void)platform::intc_redist_base();
  (void)platform::ram_base();
  (void)platform::ram_size();
  (void)platform::timer_irq();
  (void)platform::timer_frequency();
  volatile auto virt_base = platform::kernel_virt_base();
  (void)virt_base;
  platform::hardware = saved_platform;

  arch::switch_to_kernel_stack(arch::get_current_stack_pointer());
  containers::WaitQueueEntry waiter;
  ut::expect(waiter.thread == nullptr);
  containers::SPSCQueue<u32, 4> queue;
  ut::expect(queue.capacity() == 3);
  ut::expect(containers::is_aligned(64, 8) && !containers::is_aligned(65, 8));
  ut::expect(moss::align_up<32>(33) == 64);

  ut::expect(logging::get_log_level() >= logging::LogLevel::Debug);
  logging::klog::info("{} {}", true, static_cast<u32>(1));
  logging::klog::info("trailing {");

  syscall::SyscallDispatcher::print_implemented_syscalls();

  ut::expect(mm::page_to_addr(2) == 2 * moss::kernel::PAGE_SIZE);
  ut::expect(mm::addr_to_page(2 * moss::kernel::PAGE_SIZE) == 2);
  ut::expect(!mm::page_alloc::alloc_user_pages(100).has_value());
  ut::expect(!mm::page_alloc::alloc_cache_pages(100).has_value());
  ut::expect(!mm::page_alloc::alloc_emergency_pages(100).has_value());
  auto aligned = mm::mm::alloc_aligned(64, 8);
  if (aligned) {
    ut::expect(mm::mm::free(*aligned).has_value());
  }
  (void)mm::mm::get_pressure();
  (void)mm::mm::is_healthy();
  (void)mm::mm::gc();
  (void)mm::mm::optimize(0, 0);
  (void)mm::mm::compact();
  (void)mm::get_memory_stats();
  mm::print_memory_stats();
  (void)mm::check_memory_leaks();
  void *aligned_heap = mm::kmalloc_aligned(64, 8);
  if (aligned_heap != nullptr) {
    mm::kfree(aligned_heap);
  }
  void *atomic_heap = mm::kmalloc_atomic(8);
  if (atomic_heap != nullptr) {
    mm::kfree(atomic_heap);
  }
  void *huge_heap = mm::kmalloc_huge(64);
  if (huge_heap != nullptr) {
    mm::kfree(huge_heap);
  }
  (void)mm::mm::alloc_zero(0);
  (void)mm::mm::alloc_huge(0);
  (void)mm::mm::free_sized(0, 0);
  mm::invalidate_all_tlb();

  auto &timers = timer::TimerSubsystem::instance();
  (void)timers.get_statistics();
  (void)timers.clocksource().now_ms();
  timer::HrTimer probe;
  ut::expect(probe.mode() == timer::TimerMode::OneShot);
  probe.init(timer::TimerMode::Periodic, [](void *) noexcept {}, nullptr);
  ut::expect(probe.mode() == timer::TimerMode::Periodic);
  probe.cancel_sync();

  process::Thread thread(1, 0);
  process::ThreadEntry entry(1, &thread);
  process::ThreadEntry other(2, &thread);
  ut::expect(entry == entry && !(entry == other));
  ut::expect(process::is_signal_wakeable(process::ProcessState::Sleeping));
  process::VmaRegion region{};
  ut::expect(!region.is_demand_zero() && !region.has_backing());
  process::VmaRegion demand_zero(0x1000, 0x2000, process::vma_flags::DEMAND_ZERO);
  u8 backing = 0;
  process::VmaRegion backed(0x1000, 0x2000, process::vma_flags::READ, process::VmaType::DATA, &backing, 0, 1);
  ut::expect(demand_zero.is_demand_zero() && backed.has_backing());
  ut::expect(demand_zero == process::VmaRegion(0x1000, 0x2000, 0));
  ut::expect(!(demand_zero == process::VmaRegion(0x1000, 0x3000, 0)));
  process::CpuBudget budget{};
  ut::expect(!budget.configure(0, 100, 0));
  ut::expect(!budget.configure(101, 100, 0));
  ut::expect(budget.configure(50, 100, 1000));
  ut::expect(budget.configured());
  budget.advance(0);
  budget.advance(1001);
  budget.advance(1099);
  budget.advance(1100);
  budget.spent_ns = 80;
  ut::expect(budget.exhausted(1100));
  budget.runtime_ns = 50;
  budget.period_ns = 1000;
  budget.period_start_ns = 2000;
  budget.spent_ns = ~u64{0} - 1;
  budget.charge(1900, 5000);
  ut::expect(budget.spent_ns == ~u64{0});
  process::Process proc(1);
  ut::expect(!proc.has_children());
  proc.set_parent_pid(7);
  ut::expect(proc.parent_pid() == 7);
  (void)proc.capabilities();
  vfs::Inode inode{};
  inode.type = vfs::FileType::CharDev;
  ut::expect(inode.is_char_device());
  inode.type = vfs::FileType::Fifo;
  ut::expect(inode.is_pipe());

  struct ProbeObject final : capability::Object {
    ProbeObject() noexcept : Object(capability::ObjectType::Memory) {}
  } object;
  ut::expect(object.event_queue() == nullptr && !object.event_ready());

  (void)boot::get_boot_status();
  const auto arch_info = boot::get_current_arch_info();
  ut::expect(arch_info.name != nullptr && arch_info.id != 0);
  boot::update_boot_stage(boot::BootStage::Complete, ErrorCode::Success);
}

void kernel_entry_paths() {
  {
    TrapFrame frame{};
#if defined(MOSS_ARCH_ARM64)
    frame.status = 15;
#elif defined(MOSS_ARCH_X64)
    frame.cs = 0;
#else
    frame.status = 1ULL << 8;
#endif
    user_return_handler(&frame);
    auto *saved = process::CfsScheduler::get_current_task();
    process::CfsScheduler::set_current_task(nullptr);
#if defined(MOSS_ARCH_ARM64)
    frame.status = 0;
#elif defined(MOSS_ARCH_X64)
    frame.cs = 3;
#else
    frame.status = 0;
#endif
    user_return_handler(&frame);
    process::Thread thread(70, 0);
    process::CfsScheduler::set_current_task(&thread);
#if defined(MOSS_ARCH_ARM64)
    frame.status = 0;
    frame.x8 = static_cast<u64>(syscall::SyscallNumber::MAX_SYSCALL);
#elif defined(MOSS_ARCH_X64)
    frame.cs = 3;
    frame.rax = static_cast<u64>(syscall::SyscallNumber::MAX_SYSCALL);
#else
    frame.status = 0;
    frame.a7 = static_cast<u64>(syscall::SyscallNumber::MAX_SYSCALL);
#endif
    system_call_handler(&frame);
    user_return_handler(&frame);
    process::CfsScheduler::set_current_task(saved);
  }
  ut::expect(true);
}

void initramfs_edges() {
  using namespace initramfs;
  // Order 3 allocates 32 KiB, matching the former static fixture buffer.
  auto block = mm::allocate_pages(3);
  if (!ut::expect(static_cast<bool>(block))) {
    return;
  }
#if defined(MOSS_ARCH_X64)
  // Initramfs::init parses identity-mapped RAM on x64, so write through the
  // same address it will read instead of the high-half direct map.
  cpio_fixture::archive = reinterpret_cast<u8 *>(static_cast<VirtAddr>(*block));
#else
  cpio_fixture::archive = reinterpret_cast<u8 *>(phys_to_virt(*block));
#endif
  auto *buffer = cpio_fixture::archive;
  const usize size = cpio_fixture::build();
  const auto physical = *block;
  InitramfsArchive archive;
  ut::expect(archive.init(physical, size));
  ut::expect(archive.is_initialized() && archive.file_count() == 3);
  ut::expect(archive.bytes() == buffer && archive.size_bytes() == size);
  ut::expect(archive.lookup(nullptr) == nullptr);
  ut::expect(archive.lookup("a") != nullptr && archive.lookup("/a") != nullptr);
  ut::expect(archive.lookup("b") != nullptr && archive.lookup("c") != nullptr);
  ut::expect(archive.lookup("missing") == nullptr);
  u32 visited = 0;
  archive.for_each([&](const InitramfsEntry &entry) {
    ut::expect(entry.name != nullptr);
    ++visited;
  });
  ut::expect(visited == 3);

  auto expect_reject = [&](const char *reason) {
    InitramfsArchive invalid;
    ut::expect(!invalid.init(physical, size));
    ut::expect(!invalid.is_initialized() && invalid.bytes() == nullptr && invalid.size_bytes() == 0);
    (void)reason;
  };
  {
    InitramfsArchive invalid;
    ut::expect(!invalid.init(0, 0));
    ut::expect(!invalid.init(physical, sizeof(CpioNewcHeader) - 1));
  }

  cpio_fixture::build();
  buffer[0] = '9';
  expect_reject("bad magic");
  for (usize magic = 0; magic < 6; ++magic) {
    cpio_fixture::build();
    buffer[magic] = static_cast<u8>('9');
    expect_reject("bad magic byte");
  }
  cpio_fixture::build();
  buffer[6] = 'a';
  {
    InitramfsArchive lowercase;
    ut::expect(lowercase.init(physical, size));
  }
  cpio_fixture::build();
  buffer[6] = 'G';
  expect_reject("non-hex");
  cpio_fixture::build();
  auto *first = reinterpret_cast<CpioNewcHeader *>(buffer);
  cpio_fixture::hex_field(first->c_namesize, 0);
  expect_reject("zero namesize");
  cpio_fixture::build();
  cpio_fixture::hex_field(first->c_namesize, 0xFFFFFFFFU);
  expect_reject("huge namesize");
  cpio_fixture::build();
  buffer[sizeof(CpioNewcHeader)] = '.';
  buffer[sizeof(CpioNewcHeader) + 1] = 'x';
  expect_reject("unterminated name");
  cpio_fixture::build();
  buffer[sizeof(CpioNewcHeader)] = '\0';
  expect_reject("embedded nul");
  cpio_fixture::build();
  cpio_fixture::hex_field(first->c_filesize, 0xFFFFFFFFU);
  expect_reject("data overflow");
  cpio_fixture::build();
  cpio_fixture::hex_field(first->c_namesize, 0x7FFFFFFFU);
  expect_reject("name overflow");
  cpio_fixture::build();
  auto *trailer = reinterpret_cast<CpioNewcHeader *>(buffer + size - sizeof(CpioNewcHeader) - 12 - 4);
  cpio_fixture::hex_field(trailer->c_filesize, 4);
  expect_reject("trailer data");
  cpio_fixture::build();
  {
    InitramfsArchive truncated;
    ut::expect(!truncated.init(physical, size - sizeof(CpioNewcHeader) - 12));
  }
  {
    InitramfsArchive short_archive;
    ut::expect(!short_archive.init(physical, size - 1));
  }

  // Exceed the fixed lookup capacity while keeping each record valid.
  usize at = 0;
  for (u32 i = 0; i < MAX_INITRAMFS_FILES + 1; ++i) {
    char name[16]{};
    name[0] = 'f';
    u32 value = i;
    u32 digits = 0;
    char reversed[8]{};
    do {
      reversed[digits++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    for (u32 d = 0; d < digits; ++d) {
      name[1 + d] = reversed[digits - 1 - d];
    }
    at = cpio_fixture::append_entry(at, name, nullptr, 0);
  }
  at = cpio_fixture::append_entry(at, "TRAILER!!!", nullptr, 0);
  InitramfsArchive overflow;
  ut::expect(!overflow.init(physical, at));
  ut::expect(mm::free_pages(*block, 3).has_value());
}

void uart_formatting() {
  const bool valid = platform::hardware.uart.valid;
  platform::hardware.uart.valid = false;
  hal::uart::puts(nullptr);
  hal::uart::puts("line\nnext");
  hal::uart::putc('x');
  hal::uart::put_hex(0);
  hal::uart::put_hex(0xDEADBEEFCAFEBABEULL);
  hal::uart::put_hex_plain(0);
  hal::uart::put_hex_plain(0x0123456789ABCDEFULL);
  hal::uart::put_dec(0);
  hal::uart::put_dec(7);
  hal::uart::put_dec(10);
  hal::uart::put_dec(~u64{0});
  hal::uart::log("value=", 0x1234);
  hal::uart::enable_rx();
  ut::expect(hal::uart::getc() == -1);
  platform::hardware.uart.valid = valid;
  ut::expect(true);
}


} // namespace

void register_internals_cases() {
  ut::register_suite("internals", [] {
    ut::register_test("arch_primitives", arch_primitives);
    ut::register_test("containers_extended", containers_extended);
    ut::register_test("logging_formats", logging_formats);
    ut::register_test("logging_ring_and_chains", logging_ring_and_chains);
    ut::register_test("types_errors", types_errors);
    ut::register_test("memory_paths", memory_paths);
    ut::register_test("memory_manager_surfaces", memory_manager_surfaces);
    ut::register_test("uart_formatting", uart_formatting);
    ut::register_test("initramfs_edges", initramfs_edges);
    ut::register_test("kernel_elf_edges", kernel_elf_edges);
    ut::register_test("boot_edges", boot_edges);
    ut::register_test("kernel_entry_paths", kernel_entry_paths);
  });
}

} // namespace moss::test::validation
