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
                           capability::rights::DUPLICATE | capability::rights::MAP_READ |
                           capability::rights::MAP_WRITE | capability::rights::MINT |
                           capability::rights::DOMAIN_TERMINATE | capability::rights::DOMAIN_INSPECT |
                           capability::rights::DOMAIN_OBSERVE | capability::rights::DOMAIN_SPAWN |
                           capability::rights::CODE_APPROVE | capability::rights::CODE_EXEC |
                           capability::rights::CODE_IDENTIFY | capability::rights::CODE_REVOKE |
                           capability::rights::DOMAIN_SIGNAL | capability::rights::DOMAIN_SCOPE_ASSIGN |
                           capability::rights::DOMAIN_SCOPE_TERMINATE | capability::rights::DOMAIN_SCOPE_INSPECT;

// Keep this wire layout aligned with userspace's moss_ipc_message. All fields
// are fixed-width on Moss's supported 64-bit ABIs.
struct ControlMessage {
  u64 size{0};
  Handle capability{INVALID_HANDLE};
  u64 rights{0};
  u64 badge{0};
  u8 payload[kMessageBytes]{};
};
static_assert(sizeof(ControlMessage) == 4 * sizeof(u64) + kMessageBytes);

[[nodiscard]] bool valid_message(const ControlMessage &message) noexcept {
  // Only the kernel may attach a badge to a delivered request.
  if (message.size > kMessageBytes || message.badge != 0)
    return false;
  if (message.capability == INVALID_HANDLE)
    return message.rights == 0;
  return message.rights != 0 && (message.rights & ~static_cast<u64>(kAllRights)) == 0;
}

void *ipc_allocate(usize size, usize alignment) noexcept {
  auto storage = mm::RuntimeHeapAllocator::allocate_aligned(size, alignment);
  return storage ? *storage : nullptr;
}

enum class Outcome : u8 { Pending, Reply, Canceled, Expired, PeerClosed };

struct PendingCall {
  process::Thread *caller;
  process::PriorityDonation donation{};
  u64 deadline_ns;
  u64 badge;
  usize request_size;
  usize response_size{0};
  Outcome outcome{Outcome::Pending};
  bool queued{true};
  bool delivery_claimed{false};
  capability::Escrow request_cap{};
  capability::Escrow response_cap{};
  u8 request[kMessageBytes]{};
  u8 response[kMessageBytes]{};

  PendingCall(process::Thread *thread, u64 deadline, u64 sender_badge, const ControlMessage &message,
              capability::Escrow &&transferred) noexcept
      : caller(thread), deadline_ns(deadline), badge(sender_badge), request_size(message.size),
        request_cap(moss::move(transferred)) {
    if (request_size)
      __builtin_memcpy(request, message.payload, request_size);
  }
};

// Redirect the original caller's donation before a delegated recipient has
// to compete for CPU time; binding only after userspace wakeup is too late.
void bind_delegated_reply(const capability::Escrow &transferred, process::Thread *recipient) noexcept;

class Channel {
  containers::IrqSpinLock lock_;
  containers::WaitQueue receivers_;
  shared_ptr<PendingCall> calls_[kPendingCalls]{};
  bool closed_{false};

  static void wake(process::Thread *thread) noexcept {
    if (thread && process::g_scheduler)
      process::g_scheduler->task_wakeup(thread, thread->wake_cpu);
  }

