#pragma once

#include "service_state.h"

namespace eerie_leap::subsys::threading {

class IService {
public:
    virtual ~IService() = default;

    virtual bool Initialize() = 0;
    virtual bool Start() = 0;
    virtual bool Stop() = 0;

    // Suspending is optional; services that cannot suspend keep these defaults.
    virtual bool Pause() { return false; }
    virtual bool Resume() { return false; }
    [[nodiscard]] virtual bool IsPausable() const noexcept { return false; }

    [[nodiscard]] virtual ServiceState GetState() const noexcept = 0;

    [[nodiscard]] bool IsRunning() const noexcept { return GetState() == ServiceState::RUNNING; }
    [[nodiscard]] bool IsPaused() const noexcept { return GetState() == ServiceState::PAUSED; }
    [[nodiscard]] bool IsStopped() const noexcept { return GetState() == ServiceState::STOPPED; }
};

} // namespace eerie_leap::subsys::threading
