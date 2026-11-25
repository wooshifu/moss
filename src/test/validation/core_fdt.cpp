import moss.std;
import moss.types;
import moss.arch;
import moss.fdt;
import moss.mm;
import moss.platform;
import moss.hal.uart;

#include "framework/ut_kernel.hpp"
#include "validation/core_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {
namespace {

constexpr usize kStructOffset = 0x100;
constexpr usize kStringsOffset = 0x2000;
constexpr usize kBlobSize = 0x4000;
constexpr usize kReserveOffset = 0x38;

constexpr u32 kFdtBeginNode = 1;
constexpr u32 kFdtEndNode = 2;
constexpr u32 kFdtProperty = 3;
constexpr u32 kFdtEnd = 9;

// The RISC-V parser normalizes CPU bring-up to SBI and installs the
// architectural supervisor timer interrupt regardless of the DTB hints.
platform::CpuEnableMethod expected_cpu_enable(platform::CpuEnableMethod parsed) noexcept {
#if defined(MOSS_ARCH_RISCV64)
  (void)parsed;
  return platform::CpuEnableMethod::Sbi;
#else
  return parsed;
#endif
}

u32 expected_timer_interrupt(u32 parsed) noexcept {
#if defined(MOSS_ARCH_RISCV64)
  (void)parsed;
  return 5;
#else
  return parsed;
#endif
}

void store_be32(u8 *out, u32 value) noexcept {
  out[0] = static_cast<u8>(value >> 24);
  out[1] = static_cast<u8>(value >> 16);
  out[2] = static_cast<u8>(value >> 8);
  out[3] = static_cast<u8>(value);
}

void store_be64(u8 *out, u64 value) noexcept {
  store_be32(out, static_cast<u32>(value >> 32));
  store_be32(out + 4, static_cast<u32>(value));
}

class DtbBuilder {
  alignas(8) u8 blob_[kBlobSize]{};
  usize struct_size_ = 0;
  usize strings_size_ = 0;
  usize reserve_entries_ = 0;

  void append_struct(const void *data, usize size) noexcept {
    if (struct_size_ + size > kStringsOffset - kStructOffset) {
      return;
    }
    const auto *bytes = static_cast<const u8 *>(data);
    for (usize i = 0; i < size; ++i) {
      blob_[kStructOffset + struct_size_ + i] = bytes[i];
    }
    struct_size_ += size;
  }

  void pad_struct() noexcept {
    while (struct_size_ % 4) {
      const u8 zero = 0;
      append_struct(&zero, 1);
    }
  }

  void token(u32 value) noexcept {
    u8 bytes[4];
    store_be32(bytes, value);
    append_struct(bytes, sizeof(bytes));
  }

  auto string_offset(const char *name) noexcept -> u32 {
    const auto offset = static_cast<u32>(strings_size_);
    for (const char *p = name; *p; ++p) {
      if (strings_size_ < kBlobSize - kStringsOffset) {
        blob_[kStringsOffset + strings_size_++] = static_cast<u8>(*p);
      }
    }
    if (strings_size_ < kBlobSize - kStringsOffset) {
      blob_[kStringsOffset + strings_size_++] = 0;
    }
    return offset;
  }

public:
  DtbBuilder &begin(const char *name) noexcept {
    token(kFdtBeginNode);
    usize length = 0;
    while (name[length]) {
      ++length;
    }
    append_struct(name, length + 1);
    pad_struct();
    return *this;
  }

  DtbBuilder &end() noexcept {
    token(kFdtEndNode);
    return *this;
  }

  DtbBuilder &property(const char *name, const void *value, usize length) noexcept {
    token(kFdtProperty);
    u8 header[8];
    store_be32(header, static_cast<u32>(length));
    store_be32(header + 4, string_offset(name));
    append_struct(header, sizeof(header));
    append_struct(value, length);
    pad_struct();
    return *this;
  }

  DtbBuilder &words(const char *name, const u32 *values, usize count) noexcept {
    token(kFdtProperty);
    u8 header[8];
    store_be32(header, static_cast<u32>(count * 4));
    store_be32(header + 4, string_offset(name));
    append_struct(header, sizeof(header));
    for (usize i = 0; i < count; ++i) {
      u8 bytes[4];
      store_be32(bytes, values[i]);
      append_struct(bytes, sizeof(bytes));
    }
    pad_struct();
    return *this;
  }

  DtbBuilder &u32_property(const char *name, u32 value) noexcept {
    u8 bytes[4];
    store_be32(bytes, value);
    return property(name, bytes, sizeof(bytes));
  }

  DtbBuilder &u64_property(const char *name, u64 value) noexcept {
    u8 bytes[8];
    store_be64(bytes, value);
    return property(name, bytes, sizeof(bytes));
  }

  DtbBuilder &string(const char *name, const char *value) noexcept {
    usize length = 0;
    while (value[length]) {
      ++length;
    }
    return property(name, value, length + 1);
  }

  DtbBuilder &raw_string(const char *name, const char *value, usize length) noexcept {
    return property(name, value, length);
  }

  DtbBuilder &reserve(u64 base, u64 size) noexcept {
    if (reserve_entries_ == 31) {
      return *this;
    }
    store_be64(blob_ + kReserveOffset + reserve_entries_ * 16, base);
    store_be64(blob_ + kReserveOffset + reserve_entries_ * 16 + 8, size);
    ++reserve_entries_;
    return *this;
  }

  void finish() noexcept {
    token(kFdtEnd);
    store_be32(blob_ + 0, 0xd00dfeed);
    store_be32(blob_ + 4, static_cast<u32>(kStringsOffset + strings_size_));
    store_be32(blob_ + 8, static_cast<u32>(kStructOffset));
    store_be32(blob_ + 12, static_cast<u32>(kStringsOffset));
    store_be32(blob_ + 16, static_cast<u32>(kReserveOffset));
    store_be32(blob_ + 20, 17);
    store_be32(blob_ + 24, 16);
    store_be32(blob_ + 28, 0);
    store_be32(blob_ + 32, static_cast<u32>(strings_size_));
    store_be32(blob_ + 36, static_cast<u32>(struct_size_));
  }

