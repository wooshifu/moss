#pragma once

// 现代进程管理系统
// 支持多线程、优先级、实时调度等特性

#include "containers/containers.hpp"
#include "moss_std.hpp" // 裸机环境基础定义
#include "result.hpp"
#include "smart_ptr.hpp"
#include "types.hpp"
// utility 通过 containers.hpp 包含

namespace moss::kernel::process {

// 使用内核智能指针
using moss::kernel::make_unique;
using moss::kernel::unique_ptr;

// 进程状态
enum class ProcessState : u8 {
  Created = 0,    // 刚创建，未运行
  Ready = 1,      // 就绪，等待调度
  Running = 2,    // 正在运行
  Blocked = 3,    // 阻塞等待
  Terminated = 4, // 已终止
  Zombie = 5      // 僵尸状态（等待父进程清理）
};

// 调度类别
enum class SchedClass : u8 {
  Normal = 0,   // 普通进程（CFS调度）
  RealTime = 1, // 实时进程（RT调度）
  Idle = 2,     // 空闲进程
  Batch = 3     // 批处理进程
};

// 进程优先级范围
namespace Priority {
static constexpr i32 MIN_NICE = -20;
static constexpr i32 MAX_NICE = 19;
static constexpr i32 DEFAULT_NICE = 0;

static constexpr u32 MIN_RT_PRIORITY = 1;
static constexpr u32 MAX_RT_PRIORITY = 99;
static constexpr u32 DEFAULT_RT_PRIORITY = 50;
} // namespace Priority

// CPU上下文结构（ARM64）
struct alignas(16) CpuContext {
  // 通用寄存器 x0-x30
  u64 x[31];

  // 栈指针
  u64 sp;

  // 程序计数器
  u64 pc;

  // 程序状态寄存器
  u64 pstate;

  // 浮点和NEON寄存器
  u64 fpsr; // 浮点状态寄存器
  u64 fpcr; // 浮点控制寄存器

  // NEON/FP寄存器（128位 × 32个）
  struct {
    u64 low, high;
  } v[32];

  // 线程指针寄存器
  u64 tpidr_el0;

  // 初始化上下文
  constexpr CpuContext() noexcept
      : x{}, sp(0), pc(0), pstate(0), fpsr(0), fpcr(0), v{}, tpidr_el0(0) {}
};

static_assert(sizeof(CpuContext) <= 1024,
              "CpuContext should fit in reasonable size");

// 虚拟内存区域（VMA）
struct VmaRegion {
  moss::kernel::VirtAddr start_addr; // 起始虚拟地址
  moss::kernel::VirtAddr end_addr;   // 结束虚拟地址
  moss::kernel::u32 flags;           // 区域标志（读写执行权限等）
  moss::kernel::PhysAddr phys_addr;  // 对应物理地址（如果映射）

  VmaRegion(moss::kernel::VirtAddr start, moss::kernel::VirtAddr end,
            moss::kernel::u32 region_flags,
            moss::kernel::PhysAddr phys = 0) noexcept
      : start_addr(start), end_addr(end), flags(region_flags), phys_addr(phys) {
  }
};

// 虚拟内存地址空间
struct AddressSpace {
  // 页表根目录物理地址
  PhysAddr pgd_phys;

  // ASID (Address Space ID)
  u16 asid;

  // 虚拟内存区域链表
  containers::RcuList<struct VmaRegion> vma_list;

  // 内存使用统计
  containers::AtomicSize total_pages;
  containers::AtomicSize resident_pages;

  AddressSpace(PhysAddr pgd, u16 asid_val) noexcept
      : pgd_phys(pgd), asid(asid_val), total_pages(0), resident_pages(0) {}
};

// 调度实体（支持多线程）
struct SchedEntity {
  // CFS调度相关
  u64 vruntime;              // 虚拟运行时间
  u64 exec_start;            // 开始执行时间
  u64 sum_exec_runtime;      // 累计执行时间
  u64 prev_sum_exec_runtime; // 上次统计的累计时间

  // 权重和优先级
  u32 weight; // 调度权重
  i32 nice;   // nice值 (-20 to 19)
  u32 prio;   // 内部优先级

  // 负载追踪
  u32 load_weight; // 负载权重
  u64 load_sum;    // 累计负载
  u64 util_sum;    // 累计利用率
  u64 load_avg;    // 平均负载
  u64 util_avg;    // 平均利用率

