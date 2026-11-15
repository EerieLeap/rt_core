#include "can_monitor_session.h"

#include <algorithm>
#include <new>

#include <zephyr/drivers/can.h>

#include "domain/can_monitor_domain/utilities/can_bit_time.h"
#include "domain/can_monitor_domain/utilities/can_record.h"
#include "subsys/threading/scoped_mutex.h"

namespace eerie_leap::domain::can_monitor_domain::services {

using namespace eerie_leap::domain::can_monitor_domain::utilities;
using eerie_leap::subsys::threading::ScopedMutex;

CanMonitorSession::CanMonitorSession(
    const CanMonitorSessionParameters& parameters,
    CanMonitorWakeSignal& wake,
    std::unique_ptr<ByteRing> ring
) :
    token_(parameters.token),
    port_(parameters.port),
    mode_(parameters.mode),
    type_(parameters.type),
    start_time_us_(parameters.start_time_us),
    wake_(wake),
    ring_(std::move(ring)),
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    delta_(parameters.is_delta ? CanDeltaTable::Create() : nullptr),
#endif
    writer_(*ring_, parameters.start_time_us, GetDeltaTable()),
    counters_(parameters.counters),
    nominal_bitrate_(parameters.bitrate),
    data_bitrate_(parameters.data_bitrate),
    last_reported_bitrate_(parameters.bitrate) {

    k_mutex_init(&totals_lock_);
}

CanDeltaTable* CanMonitorSession::GetDeltaTable() {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    return delta_.get();
#else
    return nullptr;
#endif
}

std::unique_ptr<CanMonitorSession> CanMonitorSession::Create(
    const CanMonitorSessionParameters& parameters,
    CanMonitorWakeSignal& wake,
    std::pmr::memory_resource* memory
) {
    auto ring = ByteRing::Create(CONFIG_EERIE_LEAP_CAN_MONITOR_RING_SIZE, memory);
    if(!ring)
        return nullptr;

    // The constructor is private, so make_unique can't reach it.
    std::unique_ptr<CanMonitorSession> session(new(std::nothrow) CanMonitorSession(parameters, wake, std::move(ring)));

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    if(session && parameters.is_delta && session->GetDeltaTable() == nullptr)
        return nullptr;
#endif

    return session;
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

    last_rx_dropped_ = bus->GetRxDroppedCount();
    last_echo_lost_ = bus->GetTxEchoLostCount();

    if(counters_) {
        error_baseline_ = counters_->Read();
        last_overruns_ = error_baseline_.rx_overruns;
    }
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
        writer_.AddLoss(CanGapCause::RECEIVE_QUEUE, dropped - last_rx_dropped_);
        last_rx_dropped_ = dropped;
    }

    const uint32_t echoes = bus_->GetTxEchoLostCount();
    if(echoes != last_echo_lost_) {
        writer_.AddLoss(CanGapCause::ECHO_POOL, echoes - last_echo_lost_);
        last_echo_lost_ = echoes;
    }

    if(counters_) {
        const uint32_t overruns = counters_->Read().rx_overruns;
        if(overruns != last_overruns_) {
            writer_.AddLoss(CanGapCause::CONTROLLER, overruns - last_overruns_);
            last_overruns_ = overruns;
        }
    }
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

    if(state_listener_ != nullptr)
        state_listener_->OnBusState(state);

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

CanStatistics CanMonitorSession::Totals() {
    const ScopedMutex guard(totals_lock_);

    CanStatistics totals;
    totals.frames_received = received_total_.Update(frames_received_.load(std::memory_order_relaxed));
    totals.frames_sent = sent_total_.Update(frames_sent_.load(std::memory_order_relaxed));
    totals.busy_us = busy_total_.Update(busy_us_.load(std::memory_order_relaxed));
    totals.filtered = filtered_total_.Update(filtered_.load(std::memory_order_relaxed));

    const auto lost = [this](CanGapCause cause) {
        return lost_totals_[static_cast<size_t>(cause) - 1].Update(writer_.LostCount(cause));
    };
    totals.lost[CanStatistics::LOST_CONTROLLER] = lost(CanGapCause::CONTROLLER);
    totals.lost[CanStatistics::LOST_RECEIVE_QUEUE] = lost(CanGapCause::RECEIVE_QUEUE);
    totals.lost[CanStatistics::LOST_RING] = lost(CanGapCause::RING) + discarded_;
    totals.lost[CanStatistics::LOST_ECHOES] = lost(CanGapCause::ECHO_POOL);

    if(counters_) {
        const CanErrorCounts now = counters_->Read();
        for(size_t i = 0; i < totals.errors.size(); ++i)
            totals.errors[i] = error_totals_[i].Update(now.errors[i] - error_baseline_.errors[i]);
    }

    return totals;
}

void CanMonitorSession::AddDiscarded(uint64_t frames) {
    const ScopedMutex guard(totals_lock_);

    discarded_ += frames;
}

} // namespace eerie_leap::domain::can_monitor_domain::services