  // The selected waiting receiver needs the caller's priority before it can
  // run and claim the request. Claim may hand the call to a different worker.
  void wake_receiver(PendingCall *call) noexcept {
    const bool assigned = receivers_.wake_one([&](void *waiter) {
      auto *thread = static_cast<process::Thread *>(waiter);
      if (process::g_scheduler)
        process::g_scheduler->bind_ipc_server(&call->donation, thread);
      bind_delegated_reply(call->request_cap, thread);
      wake(thread);
    });
    if (!assigned && process::g_scheduler)
      process::g_scheduler->bind_ipc_server(&call->donation, nullptr);
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
        wake_receiver(slot.get());
        return 0;
      }
    }
    return -errc::EAGAIN;
  }

  // Claim before copyout: only one receiver may inspect or release the
  // in-flight capability while competing receivers race for this call.
  [[nodiscard]] shared_ptr<PendingCall> claim(process::Thread *server) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    // Keep the waiter registered through wakeup so a new call can donate
    // before this thread gets CPU time to claim it.
    receivers_.remove_waiter(server);
    for (const auto &call : calls_) {
      if (call && call->queued && !call->delivery_claimed && call->outcome == Outcome::Pending) {
        call->delivery_claimed = true;
        process::g_scheduler->bind_ipc_server(&call->donation, server);
        bind_delegated_reply(call->request_cap, server);
        return call;
      }
    }
    return {};
  }

  void release_claim(PendingCall *call) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    if (call->delivery_claimed && call->queued && call->outcome == Outcome::Pending) {
      call->delivery_claimed = false;
      wake_receiver(call);
    }
  }

  [[nodiscard]] bool receive(PendingCall *call) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    for (const auto &slot : calls_) {
      if (slot.get() == call && call->queued && call->delivery_claimed && call->outcome == Outcome::Pending) {
        call->queued = false;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] Outcome complete(PendingCall *call, Outcome outcome, const u8 *data = nullptr, usize size = 0,
                                 bool *committed = nullptr, capability::Escrow *transferred = nullptr) noexcept {
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
        if (transferred)
          call->response_cap = moss::move(*transferred);
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
      if (process::g_scheduler)
        process::g_scheduler->end_ipc_call(&call->donation);
      if (outcome == Outcome::Reply)
        bind_delegated_reply(call->response_cap, call->caller);
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
    if (closed_) {
      wake(thread);
      return;
    }
    for (const auto &call : calls_) {
      if (call && call->queued && !call->delivery_claimed && call->outcome == Outcome::Pending) {
        process::g_scheduler->bind_ipc_server(&call->donation, thread);
        bind_delegated_reply(call->request_cap, thread);
        wake(thread);
        break;
      }
    }
  }

  void release_receiver(process::Thread *thread) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    for (const auto &call : calls_) {
      if (call && call->queued && !call->delivery_claimed && process::g_scheduler) {
        process::Thread *next = nullptr;
        receivers_.wake_one([&](void *waiter) { next = static_cast<process::Thread *>(waiter); });
        if (process::g_scheduler->rebind_ipc_server(&call->donation, thread, next))
          wake(next);
      }
    }
  }

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
        // Once delivered, a Reply holder owns the call even if the original
        // receiver closes. Only requests without an owner fail with the channel.
        if (calls_[i] && calls_[i]->queued) {
          calls_[i]->outcome = Outcome::PeerClosed;
          calls_[i]->queued = false;
          if (process::g_scheduler)
            process::g_scheduler->end_ipc_call(&calls_[i]->donation);
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
  const u64 badge;
  explicit Sender(shared_ptr<Channel> target, u64 sender_badge = 0) noexcept
      : Object(capability::ObjectType::Endpoint), channel(moss::move(target)), badge(sender_badge) {}
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

class MemoryObject final : public capability::Object {
  PhysAddr page_{0};
  bool has_page_{false};

public:
  MemoryObject() noexcept : Object(capability::ObjectType::Memory) {}
  ~MemoryObject() override {
    // The allocation owns one reference; each resident mapping owns another.
    if (has_page_ && mm::PageFrameAllocator::page_ref_dec(page_) == 0)
      (void)mm::free_pages(page_, 0);
  }
  void adopt_page(PhysAddr page) noexcept {
    page_ = page;
    has_page_ = true;
  }
  [[nodiscard]] PhysAddr page() const noexcept { return page_; }
};

class Reply final : public capability::Object {
public:
  shared_ptr<Channel> channel;
  shared_ptr<PendingCall> call;
  atomic<bool> armed{false};
  Reply(shared_ptr<Channel> owner, shared_ptr<PendingCall> pending) noexcept
      : Object(capability::ObjectType::Reply), channel(moss::move(owner)), call(moss::move(pending)) {}
  ~Reply() override {
    if (armed.load(memory_order_acquire))
      (void)channel->complete(call.get(), Outcome::PeerClosed);
  }
};

void bind_delegated_reply(const capability::Escrow &transferred, process::Thread *recipient) noexcept {
  if (!process::g_scheduler)
    return;
  auto *object = transferred.get();
  if (object && object->type() == capability::ObjectType::Reply) {
    auto *reply = static_cast<Reply *>(object);
    process::g_scheduler->bind_ipc_server(&reply->call->donation, recipient);
  }
}

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

long sys_cap_set_exec(long handle, long keep, long, long, long, long) noexcept {
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  if (keep != 0 && keep != 1)
    return -errc::EINVAL;
  auto result = proc->capabilities().set_keep_on_exec(static_cast<Handle>(handle), keep == 1);
  return result ? 0 : cap_error(result.error());
}

long sys_mem_create(long size, long, long, long, long, long) noexcept {
  // ponytail: one page is enough to establish the shared data path; extend
  // object sizing when a service protocol needs a larger contiguous window.
  if (size != static_cast<long>(PAGE_SIZE))
    return -errc::EINVAL;
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  // Construct first: try_make may destroy a constructed object if its control
  // block allocation fails, so no physical page may be owned until it returns.
  auto object = shared_ptr<capability::Object>::try_make<MemoryObject>(ipc_allocate);
  if (!object)
    return -errc::ENOMEM;
  auto page = mm::allocate_pages(0);
  if (!page)
    return -errc::ENOMEM;
  __builtin_memset(reinterpret_cast<void *>(phys_to_virt(*page)), 0, PAGE_SIZE);
  static_cast<MemoryObject *>(object.get())->adopt_page(*page);
  constexpr u32 initial_rights = capability::rights::MAP_READ | capability::rights::MAP_WRITE |
                                 capability::rights::TRANSFER | capability::rights::DUPLICATE;
  auto handle = proc->capabilities().install(moss::move(object), initial_rights);
  return handle ? static_cast<long>(*handle) : cap_error(handle.error());
}

long sys_mem_map(long handle, long rights, long, long, long, long) noexcept {
  constexpr u32 mapping_rights = capability::rights::MAP_READ | capability::rights::MAP_WRITE;
  if (rights <= 0 || (static_cast<u64>(rights) & ~static_cast<u64>(mapping_rights)) != 0)
    return -errc::EINVAL;
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  auto looked = proc->capabilities().lookup(static_cast<Handle>(handle), capability::ObjectType::Memory,
                                            static_cast<u32>(rights));
  if (!looked)
    return cap_error(looked.error());
  auto as = proc->address_space();
  if (!as)
    return -errc::ESRCH;
  auto transaction = as->lock_vm();
  // ponytail: use the existing monotonic mmap cursor; add hole search when
  // services need to recycle virtual ranges after unmap.
  const VirtAddr address = as->mmap_next;
  if (address > USER_MAX - PAGE_SIZE || !mm::PageTableManager::is_user_range(address, PAGE_SIZE))
    return -errc::ENOMEM;
  // Every supported architecture makes a writable user mapping readable.
  const u32 flags =
      process::vma_flags::READ | ((rights & capability::rights::MAP_WRITE) ? process::vma_flags::WRITE : 0U);
  if (!as->add_vma(address, address + PAGE_SIZE, flags, process::VmaType::MMAP, nullptr, 0, 0, *looked,
                   static_cast<MemoryObject *>((*looked).get())->page()))
    return -errc::ENOMEM;
  as->mmap_next = address + PAGE_SIZE;
  return static_cast<long>(address);
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
  auto send_handle =
      proc->capabilities().install(moss::move(sender), capability::rights::SEND | capability::rights::TRANSFER |
                                                           capability::rights::DUPLICATE | capability::rights::MINT);
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

long sys_ipc_mint_badge(long endpoint, long badge, long, long, long, long) noexcept {
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  // The derived sender has SEND, TRANSFER and DUPLICATE. Require all three
  // on the source so minting cannot recover rights removed by attenuation.
  constexpr u32 mint_authority = capability::rights::SEND | capability::rights::TRANSFER |
                                 capability::rights::DUPLICATE | capability::rights::MINT;
  auto looked =
      proc->capabilities().lookup(static_cast<Handle>(endpoint), capability::ObjectType::Endpoint, mint_authority);
  if (!looked)
    return cap_error(looked.error());
  auto channel = static_cast<Sender *>((*looked).get())->channel;
  auto sender = shared_ptr<capability::Object>::try_make<Sender>(ipc_allocate, channel, static_cast<u64>(badge));
  if (!sender)
    return -errc::ENOMEM;
  // Minted file-object authority can be delegated, but cannot mint another
  // identity unless the service explicitly delegates the original mint right.
  constexpr u32 rights = capability::rights::SEND | capability::rights::TRANSFER | capability::rights::DUPLICATE;
  auto handle = proc->capabilities().install(moss::move(sender), rights);
  return handle ? static_cast<long>(*handle) : cap_error(handle.error());
}

long sys_ipc_call(long endpoint, long request_addr, long response_addr, long deadline_ns, long, long) noexcept {
  auto proc = caller_process();
  auto *thread = process::CfsScheduler::get_current_task();
  if (!proc || !thread || !process::g_scheduler)
    return -errc::ESRCH;
  if (deadline_ns < 0)
    return -errc::EINVAL;
  auto looked = proc->capabilities().lookup(static_cast<Handle>(endpoint), capability::ObjectType::Endpoint,
                                            capability::rights::SEND);
  if (!looked)
    return cap_error(looked.error());
  ControlMessage request{};
  if (process::copy_from_user(&request, static_cast<u64>(request_addr), sizeof(request)) != 0)
    return -errc::EFAULT;
  if (!valid_message(request))
    return -errc::EINVAL;
  capability::IpcCapture capture;
  capability::Escrow transferred;
  if (request.capability != INVALID_HANDLE) {
    auto captured = proc->capabilities().capture_for_ipc(request.capability, static_cast<u32>(request.rights));
    if (!captured)
      return cap_error(captured.error());
    capture = moss::move(*captured);
    transferred = capture.take_escrow();
  }
  auto *sender = static_cast<Sender *>((*looked).get());
  auto channel = sender->channel;
  auto call = shared_ptr<PendingCall>::try_make(ipc_allocate, thread, static_cast<u64>(deadline_ns), sender->badge,
                                                request, moss::move(transferred));
  if (!call)
    return -errc::ENOMEM;
  if (deadline_ns != 0 && timer::TimerSubsystem::instance().now_ns() >= static_cast<u64>(deadline_ns))
    return -errc::ETIMEDOUT;
  if (!process::g_scheduler->begin_ipc_call(&call->donation, thread))
    return -errc::EAGAIN;
  const long enqueued = channel->enqueue(call);
  if (enqueued < 0) {
    process::g_scheduler->end_ipc_call(&call->donation);
    return enqueued;
  }
  capture.commit();

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
  case Outcome::Reply: {
    ControlMessage delivered{};
    delivered.size = response_size;
    if (response_size)
      __builtin_memcpy(delivered.payload, response, response_size);
    if (call->response_cap) {
      delivered.rights = call->response_cap.rights();
      auto reserved = proc->capabilities().reserve_escrow(call->response_cap);
      if (!reserved)
        return cap_error(reserved.error());
      delivered.capability = *reserved;
    }
    if (process::copy_to_user(static_cast<u64>(response_addr), &delivered, sizeof(delivered)) != 0) {
      if (delivered.capability != INVALID_HANDLE)
        (void)proc->capabilities().discard_reserved(delivered.capability);
      return -errc::EFAULT;
    }
    if (delivered.capability != INVALID_HANDLE) {
      auto published = proc->capabilities().publish_reserved(delivered.capability);
      if (!published)
        return cap_error(published.error());
      call->response_cap.reset();
    }
    return static_cast<long>(response_size);
  }
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

long sys_ipc_receive(long endpoint, long request_addr, long reply_addr, long, long, long) noexcept {
  auto proc = caller_process();
  auto *thread = process::CfsScheduler::get_current_task();
  if (!proc || !thread || !process::g_scheduler)
    return -errc::ESRCH;
  auto looked = proc->capabilities().lookup(static_cast<Handle>(endpoint), capability::ObjectType::Receiver,
                                            capability::rights::RECEIVE);
  if (!looked)
    return cap_error(looked.error());
  auto channel = static_cast<Receiver *>((*looked).get())->channel;
  while (true) {
    auto call = channel->claim(thread);
    if (call) {
      auto reply = shared_ptr<capability::Object>::try_make<Reply>(ipc_allocate, channel, call);
      if (!reply) {
        channel->release_claim(call.get());
        return -errc::ENOMEM;
      }
      auto reply_handle = proc->capabilities().reserve(reply, capability::rights::SEND | capability::rights::TRANSFER);
      if (!reply_handle) {
        channel->release_claim(call.get());
        return cap_error(reply_handle.error());
      }
      const Handle handle = *reply_handle;
      ControlMessage delivered{};
      delivered.size = call->request_size;
      delivered.badge = call->badge;
      if (call->request_size)
        __builtin_memcpy(delivered.payload, call->request, call->request_size);
      if (call->request_cap) {
        delivered.rights = call->request_cap.rights();
        auto installed = proc->capabilities().reserve_escrow(call->request_cap);
        if (!installed) {
          (void)proc->capabilities().discard_reserved(handle);
          channel->release_claim(call.get());
          return cap_error(installed.error());
        }
        delivered.capability = *installed;
      }
      if (process::copy_to_user(static_cast<u64>(request_addr), &delivered, sizeof(delivered)) != 0 ||
          process::copy_to_user(static_cast<u64>(reply_addr), &handle, sizeof(handle)) != 0) {
        if (delivered.capability != INVALID_HANDLE)
          (void)proc->capabilities().discard_reserved(delivered.capability);
        (void)proc->capabilities().discard_reserved(handle);
        channel->release_claim(call.get());
        return -errc::EFAULT;
      }
      if (channel->receive(call.get())) {
        static_cast<Reply *>(reply.get())->armed.store(true, memory_order_release);
        auto published = proc->capabilities().publish_reserved(handle, delivered.capability);
        if (!published) {
          (void)channel->complete(call.get(), Outcome::PeerClosed);
          (void)proc->capabilities().discard_reserved(delivered.capability);
          (void)proc->capabilities().discard_reserved(handle);
          return cap_error(published.error());
        }
        call->request_cap.reset();
        return static_cast<long>(call->request_size);
      }
      if (delivered.capability != INVALID_HANDLE)
        (void)proc->capabilities().discard_reserved(delivered.capability);
      (void)proc->capabilities().discard_reserved(handle);
      continue;
    }
    if (channel->closed()) {
      channel->release_receiver(thread);
      return -errc::EPIPE;
    }
    if (moss::abi::bridge::moss_io_wait_interrupted()) {
      channel->release_receiver(thread);
      return -errc::EINTR;
    }
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    (void)moss::abi::bridge::moss_prepare_io_wait();
    channel->arm_receiver_wait(thread);
    process::g_scheduler->commit_sleep();
    if (restore_irqs)
      arch::enable_interrupts();
  }
}

long sys_ipc_reply(long reply_handle, long response_addr, long, long, long, long) noexcept {
  auto proc = caller_process();
  if (!proc)
    return -errc::ESRCH;
  auto looked = proc->capabilities().lookup(static_cast<Handle>(reply_handle), capability::ObjectType::Reply,
                                            capability::rights::SEND);
  if (!looked)
    return cap_error(looked.error());
  ControlMessage response{};
  if (process::copy_from_user(&response, static_cast<u64>(response_addr), sizeof(response)) != 0)
    return -errc::EFAULT;
  if (!valid_message(response))
    return -errc::EINVAL;
  auto *reply = static_cast<Reply *>((*looked).get());
  // Receive arms the one-shot token before its reserved handle becomes visible.
  if (!reply->armed.load(memory_order_acquire))
    return -errc::EAGAIN;
  // A Reply cannot carry itself in its response: the call would then own its
  // own Reply through response escrow and neither object could be retired.
  if (response.capability == static_cast<Handle>(reply_handle))
    return -errc::EINVAL;
  capability::IpcCapture capture;
  capability::Escrow transferred;
  if (response.capability != INVALID_HANDLE) {
    auto captured = proc->capabilities().capture_for_ipc(response.capability, static_cast<u32>(response.rights));
    if (!captured)
      return cap_error(captured.error());
    capture = moss::move(*captured);
    transferred = capture.take_escrow();
  }
  // Consume table authority before completing the call. A concurrent
  // transfer may retain the object, but only one side may spend its handle.
  auto consumed = proc->capabilities().close(static_cast<Handle>(reply_handle));
  if (!consumed)
    return cap_error(consumed.error());
  // A cached Reply outcome does not mean this invocation won the one-shot
  // transition against cancellation or deadline expiry.
  bool committed = false;
  const Outcome result = reply->channel->complete(reply->call.get(), Outcome::Reply, response.payload,
                                                  static_cast<usize>(response.size), &committed, &transferred);
  if (result == Outcome::Reply && committed) {
    capture.commit();
    return 0;
  }
  return result == Outcome::Expired ? -errc::ETIMEDOUT : -errc::EPIPE;
}

} // namespace moss::kernel::syscall::handlers