  SchedEntity() noexcept
      : vruntime(0), exec_start(0), sum_exec_runtime(0),
        prev_sum_exec_runtime(0), weight(1024), nice(0), prio(120),
        load_weight(1024), load_sum(0), util_sum(0), load_avg(0), util_avg(0) {}
};

// 实时调度实体
struct RtSchedEntity {
  u32 priority; // 实时优先级 (1-99)
  u64 runtime;  // 本周期内已运行时间
  u64 deadline; // 截止时间
  u64 period;   // 周期时间

  RtSchedEntity() noexcept
      : priority(Priority::DEFAULT_RT_PRIORITY), runtime(0), deadline(0),
        period(0) {}
};

// 线程结构
struct Thread {
  ThreadId tid;        // 线程ID
  ProcessId owner_pid; // 所属进程ID

  // CPU状态
  CpuContext context; // CPU上下文
  u32 cpu;            // 当前运行的CPU
  u32 wake_cpu;       // 唤醒时的CPU

  // 调度相关
  ProcessState state;     // 线程状态
  SchedClass sched_class; // 调度类别
  SchedEntity se;         // CFS调度实体
  RtSchedEntity rt;       // 实时调度实体

  // 时间统计
  u64 start_time; // 创建时间
  u64 utime;      // 用户态时间
  u64 stime;      // 内核态时间

  // 栈信息
  VirtAddr stack_base; // 栈基址
  usize stack_size;    // 栈大小

  // 等待和信号
  VirtAddr wait_queue; // 等待队列
  u64 signal_mask;     // 信号掩码
  u64 pending_signals; // 待处理信号

  Thread(ThreadId id, ProcessId pid) noexcept
      : tid(id), owner_pid(pid), context{}, cpu(0), wake_cpu(0),
        state(ProcessState::Created), sched_class(SchedClass::Normal), se{},
        rt{}, start_time(0), utime(0), stime(0), stack_base(0), stack_size(0),
        wait_queue(0), signal_mask(0), pending_signals(0) {}
};

// 进程控制块
class Process {
private:
  ProcessId pid_;        // 进程ID
  ProcessId parent_pid_; // 父进程ID

  // 内存管理
  unique_ptr<AddressSpace> address_space_;

  // 线程管理
  containers::RcuHashMap<ThreadId, Thread *> threads_;
  containers::AtomicCounter<u32> thread_count_;
  ThreadId main_thread_id_;

  // 进程状态
  ProcessState state_;
  i32 exit_code_;

  // 资源限制和统计
  struct {
    u64 max_memory;   // 最大内存使用
    u64 max_cpu_time; // 最大CPU时间
    u32 max_threads;  // 最大线程数
    u32 max_files;    // 最大文件描述符数
  } limits_;

  struct {
    u64 memory_usage;               // 当前内存使用
    u64 cpu_time;                   // 累计CPU时间
    u32 minor_faults;               // 次要页错误
    u32 major_faults;               // 主要页错误
    u32 voluntary_ctxt_switches;    // 主动上下文切换
    u32 nonvoluntary_ctxt_switches; // 被动上下文切换
  } stats_;

  // 同步和互斥
  mutable containers::AtomicU32 ref_count_;

public:
  Process(ProcessId pid, ProcessId parent = INVALID_PROCESS_ID) noexcept
      : pid_(pid), parent_pid_(parent), address_space_(nullptr),
        thread_count_(0), main_thread_id_(INVALID_THREAD_ID),
        state_(ProcessState::Created), exit_code_(0), limits_{}, stats_{},
        ref_count_(1) {}

  ~Process() noexcept {
    // 清理所有线程
    cleanup_threads();
  }

  // 禁用拷贝，允许移动
  NON_COPYABLE(Process)

  Process(Process &&other) noexcept
      : pid_(other.pid_), parent_pid_(other.parent_pid_),
        address_space_(moss::move(other.address_space_)),
        threads_{},       // 新进程开始时线程列表为空
        thread_count_(0), // 重置线程计数
        main_thread_id_(other.main_thread_id_), state_(other.state_),
        exit_code_(other.exit_code_), limits_(other.limits_),
        stats_(other.stats_), ref_count_(1) { // 重置引用计数
    other.pid_ = INVALID_PROCESS_ID;
    // 注意：实际实现中需要迁移线程到新进程
  }

  // 基本属性访问
  [[nodiscard]] ProcessId pid() const noexcept { return pid_; }
  [[nodiscard]] ProcessId parent_pid() const noexcept { return parent_pid_; }
  [[nodiscard]] ProcessState state() const noexcept { return state_; }
  [[nodiscard]] i32 exit_code() const noexcept { return exit_code_; }

