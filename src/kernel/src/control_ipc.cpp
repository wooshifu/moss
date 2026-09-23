module moss.kernel;

namespace moss::kernel::syscall::handlers {
namespace {

// Initial native ABI bound, shared with MOSS_IPC_MAX_MESSAGE in syscall.h.
// No workload measurement has established a larger control payload yet.
constexpr usize kMessageBytes = 256;
// ponytail: 16 pending calls bound kernel memory per endpoint; raise this only
// when measured service concurrency needs more outstanding calls.
constexpr usize kPendingCalls = 16;
constexpr u32 kAllRights = capability::rights::SEND | capability::rights::RECEIVE | capability::rights::TRANSFER |
                           capability::rights::DUPLICATE;

void *ipc_allocate(usize size, usize alignment) noexcept {
  auto storage = mm::RuntimeHeapAllocator::allocate_aligned(size, alignment);
  return storage ? *storage : nullptr;
}

enum class Outcome : u8 { Pending, Reply, Canceled, Expired, PeerClosed };

struct PendingCall {
  process::Thread *caller;
  u64 deadline_ns;
  usize request_size;
  usize response_size{0};
  Outcome outcome{Outcome::Pending};
  bool queued{true};
  u8 request[kMessageBytes]{};
  u8 response[kMessageBytes]{};

  PendingCall(process::Thread *thread, u64 deadline, const u8 *data, usize size) noexcept
      : caller(thread), deadline_ns(deadline), request_size(size) {
    if (size)
      __builtin_memcpy(request, data, size);
  }
};

class Channel {
  containers::IrqSpinLock lock_;
  containers::WaitQueue receivers_;
  shared_ptr<PendingCall> calls_[kPendingCalls]{};
  bool closed_{false};

  [[nodiscard]] bool has_queued_locked() const noexcept {
    for (const auto &call : calls_) {
      if (call && call->queued && call->outcome == Outcome::Pending)
        return true;
    }
    return false;
  }

  static void wake(process::Thread *thread) noexcept {
    if (process::g_scheduler)
      process::g_scheduler->task_wakeup(thread, thread->wake_cpu);
  }

public:
  ~Channel() noexcept { close(); }

  [[nodiscard]] long enqueue(shared_ptr<PendingCall> call) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    if (closed_)
      return -errc::EPIPE;
    for (auto &slot : calls_) {
      if (!slot) {
        slot = moss::move(call);
        receivers_.wake_one([](void *thread) { wake(static_cast<process::Thread *>(thread)); });
        return 0;
      }
    }
    return -errc::EAGAIN;
  }

  [[nodiscard]] shared_ptr<PendingCall> peek() noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    for (const auto &call : calls_) {
      if (call && call->queued && call->outcome == Outcome::Pending)
        return call;
    }
    return {};
  }

  [[nodiscard]] bool receive(PendingCall *call) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    for (const auto &slot : calls_) {
      if (slot.get() == call && call->queued && call->outcome == Outcome::Pending) {
        call->queued = false;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] Outcome complete(PendingCall *call, Outcome outcome, const u8 *data = nullptr, usize size = 0,
                                 bool *committed = nullptr) noexcept {
    shared_ptr<PendingCall> retired;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(lock_);
      if (call->outcome != Outcome::Pending) {
        if (committed)
          *committed = false;
        return call->outcome;
      }
      // The same lock serializes reply, cancellation, timer expiry and peer
      // loss. A reply after its absolute deadline cannot win by timer delay.
      if (outcome == Outcome::Reply && call->deadline_ns != 0 &&
          timer::TimerSubsystem::instance().now_ns() >= call->deadline_ns)
        outcome = Outcome::Expired;
      if (outcome == Outcome::Reply) {
        call->response_size = size;
        if (size)
          __builtin_memcpy(call->response, data, size);
      }
      call->outcome = outcome;
      if (committed)
        *committed = true;
      call->queued = false;
      for (auto &slot : calls_) {
        if (slot.get() == call) {
          retired = moss::move(slot);
          break;
        }
      }
      wake(call->caller);
    }
    return outcome;
  }