  [[nodiscard]] auto data() noexcept -> const void * { return blob_; }
  [[nodiscard]] auto bytes() noexcept -> u8 * { return blob_; }
};

class SavedPlatform {
  platform::PlatformInfo saved_ = platform::hardware;

public:
  SavedPlatform() = default;
  SavedPlatform(const SavedPlatform &) = delete;
  auto operator=(const SavedPlatform &) -> SavedPlatform & = delete;
  ~SavedPlatform() { restore(); }
  void restore() noexcept { platform::hardware = saved_; }
};

struct ParsedPlatform {
  bool ok;
  platform::PlatformInfo info;
};

auto parse_platform(DtbBuilder &dtb, SavedPlatform &saved) noexcept -> ParsedPlatform {
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  const bool ok = fdt::parse_dtb(dtb.data());
  const auto info = platform::hardware;
  saved.restore();
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  return {.ok = ok, .info = info};
}

bool parse_raw(const void *dtb, SavedPlatform &saved) noexcept {
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  const bool ok = fdt::parse_dtb(dtb);
  saved.restore();
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  return ok;
}

template <typename T, usize N> constexpr auto count_of(const T (&)[N]) noexcept -> usize { return N; }

void missing_nodes() {
  SavedPlatform saved;
  DtbBuilder dtb;
  dtb.begin("").end();
  dtb.finish();
  const auto parsed = parse_platform(dtb, saved);
  ut::expect(parsed.ok);
  ut::expect(parsed.info.dtb_valid);
  ut::expect(parsed.info.cpu_count == 0);
  ut::expect(parsed.info.memory_region_count == 0);
  ut::expect(!parsed.info.memory_map_valid);
  ut::expect(parsed.info.reserved_region_count == 1);
  ut::expect(!parsed.info.uart.valid);
  ut::expect(!parsed.info.intc.valid);

  ut::expect(!parse_raw(nullptr, saved));
  DtbBuilder corrupt;
  corrupt.begin("").end();
  corrupt.finish();
  corrupt.bytes()[0] = 0;
  ut::expect(!parse_raw(corrupt.data(), saved));
  ut::expect(!parse_raw(static_cast<const u8 *>(dtb.data()) + 1, saved));
}

void cpu_topology() {
  SavedPlatform saved;
  DtbBuilder dtb;
  dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
  dtb.begin("cpus")
      .u32_property("#address-cells", 1)
      .u32_property("#size-cells", 0)
      .u32_property("timebase-frequency", 24000000);
  dtb.begin("cpu@0")
      .string("device_type", "cpu")
      .u32_property("reg", 0)
      .string("enable-method", "psci")
      .string("mmu-type", "riscv64,sv39")
      .end();
  dtb.begin("cpu@1").string("device_type", "cpu").u32_property("reg", 1).string("enable-method", "spin-table");
  dtb.u64_property("cpu-release-addr", 0x123456789abcdef0ULL).end();
  dtb.begin("cpu@2").string("device_type", "cpu").u32_property("reg", 2).string("enable-method", "unknown").end();
  dtb.begin("cpu@3").string("device_type", "cpu").u32_property("reg", 3).string("status", "disabled").end();
  dtb.begin("not-a-cpu").string("device_type", "memory").u32_property("reg", 9).end();
  dtb.begin("empty-type").property("device_type", "", 0).u32_property("reg", 10).end();
  dtb.end();
  dtb.end();
  dtb.finish();
  const auto parsed = parse_platform(dtb, saved);
  ut::expect(parsed.ok);
  const auto &info = parsed.info;
  ut::expect(info.cpu_count == 3);
  ut::expect(info.timebase_frequency == 24000000);
  ut::expect(info.mmu_levels == 3);
  ut::expect(info.cpus[0].hardware_id == 0 && info.cpus[1].hardware_id == 1 && info.cpus[2].hardware_id == 2);
#if defined(MOSS_ARCH_RISCV64)
  // The RISC-V parser normalizes every CPU to SBI regardless of the DTB hint.
  ut::expect(info.cpus[0].enable_method == platform::CpuEnableMethod::Sbi);
  ut::expect(info.cpus[1].enable_method == platform::CpuEnableMethod::Sbi);
  ut::expect(info.cpus[2].enable_method == platform::CpuEnableMethod::Sbi);
#else
  ut::expect(info.cpus[0].enable_method == platform::CpuEnableMethod::Psci);
  ut::expect(info.cpus[1].enable_method == platform::CpuEnableMethod::SpinTable);
  ut::expect(info.cpus[1].release_address == 0x123456789abcdef0ULL);
  ut::expect(info.cpus[2].enable_method == platform::CpuEnableMethod::None);
#endif
}

void cpu_rejections() {
  SavedPlatform saved;
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    for (u32 i = 0; i < 17; ++i) {
      dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", i).end();
    }
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.cpu_count == 17);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.begin("cpu").string("device_type", "cpu").end();
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.cpu_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 3);
    dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", 0).end();
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.cpu_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1);
    dtb.begin("cpus").raw_string("#address-cells", "", 2);
    const u32 reg[] = {0, 0};
    dtb.begin("cpu").string("device_type", "cpu").words("reg", reg, count_of(reg)).end();
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.cpu_count == 0);
  }
}

