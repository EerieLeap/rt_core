#pragma once

#include <memory>
#include <optional>

#include <zephyr/spinlock.h>

#include "i_time_provider.h"
#include "i_time_service.h"

namespace eerie_leap::subsys::time {

class TimeService : public ITimeService {
private:
    std::shared_ptr<ITimeProvider> rtc_provider_;
    std::shared_ptr<ITimeProvider> boot_elapsed_time_provider_;

    // The offset is 64 bits wide and the 32-bit targets have no lock-free 64-bit atomics, so a spinlock
    // guards it and the flag together. Held for a copy only.
    mutable k_spinlock wall_clock_lock_{};
    std::chrono::system_clock::duration wall_clock_offset_{};
    bool is_wall_clock_set_ = false;

public:
    TimeService(std::shared_ptr<ITimeProvider> rtc_provider, std::shared_ptr<ITimeProvider> boot_elapsed_time_provider);

    static void Initialize();

    std::chrono::system_clock::time_point GetCurrentTime() override;
    std::chrono::system_clock::time_point GetTimeSinceBoot() override;

    /** Any thread, the MCUmgr work queue included: no allocation, no exceptions. */
    void SetWallClock(std::chrono::system_clock::time_point wall_now) override;
    [[nodiscard]] std::optional<std::chrono::system_clock::duration> GetWallClockOffset() const override;
};

} // namespace eerie_leap::subsys::time
