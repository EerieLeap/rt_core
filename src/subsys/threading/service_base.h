#pragma once

#include <type_traits>

#include <zephyr/sys/atomic.h>

#include "i_service.h"

namespace eerie_leap::subsys::threading {

// Owns every ServiceState transition so implementations only provide the Do* work.
// TService allows extended interfaces (e.g. ICdmpCanbusService) to reuse it without an inheritance diamond.
template<typename TService = IService>
class ServiceBase : public TService {
    static_assert(std::is_base_of_v<IService, TService>, "TService must derive from IService.");

private:
    atomic_t service_state_ = ATOMIC_INIT(static_cast<atomic_val_t>(ServiceState::STOPPED));

    void SetState(ServiceState state) noexcept {
        atomic_set(&service_state_, static_cast<atomic_val_t>(state));
    }

protected:
    virtual bool DoInitialize() { return true; }
    virtual bool DoStart() = 0;
    virtual bool DoStop() = 0;

    // Only reached when IsPausable() is overridden to return true.
    virtual bool DoPause() { return false; }
    virtual bool DoResume() { return false; }

public:
    [[nodiscard]] ServiceState GetState() const noexcept final {
        return static_cast<ServiceState>(atomic_get(&service_state_));
    }

    bool Initialize() final {
        if(GetState() != ServiceState::STOPPED)
            return false;

        return DoInitialize();
    }

    bool Start() final {
        const ServiceState previous_state = GetState();

        if(previous_state == ServiceState::RUNNING)
            return true;

        if(previous_state == ServiceState::STARTING)
            return false;

        SetState(ServiceState::STARTING);

        if(!DoStart()) {
            SetState(previous_state);
            return false;
        }

        SetState(ServiceState::RUNNING);

        return true;
    }

    bool Stop() final {
        const ServiceState previous_state = GetState();

        if(previous_state == ServiceState::STOPPED)
            return true;

        if(previous_state == ServiceState::STOPPING)
            return false;

        SetState(ServiceState::STOPPING);

        if(!DoStop()) {
            SetState(previous_state);
            return false;
        }

        SetState(ServiceState::STOPPED);

        return true;
    }

    bool Pause() final {
        if(!this->IsPausable() || GetState() != ServiceState::RUNNING)
            return false;

        if(!DoPause())
            return false;

        SetState(ServiceState::PAUSED);

        return true;
    }

    bool Resume() final {
        if(!this->IsPausable() || GetState() != ServiceState::PAUSED)
            return false;

        if(!DoResume())
            return false;

        SetState(ServiceState::RUNNING);

        return true;
    }
};

} // namespace eerie_leap::subsys::threading
