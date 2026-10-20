#include <algorithm>

#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"

#include "image_confirmation.h"

namespace eerie_leap::subsys::dfu {

#ifdef CONFIG_EERIE_LEAP_DFU_LOG_LEVEL
LOG_MODULE_REGISTER(image_confirmation_logger, CONFIG_EERIE_LEAP_DFU_LOG_LEVEL);
#else
LOG_MODULE_REGISTER(image_confirmation_logger);
#endif

using threading::ScopedMutex;

ImageConfirmation::ImageConfirmation(
    IBootControl& boot_control,
    IWatchdog* watchdog,
    Clock clock,
    ImageConfirmationConfig config
) :
    boot_control_(boot_control),
    watchdog_(watchdog),
    clock_(std::move(clock)),
    config_(config) {

    k_mutex_init(&probes_mutex_);
}

uint32_t ImageConfirmation::NowMs() const {
    // The 32-bit count wraps after 49 days; IsReached() compares across the wrap.
    return static_cast<uint32_t>(clock_());
}

bool ImageConfirmation::IsReached(uint32_t now, uint32_t at) {
    return static_cast<int32_t>(now - at) >= 0;
}

int ImageConfirmation::ArmWatchdog() {
    int result = watchdog_->Install(config_.watchdog_timeout_ms);
    if(result != 0) {
        LOG_ERR("Installing the %u ms watchdog failed (%d).", config_.watchdog_timeout_ms, result);
        return result;
    }

    result = watchdog_->Start();
    if(result != 0) {
        LOG_ERR("Starting the watchdog failed (%d).", result);
        return result;
    }

    is_watchdog_armed_.store(true, std::memory_order_release);

    return 0;
}

void ImageConfirmation::DisableWatchdog() {
    if(!is_watchdog_armed_.load(std::memory_order_acquire))
        return;

    // Fed first, so that a failed disable leaves a full timeout for the retry on the next tick.
    if(const int result = watchdog_->Feed(); result != 0)
        LOG_ERR("Feeding the watchdog failed (%d).", result);

    if(const int result = watchdog_->Disable(); result != 0) {
        LOG_ERR("Disabling the watchdog failed (%d); it is fed until it can be disabled.", result);
        return;
    }

    is_watchdog_armed_.store(false, std::memory_order_release);
}

void ImageConfirmation::StandDown() {
    state_.store(DfuState::CONFIRMED, std::memory_order_release);
    DisableWatchdog();
}

bool ImageConfirmation::CheckAndRearmProbes() {
    ScopedMutex lock(probes_mutex_);

    bool all_ran = true;
    for(const ProbeEntry& entry : probes_) {
        if(entry.armed && !entry.probe->Check()) {
            LOG_ERR("The %s probe did not run within %u ms.", entry.probe->GetName(), GetTickIntervalMs());
            all_ran = false;
        }
    }

    if(!all_ran)
        return false;

    for(ProbeEntry& entry : probes_) {
        entry.probe->Arm();
        entry.armed = true;
    }

    return true;
}

int ImageConfirmation::Initialize() {
    if(boot_control_.IsConfirmed()) {
        state_.store(DfuState::CONFIRMED, std::memory_order_release);
        return 0;
    }

    int result = 0;
    if(watchdog_ == nullptr)
        LOG_WRN("The image is unconfirmed and the board has no watchdog: a failure will not reset the unit.");
    else
        result = ArmWatchdog();

    deadline_ms_ = NowMs() + config_.boot_deadline_ms;
    state_.store(DfuState::VERIFYING, std::memory_order_release);

    LOG_INF("Unconfirmed image: boot must complete within %u ms.", config_.boot_deadline_ms);

    return result;
}

void ImageConfirmation::AddProbe(IProbe& probe) {
    ScopedMutex lock(probes_mutex_);

    probes_.push_back({ .probe = &probe, .armed = false });
}

void ImageConfirmation::ReportBootComplete() {
    DfuState expected = DfuState::VERIFYING;
    if(state_.load(std::memory_order_acquire) != expected)
        return;

    const uint32_t now = NowMs();

    if(IsReached(now, deadline_ms_)) {
        if(state_.compare_exchange_strong(expected, DfuState::FAILED, std::memory_order_acq_rel))
            LOG_ERR(
                "Boot completed after the %u ms deadline; the watchdog is no longer fed.", config_.boot_deadline_ms
            );

        return;
    }

    // Published by the exchange below, for GetConfirmInMs().
    confirm_at_ms_.store(now + config_.confirm_delay_ms, std::memory_order_relaxed);

    if(state_.compare_exchange_strong(expected, DfuState::CONFIRMING, std::memory_order_acq_rel))
        LOG_INF("Boot complete: the image confirms itself in %u ms.", config_.confirm_delay_ms);
}

void ImageConfirmation::Tick() {
    DfuState state = state_.load(std::memory_order_acquire);

    if(state == DfuState::CONFIRMED) {
        // Only a watchdog that could not be disabled is left to do.
        DisableWatchdog();
        return;
    }

    if(state == DfuState::FAILED)
        return;

    if(boot_control_.IsConfirmed()) {
        LOG_INF("The image was confirmed by a client.");
        StandDown();
        return;
    }

    const uint32_t now = NowMs();

    if(state == DfuState::VERIFYING && IsReached(now, deadline_ms_)) {
        if(state_.compare_exchange_strong(state, DfuState::FAILED, std::memory_order_acq_rel)) {
            LOG_ERR("Boot did not complete within %u ms; the watchdog is no longer fed.", config_.boot_deadline_ms);
            return;
        }

        // ReportBootComplete() ran in between: the state is CONFIRMING (or FAILED, when it was late).
        if(state == DfuState::FAILED)
            return;
    }

    if(!CheckAndRearmProbes()) {
        state_.store(DfuState::FAILED, std::memory_order_release);
        LOG_ERR("The watchdog is no longer fed.");
        return;
    }

    if(is_watchdog_armed_.load(std::memory_order_acquire)) {
        if(const int result = watchdog_->Feed(); result != 0)
            LOG_ERR("Feeding the watchdog failed (%d).", result);
    }

    if(!config_.self_confirm || state_.load(std::memory_order_acquire) != DfuState::CONFIRMING)
        return;

    if(!IsReached(now, confirm_at_ms_.load(std::memory_order_relaxed)))
        return;

    if(const int result = boot_control_.Confirm(); result != 0) {
        LOG_ERR("Confirming the image failed (%d); retrying in %u ms.", result, GetTickIntervalMs());
        return;
    }

    LOG_INF("The image confirmed itself.");
    StandDown();
}

uint32_t ImageConfirmation::GetTickIntervalMs() const {
    return std::max<uint32_t>(config_.watchdog_timeout_ms / 4, 1);
}

bool ImageConfirmation::NeedsTick() const {
    switch(state_.load(std::memory_order_acquire)) {
        case DfuState::VERIFYING:
        case DfuState::CONFIRMING:
            return true;
        case DfuState::CONFIRMED:
            return is_watchdog_armed_.load(std::memory_order_acquire);
        case DfuState::FAILED:
            break;
    }

    return false;
}

DfuState ImageConfirmation::GetState() const {
    return state_.load(std::memory_order_acquire);
}

bool ImageConfirmation::IsConfirmed() const {
    return GetState() == DfuState::CONFIRMED;
}

std::optional<uint32_t> ImageConfirmation::GetConfirmInMs() const {
    if(!config_.self_confirm || state_.load(std::memory_order_acquire) != DfuState::CONFIRMING)
        return std::nullopt;

    const auto remaining = static_cast<int32_t>(confirm_at_ms_.load(std::memory_order_relaxed) - NowMs());

    return remaining > 0 ? static_cast<uint32_t>(remaining) : 0U;
}

} // namespace eerie_leap::subsys::dfu
