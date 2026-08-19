#pragma once

#include <zephyr/kernel.h>

#include "subsys/threading/scoped_mutex.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::threading::ScopedMutex;

// Deferred readings outlive their reader: the interrupt source can be torn down
// (service pause/stop, reconfiguration) while work is still queued. Work items
// therefore hold a shared reference to this guard instead of the reader, so a
// dispatch that loses the race becomes a no-op instead of a dangling access.
template<typename TReader>
class IsrDispatchGuard {
private:
    k_mutex lock_;
    k_sem throttle_;
    TReader* reader_;

public:
    explicit IsrDispatchGuard(TReader* reader) : reader_(reader) {
        k_mutex_init(&lock_);
        k_sem_init(&throttle_, 1, 1);
    }

    IsrDispatchGuard(const IsrDispatchGuard&) = delete;
    IsrDispatchGuard& operator=(const IsrDispatchGuard&) = delete;
    IsrDispatchGuard(IsrDispatchGuard&&) = delete;
    IsrDispatchGuard& operator=(IsrDispatchGuard&&) = delete;

    // Drops events arriving while the previous one is still being processed.
    [[nodiscard]] bool TryAcquire() { return k_sem_take(&throttle_, K_NO_WAIT) == 0; }
    void Release() { k_sem_give(&throttle_); }

    template<typename TDispatch>
    void Dispatch(TDispatch&& dispatch) {
        ScopedMutex guard(lock_);

        if(reader_ != nullptr)
            dispatch(*reader_);
    }

    // Blocks until an in-flight dispatch returns, every later one is a no-op.
    void Detach() {
        ScopedMutex guard(lock_);

        reader_ = nullptr;
    }
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