void memory_layout() {
  SavedPlatform saved;
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 regs[] = {0, 0, 0, 0x100000, 0, 0x200000, 0, 0x100000};
    dtb.begin("memory@0").string("device_type", "memory").words("reg", regs, count_of(regs)).end();
    const u32 high[] = {0, 0x40000000, 0, 0x100000};
    dtb.begin("memory@40000000").string("device_type", "memory").words("reg", high, count_of(high)).end();
    const u32 disabled_reg[] = {0, 0, 0, 0x1000};
    dtb.begin("disabled")
        .string("device_type", "memory")
        .string("status", "disabled")
        .words("reg", disabled_reg, count_of(disabled_reg))
        .end();
    dtb.begin("other").string("device_type", "cpu").words("reg", disabled_reg, count_of(disabled_reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    const auto &info = parsed.info;
    ut::expect(info.memory_region_count == 3);
    ut::expect(info.total_memory_start == 0);
    ut::expect(info.total_memory_size == 0x300000);
    ut::expect(info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("")
        .u32_property("phandle", 1)
        .u32_property("#interrupt-cells", 1)
        .u32_property("#address-cells", 1)
        .u32_property("#size-cells", 1);
    const u32 regs[] = {0x1000, 0x2000, 0x2000, 0x2000};
    dtb.begin("memory").string("device_type", "memory").words("reg", regs, count_of(regs)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.memory_region_count == 0);
    ut::expect(!parsed.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 regs[] = {0xffffffff, 0, 0, 0x100000};
    dtb.begin("memory").string("device_type", "memory").words("reg", regs, count_of(regs)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.memory_region_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    u32 regs[18];
    for (u32 i = 0; i < 9; ++i) {
      regs[i * 2] = i * 0x2000;
      regs[i * 2 + 1] = 0x1000;
    }
    dtb.begin("memory").string("device_type", "memory").words("reg", regs, count_of(regs)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.memory_region_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 3).u32_property("#size-cells", 1);
    const u32 regs[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", regs, count_of(regs)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.memory_region_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 2);
    const u32 regs[] = {0, 0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", regs, count_of(regs)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.memory_region_count == 1);
  }
}

void chosen_uart() {
  SavedPlatform saved;
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1).u32_property("#interrupt-cells", 1);
    dtb.begin("chosen")
        .string("bootargs", "console=ttyAMA0")
        .string("stdout-path", "/serial0:115200n8")
        .u32_property("linux,initrd-start", 0x40000000)
        .u32_property("linux,initrd-end", 0x40100000)
        .end();
    const u32 serial_reg[] = {0x9000000, 0x1000};
    dtb.begin("serial0")
        .string("compatible", "arm,pl011")
        .words("reg", serial_reg, count_of(serial_reg))
        .u32_property("clock-frequency", 24000000)
        .end();
    const u32 memory_reg[] = {0, 0x100000};
    dtb.begin("memory").string("device_type", "memory").words("reg", memory_reg, count_of(memory_reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    const auto &info = parsed.info;
    ut::expect(info.bootargs != nullptr && info.bootargs[0] == 'c');
    ut::expect(info.stdout_path != nullptr && info.stdout_path[0] == '/');
    ut::expect(info.uart.valid && info.uart.kind == platform::UartKind::Pl011);
    ut::expect(info.uart.base_addr == 0x9000000 && info.uart.size == 0x1000 && info.uart.reg_width == 4);
    ut::expect(info.initrd_start == 0x40000000 && info.initrd_end == 0x40100000);
    ut::expect(info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("chosen")
        .u64_property("linux,initrd-start", 0x80000000ULL)
        .u64_property("linux,initrd-end", 0x81000000ULL);
    dtb.property("bootargs", "", 0).raw_string("stdout-path", "serial0", 7).end();
    const u32 serial_reg[] = {0x10000000, 0x100};
    dtb.begin("serial0")
        .string("compatible", "ns16550a")
        .words("reg", serial_reg, count_of(serial_reg))
        .u32_property("reg-shift", 2)
        .u32_property("reg-io-width", 4)
        .u32_property("clock-frequency", 1843200)
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    const auto &info = parsed.info;
    ut::expect(info.bootargs == nullptr);
    ut::expect(info.stdout_path == nullptr);
    ut::expect(info.initrd_start == 0x80000000 && info.initrd_end == 0x81000000);
    ut::expect(info.uart.valid && info.uart.kind == platform::UartKind::Ns16550);
    ut::expect(info.uart.reg_shift == 2 && info.uart.reg_width == 4 && info.uart.clock_freq == 1843200);
  }
}

void uart_rejections() {
  SavedPlatform saved;
  const auto root = [](DtbBuilder &dtb) {
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
  };
  {
    DtbBuilder dtb;
    root(dtb);
    const u32 reg[] = {0x1000, 0x44};
    dtb.begin("serial").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    root(dtb);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("serial")
        .string("compatible", "ns16550a")
        .words("reg", reg, count_of(reg))
        .u32_property("reg-shift", 4)
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    root(dtb);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("serial")
        .string("compatible", "ns16550")
        .words("reg", reg, count_of(reg))
        .u32_property("reg-io-width", 2)
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    root(dtb);
    dtb.begin("serial").string("compatible", "arm,pl011").end();
    const u32 reg[] = {0x1000, 0x1000};
    dtb.begin("disabled")
        .string("compatible", "arm,pl011")
        .string("status", "disabled")
        .words("reg", reg, count_of(reg))
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    root(dtb);
    dtb.begin("chosen").string("stdout-path", "missing").end();
    const u32 reg[] = {0x1000, 0x1000};
    dtb.begin("serial").string("compatible", "arm,pl011").string("status", "okay").words("reg", reg, count_of(reg));
    dtb.end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.uart.valid);
  }
}

void interrupt_controllers() {
  SavedPlatform saved;
  const auto add_cpu = [](DtbBuilder &dtb, u32 id, u32 phandle) {
    dtb.begin("cpu");
    dtb.string("device_type", "cpu").u32_property("reg", id);
    if (phandle) {
      dtb.begin("interrupt-controller").u32_property("#interrupt-cells", 1).u32_property("phandle", phandle).end();
    }
    dtb.end();
  };
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x8000000, 0x1000, 0x8001000, 0x1000};
    dtb.begin("intc").string("compatible", "arm,gic-400").u32_property("phandle", 1);
    dtb.words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    const auto &intc = parsed.info.intc;
    ut::expect(intc.valid && intc.gic_version == 2);
    ut::expect(intc.dist_base == 0x8000000 && intc.cpu_base == 0x8001000);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x8000000, 0x10000, 0x80a0000, 0x20000};
    dtb.begin("intc").string("compatible", "arm,gic-v3");
    dtb.words("reg", reg, count_of(reg))
        .u64_property("redistributor-stride", 0x20000)
        .u32_property("#redistributor-regions", 1);
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    const auto &intc = parsed.info.intc;
    ut::expect(intc.valid && intc.gic_version == 3 && intc.redist_base == 0x80a0000);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x8000000, 0x10000, 0x80a0000, 0x20000};
    dtb.begin("intc").string("compatible", "arm,gic-v3");
    dtb.words("reg", reg, count_of(reg))
        .u64_property("redistributor-stride", 0x30000)
        .u32_property("#redistributor-regions", 2);
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus");
    add_cpu(dtb, 0, 1);
    add_cpu(dtb, 1, 2);
    dtb.end();
    const u32 reg[] = {0xc000000, 0x1000000};
    dtb.begin("intc")
        .string("compatible", "riscv64,plic0")
        .u32_property("phandle", 3)
        .u32_property("#interrupt-cells", 1);
    const u32 extended[] = {1, 9, 2, 9};
    dtb.words("reg", reg, count_of(reg)).words("interrupts-extended", extended, count_of(extended)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    const auto &info = parsed.info;
    ut::expect(info.intc.valid && info.intc.gic_version == 0);
    ut::expect(info.plic_contexts[0] == 0 && info.plic_contexts[1] == 1);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus");
    add_cpu(dtb, 0, 1);
    dtb.end();
    const u32 reg[] = {0xc000000, 0x1000000};
    dtb.begin("intc")
        .string("compatible", "sifive,plic-1.0.0")
        .u32_property("#interrupt-cells", 1)
        .words("reg", reg, count_of(reg))
        .u32_property("interrupts-extended", 1)
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("")
        .u32_property("phandle", 1)
        .u32_property("#interrupt-cells", 1)
        .u32_property("#address-cells", 1)
        .u32_property("#size-cells", 1);
    dtb.begin("cpus");
    add_cpu(dtb, 0, 0);
    dtb.end();
    const u32 local_reg[] = {0x40000000, 0x100};
    const u32 armctrl_reg[] = {0x4000b000, 0x28};
    dtb.begin("local").string("compatible", "brcm,bcm2836-l1-intc").words("reg", local_reg, count_of(local_reg)).end();
    dtb.begin("armctrl")
        .string("compatible", "brcm,bcm2836-armctrl-ic")
        .u32_property("interrupt-parent", 1)
        .u32_property("#interrupt-cells", 1)
        .u32_property("interrupts", 24)
        .words("reg", armctrl_reg, count_of(armctrl_reg))
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    const auto &intc = parsed.info.intc;
    ut::expect(intc.valid && intc.gic_version == 1 && intc.cpu_base == 0x4000b000);
  }
}

void reserved_memory() {
  SavedPlatform saved;
  {
    DtbBuilder dtb;
    dtb.reserve(0x30000000, 0x1000);
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 memory_reg[] = {0, 0, 0, 0x10000000};
    dtb.begin("memory").string("device_type", "memory").words("reg", memory_reg, count_of(memory_reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 fixed_reg[] = {0x10000000, 0, 0, 0x1000};
    dtb.begin("fixed").words("reg", fixed_reg, count_of(fixed_reg)).end();
    const u32 pool_size[] = {0, 0x1000};
    dtb.begin("pool").string("compatible", "shared-dma-pool").words("size", pool_size, count_of(pool_size));
    dtb.property("reusable", "", 0).end();
    dtb.begin("disabled").string("status", "disabled").end();
    dtb.end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(parsed.info.memory_map_valid);
    ut::expect(parsed.info.reserved_region_count == 3);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 memory_reg[] = {0, 0, 0, 0x10000000};
    dtb.begin("memory").string("device_type", "memory").words("reg", memory_reg, count_of(memory_reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 pool_size[] = {0, 0x1000};
    dtb.begin("pool").string("compatible", "shared-dma-pool").words("size", pool_size, count_of(pool_size));
    dtb.property("reusable", "", 0).property("no-map", "", 0).end();
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 memory_reg[] = {0, 0, 0, 0x10000000};
    dtb.begin("memory").string("device_type", "memory").words("reg", memory_reg, count_of(memory_reg)).end();
    dtb.begin("reserved-memory");
    dtb.begin("bad").u32_property("reg", 0).end();
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 memory_reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", memory_reg, count_of(memory_reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 3).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 memory_reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", memory_reg, count_of(memory_reg)).end();
    dtb.begin("chosen").u32_property("linux,initrd-start", 0x1000).u32_property("linux,initrd-end", 0x800).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
    ut::expect(!parsed.info.memory_map_valid);
  }
}

void interrupt_sources_and_firmware() {
  SavedPlatform saved;
  DtbBuilder dtb;
  dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
  const u32 intc_reg[] = {0x8000000, 0x1000, 0x8001000, 0x1000};
  dtb.begin("intc")
      .string("compatible", "arm,gic-400")
      .u32_property("phandle", 5)
      .u32_property("#interrupt-cells", 3)
      .words("reg", intc_reg, count_of(intc_reg))
      .end();
  const u32 irqs[] = {1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4};
  dtb.begin("timer")
      .string("compatible", "arm,armv8-timer")
      .u32_property("interrupt-parent", 5)
      .words("interrupts", irqs, count_of(irqs))
      .end();
  dtb.begin("psci").string("compatible", "arm,psci-1.0").string("method", "hvc").end();
  const u32 memory_reg[] = {0, 0x1000};
  dtb.begin("memory").string("device_type", "memory").words("reg", memory_reg, count_of(memory_reg)).end();
  dtb.end().finish();
  const auto parsed = parse_platform(dtb, saved);
  ut::expect(parsed.ok);
#if defined(MOSS_ARCH_RISCV64)
  // RISC-V uses the architectural supervisor timer interrupt, not a GIC PPI.
  ut::expect(parsed.info.timer_interrupt == 5);
#else
  ut::expect(parsed.info.timer_interrupt == 27);
#endif
  ut::expect(parsed.info.psci_valid && !parsed.info.psci_smc);

  DtbBuilder smc;
  smc.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
  smc.begin("broken").string("compatible", "arm,psci-0.2").raw_string("method", "abc", 3).end();
  smc.begin("psci").string("compatible", "arm,psci-0.2").string("method", "smc").end();
  smc.begin("timer").string("compatible", "arm,armv7-timer").string("status", "ok").u32_property("interrupts", 1);
  smc.u32_property("interrupts", 30).u32_property("interrupts", 1).end();
  const u32 smc_memory_reg[] = {0, 0x1000};
  smc.begin("memory").string("device_type", "memory").words("reg", smc_memory_reg, count_of(smc_memory_reg)).end();
  smc.end().finish();
  const auto smc_parsed = parse_platform(smc, saved);
  ut::expect(smc_parsed.ok);
  ut::expect(smc_parsed.info.psci_valid && smc_parsed.info.psci_smc);
}

void order_cpu_topology() {
  SavedPlatform saved;
  auto &info = platform::hardware;
  info.cpu_count = 3;
  info.cpus[0] = {.hardware_id = 7, .release_address = 0, .enable_method = platform::CpuEnableMethod::Psci};
  info.cpus[1] = {.hardware_id = 3, .release_address = 0, .enable_method = platform::CpuEnableMethod::Psci};
  info.cpus[2] = {.hardware_id = 9, .release_address = 0, .enable_method = platform::CpuEnableMethod::Psci};
  info.plic_contexts[0] = 4;
  info.plic_contexts[1] = 5;
  info.plic_contexts[2] = 6;
  ut::expect(platform::order_cpus(3));
  ut::expect(info.cpus[0].hardware_id == 3 && info.cpus[1].hardware_id == 7);
  ut::expect(info.plic_contexts[0] == 5 && info.plic_contexts[1] == 4);
  ut::expect(platform::logical_cpu(9) == 2);
  ut::expect(platform::logical_cpu(42) == 16);
  info.cpus[2].hardware_id = 7;
  ut::expect(!platform::order_cpus(3));
  info.cpu_count = 0;
  ut::expect(!platform::order_cpus(0));
  ut::expect(platform::logical_cpu(0) == 0);
  info.cpu_count = 17;
  ut::expect(!platform::order_cpus(0));
}

void matrix_edges() {
  SavedPlatform saved;
  const auto parsed = [&](DtbBuilder &dtb) { return parse_platform(dtb, saved); };

  // read_reg: valid translation through a bus ranges window.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 ranges[] = {0x100, 0x1000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.words("ranges", ranges, count_of(ranges));
    const u32 reg[] = {0x100, 0x100};
    dtb.begin("uart")
        .string("compatible", "arm,pl011")
        .words("reg", reg, count_of(reg))
        .end()
        .end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok);
    ut::expect(info.info.uart.valid);
    ut::expect(info.info.uart.base_addr == 0x1000);
    ut::expect(info.info.uart.size == 0x100);
  }
  // Empty ranges are an identity mapping; absent ranges are not.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 empty[] = {0};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.property("ranges", empty, 0);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.uart.valid && info.info.uart.base_addr == 0x1000);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  // Ranges that do not cover the child address, malformed stride, and
  // overflowing translation must all leave the device undiscovered.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 ranges[] = {0x10000, 0x40000000, 0x10};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.words("ranges", ranges, count_of(ranges));
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 ranges[] = {0x1000, 0x40000000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.property("ranges", ranges, 7);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 ranges[] = {0x0, 0xffffffff, 0xffffffff};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.words("ranges", ranges, count_of(ranges));
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  // Invalid cell counts and reg shapes.
  const u32 address_values[] = {0U, 3U};
  for (u32 addresses : address_values) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", addresses).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  const u32 size_values[] = {0U, 3U};
  for (u32 sizes : size_values) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", sizes);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100, 0x2000};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  // Interrupt specifier layouts and controller identities.
  const auto interrupt_case = [&](u32 cells, const char *compatible, const u32 *values, usize count, u32 expected) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc")
        .string("compatible", compatible)
        .u32_property("phandle", 1)
        .u32_property("#interrupt-cells", cells);
    if (compatible[0] == 'b' && compatible[5] == 'b') {
      const u32 reg[] = {0x40000000, 0x1000};
      dtb.words("reg", reg, count_of(reg));
    }
    dtb.end();
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart")
        .string("compatible", "arm,pl011")
        .u32_property("interrupt-parent", 1)
        .words("interrupts", values, count)
        .words("reg", reg, count_of(reg))
        .end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && static_cast<u32>(info.info.uart.irq) == expected);
  };
  const u32 gic_spi[] = {0, 5, 4};
  interrupt_case(3, "arm,gic-400", gic_spi, 3, 37);
  const u32 gic_ppi[] = {1, 5, 4};
  interrupt_case(3, "arm,gic-400", gic_ppi, 3, 21);
  const u32 gic_bad[] = {2, 5, 4};
  interrupt_case(3, "arm,gic-400", gic_bad, 3, 0);
  const u32 plic[] = {7};
  interrupt_case(1, "sifive,plic-1.0.0", plic, 1, 7);
  const u32 bcm_l1[] = {4, 0};
  interrupt_case(2, "brcm,bcm2836-l1-intc", bcm_l1, 2, 20);
  const u32 bcm_l1_bad[] = {12, 0};
  interrupt_case(2, "brcm,bcm2836-l1-intc", bcm_l1_bad, 2, 0);
  const u32 armctrl_basic[] = {0, 3};
  interrupt_case(2, "brcm,bcm2836-armctrl-ic", armctrl_basic, 2, 99);
  const u32 armctrl_bank[] = {1, 5};
  interrupt_case(2, "brcm,bcm2836-armctrl-ic", armctrl_bank, 2, 37);
  const u32 armctrl_second[] = {2, 5};
  interrupt_case(2, "brcm,bcm2836-armctrl-ic", armctrl_second, 2, 69);
  const u32 armctrl_bad[] = {3, 5};
  interrupt_case(2, "brcm,bcm2836-armctrl-ic", armctrl_bad, 2, 0);
  const u32 unknown_cells[] = {0, 1, 2, 3};
  interrupt_case(4, "arm,gic-400", unknown_cells, 4, 0);
  const u32 short_interrupts[] = {0};
  interrupt_case(3, "arm,gic-400", short_interrupts, 1, 0);

  // CPU topology: disabled, spin-table release address, psci, malformed reg,
  // oversized reg, and more than sixteen CPUs.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.u32_property("timebase-frequency", 0);
    dtb.begin("cpu0").string("device_type", "cpu").u32_property("reg", 0).string("status", "disabled").end();
    dtb.begin("cpu1").string("device_type", "cpu").u32_property("reg", 1).string("enable-method", "psci").end();
    dtb.begin("cpu2")
        .string("device_type", "cpu")
        .u32_property("reg", 2)
        .string("enable-method", "spin-table")
        .u64_property("cpu-release-addr", 0x80000000)
        .end();
    dtb.begin("cpu3").string("device_type", "other").u32_property("reg", 3).end();
    dtb.begin("cpu4").string("device_type", "cpu").end();
    dtb.begin("cpu5").string("device_type", "cpu").words("reg", plic, 2).end();
    dtb.end().end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    for (u32 i = 0; i < 17; ++i) {
      dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", i).end();
    }
    dtb.end().end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.cpu_count == 17);
  }

  // Memory layout: valid, overlapping, oversized, zero, and malformed reg.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 reg[] = {0, 0x40000000, 0, 0x200000, 0, 0x50000000, 0, 0x100000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.memory_region_count == 2);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x100, 0x100, 0x180, 0x100};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.memory_region_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x0};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.memory_region_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0xfffff000, 0x2000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.memory_region_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 0).u32_property("#size-cells", 0);
    const u32 reg[] = {0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.memory_region_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("memory").string("device_type", "memory").string("status", "disabled").end();
    dtb.begin("not-memory").string("device_type", "cpu").string("status", "okay").end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok);
  }

  // UART variants and rejection of impossible register geometry.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("serial")
        .string("compatible", "ns16550a")
        .words("reg", reg, count_of(reg))
        .u32_property("reg-io-width", 4)
        .u32_property("reg-shift", 2)
        .u32_property("clock-frequency", 48000000)
        .u32_property("interrupt-parent", 0)
        .end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.uart.valid && info.info.uart.kind == platform::UartKind::Ns16550);
  }
  const u32 shift_values[] = {4U, 8U};
  for (u32 shift : shift_values) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("serial").string("compatible", "ns16550a").words("reg", reg, count_of(reg)).u32_property("reg-shift", shift).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  const u32 width_values[] = {2U, 8U};
  for (u32 width : width_values) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("serial").string("compatible", "ns16550a").words("reg", reg, count_of(reg)).u32_property("reg-io-width", width).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x40};
    dtb.begin("serial").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("serial").string("compatible", "unknown,uart").words("reg", reg, count_of(reg)).end();
    dtb.begin("serial2").string("compatible", "arm,pl011").string("status", "disabled").end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }

  // Chosen properties: 32-bit initrd fields, stdout-path alias syntax, and
  // malformed strings.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("chosen")
        .string("bootargs", "console=ttyAMA0")
        .string("stdout-path", "/soc/serial:115200")
        .u32_property("linux,initrd-start", 0x40000000)
        .u32_property("linux,initrd-end", 0x40100000)
        .end();
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 serial_ranges[] = {0x1000, 0x1000, 0x100};
    dtb.words("ranges", serial_ranges, count_of(serial_ranges));
    dtb.begin("serial").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end();
    dtb.end().end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok);
    ut::expect(info.info.bootargs != nullptr && info.info.stdout_path != nullptr);
    ut::expect(info.info.initrd_start == 0x40000000 && info.info.initrd_end == 0x40100000);
    ut::expect(info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("");
    dtb.begin("chosen").raw_string("bootargs", "x", 1).raw_string("stdout-path", "y", 1).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.bootargs == nullptr && info.info.stdout_path == nullptr);
  }
  {
    DtbBuilder dtb;
    dtb.begin("");
    dtb.begin("chosen")
        .u64_property("linux,initrd-start", 0x140000000ULL)
        .u64_property("linux,initrd-end", 0x140100000ULL)
        .end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok && info.info.initrd_start == 0x140000000ULL && info.info.initrd_end == 0x140100000ULL);
  }
  {
    DtbBuilder dtb;
    dtb.begin("");
    dtb.begin("chosen").u32_property("linux,initrd-start", 0x10).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok);
  }

  // Status spellings accepted and rejected by every enabled() caller.
  const char *status_values[] = {"ok", "okay", "disabled", "fail", "okay-extra"};
  for (const char *status : status_values) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("serial").string("compatible", "arm,pl011").string("status", status).words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed(dtb);
    ut::expect(info.ok);
    const bool expected = __builtin_strcmp(status, "ok") == 0 || __builtin_strcmp(status, "okay") == 0;
    ut::expect(info.info.uart.valid == expected);
  }
}

