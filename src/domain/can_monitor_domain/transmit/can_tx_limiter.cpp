#include "domain/can_monitor_domain/transmit/can_tx_limiter.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "domain/can_monitor_domain/utilities/can_bit_time.h"

namespace eerie_leap::domain::can_monitor_domain::transmit {

using eerie_leap::subsys::canbus::CanFrame;
using utilities::CanBitTime;
using utilities::CanRecordFrame;

namespace {

constexpr int64_t NANOS_PER_MICRO = 1000;
constexpr int64_t SCALE = 100;
constexpr size_t MAX_CLASSICAL_PAYLOAD = 8;
constexpr size_t MAX_FD_PAYLOAD = 64;

/** @brief The scaled cost of a frame, never more than @p capacity. */
int64_t ScaledCost(uint64_t cost_ns, int64_t capacity) {
    // A cost this large is a bug elsewhere; capped before scaling so it cannot overflow.
    constexpr uint64_t MAX_COST_NS = static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / SCALE);

    return std::min(static_cast<int64_t>(std::min(cost_ns, MAX_COST_NS)) * SCALE, capacity);
}

} // namespace

CanTxLimiter::CanTxLimiter(
    uint32_t nominal_bitrate,
    uint32_t data_bitrate,
    bool is_fd,
    uint8_t share_percent,
    uint32_t burst_frames,
    int64_t now_us
) {
    if(nominal_bitrate == 0 || share_percent == 0 || share_percent > 100 || burst_frames == 0)
        throw std::invalid_argument("Invalid CAN transmit limiter parameters");

    nominal_bitrate_ = nominal_bitrate;
    data_bitrate_ = is_fd ? (data_bitrate == 0 ? nominal_bitrate : data_bitrate) : 0;

    // The largest frame the channel carries; a 29-bit ID is the longer one.
    const uint64_t largest_ns = is_fd
        ? CanBitTime::Compute(true, true, true, false, MAX_FD_PAYLOAD, nominal_bitrate_, data_bitrate_).time_ns
        : CanBitTime::Compute(true, false, false, false, MAX_CLASSICAL_PAYLOAD, nominal_bitrate_, 0).time_ns;

    rate_ = static_cast<int64_t>(share_percent) * NANOS_PER_MICRO;
    capacity_ = static_cast<int64_t>(burst_frames) * static_cast<int64_t>(largest_ns) * SCALE;

    Reset(now_us);
}

void CanTxLimiter::Reset(int64_t now_us) {
    credit_ = capacity_;
    last_update_us_ = now_us;
}

void CanTxLimiter::Reset(
    uint32_t nominal_bitrate,
    uint32_t data_bitrate,
    bool is_fd,
    uint8_t share_percent,
    uint32_t burst_frames,
    int64_t now_us
) {
    *this = CanTxLimiter(nominal_bitrate, data_bitrate, is_fd, share_percent, burst_frames, now_us);
}

void CanTxLimiter::Update(int64_t now_us) {
    if(rate_ == 0 || now_us <= last_update_us_)
        return;

    // Capped before multiplying, so a long idle period cannot overflow.
    const int64_t elapsed_us = std::min(now_us - last_update_us_, (capacity_ / rate_) + 1);
    credit_ = std::min(capacity_, credit_ + (elapsed_us * rate_));
    last_update_us_ = now_us;
}

bool CanTxLimiter::TryTake(uint64_t cost_ns) {
    if(rate_ == 0)
        return false;

    const int64_t cost = ScaledCost(cost_ns, capacity_);
    if(credit_ < cost)
        return false;

    credit_ -= cost;

    return true;
}

int64_t CanTxLimiter::WaitUs(uint64_t cost_ns) const {
    if(rate_ == 0)
        return std::numeric_limits<int64_t>::max();

    const int64_t cost = ScaledCost(cost_ns, capacity_);
    if(credit_ >= cost)
        return 0;

    return (cost - credit_ + rate_ - 1) / rate_;
}

uint64_t CanTxLimiter::CostNs(const CanFrame& frame) const {
    return CanBitTime::Compute(
        frame.is_extended, frame.is_can_fd, frame.is_bitrate_switch, frame.is_remote_request, frame.data.size(),
        nominal_bitrate_, data_bitrate_
    )
        .time_ns;
}

uint64_t CanTxLimiter::CostNs(const CanRecordFrame& frame) const {
    return CanBitTime::Compute(frame, nominal_bitrate_, data_bitrate_).time_ns;
}

} // namespace eerie_leap::domain::can_monitor_domain::transmit