  [[nodiscard]] Outcome snapshot(PendingCall *call, u8 *response, usize &size) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    size = call->response_size;
    if (call->outcome == Outcome::Reply && size)
      __builtin_memcpy(response, call->response, size);
    return call->outcome;
  }

  void arm_receiver_wait(process::Thread *thread) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    receivers_.add_waiter(thread, true);
    if (closed_ || has_queued_locked())
      wake(thread);
  }

  void disarm_receiver_wait(process::Thread *thread) noexcept { receivers_.remove_waiter(thread); }

  [[nodiscard]] bool closed() noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    return closed_;
  }

  void close() noexcept {
    shared_ptr<PendingCall> retired[kPendingCalls];
    {
      containers::LockGuard<containers::IrqSpinLock> guard(lock_);
      if (closed_)
        return;
      closed_ = true;
      for (usize i = 0; i < kPendingCalls; ++i) {
        if (calls_[i]) {
          calls_[i]->outcome = Outcome::PeerClosed;
          calls_[i]->queued = false;
          wake(calls_[i]->caller);
          retired[i] = moss::move(calls_[i]);
        }
      }
      receivers_.wake_up([](void *thread) { wake(static_cast<process::Thread *>(thread)); });
    }
  }
};

class Sender final : public capability::Object {
public:
  shared_ptr<Channel> channel;
  explicit Sender(shared_ptr<Channel> target) noexcept
      : Object(capability::ObjectType::Endpoint), channel(moss::move(target)) {}
};

class Receiver final : public capability::Object {
protected:
  void on_last_handle_closed() noexcept override { channel->close(); }

public:
  shared_ptr<Channel> channel;
  explicit Receiver(shared_ptr<Channel> target) noexcept
      : Object(capability::ObjectType::Receiver), channel(moss::move(target)) {}
  ~Receiver() override { channel->close(); }
};

class Reply final : public capability::Object {
public:
  shared_ptr<Channel> channel;
  shared_ptr<PendingCall> call;
  bool armed{false};
  Reply(shared_ptr<Channel> owner, shared_ptr<PendingCall> pending) noexcept
      : Object(capability::ObjectType::Reply), channel(moss::move(owner)), call(moss::move(pending)) {}
  ~Reply() override {
    if (armed)
      (void)channel->complete(call.get(), Outcome::PeerClosed);
  }
};

[[nodiscard]] long cap_error(ErrorCode error) noexcept {
  if (error == ErrorCode::NotFound)
    return -errc::EBADF;
  if (error == ErrorCode::PermissionDenied)
    return -errc::EACCES;
  if (error == ErrorCode::ResourceExhausted)
    return -errc::EMFILE;
  if (error == ErrorCode::OutOfMemory)
    return -errc::ENOMEM;
  return -errc::EINVAL;
}

[[nodiscard]] shared_ptr<process::Process> caller_process() noexcept { return process::current_process(); }

struct DeadlineWake {
  Channel *channel;
  PendingCall *call;
};

void deadline_wake(void *context) noexcept {
  auto *wake = static_cast<DeadlineWake *>(context);
  (void)wake->channel->complete(wake->call, Outcome::Expired);
}

} // namespace

long sys_cap_close(long handle, long, long, long, long, long) noexcept {
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  auto result = proc->capabilities().close(static_cast<Handle>(handle));
  return result ? 0 : cap_error(result.error());
}

long sys_cap_duplicate(long handle, long rights, long, long, long, long) noexcept {
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  if (rights <= 0 || (static_cast<u64>(rights) & ~static_cast<u64>(kAllRights)) != 0)
    return -errc::EINVAL;
  auto result = proc->capabilities().duplicate(static_cast<Handle>(handle), static_cast<u32>(rights));
  return result ? static_cast<long>(*result) : cap_error(result.error());
}

long sys_cap_set_inherit(long handle, long inherit, long, long, long, long) noexcept {
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  if (inherit != 0 && inherit != 1)
    return -errc::EINVAL;
  auto result = proc->capabilities().set_inheritable(static_cast<Handle>(handle), inherit == 1);
  return result ? 0 : cap_error(result.error());
}

