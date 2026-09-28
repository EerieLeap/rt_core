#include <algorithm>
#include <stdexcept>

#include "smp_can_pacer.h"

namespace eerie_leap::subsys::smp::can {

namespace {

constexpr int64_t NANOS_PER_MICRO = 1000;
constexpr int64_t MICROS_PER_SECOND = 1'000'000;
constexpr int64_t PERCENT = 100;

} // namespace

SmpCanPacer::SmpCanPacer(uint32_t frame_time_ns, uint8_t share_percent, uint32_t burst_frames) {
    if(frame_time_ns == 0 || share_percent == 0 || share_percent > 100 || burst_frames == 0)
        throw std::invalid_argument("Invalid SMP CAN pacing parameters");

    rate_ = static_cast<int64_t>(share_percent) * NANOS_PER_MICRO;
    frame_cost_ = static_cast<int64_t>(frame_time_ns) * PERCENT;
    capacity_ = static_cast<int64_t>(burst_frames) * frame_cost_;
}

void SmpCanPacer::Reset(int64_t now_us) {
    credit_ = capacity_;
    last_update_us_ = now_us;
}

void SmpCanPacer::Update(int64_t now_us) {
    if(rate_ == 0 || now_us <= last_update_us_)
        return;

    // Capped before multiplying, so a long idle period cannot overflow.
    const int64_t elapsed_us = std::min(now_us - last_update_us_, capacity_ / rate_ + 1);
    credit_ = std::min(capacity_, credit_ + elapsed_us * rate_);
    last_update_us_ = now_us;
}

void SmpCanPacer::OnSent() {
    credit_ -= frame_cost_;
}

int64_t SmpCanPacer::GetWaitUs(uint32_t frames) const {
    if(rate_ == 0)
        return INT64_MAX;

    const int64_t needed = std::min(capacity_, static_cast<int64_t>(std::max(frames, 1U)) * frame_cost_);
    if(credit_ >= needed)
        return 0;

    return (needed - credit_ + rate_ - 1) / rate_;
}

uint32_t SmpCanPacer::GetFramesPerSecond() const {
    if(frame_cost_ == 0)
        return 0;

    return static_cast<uint32_t>(rate_ * MICROS_PER_SECOND / frame_cost_);
}

} // namespace eerie_leap::subsys::smp::can
