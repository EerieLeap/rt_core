#pragma once

#include <chrono>
#include <optional>

namespace eerie_leap::subsys::time {

class ITimeService {
public:
    virtual ~ITimeService() = default;

    std::chrono::system_clock::time_point virtual GetCurrentTime() = 0;
    std::chrono::system_clock::time_point virtual GetTimeSinceBoot() = 0;

    /**
     * Tells the service what real time it is now. The unit's clock (GetCurrentTime()) does not move:
     * the service keeps the difference as an offset, so nothing stamped with that clock steps. The
     * latest call wins. The default does nothing, for a service without a settable clock.
     */
    virtual void SetWallClock([[maybe_unused]] std::chrono::system_clock::time_point wall_now) {}

    /**
     * Real time minus the unit's clock, once SetWallClock() has been called since power-on; nullopt
     * until then (the default).
     */
    [[nodiscard]] virtual std::optional<std::chrono::system_clock::duration> GetWallClockOffset() const {
        return std::nullopt;
    }
};

} // namespace eerie_leap::subsys::time