long sys_ipc_create(long pair_addr, long, long, long, long, long) noexcept {
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  auto channel = shared_ptr<Channel>::try_make(ipc_allocate);
  if (!channel)
    return -errc::ENOMEM;
  auto sender = shared_ptr<capability::Object>::try_make<Sender>(ipc_allocate, channel);
  auto receiver = shared_ptr<capability::Object>::try_make<Receiver>(ipc_allocate, channel);
  if (!sender || !receiver)
    return -errc::ENOMEM;
  auto send_handle = proc->capabilities().install(
      moss::move(sender), capability::rights::SEND | capability::rights::TRANSFER | capability::rights::DUPLICATE);
  if (!send_handle)
    return cap_error(send_handle.error());
  auto receive_handle = proc->capabilities().install(
      moss::move(receiver), capability::rights::RECEIVE | capability::rights::TRANSFER | capability::rights::DUPLICATE);
  if (!receive_handle) {
    (void)proc->capabilities().close(*send_handle);
    return cap_error(receive_handle.error());
  }
  const Handle pair[2] = {*send_handle, *receive_handle};
  if (process::copy_to_user(static_cast<u64>(pair_addr), pair, sizeof(pair)) != 0) {
    (void)proc->capabilities().close(*send_handle);
    (void)proc->capabilities().close(*receive_handle);
    return -errc::EFAULT;
  }
  return 0;
}

long sys_ipc_call(long endpoint, long request_addr, long request_size, long response_addr, long response_capacity,
                  long deadline_ns) noexcept {
  auto proc = caller_process();
  auto *thread = process::CfsScheduler::get_current_task();
  if (!proc || !thread || !process::g_scheduler)
    return -errc::ESRCH;
  if (request_size < 0 || request_size > static_cast<long>(kMessageBytes) || response_capacity < 0 ||
      response_capacity > static_cast<long>(kMessageBytes) || deadline_ns < 0)
    return -errc::EINVAL;
  auto looked = proc->capabilities().lookup(static_cast<Handle>(endpoint), capability::ObjectType::Endpoint,
                                            capability::rights::SEND);
  if (!looked)
    return cap_error(looked.error());
  u8 request[kMessageBytes]{};
  if (request_size &&
      process::copy_from_user(request, static_cast<u64>(request_addr), static_cast<usize>(request_size)) != 0)
    return -errc::EFAULT;
  auto channel = static_cast<Sender *>((*looked).get())->channel;
  auto call = shared_ptr<PendingCall>::try_make(ipc_allocate, thread, static_cast<u64>(deadline_ns), request,
                                                static_cast<usize>(request_size));
  if (!call)
    return -errc::ENOMEM;
  if (deadline_ns != 0 && timer::TimerSubsystem::instance().now_ns() >= static_cast<u64>(deadline_ns))
    return -errc::ETIMEDOUT;
  const long enqueued = channel->enqueue(call);
  if (enqueued < 0)
    return enqueued;

  DeadlineWake wake{channel.get(), call.get()};
  timer::HrTimer deadline_timer;
  bool timer_armed = false;
  if (deadline_ns != 0) {
    deadline_timer.init(timer::TimerMode::OneShot, deadline_wake, &wake);
    auto started = deadline_timer.start(static_cast<u64>(deadline_ns));
    if (!started) {
      (void)channel->complete(call.get(), Outcome::Canceled);
      return started.error() == ErrorCode::ResourceExhausted ? -errc::ENOMEM : -errc::EINVAL;
    }
    timer_armed = true;
  }

  u8 response[kMessageBytes]{};
  usize response_size = 0;
  Outcome result;
  while ((result = channel->snapshot(call.get(), response, response_size)) == Outcome::Pending) {
    if (moss::abi::bridge::moss_io_wait_interrupted()) {
      (void)channel->complete(call.get(), Outcome::Canceled);
      continue;
    }
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    (void)moss::abi::bridge::moss_prepare_io_wait();
    usize ignored = 0;
    u8 unused[kMessageBytes];
    if (channel->snapshot(call.get(), unused, ignored) != Outcome::Pending)
      process::g_scheduler->task_wakeup(thread, thread->wake_cpu);
    process::g_scheduler->commit_sleep();
    if (restore_irqs)
      arch::enable_interrupts();
  }
  if (timer_armed)
    deadline_timer.cancel_sync();
  switch (result) {
  case Outcome::Reply:
    if (response_size > static_cast<usize>(response_capacity))
      return -errc::E2BIG;
    if (response_size && process::copy_to_user(static_cast<u64>(response_addr), response, response_size) != 0)
      return -errc::EFAULT;
    return static_cast<long>(response_size);
  case Outcome::Canceled:
    return -errc::EINTR;
  case Outcome::Expired:
    return -errc::ETIMEDOUT;
  case Outcome::PeerClosed:
  case Outcome::Pending:
    return -errc::EPIPE;
  default:
    return -errc::EPIPE;
  }
}

