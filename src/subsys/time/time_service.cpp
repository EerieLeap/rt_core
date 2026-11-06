#include <zephyr/logging/log.h>

#include "time_service.h"

namespace eerie_leap::subsys::time {

LOG_MODULE_REGISTER(time_service_logger);

TimeService::TimeService(
    std::shared_ptr<ITimeProvider> rtc_provider,
    std::shared_ptr<ITimeProvider> boot_elapsed_time_provider
) :
    rtc_provider_(std::move(rtc_provider)),
    boot_elapsed_time_provider_(std::move(boot_elapsed_time_provider)) {}

void TimeService::Initialize() {
    LOG_INF("Time Service initialized");
}

std::chrono::system_clock::time_point TimeService::GetCurrentTime() {
    return rtc_provider_->GetTime();
}

std::chrono::system_clock::time_point TimeService::GetTimeSinceBoot() {
    return boot_elapsed_time_provider_->GetTime();
}

void TimeService::SetWallClock(std::chrono::system_clock::time_point wall_now) {
    // Read outside the lock: the provider may take its own locks.
    const auto offset = wall_now - GetCurrentTime();

    const k_spinlock_key_t key = k_spin_lock(&wall_clock_lock_);
    wall_clock_offset_ = offset;
    is_wall_clock_set_ = true;
    k_spin_unlock(&wall_clock_lock_, key);
}

std::optional<std::chrono::system_clock::duration> TimeService::GetWallClockOffset() const {
    const k_spinlock_key_t key = k_spin_lock(&wall_clock_lock_);
    const bool is_set = is_wall_clock_set_;
    const auto offset = wall_clock_offset_;
    k_spin_unlock(&wall_clock_lock_, key);

    if(!is_set)
        return std::nullopt;

    return offset;
}

} // namespace eerie_leap::subsys::time
