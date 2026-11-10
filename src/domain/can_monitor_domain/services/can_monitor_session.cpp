#include "can_monitor_session.h"

#include <algorithm>
#include <new>

#include <zephyr/drivers/can.h>

#include "domain/can_monitor_domain/utilities/can_bit_time.h"
#include "domain/can_monitor_domain/utilities/can_record.h"

namespace eerie_leap::domain::can_monitor_domain::services {

using namespace eerie_leap::domain::can_monitor_domain::utilities;

CanMonitorSession::CanMonitorSession(
    uint32_t token,
    uint8_t port,
    CanMonitorMode mode,
    CanbusType type,
    uint32_t bitrate,
    uint32_t data_bitrate,
    uint64_t start_time_us,
    CanMonitorWakeSignal& wake,
    std::unique_ptr<ByteRing> ring
) :
    token_(token),
    port_(port),
    mode_(mode),
    type_(type),
    start_time_us_(start_time_us),
    wake_(wake),
    ring_(std::move(ring)),
    writer_(*ring_, start_time_us),
    nominal_bitrate_(bitrate),
    data_bitrate_(data_bitrate),
    last_reported_bitrate_(bitrate) {}

std::unique_ptr<CanMonitorSession> CanMonitorSession::Create(
    uint32_t token,
    uint8_t port,
    CanMonitorMode mode,
    CanbusType type,
    uint32_t bitrate,
    uint32_t data_bitrate,
    uint64_t start_time_us,
    CanMonitorWakeSignal& wake,
    std::pmr::memory_resource* memory
) {
    auto ring = ByteRing::Create(CONFIG_EERIE_LEAP_CAN_MONITOR_RING_SIZE, memory);
    if(!ring)
        return nullptr;

    // The constructor is private, so make_unique can't reach it.
    return std::unique_ptr<CanMonitorSession>(new(std::nothrow)
            CanMonitorSession(token, port, mode, type, bitrate, data_bitrate, start_time_us, wake, std::move(ring)));
}

bool CanMonitorSession::SetFilters(std::span<const CanUnitFilter> filters) {
    if(filters.size() > filters_.size())
        return false;

    const k_spinlock_key_t key = k_spin_lock(&filter_lock_);
    std::ranges::copy(filters, filters_.begin());
    filter_count_ = filters.size();
    k_spin_unlock(&filter_lock_, key);

    return true;
}

void CanMonitorSession::BindBus(Canbus* bus) {
    bus_ = bus;
    if(bus == nullptr)
        return;

    device_ = bus->GetConfig().canbus_dev;
    last_rx_dropped_ = bus->GetRxDroppedCount();
    last_echo_lost_ = bus->GetTxEchoLostCount();
#ifdef CONFIG_CAN_STATS
    last_overruns_ = can_stats_get_rx_overruns(device_);
#endif
}

bool CanMonitorSession::PassesFilter(uint32_t id, bool ext) {
    const k_spinlock_key_t key = k_spin_lock(&filter_lock_);

    bool passes = filter_count_ == 0;
    for(size_t i = 0; !passes && i < filter_count_; ++i)
        passes = filters_[i].Matches(id, ext);

    k_spin_unlock(&filter_lock_, key);

    return passes;
}

// Losses the bus counted before the tap saw the frames: the receive queue, the echo pool and the
// controller. They wait in the writer and become gap records before the next record.
void CanMonitorSession::CollectLosses() {
    if(bus_ == nullptr)
        return;

    const uint32_t dropped = bus_->GetRxDroppedCount();
    if(dropped != last_rx_dropped_) {
        writer_.AddLoss(CanGapCause::RECEIVE_QUEUE, static_cast<uint32_t>(dropped - last_rx_dropped_));
        last_rx_dropped_ = dropped;
    }

    const uint32_t echoes = bus_->GetTxEchoLostCount();
    if(echoes != last_echo_lost_) {
        writer_.AddLoss(CanGapCause::ECHO_POOL, static_cast<uint32_t>(echoes - last_echo_lost_));
        last_echo_lost_ = echoes;
    }

#ifdef CONFIG_CAN_STATS
    const uint32_t overruns = can_stats_get_rx_overruns(device_);
    if(overruns != last_overruns_) {
        writer_.AddLoss(CanGapCause::CONTROLLER, static_cast<uint32_t>(overruns - last_overruns_));
        last_overruns_ = overruns;
    }
#endif
}

// Whole µs go to the counter the sender reads; the nanoseconds left over wait for the next frame, so the
// error stays under a µs however many frames add up.
void CanMonitorSession::AddBusTime(const CanRecordFrame& frame) {
    if(nominal_bitrate_ == 0)
        return;

    busy_remainder_ns_ += CanBitTime::Compute(frame, nominal_bitrate_, data_bitrate_).time_ns;
    if(busy_remainder_ns_ >= 1000) {
        busy_us_.fetch_add(static_cast<uint32_t>(busy_remainder_ns_ / 1000), std::memory_order_relaxed);
        busy_remainder_ns_ %= 1000;
    }
}

// The ring is empty or fills up to a notification: the cases in which the sender has something to do and
// may be asleep. Anything in between waits for the flush time, which the sender sleeps towards.
void CanMonitorSession::WakeSender(size_t size_before) {
    const size_t size_after = ring_->Size();
    if(size_after == 0)
        return;

    const uint32_t threshold = wake_.fill_threshold.load(std::memory_order_relaxed);
    if(size_before == 0 || (size_before < threshold && size_after >= threshold))
        wake_.Wake();
}

void CanMonitorSession::OnFrame(const CanFrame& frame, uint64_t time_us, bool is_transmit, int tx_error) {
    CollectLosses();

    CanRecordFrame record;
    record.id = frame.id;
    record.ide = frame.is_extended;
    record.fdf = frame.is_can_fd;
    record.brs = frame.is_can_fd && frame.is_bitrate_switch;
    record.rtr = frame.is_remote_request;
    record.esi = frame.is_error_state_indicator;
    record.tx = is_transmit;
    record.tx_failed = is_transmit && tx_error != 0;

    // A frame built for sending may leave the DLC 0.
    record.dlc = frame.dlc;
    if(record.dlc == 0 && !record.rtr && !frame.data.empty())
        record.dlc = can_bytes_to_dlc(static_cast<uint8_t>(std::min<size_t>(frame.data.size(), CAN_MAX_PAYLOAD_SIZE)));

    const size_t length = PayloadLength(record.dlc, record.fdf, record.rtr);
    record.payload = std::span<const uint8_t>(frame.data.data(), std::min<size_t>(length, frame.data.size()));

    // Bus load counts every frame on the bus, whatever the unit filter lets through. A send that failed
    // never completed on the wire.
    if(!record.tx_failed) {
        if(is_transmit)
            frames_sent_.fetch_add(1, std::memory_order_relaxed);
        else
            frames_received_.fetch_add(1, std::memory_order_relaxed);

        AddBusTime(record);
    }

    if(!PassesFilter(frame.id, frame.is_extended)) {
        filtered_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const size_t size_before = ring_->Size();
    writer_.WriteFrame(time_us, record);
    WakeSender(size_before);
}

void CanMonitorSession::OnState(can_state state, can_bus_err_cnt counters, uint64_t time_us) {
    CollectLosses();

    // The bus holds its lock while it calls the tap, and the lock is recursive for this thread.
    uint64_t detected_bitrate = 0;
    if(bus_ != nullptr && bus_->IsBitrateDetected()) {
        const auto config = bus_->GetConfig();
        if(config.bitrate != last_reported_bitrate_) {
            last_reported_bitrate_ = config.bitrate;
            nominal_bitrate_ = config.bitrate;
            data_bitrate_ = config.data_bitrate;
            detected_bitrate = config.bitrate;
        }
    }

    const size_t size_before = ring_->Size();
    writer_.WriteState(time_us, ToCanBusState(state), counters.tx_err_cnt, counters.rx_err_cnt, detected_bitrate);
    WakeSender(size_before);
}

void CanMonitorSession::OnFilterSwap(bool may_have_missed, uint64_t time_us) {
    CollectLosses();

    const size_t size_before = ring_->Size();
    writer_.WriteTap(time_us, may_have_missed ? CanTapMark::MAY_HAVE_MISSED : CanTapMark::MAY_HAVE_REPEATED);
    WakeSender(size_before);
}

} // namespace eerie_leap::domain::can_monitor_domain::services