void intc_edges() {
  SavedPlatform saved;
  const auto cpu = [](DtbBuilder &dtb, u32 id, const char *mmu = nullptr) {
    dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", id);
    if (mmu) {
      dtb.string("mmu-type", mmu);
    }
    dtb.end();
  };

  // GICv3 without a redistributor window.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x8000000, 0x10000};
    dtb.begin("intc").string("compatible", "arm,gic-v3").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.intc.valid);
  }
  // BCM L1 rejects a core outside its four slots.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    cpu(dtb, 0);
    cpu(dtb, 4);
    dtb.end();
    const u32 reg[] = {0x40000000, 0x100};
    dtb.begin("local").string("compatible", "brcm,bcm2836-l1-intc").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.intc.valid);
  }
  // BCM L1 requires a readable ARMCTRL window.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    cpu(dtb, 0);
    dtb.end();
    const u32 reg[] = {0x40000000, 0x100};
    dtb.begin("local").string("compatible", "brcm,bcm2836-l1-intc").words("reg", reg, count_of(reg)).end();
    dtb.begin("armctrl")
        .string("compatible", "brcm,bcm2836-armctrl-ic")
        .u32_property("interrupt-parent", 1)
        .u32_property("#interrupt-cells", 1)
        .u32_property("interrupts", 24)
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.intc.valid);
  }
  // GICv2 without a CPU interface window.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x8000000, 0x10000};
    dtb.begin("intc").string("compatible", "arm,gic-400").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.intc.valid);
  }
  // PLIC context whose controller lacks #interrupt-cells.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    cpu(dtb, 0);
    dtb.end();
    const u32 reg[] = {0xc000000, 0x400000};
    dtb.begin("plic").string("compatible", "riscv64,plic0").u32_property("phandle", 1).words("reg", reg, count_of(reg)).end();
    const u32 extended[] = {1, 9};
    dtb.begin("plic2").string("compatible", "riscv64,plic0").u32_property("phandle", 2).words("reg", reg, count_of(reg)).words("interrupts-extended", extended, count_of(extended)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.intc.valid);
  }
  // PLIC context for a non-external interrupt does not populate a context.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    cpu(dtb, 0);
    dtb.end();
    const u32 reg[] = {0xc000000, 0x400000};
    const u32 extended[] = {1, 8};
    dtb.begin("plic")
        .string("compatible", "riscv64,plic0")
        .u32_property("phandle", 1)
        .u32_property("#interrupt-cells", 1)
        .words("reg", reg, count_of(reg))
        .words("interrupts-extended", extended, count_of(extended))
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.intc.valid);
  }
  // PLIC context whose CPU reg is malformed.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.begin("cpu").string("device_type", "cpu").end();
    dtb.end();
    const u32 reg[] = {0xc000000, 0x400000};
    const u32 extended[] = {1, 9};
    dtb.begin("plic")
        .string("compatible", "riscv64,plic0")
        .u32_property("phandle", 1)
        .u32_property("#interrupt-cells", 1)
        .words("reg", reg, count_of(reg))
        .words("interrupts-extended", extended, count_of(extended))
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.intc.valid);
  }
  // PLIC window too small for the context page range.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    cpu(dtb, 0);
    dtb.end();
    const u32 reg[] = {0xc000000, 0x1fffff};
    const u32 extended[] = {1, 9};
    dtb.begin("plic")
        .string("compatible", "riscv64,plic0")
        .u32_property("phandle", 1)
        .u32_property("#interrupt-cells", 1)
        .words("reg", reg, count_of(reg))
        .words("interrupts-extended", extended, count_of(extended))
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.intc.valid);
  }
  // ARMCTRL basic IRQ out of range and unsupported bank index.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    cpu(dtb, 0);
    dtb.end();
    const u32 local_reg[] = {0x40000000, 0x100};
    const u32 reg[] = {0x4000b000, 0x28};
    dtb.begin("local").string("compatible", "brcm,bcm2836-l1-intc").u32_property("phandle", 1).words("reg", local_reg, count_of(local_reg)).end();
    dtb.begin("armctrl")
        .string("compatible", "brcm,bcm2836-armctrl-ic")
        .u32_property("interrupt-parent", 1)
        .u32_property("#interrupt-cells", 1)
        .words("reg", reg, count_of(reg))
        .u32_property("interrupts", 24)
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    cpu(dtb, 0, "riscv64,sv48");
    cpu(dtb, 1, "riscv64,sv57");
    cpu(dtb, 2, "riscv64,sv99");
    dtb.end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && parsed.info.mmu_levels == 4);
  }
  // Reserved memory size zero is ignored; overflow invalidates the map.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.reserve(0x1000, 0);
    dtb.reserve(0xfffffffffffff000ULL, 0x2000);
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.memory_map_valid);
  }
}

