#pragma once

#include <utility>

#include <zephyr/kernel.h>

#include "subsys/threading/scoped_mutex.h"

namespace eerie_leap::subsys::event_bus {

using eerie_leap::subsys::threading::ScopedMutex;

// EventChannel::Dispatch copies the matching handlers out from under its subscriber lock and only
// then invokes them, so unsubscribing cannot cancel a dispatch that was already snapshotted. A
// handler that can outlive its owner therefore holds a shared reference to this guard instead of
// the owner and runs its body through Dispatch(); the owner calls Detach() before it tears down,
// and a dispatch that loses the race against destruction becomes a no-op instead of a dangling
// access.
//
// Detach() waits for a body running on another thread. The lock is recursive, so never call it
// from inside this guard's own Dispatch() body: it would not wait, and the body would outlive its
// owner. Nor call it while holding a lock a body takes inside the guard, which deadlocks.
// Detaching from another subscriber's handler is fine, and so is holding a lock the handlers take
// before Dispatch(): UI owners take the LVGL lock in the handler and then this guard, so the order
// is always LvglLock, then this lock, and a widget detaches under the LVGL lock.
class DispatchGuard {
private:
    k_mutex lock_{};
    void* owner_;

public:
    explicit DispatchGuard(void* owner) :
        owner_(owner) {
        k_mutex_init(&lock_);
    }

    DispatchGuard(const DispatchGuard&) = delete;
    DispatchGuard& operator=(const DispatchGuard&) = delete;
    DispatchGuard(DispatchGuard&&) = delete;
    DispatchGuard& operator=(DispatchGuard&&) = delete;
    ~DispatchGuard() = default;

    template<typename TDispatch>
    void Dispatch(TDispatch&& dispatch) {
        ScopedMutex guard(lock_);

        if(owner_ != nullptr)
            std::forward<TDispatch>(dispatch)();
    }

    // Blocks until a dispatch in flight on another thread returns; every later one is a no-op.
    void Detach() {
        ScopedMutex guard(lock_);

        owner_ = nullptr;
    }
};

} // namespace eerie_leap::subsys::event_bus