long sys_ipc_receive(long endpoint, long request_addr, long request_capacity, long reply_addr, long, long) noexcept {
  auto proc = caller_process();
  auto *thread = process::CfsScheduler::get_current_task();
  if (!proc || !thread || !process::g_scheduler)
    return -errc::ESRCH;
  if (request_capacity < 0 || request_capacity > static_cast<long>(kMessageBytes))
    return -errc::EINVAL;
  auto looked = proc->capabilities().lookup(static_cast<Handle>(endpoint), capability::ObjectType::Receiver,
                                            capability::rights::RECEIVE);
  if (!looked)
    return cap_error(looked.error());
  auto channel = static_cast<Receiver *>((*looked).get())->channel;
  while (true) {
    auto call = channel->peek();
    if (call) {
      if (call->request_size > static_cast<usize>(request_capacity))
        return -errc::E2BIG;
      if (call->request_size &&
          process::copy_to_user(static_cast<u64>(request_addr), call->request, call->request_size) != 0)
        return -errc::EFAULT;
      auto reply = shared_ptr<capability::Object>::try_make<Reply>(ipc_allocate, channel, call);
      if (!reply)
        return -errc::ENOMEM;
      auto reply_handle = proc->capabilities().install(reply, capability::rights::SEND);
      if (!reply_handle)
        return cap_error(reply_handle.error());
      const Handle handle = *reply_handle;
      if (process::copy_to_user(static_cast<u64>(reply_addr), &handle, sizeof(handle)) != 0) {
        (void)proc->capabilities().close(handle);
        return -errc::EFAULT;
      }
      if (channel->receive(call.get())) {
        static_cast<Reply *>(reply.get())->armed = true;
        return static_cast<long>(call->request_size);
      }
      (void)proc->capabilities().close(handle);
      continue;
    }
    if (channel->closed())
      return -errc::EPIPE;
    if (moss::abi::bridge::moss_io_wait_interrupted())
      return -errc::EINTR;
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    (void)moss::abi::bridge::moss_prepare_io_wait();
    channel->arm_receiver_wait(thread);
    process::g_scheduler->commit_sleep();
    channel->disarm_receiver_wait(thread);
    if (restore_irqs)
      arch::enable_interrupts();
  }
}

long sys_ipc_reply(long reply_handle, long response_addr, long response_size, long, long, long) noexcept {
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  if (response_size < 0 || response_size > static_cast<long>(kMessageBytes))
    return -errc::EINVAL;
  auto looked = proc->capabilities().lookup(static_cast<Handle>(reply_handle), capability::ObjectType::Reply,
                                            capability::rights::SEND);
  if (!looked)
    return cap_error(looked.error());
  u8 response[kMessageBytes]{};
  if (response_size &&
      process::copy_from_user(response, static_cast<u64>(response_addr), static_cast<usize>(response_size)) != 0)
    return -errc::EFAULT;
  auto *reply = static_cast<Reply *>((*looked).get());
  // A cached Reply outcome does not mean this invocation won the one-shot
  // transition: another thread may have looked up the same handle earlier.
  bool committed = false;
  const Outcome result = reply->channel->complete(reply->call.get(), Outcome::Reply, response,
                                                  static_cast<usize>(response_size), &committed);
  (void)proc->capabilities().close(static_cast<Handle>(reply_handle));
  if (result == Outcome::Reply && committed)
    return 0;
  return result == Outcome::Expired ? -errc::ETIMEDOUT : -errc::EPIPE;
}

} // namespace moss::kernel::syscall::handlers