void parser_edges() {
  SavedPlatform saved;
  const auto parsed_dtb = [&](DtbBuilder &dtb) { return parse_platform(dtb, saved); };
  {
    DtbBuilder dtb;
    dtb.begin("").string("compatible", "arm,pl011").u32_property("reg", 0x1000);
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("chosen").string("stdout-path", "/not-a-uart").end();
    dtb.begin("not-a-uart").string("compatible", "vendor,other").end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", 0).string("mmu-type", "riscv64,sv57").end();
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && parsed.info.mmu_levels == 5);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    const u32 reg[] = {0, 1};
    dtb.begin("cpu").string("device_type", "cpu").words("reg", reg, count_of(reg)).end();
    dtb.end().end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && parsed.info.cpu_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).raw_string("#size-cells", "", 2);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u32 reg[] = {1, 0, 1, 0};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u64 ranges[] = {0, 0xffffffffffffffffULL, 0x10000};
    dtb.begin("soc").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    dtb.property("ranges", ranges, sizeof(ranges));
    const u64 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").property("reg", reg, sizeof(reg)).end().end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && !parsed.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc")
        .string("compatible", "arm,gic-400")
        .u32_property("phandle", 1)
        .u32_property("#interrupt-cells", 2);
    const u32 intc_reg[] = {0x8000000, 0x1000};
    dtb.words("reg", intc_reg, count_of(intc_reg)).end();
    const u32 irq[] = {0, 5};
    const u32 uart_reg[] = {0x1000, 0x100};
    dtb.begin("uart")
        .string("compatible", "arm,pl011")
        .u32_property("interrupt-parent", 1)
        .words("interrupts", irq, count_of(irq))
        .words("reg", uart_reg, count_of(uart_reg))
        .end();
    dtb.end().finish();
    const auto parsed = parse_platform(dtb, saved);
    ut::expect(parsed.ok && parsed.info.uart.valid && parsed.info.uart.irq == 0);
  }

  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").property("compatible", "", 0).words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart")
        .string("compatible", "arm,pl011")
        .raw_string("status", "ok", 2)
        .words("reg", reg, count_of(reg))
        .end()
        .end()
        .finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").raw_string("timebase-frequency", "abc", 3).end();
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.timebase_frequency != 0x616263);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("memory").string("device_type", "memory").end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 regs[] = {0x200000, 0x1000, 0x100000, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", regs, count_of(regs)).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.memory_region_count == 2 && info.info.total_memory_start == 0x100000);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 3).u32_property("#size-cells", 1);
    const u32 ranges[] = {0, 0, 0, 0, 0x100};
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.words("ranges", ranges, count_of(ranges));
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 3);
    const u32 ranges[] = {0, 0, 0, 0x100};
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.words("ranges", ranges, count_of(ranges));
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    const u64 ranges[] = {0, 0xfffffffffffffffeULL, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    dtb.property("ranges", ranges, sizeof(ranges));
    const u64 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").property("reg", reg, sizeof(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.uart.irq == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc").string("compatible", "arm,gic-400").u32_property("phandle", 1).u32_property("#interrupt-cells", 3);
    const u32 intc_reg[] = {0x8000000, 0x1000};
    dtb.words("reg", intc_reg, count_of(intc_reg)).end();
    const u32 irq[] = {2, 5, 4};
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").u32_property("interrupt-parent", 1).words("interrupts", irq, count_of(irq)).words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.uart.irq == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc").string("compatible", "vendor,intc").u32_property("phandle", 1).u32_property("#interrupt-cells", 2).end();
    const u32 irq[] = {1, 5};
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").u32_property("interrupt-parent", 1).words("interrupts", irq, count_of(irq)).words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.uart.irq == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc").string("compatible", "brcm,bcm2836-armctrl-ic").u32_property("phandle", 1).u32_property("#interrupt-cells", 2).end();
    const u32 irq[] = {0, 8};
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").u32_property("interrupt-parent", 1).words("interrupts", irq, count_of(irq)).words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.uart.irq == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", 0).raw_string("enable-method", "psci", 4).raw_string("mmu-type", "riscv64,sv", 10).end();
    dtb.end();
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.cpu_count == 1 &&
               info.info.cpus[0].enable_method == expected_cpu_enable(platform::CpuEnableMethod::None));
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 2);
    dtb.begin("intc").string("compatible", "arm,gic-v3").u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    const u32 regs[] = {0x8000000, 0x10000, 0x8010000, 0x20000};
    dtb.words("reg", regs, count_of(regs)).u32_property("#redistributor-regions", 2).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 2);
    dtb.begin("intc").string("compatible", "arm,gic-v3").u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    const u32 regs[] = {0x8000000, 0x10000, 0x8010000, 0x20000};
    dtb.words("reg", regs, count_of(regs)).property("redistributor-stride", "abc", 3).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 2);
    dtb.begin("intc").string("compatible", "arm,gic-v3").u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    const u32 regs[] = {0x8000000, 0x10000, 0x8010000, 0x20000};
    dtb.words("reg", regs, count_of(regs)).u64_property("redistributor-stride", 0x10000).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1).end();
    const u32 regs[] = {0x40000000, 0x100};
    dtb.begin("local").string("compatible", "brcm,bcm2836-l1-intc").words("reg", regs, count_of(regs)).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", 0).end().end();
    const u32 regs[] = {0x40000000, 0x80};
    dtb.begin("local").string("compatible", "brcm,bcm2836-l1-intc").words("reg", regs, count_of(regs)).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", 0).end().end();
    const u32 regs[] = {0xc000000, 0x400000};
    const u32 extended[] = {1, 9};
    dtb.begin("plic").string("compatible", "riscv64,plic0").u32_property("phandle", 1).u32_property("#interrupt-cells", 1).words("reg", regs, count_of(regs)).property("interrupts-extended", extended, 4).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 3).u32_property("#size-cells", 1);
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 3).u32_property("#size-cells", 1);
    dtb.begin("pool").string("compatible", "shared-dma-pool").u32_property("size", 0x1000).property("reusable", "", 0).end();
    dtb.end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("pool").string("compatible", "shared-dma-pool").u32_property("size", 0x1000).property("reusable", "", 0).end();
    dtb.end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("pool").string("compatible", "shared-dma-pool").u32_property("size", 0x1000).property("reusable", "", 0).property("no-map", "", 0).end();
    dtb.end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("broken").property("reg", "", 0).end();
    dtb.end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.memory_map_valid);
  }
  // A status with the exact length but no terminator/suffix must not enable a
  // device; both the accepted and every rejected representation are exercised.
  const char *statuses[] = {"okay", "ok", "off", "okayx", "disable"};
  for (const char *status : statuses) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    const usize length = __builtin_strlen(status) + 1;
    dtb.begin("uart").string("compatible", "arm,pl011").raw_string("status", status, length).words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    const bool enabled = length == 3 || length == 5;
    ut::expect(info.ok && info.info.uart.valid == enabled);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").property("status", "", 0).words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  // NS16550 register window validation.
  const u32 shifts[] = {0U, 3U, 4U};
  for (u32 shift : shifts) {
    const u32 widths[] = {1U, 2U, 4U};
    for (u32 width : widths) {
      DtbBuilder dtb;
      dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
      const u32 reg[] = {0x1000, 0x100};
      dtb.begin("uart").string("compatible", "ns16550a").u32_property("reg-shift", shift).u32_property("reg-io-width", width).words("reg", reg, count_of(reg)).end().end().finish();
      const auto info = parsed_dtb(dtb);
      ut::expect(info.ok && info.info.uart.valid == (shift <= 3 && (width == 1 || width == 4)));
    }
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 4};
    dtb.begin("uart").string("compatible", "ns16550a").words("reg", reg, count_of(reg)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  // CPU enable methods, release addresses and MMU levels.
  const char *methods[] = {"psci", "spin-table", "other"};
  for (const char *method : methods) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    auto &cpu = dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", 0).string("enable-method", method);
    if (method[0] == 's') {
      cpu.u64_property("cpu-release-addr", 0x80000);
    }
    cpu.end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    const auto expected = method[0] == 'p' ? platform::CpuEnableMethod::Psci
                          : method[0] == 's' ? platform::CpuEnableMethod::SpinTable
                                             : platform::CpuEnableMethod::None;
    ut::expect(info.ok && info.info.cpu_count == 1 &&
               info.info.cpus[0].enable_method == expected_cpu_enable(expected));
    if (expected == platform::CpuEnableMethod::SpinTable) {
      ut::expect(info.info.cpus[0].release_address == 0x80000);
    }
  }
  const char *mmus[] = {"riscv64,sv39", "riscv64,sv48", "riscv64,sv57", "riscv64,sv"};
  for (const char *mmu : mmus) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.begin("cpu").string("device_type", "cpu").u32_property("reg", 0).string("mmu-type", mmu).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    const u32 levels = mmu[10] == '3' ? 3 : mmu[10] == '4' ? 4 : mmu[10] == '5' ? 5 : 0;
    ut::expect(info.ok && info.info.mmu_levels == levels);
  }
  // PSCI method decoding.
  const char *psci_methods[] = {"smc", "hvc", "other"};
  for (const char *method : psci_methods) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("psci").string("compatible", "arm,psci-1.0").string("method", method).end().end().finish();
    const auto info = parsed_dtb(dtb);
    const bool expected = method[0] == 's' || method[0] == 'h';
    ut::expect(info.ok && info.info.psci_valid == expected && info.info.psci_smc == (method[0] == 's'));
  }
  // Ranges translation: identity, missing, malformed and non-containing.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1).property("ranges", "", 0);
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 3).u32_property("#size-cells", 1);
    const u32 ranges[] = {0, 0, 0, 0, 0x100};
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1).words("ranges", ranges, count_of(ranges));
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 ranges[] = {0x2000, 0x10000, 0x100};
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 1).u32_property("#size-cells", 1).words("ranges", ranges, count_of(ranges));
    dtb.begin("uart").string("compatible", "arm,pl011").words("reg", reg, count_of(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 1);
    const u64 ranges[] = {0, 0xffffffffffffff00ULL, 0x200};
    const u64 reg[] = {0, 0x100};
    dtb.begin("soc").u32_property("#address-cells", 2).u32_property("#size-cells", 1);
    dtb.property("ranges", ranges, sizeof(ranges));
    dtb.begin("uart").string("compatible", "arm,pl011").property("reg", reg, sizeof(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.uart.valid);
  }
  // GICv3 redistributor validation.
  const u32 region_counts[] = {1U, 2U};
  for (u32 regions : region_counts) {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 2);
    dtb.begin("intc").string("compatible", "arm,gic-v3").u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    const u32 regs[] = {0x8000000, 0x10000, 0x8010000, 0x10000};
    dtb.words("reg", regs, count_of(regs)).u32_property("#redistributor-regions", regions).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 2);
    dtb.begin("intc").string("compatible", "arm,gic-v3").u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    const u32 regs[] = {0x8000000, 0x10000, 0x8010000, 0x20000};
    dtb.words("reg", regs, count_of(regs)).u64_property("redistributor-stride", 0x10000).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc").string("compatible", "arm,gic-v3").u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    const u32 regs[] = {0x8000000, 0x10000, 0x8010000, 0x20000};
    dtb.words("reg", regs, count_of(regs)).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.intc.valid);
  }
  // Invalid #interrupt-cells and truncated interrupts properties.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc").string("compatible", "arm,gic-400").u32_property("phandle", 1).raw_string("#interrupt-cells", "x", 4);
    const u32 intc_reg[] = {0x8000000, 0x1000};
    dtb.words("reg", intc_reg, count_of(intc_reg)).end();
    const u32 irq[] = {0, 5};
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").u32_property("interrupt-parent", 1).words("interrupts", irq, count_of(irq)).words("reg", reg, count_of(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.uart.valid && info.info.uart.irq == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc").string("compatible", "arm,gic-400").u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    const u32 intc_reg[] = {0x8000000, 0x1000};
    dtb.words("reg", intc_reg, count_of(intc_reg)).end();
    const u32 reg[] = {0x1000, 0x100};
    dtb.begin("uart").string("compatible", "arm,pl011").u32_property("interrupt-parent", 1).property("interrupts", "", 1).words("reg", reg, count_of(reg)).end().end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.uart.valid && info.info.uart.irq == 0);
  }
  // Chosen initrd width and malformed ordering.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("chosen").u32_property("linux,initrd-start", 0x1000).u32_property("linux,initrd-end", 0x2000).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.initrd_start == 0x1000 && info.info.initrd_end == 0x2000);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 2).u32_property("#size-cells", 2);
    dtb.begin("chosen").u64_property("linux,initrd-start", 0x100000000ULL).u64_property("linux,initrd-end", 0x100002000ULL).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.initrd_start == 0x100000000ULL && info.info.initrd_end == 0x100002000ULL);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("chosen").u32_property("linux,initrd-start", 0x2000).u32_property("linux,initrd-end", 0x1000).end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.memory_map_valid);
  }
  // Reservation-only shared DMA pool and oversized reservation table.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("pool").string("compatible", "shared-dma-pool").u32_property("size", 0x1000).property("reusable", "", 0).end();
    dtb.end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("reserved-memory").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("pool").string("compatible", "shared-dma-pool").raw_string("size", "x", 4).property("reusable", "", 0).end();
    dtb.end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.memory_map_valid);
  }
  // Exercise the enabled()/read_reg() copies in every parser consumer, not just
  // the UART path; each inlined copy owns independent coverage counters.
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("cpus").u32_property("#address-cells", 1);
    dtb.begin("cpu").string("device_type", "cpu").raw_string("status", "off", 4).u32_property("reg", 0).end();
    dtb.end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.cpu_count == 0);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").raw_string("status", "off", 4).words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc").string("compatible", "arm,gic-400").raw_string("status", "off", 4).u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    const u32 reg[] = {0x8000000, 0x1000};
    dtb.words("reg", reg, count_of(reg)).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("timer").string("compatible", "arm,armv8-timer").raw_string("status", "off", 4).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.timer_interrupt == expected_timer_interrupt(0));
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    const u32 reg[] = {0, 0x1000};
    dtb.begin("memory").string("device_type", "memory").words("reg", reg, count_of(reg)).end();
    dtb.begin("reserved-memory").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("pool").string("compatible", "shared-dma-pool").raw_string("status", "off", 4).property("reg", reg, sizeof(reg)).end();
    dtb.end().end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && info.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("memory").string("device_type", "memory").property("reg", "", 3).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.memory_map_valid);
  }
  {
    DtbBuilder dtb;
    dtb.begin("").u32_property("#address-cells", 1).u32_property("#size-cells", 1);
    dtb.begin("intc").string("compatible", "arm,gic-400").u32_property("phandle", 1).u32_property("#interrupt-cells", 2);
    dtb.property("reg", "", 3).end();
    dtb.end().finish();
    const auto info = parsed_dtb(dtb);
    ut::expect(info.ok && !info.info.intc.valid);
  }
}

} // namespace

void register_fdt_cases() {
  ut::register_suite("fdt", [] {
    ut::register_test("missing_nodes", missing_nodes);
    ut::register_test("cpu_topology", cpu_topology);
    ut::register_test("cpu_rejections", cpu_rejections);
    ut::register_test("memory_layout", memory_layout);
    ut::register_test("chosen_uart", chosen_uart);
    ut::register_test("uart_rejections", uart_rejections);
    ut::register_test("interrupt_controllers", interrupt_controllers);
    ut::register_test("reserved_memory", reserved_memory);
    ut::register_test("interrupt_sources", interrupt_sources_and_firmware);
    ut::register_test("order_cpu_topology", order_cpu_topology);
    ut::register_test("matrix_edges", matrix_edges);
    ut::register_test("intc_edges", intc_edges);
    ut::register_test("parser_edges", parser_edges);
  });
}

} // namespace moss::test::validation
