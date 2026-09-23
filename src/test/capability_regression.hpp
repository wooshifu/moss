#pragma once

#include "framework/ut_kernel.hpp"

namespace moss::test::capability_regression {

inline void process_handles() {
  namespace cap = moss::kernel::capability;
  using moss::kernel::ErrorCode;
  using moss::kernel::shared_ptr;

  struct TrackedObject final : cap::Object {
    unsigned &destructions;
    explicit TrackedObject(unsigned &count) : Object(cap::ObjectType::Endpoint), destructions(count) {}
    ~TrackedObject() override { ++destructions; }
  };

  unsigned destructions = 0;
  auto object = shared_ptr<cap::Object>::try_make<TrackedObject>(
      [](moss::kernel::usize size, moss::kernel::usize alignment) -> void * {
        auto storage = moss::kernel::mm::RuntimeHeapAllocator::allocate_aligned(size, alignment);
        return storage ? *storage : nullptr;
      },
      destructions);
  if (!boost::ut::expect(static_cast<bool>(object)))
    return;

  cap::Table sender, receiver;
  constexpr auto full_rights =
      cap::rights::SEND | cap::rights::RECEIVE | cap::rights::TRANSFER | cap::rights::DUPLICATE;
  auto original = sender.install(object, full_rights);
  if (!boost::ut::expect(static_cast<bool>(original)))
    return;
  auto send_only = sender.duplicate(*original, cap::rights::SEND);
  if (!boost::ut::expect(static_cast<bool>(send_only)))
    return;
  auto denied = sender.lookup(*send_only, cap::ObjectType::Endpoint, cap::rights::RECEIVE);
  boost::ut::expect(!denied && denied.error() == ErrorCode::PermissionDenied);
  auto amplified = sender.duplicate(*send_only, full_rights);
  boost::ut::expect(!amplified && amplified.error() == ErrorCode::PermissionDenied);
  auto wrong_type = sender.lookup(*original, cap::ObjectType::Reply, cap::rights::SEND);
  boost::ut::expect(!wrong_type && wrong_type.error() == ErrorCode::InvalidArgument);

  auto copied = sender.transfer_to(receiver, *original, cap::rights::SEND, false);
  if (!boost::ut::expect(static_cast<bool>(copied)))
    return;
  boost::ut::expect(static_cast<bool>(sender.lookup(*original, cap::ObjectType::Endpoint, cap::rights::SEND)));
  boost::ut::expect(static_cast<bool>(receiver.close(*copied)));

  auto moved = sender.transfer_to(receiver, *original, cap::rights::RECEIVE, true);
  if (!boost::ut::expect(static_cast<bool>(moved)))
    return;
  boost::ut::expect(!sender.lookup(*original, cap::ObjectType::Endpoint, 0));
  boost::ut::expect(static_cast<bool>(receiver.lookup(*moved, cap::ObjectType::Endpoint, cap::rights::RECEIVE)));
  boost::ut::expect(!receiver.lookup(*moved, cap::ObjectType::Endpoint, cap::rights::SEND));
  boost::ut::expect(!sender.transfer_to(receiver, *send_only, cap::rights::SEND, true));
  boost::ut::expect(static_cast<bool>(sender.close(*send_only)));
  boost::ut::expect(!sender.close(*send_only));
  boost::ut::expect(!sender.close(moss::kernel::INVALID_HANDLE));
  boost::ut::expect(static_cast<bool>(receiver.close(*moved)));

  cap::Table parent, child, occupied;
  auto kept = parent.install(object, full_rights);
  if (!boost::ut::expect(static_cast<bool>(kept)))
    return;
  auto dropped = parent.duplicate(*kept, cap::rights::SEND);
  if (!boost::ut::expect(static_cast<bool>(dropped)))
    return;
  auto denied_inherit = parent.set_inheritable(*dropped, true);
  boost::ut::expect(!denied_inherit && denied_inherit.error() == ErrorCode::PermissionDenied);
  boost::ut::expect(static_cast<bool>(parent.set_inheritable(*kept, true)));
  boost::ut::expect(static_cast<bool>(parent.clone_inheritable_to(child)));
  boost::ut::expect(static_cast<bool>(child.lookup(*kept, cap::ObjectType::Endpoint, cap::rights::SEND)));
  boost::ut::expect(!child.lookup(*dropped, cap::ObjectType::Endpoint, 0));
  auto after_fork = child.install(object, cap::rights::SEND);
  if (!boost::ut::expect(static_cast<bool>(after_fork)))
    return;
  boost::ut::expect(*after_fork > *dropped);
  child.close_uninheritable();
  boost::ut::expect(static_cast<bool>(child.lookup(*kept, cap::ObjectType::Endpoint, cap::rights::SEND)));
  boost::ut::expect(!child.lookup(*after_fork, cap::ObjectType::Endpoint, 0));
  auto occupied_handle = occupied.install(object, cap::rights::SEND);
  boost::ut::expect(static_cast<bool>(occupied_handle));
  auto refused = parent.clone_inheritable_to(occupied);
  boost::ut::expect(!refused && refused.error() == ErrorCode::AlreadyExists);
  boost::ut::expect(static_cast<bool>(occupied.close(*occupied_handle)));
  refused = parent.clone_inheritable_to(occupied);
  boost::ut::expect(!refused && refused.error() == ErrorCode::AlreadyExists);
  parent.close_uninheritable();
  boost::ut::expect(!parent.lookup(*dropped, cap::ObjectType::Endpoint, 0));
  boost::ut::expect(static_cast<bool>(parent.lookup(*kept, cap::ObjectType::Endpoint, cap::rights::SEND)));
  boost::ut::expect(static_cast<bool>(parent.close(*kept)));
  boost::ut::expect(static_cast<bool>(child.close(*kept)));
  object.reset();
  boost::ut::expect(destructions == 1);
}

} // namespace moss::test::capability_regression
