#pragma once

#include "framework/ut_kernel.hpp"

namespace moss::test::capability_regression {

inline void process_handles() {
  namespace cap = moss::kernel::capability;
  using moss::kernel::ErrorCode;
  using moss::kernel::shared_ptr;

  struct TrackedObject final : cap::Object {
    unsigned &destructions;
    unsigned &last_closes;
    TrackedObject(unsigned &destroyed, unsigned &closed)
        : Object(cap::ObjectType::Endpoint), destructions(destroyed), last_closes(closed) {}
    ~TrackedObject() override { ++destructions; }

  protected:
    void on_last_handle_closed() noexcept override { ++last_closes; }
  };

  auto allocate = [](moss::kernel::usize size, moss::kernel::usize alignment) -> void * {
    auto storage = moss::kernel::mm::RuntimeHeapAllocator::allocate_aligned(size, alignment);
    return storage ? *storage : nullptr;
  };
  unsigned destructions = 0, last_closes = 0;
  auto object = shared_ptr<cap::Object>::try_make<TrackedObject>(allocate, destructions, last_closes);
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
  boost::ut::expect(last_closes == 1);

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
  {
    auto held_lookup = child.lookup(*kept, cap::ObjectType::Endpoint, cap::rights::SEND);
    boost::ut::expect(static_cast<bool>(held_lookup));
    boost::ut::expect(static_cast<bool>(child.close(*kept)));
    boost::ut::expect(last_closes == 2);
  }
  object.reset();
  boost::ut::expect(destructions == 1);

  unsigned escrow_destructions = 0, escrow_last_closes = 0;
  auto escrow_object =
      shared_ptr<cap::Object>::try_make<TrackedObject>(allocate, escrow_destructions, escrow_last_closes);
  if (!boost::ut::expect(static_cast<bool>(escrow_object)))
    return;
  cap::Table source, destination;
  auto source_handle = source.install(escrow_object, full_rights);
  if (!boost::ut::expect(static_cast<bool>(source_handle)))
    return;
  auto escrow = source.snapshot_for_transfer(*source_handle, cap::rights::SEND);
  if (!boost::ut::expect(static_cast<bool>(escrow)))
    return;
  boost::ut::expect(static_cast<bool>(source.close(*source_handle)));
  boost::ut::expect(escrow_last_closes == 0);
  auto reserved = destination.reserve_escrow(*escrow);
  if (!boost::ut::expect(static_cast<bool>(reserved)))
    return;
  boost::ut::expect(!destination.lookup(*reserved, cap::ObjectType::Endpoint, cap::rights::SEND));
  boost::ut::expect(!destination.close(*reserved));
  auto rolled_back = destination.reserve_escrow(*escrow);
  if (!boost::ut::expect(static_cast<bool>(rolled_back)))
    return;
  boost::ut::expect(static_cast<bool>(destination.discard_reserved(*rolled_back)));
  boost::ut::expect(!destination.publish_reserved(*reserved, *rolled_back));
  boost::ut::expect(!destination.lookup(*reserved, cap::ObjectType::Endpoint, cap::rights::SEND));
  boost::ut::expect(!destination.publish_reserved(*rolled_back));
  boost::ut::expect(static_cast<bool>(destination.publish_reserved(*reserved)));
  boost::ut::expect(static_cast<bool>(destination.lookup(*reserved, cap::ObjectType::Endpoint, cap::rights::SEND)));
  boost::ut::expect(!destination.lookup(*reserved, cap::ObjectType::Endpoint, cap::rights::RECEIVE));
  boost::ut::expect(!destination.snapshot_for_transfer(*reserved, cap::rights::SEND));
  escrow->reset();
  boost::ut::expect(escrow_last_closes == 0);
  boost::ut::expect(static_cast<bool>(destination.close(*reserved)));
  boost::ut::expect(escrow_last_closes == 1);
  escrow_object.reset();
  boost::ut::expect(escrow_destructions == 1);
}

} // namespace moss::test::capability_regression
