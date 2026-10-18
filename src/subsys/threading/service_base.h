#pragma once

#include <type_traits>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "i_service.h"
#include "scoped_mutex.h"

namespace eerie_leap::subsys::threading {

// Owns every ServiceState transition so implementations only provide the Do* work.
// TService allows extended interfaces (e.g. ICdmpCanbusService) to reuse it without an inheritance diamond.
// A DoStart() or DoStop() that throws leaves the state it found and the exception reaches the caller.
// Transitions may come from several threads (a configuration restart, a calibration pause), so each
// one holds transition_mutex_ from its state check to its new state: a Pause() that arrives during a
// restart waits for it and acts on what it left. The mutex is recursive for its owner, and a child
// service has its own, so a Do* that drives itself or its children cannot deadlock on it. Never call
// a transition from a thread the Do* work waits for.
template<typename TService = IService>
class ServiceBase : public TService {
    static_assert(std::is_base_of_v<IService, TService>, "TService must derive from IService.");

private:
    atomic_t service_state_ = ATOMIC_INIT(static_cast<atomic_val_t>(ServiceState::STOPPED));
    k_mutex transition_mutex_;

    void SetState(ServiceState state) noexcept {
        atomic_set(&service_state_, static_cast<atomic_val_t>(state));
    }

protected:
    virtual bool DoInitialize() {
        return true;
    }
    virtual bool DoStart() = 0;
    virtual bool DoStop() = 0;

    // Only reached when IsPausable() is overridden to return true.
    virtual bool DoPause() {
        return false;
    }
    virtual bool DoResume() {
        return false;
    }

public:
    ServiceBase() {
        k_mutex_init(&transition_mutex_);
    }

    [[nodiscard]] ServiceState GetState() const noexcept final {
        return static_cast<ServiceState>(atomic_get(&service_state_));
    }

    bool Initialize() final {
        const ScopedMutex guard(transition_mutex_);

        if(GetState() != ServiceState::STOPPED)
            return false;

        return DoInitialize();
    }

    bool Start() final {
        const ScopedMutex guard(transition_mutex_);

        const ServiceState previous_state = GetState();

        if(previous_state == ServiceState::RUNNING)
            return true;

        if(previous_state == ServiceState::STARTING)
            return false;

        SetState(ServiceState::STARTING);

        bool is_started = false;
        try {
            is_started = DoStart();
        } catch(...) {
            SetState(previous_state);
            throw;
        }

        if(!is_started) {
            SetState(previous_state);
            return false;
        }

        SetState(ServiceState::RUNNING);

        return true;
    }

    bool Stop() final {
        const ScopedMutex guard(transition_mutex_);

        const ServiceState previous_state = GetState();

        if(previous_state == ServiceState::STOPPED)
            return true;

        if(previous_state == ServiceState::STOPPING)
            return false;

        SetState(ServiceState::STOPPING);

        bool is_stopped = false;
        try {
            is_stopped = DoStop();
        } catch(...) {
            SetState(previous_state);
            throw;
        }

        if(!is_stopped) {
            SetState(previous_state);
            return false;
        }

        SetState(ServiceState::STOPPED);

        return true;
    }

    bool Pause() final {
        const ScopedMutex guard(transition_mutex_);

        if(!this->IsPausable() || GetState() != ServiceState::RUNNING)
            return false;

        if(!DoPause())
            return false;

        SetState(ServiceState::PAUSED);

        return true;
    }

    bool Resume() final {
        const ScopedMutex guard(transition_mutex_);

        if(!this->IsPausable() || GetState() != ServiceState::PAUSED)
            return false;

        if(!DoResume())
            return false;

        SetState(ServiceState::RUNNING);

        return true;
    }
};

} // namespace eerie_leap::subsys::threading