  // 线程管理
  [[nodiscard]] KernelResult<ThreadId> create_thread(VirtAddr entry_point,
                                                     VirtAddr stack_base,
                                                     usize stack_size) noexcept;

  [[nodiscard]] Thread *get_thread(ThreadId tid) const noexcept;
  [[nodiscard]] Thread *get_main_thread() const noexcept;

  [[nodiscard]] u32 thread_count() const noexcept {
    return thread_count_.load(containers::MemoryOrder::Relaxed);
  }

  // 内存管理
  [[nodiscard]] VoidResult
  set_address_space(unique_ptr<AddressSpace> as) noexcept;
  [[nodiscard]] AddressSpace *address_space() const noexcept {
    return address_space_.get();
  }

  // 进程状态管理
  void set_state(ProcessState new_state) noexcept;
  void set_exit_code(i32 code) noexcept { exit_code_ = code; }

  // 统计信息更新
  void update_cpu_time(u64 user_time, u64 kernel_time) noexcept;
  void record_context_switch(bool voluntary) noexcept;
  void record_page_fault(bool major) noexcept;

  // 引用计数管理
  void add_ref() const noexcept {
    (void)ref_count_.fetch_add(1, containers::MemoryOrder::Relaxed);
  }

  void release() const noexcept {
    if (ref_count_.fetch_sub(1, containers::MemoryOrder::AcqRel) == 1) {
      delete this;
    }
  }

  [[nodiscard]] u32 ref_count() const noexcept {
    return ref_count_.load(containers::MemoryOrder::Acquire);
  }

private:
  void cleanup_threads() noexcept;
  [[nodiscard]] ThreadId allocate_thread_id() noexcept;
};

// 进程管理器
class ProcessManager {
private:
  // 进程注册表
  containers::RcuHashMap<ProcessId, Process *> processes_;
  containers::AtomicCounter<ProcessId> next_pid_;

  // 全局统计
  containers::PerCpuAtomicCounter<u64> total_context_switches_;
  containers::PerCpuAtomicCounter<u64> total_forks_;
  containers::PerCpuAtomicCounter<u64> total_exits_;

public:
  ProcessManager() noexcept : next_pid_(1) {}

  // 进程创建和销毁
  [[nodiscard]] KernelResult<Process *>
  create_process(ProcessId parent_pid = INVALID_PROCESS_ID) noexcept;
  [[nodiscard]] VoidResult terminate_process(ProcessId pid,
                                             i32 exit_code) noexcept;

  // 进程查找
  [[nodiscard]] Process *find_process(ProcessId pid) const noexcept;
  [[nodiscard]] bool process_exists(ProcessId pid) const noexcept;

  // 系统调用接口
  [[nodiscard]] KernelResult<ProcessId> sys_fork() noexcept;
  [[nodiscard]] VoidResult sys_exit(i32 exit_code) noexcept;
  [[nodiscard]] KernelResult<ProcessId> sys_wait(ProcessId pid) noexcept;

  // 统计信息
  [[nodiscard]] u64 total_processes() const noexcept;
  [[nodiscard]] u64 total_context_switches() const noexcept {
    return total_context_switches_;
  }
  [[nodiscard]] u64 total_forks() const noexcept { return total_forks_; }
  [[nodiscard]] u64 total_exits() const noexcept { return total_exits_; }

  // 遍历所有进程
  template <typename Func> void for_each_process(Func &&func) const {
    processes_.for_each(
        [&func](const auto &entry) { func(entry.key, entry.value); });
  }

private:
  [[nodiscard]] ProcessId allocate_pid() noexcept;
  void record_fork() noexcept { (void)total_forks_.fetch_add_local(1); }
  void record_exit() noexcept { (void)total_exits_.fetch_add_local(1); }
};

// 全局进程管理器实例
extern ProcessManager *g_process_manager;

// 便利函数
[[nodiscard]] inline Process *current_process() noexcept {
  // 在实际实现中，这里会从当前CPU的运行队列获取
  // 目前简化实现
  return nullptr;
}

[[nodiscard]] inline Thread *current_thread() noexcept {
  // 在实际实现中，这里会从CPU特定的存储获取
  // 目前简化实现
  return nullptr;
}

[[nodiscard]] inline u32 current_cpu() noexcept {
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<u32>(mpidr & 0xFF) % MAX_CPUS;
}

} // namespace moss::kernel::process
