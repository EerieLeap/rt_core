#include "domain/can_monitor_domain/transmit/can_session_transmitter.h"

#include <algorithm>
#include <new>
#include <utility>

#include <zephyr/drivers/can.h>

#include "subsys/canbus/canbus_time.h"
#include "subsys/threading/scoped_mutex.h"
#include "utilities/memory/memory_resource_manager.h"

namespace eerie_leap::domain::can_monitor_domain::transmit {

using models::CAN_TX_FLAG_BRS;
using models::CAN_TX_FLAG_FDF;
using models::CAN_TX_FLAG_IDE;
using models::CAN_TX_FLAG_RTR;
using models::CAN_TX_FLAGS_MASK;
using models::CAN_TX_MAX_DATA_SIZE;
using models::CanMonitorArmState;
using models::CanMonitorError;
using models::CanMonitorPeriodic;
using models::CanMonitorReplayOp;
using models::CanMonitorReplayRequest;
using models::CanMonitorSendResult;
using models::CanMonitorTxFrame;
using utilities::CanTxEvent;

using eerie_leap::subsys::canbus::Canbus;
using eerie_leap::subsys::canbus::CanbusConfig;
using eerie_leap::subsys::canbus::CanbusType;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::NowUs;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;

namespace {

/// Frames the replay sends in one run of its work item, so that the queue serves the others in between.
constexpr int MAX_REPLAY_FRAMES_PER_RUN = 32;

/// The shortest wait the work items ask the queue for.
constexpr int64_t MIN_WAIT_US = 1;

k_timeout_t WaitFor(int64_t us) {
    return K_USEC(std::max(us, MIN_WAIT_US));
}

void CancelSync(k_work_delayable& work) {
    k_work_sync sync{};
    k_work_cancel_delayable_sync(&work, &sync);
}

} // namespace

CanSessionTransmitter::CanSessionTransmitter(Canbus& bus, uint8_t port, k_work_q* queue, EventSink sink) :
    bus_(bus),
    port_(port),
    queue_(queue),
    sink_(std::move(sink)) {

    k_mutex_init(&lock_);

    replay_work_.owner = this;
    k_work_init_delayable(&replay_work_.work, ReplayHandler);
    bus_off_work_.owner = this;
    k_work_init_delayable(&bus_off_work_.work, BusOffHandler);
    skip_work_.owner = this;
    k_work_init_delayable(&skip_work_.work, SkipHandler);
}

CanSessionTransmitter::~CanSessionTransmitter() {
    Shutdown();
}

void CanSessionTransmitter::Shutdown() {
    Disarm(CanTxEvent::DISARMED);

    CancelSync(bus_off_work_.work);
    CancelSync(replay_work_.work);
    CancelSync(skip_work_.work);
}

bool CanSessionTransmitter::IsArmed() {
    const ScopedMutex guard(lock_);

    return is_armed_;
}

void CanSessionTransmitter::OnBusState(can_state state) {
    if(state != CAN_STATE_BUS_OFF) {
        is_bus_off_.store(false, std::memory_order_relaxed);
        return;
    }

    // The receive thread only flags it; the work queue stops what is running.
    is_bus_off_.store(true, std::memory_order_relaxed);
    k_work_schedule_for_queue(queue_, &bus_off_work_.work, K_NO_WAIT);
}

void CanSessionTransmitter::BusOffHandler(k_work* work) {
    const Work* item = CONTAINER_OF(k_work_delayable_from_work(work), Work, work);

    item->owner->Disarm(CanTxEvent::BUS_OFF);
}

std::expected<void, CanMonitorError> CanSessionTransmitter::CheckReady() const {
    if(!is_armed_)
        return std::unexpected(CanMonitorError::NOT_ARMED);

    if(is_bus_off_.load(std::memory_order_relaxed))
        return std::unexpected(CanMonitorError::BUS_OFF);

    return {};
}

// A frame as the bus would take it, or INVALID: the rules of Canbus::SendFrame(const CanFrame&), checked
// here so that a request is refused whole before anything of it is sent. A remote frame's length is the
// number of bytes given, which are not sent.
std::expected<CanFrame, CanMonitorError> CanSessionTransmitter::BuildFrame(const CanMonitorTxFrame& tx) const {
    const bool is_extended = (tx.flags & CAN_TX_FLAG_IDE) != 0;
    const bool is_fd = (tx.flags & CAN_TX_FLAG_FDF) != 0;
    const bool is_brs = (tx.flags & CAN_TX_FLAG_BRS) != 0;
    const bool is_remote = (tx.flags & CAN_TX_FLAG_RTR) != 0;

    if((tx.flags & ~CAN_TX_FLAGS_MASK) != 0 || tx.size > CAN_TX_MAX_DATA_SIZE)
        return std::unexpected(CanMonitorError::INVALID);

    if(tx.id > (is_extended ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK) || (is_brs && !is_fd) || (is_fd && !is_fd_channel_))
        return std::unexpected(CanMonitorError::INVALID);

    CanFrame frame;
    frame.id = tx.id;
    frame.is_extended = is_extended;
    frame.is_can_fd = is_fd;
    frame.is_bitrate_switch = is_brs;
    frame.is_remote_request = is_remote;

    if(is_remote) {
        if(is_fd || tx.size > CAN_MAX_DLC)
            return std::unexpected(CanMonitorError::INVALID);

        frame.dlc = tx.size;

        return frame;
    }

    if(!is_fd && tx.size > CAN_MAX_DLC)
        return std::unexpected(CanMonitorError::INVALID);

    // An FD length is one of the DLC's: 0 to 8, 12, 16, 20, 24, 32, 48, 64.
    const uint8_t dlc = can_bytes_to_dlc(tx.size);
    if(can_dlc_to_bytes(dlc) != tx.size)
        return std::unexpected(CanMonitorError::INVALID);

    frame.dlc = dlc;
    frame.data.Assign(std::span<const uint8_t>(tx.data.data(), tx.size));

    return frame;
}

std::expected<CanMonitorArmState, CanMonitorError>
CanSessionTransmitter::Arm(bool on, const eerie_leap::domain::canbus_domain::models::CanbusComConfiguration& com) {
    if(!on)
        Disarm(CanTxEvent::DISARMED);

    const ScopedMutex guard(lock_);

    if(on && !is_armed_) {
        if(is_bus_off_.load(std::memory_order_relaxed))
            return std::unexpected(CanMonitorError::BUS_OFF);

        // The cap is bus time at the bus's bitrates, so a bus that is still detecting its bitrate cannot be armed.
        const CanbusConfig config = bus_.GetConfig();

        // The transmitter learns of bus-off from state changes; a bus that is off already says so now.
        can_state state = CAN_STATE_ERROR_ACTIVE;
        if(can_get_state(config.canbus_dev, &state, nullptr) == 0 && state == CAN_STATE_BUS_OFF)
            return std::unexpected(CanMonitorError::BUS_OFF);

        if(!bus_.IsBitrateDetected() || config.bitrate == 0)
            return std::unexpected(CanMonitorError::BAD_BITRATE);

        is_fd_channel_ = config.type == CanbusType::CANFD;
        limiter_.Reset(
            config.bitrate, is_fd_channel_ ? config.data_bitrate : 0, is_fd_channel_,
            CanTxLimiter::DEFAULT_SHARE_PERCENT, CanTxLimiter::DEFAULT_BURST_FRAMES, static_cast<int64_t>(NowUs())
        );
        reserved_ = CanReservedIds(port_, com);
        is_armed_ = true;
    }

    CanMonitorArmState state;
    state.is_armed = is_armed_;
    state.share_pct = CanTxLimiter::DEFAULT_SHARE_PERCENT;
    if(is_armed_) {
        for(const CanReservedRange& range : reserved_.Ranges())
            state.reserved.push_back({ range.from, range.to, range.ext });
    }

    return state;
}

std::expected<CanMonitorSendResult, CanMonitorError>
CanSessionTransmitter::Send(std::span<const CanMonitorTxFrame> frames) {
    if(frames.empty() || frames.size() > MAX_SEND_FRAMES)
        return std::unexpected(CanMonitorError::INVALID);

    const ScopedMutex guard(lock_);

    if(const auto ready = CheckReady(); !ready)
        return std::unexpected(ready.error());

    std::vector<CanFrame> built;
    try {
        built.reserve(frames.size());
    } catch(const std::bad_alloc&) {
        return std::unexpected(CanMonitorError::NO_MEMORY);
    }

    for(const CanMonitorTxFrame& frame : frames) {
        auto converted = BuildFrame(frame);
        if(!converted)
            return std::unexpected(converted.error());

        built.push_back(*converted);
    }

    for(const CanFrame& frame : built) {
        if(reserved_.IsReserved(frame.id, frame.is_extended))
            return std::unexpected(CanMonitorError::RESERVED);
    }

    uint32_t sent = 0;
    bool is_capped = false;
    for(const CanFrame& frame : built) {
        limiter_.Update(static_cast<int64_t>(NowUs()));
        if(!limiter_.TryTake(limiter_.CostNs(frame))) {
            is_capped = true;
            break;
        }

        if(bus_.SendFrame(frame) != 0)
            break;

        ++sent;
    }

    if(sent == 0)
        return std::unexpected(is_capped ? CanMonitorError::RATE : CanMonitorError::UNKNOWN);

    return CanMonitorSendResult{ sent, static_cast<uint32_t>(built.size()) - sent };
}

std::expected<void, CanMonitorError> CanSessionTransmitter::SetPeriodic(std::span<const CanMonitorPeriodic> list) {
    {
        const ScopedMutex guard(lock_);

        if(const auto ready = CheckReady(); !ready)
            return ready;
    }

    if(list.size() > CONFIG_EERIE_LEAP_CAN_MONITOR_PERIODIC_SLOTS)
        return std::unexpected(CanMonitorError::INVALID);

    std::vector<std::unique_ptr<PeriodicSlot>> fresh;
    try {
        fresh.reserve(list.size());

        for(const CanMonitorPeriodic& entry : list) {
            if(entry.period_ms == 0 || std::ranges::any_of(fresh, [&entry](const auto& slot) {
                   return slot->slot == entry.slot;
               }))
                return std::unexpected(CanMonitorError::INVALID);

            auto frame = BuildFrame(entry.frame);
            if(!frame)
                return std::unexpected(frame.error());

            auto slot = std::make_unique<PeriodicSlot>();
            slot->work.owner = this;
            slot->work.slot = slot.get();
            k_work_init_delayable(&slot->work.work, PeriodicHandler);
            slot->slot = entry.slot;
            slot->frame = *frame;
            slot->period_us = static_cast<uint64_t>(entry.period_ms) * 1000;
            slot->has_count = entry.count.has_value();
            slot->remaining = entry.count.value_or(0);
            // A count of 0 sends nothing.
            slot->is_active = !slot->has_count || slot->remaining > 0;
            fresh.push_back(std::move(slot));
        }
    } catch(const std::bad_alloc&) {
        return std::unexpected(CanMonitorError::NO_MEMORY);
    }

    for(const auto& slot : fresh) {
        if(reserved_.IsReserved(slot->frame.id, slot->frame.is_extended))
            return std::unexpected(CanMonitorError::RESERVED);
    }

    // The old slots stop first: a work item that runs meanwhile finds its slot inactive.
    std::vector<std::unique_ptr<PeriodicSlot>> old;
    {
        const ScopedMutex guard(lock_);

        for(const auto& slot : slots_)
            slot->is_active = false;
        old = std::move(slots_);
        slots_.clear();
    }
    for(const auto& slot : old)
        CancelSync(slot->work.work);
    old.clear();

    const ScopedMutex guard(lock_);

    // Disarmed or bus-off meanwhile: the new ones do not start.
    if(const auto ready = CheckReady(); !ready)
        return ready;

    const uint64_t now_us = NowUs();
    for(auto& slot : fresh) {
        slot->due_us = now_us;
        if(slot->is_active)
            k_work_schedule_for_queue(queue_, &slot->work.work, K_NO_WAIT);
    }
    slots_ = std::move(fresh);

    return {};
}

void CanSessionTransmitter::PeriodicHandler(k_work* work) {
    const Work* item = CONTAINER_OF(k_work_delayable_from_work(work), Work, work);

    item->owner->RunPeriodic(*static_cast<PeriodicSlot*>(item->slot));
}

void CanSessionTransmitter::RunPeriodic(PeriodicSlot& slot) {
    const ScopedMutex guard(lock_);

    // Bus-off is the bus-off work's to handle: it disarms.
    if(!is_armed_ || !slot.is_active || is_bus_off_.load(std::memory_order_relaxed))
        return;

    const uint64_t now_us = NowUs();
    limiter_.Update(static_cast<int64_t>(now_us));

    if(limiter_.TryTake(limiter_.CostNs(slot.frame)) && bus_.SendFrame(slot.frame, K_NO_WAIT) == 0) {
        if(slot.has_count && --slot.remaining == 0) {
            slot.is_active = false;
            return;
        }
    } else {
        NoteSkipped(now_us, 1);
    }

    // A fixed rate; a slot that fell a whole period behind does not make up for it with a burst.
    slot.due_us += slot.period_us;
    if(slot.due_us <= now_us)
        slot.due_us = now_us + slot.period_us;

    k_work_reschedule_for_queue(queue_, &slot.work.work, WaitFor(static_cast<int64_t>(slot.due_us - now_us)));
}

// A frame the cap held back, or a replay frame that the reserved IDs or the bus refused. Counted, and
// reported once a second at most.
void CanSessionTransmitter::NoteSkipped(uint64_t now_us, uint64_t count) {
    skipped_ += count;

    const int64_t since_report_us = static_cast<int64_t>(now_us - last_skip_report_us_);
    if(!has_reported_skips_ || since_report_us >= SKIP_REPORT_PERIOD_US) {
        ReportSkipped(now_us);
        return;
    }

    // Too soon after the last report: the work item sends the count when the second is over, whether or not
    // another frame is skipped (scheduling one already waiting changes nothing).
    k_work_schedule_for_queue(queue_, &skip_work_.work, WaitFor(SKIP_REPORT_PERIOD_US - since_report_us));
}

void CanSessionTransmitter::SkipHandler(k_work* work) {
    const Work* item = CONTAINER_OF(k_work_delayable_from_work(work), Work, work);

    item->owner->RunSkipReport();
}

void CanSessionTransmitter::RunSkipReport() {
    const ScopedMutex guard(lock_);

    ReportSkipped(NowUs());
}

void CanSessionTransmitter::ReportSkipped(uint64_t now_us) {
    if(skipped_ == 0)
        return;

    sink_(CanTxEvent::FRAMES_SKIPPED, skipped_);
    skipped_ = 0;
    last_skip_report_us_ = now_us;
    has_reported_skips_ = true;
}

void CanSessionTransmitter::ScheduleReplay(k_timeout_t delay) {
    k_work_reschedule_for_queue(queue_, &replay_work_.work, delay);
}

void CanSessionTransmitter::ReplayHandler(k_work* work) {
    const Work* item = CONTAINER_OF(k_work_delayable_from_work(work), Work, work);

    item->owner->RunReplay();
}

void CanSessionTransmitter::RunReplay() {
    const ScopedMutex guard(lock_);

    if(!is_replay_running_ || !replay_ || !is_armed_ || is_bus_off_.load(std::memory_order_relaxed))
        return;

    bool should_reschedule = false;
    k_timeout_t delay = K_NO_WAIT;

    for(int played = 0;; ++played) {
        const uint64_t now_us = NowUs();

        // Nothing waits: the app has to send the next chunk, or the replay is over.
        const std::optional<uint64_t> due_us = replay_->NextDueUs();
        if(!due_us.has_value())
            break;

        if(*due_us > now_us) {
            delay = WaitFor(static_cast<int64_t>(*due_us - now_us));
            should_reschedule = true;
            break;
        }

        if(played >= MAX_REPLAY_FRAMES_PER_RUN) {
            should_reschedule = true;
            break;
        }

        const std::optional<CanReplayFrame> next = replay_->Peek();
        if(!next.has_value())
            break;

        // A frame over the cap waits for the credit it needs.
        limiter_.Update(static_cast<int64_t>(now_us));
        const uint64_t cost_ns = limiter_.CostNs(next->frame);
        if(!limiter_.TryTake(cost_ns)) {
            delay = WaitFor(limiter_.WaitUs(cost_ns));
            should_reschedule = true;
            break;
        }

        const std::optional<CanReplayFrame> popped = replay_->Pop(now_us);
        if(!popped.has_value())
            break;

        if(reserved_.IsReserved(popped->frame.id, popped->frame.is_extended)
            || bus_.SendFrame(popped->frame, K_NO_WAIT) != 0)
            NoteSkipped(now_us, 1);
    }

    while(reported_underruns_ < replay_->Underruns()) {
        ++reported_underruns_;
        sink_(CanTxEvent::REPLAY_UNDERRUN, reported_underruns_);
    }

    if(replay_->IsDone()) {
        sink_(CanTxEvent::REPLAY_DONE, replay_->FramesPlayed());
        replay_.reset();
        is_replay_running_ = false;
        return;
    }

    if(should_reschedule)
        ScheduleReplay(delay);
}

// Ends the replay and frees its buffer; no event.
void CanSessionTransmitter::StopReplay() {
    {
        const ScopedMutex guard(lock_);
        is_replay_running_ = false;
    }

    CancelSync(replay_work_.work);

    std::unique_ptr<CanReplayBuffer> old;
    {
        const ScopedMutex guard(lock_);
        old = std::move(replay_);
    }
}

std::expected<uint32_t, CanMonitorError> CanSessionTransmitter::Replay(const CanMonitorReplayRequest& request) {
    constexpr uint32_t CAPACITY = CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_BUFFER_SIZE;

    switch(request.op) {
        case CanMonitorReplayOp::START: {
            {
                const ScopedMutex guard(lock_);

                if(const auto ready = CheckReady(); !ready)
                    return std::unexpected(ready.error());
            }

            if(request.speed_pct < CanReplayBuffer::MIN_SPEED_PCT || request.speed_pct > CanReplayBuffer::MAX_SPEED_PCT)
                return std::unexpected(CanMonitorError::INVALID);

            StopReplay();

            std::unique_ptr<CanReplayBuffer> buffer = CanReplayBuffer::Create(Mrm::GetExtPmr());
            if(!buffer)
                return std::unexpected(CanMonitorError::NO_MEMORY);

            buffer->Start(request.speed_pct, request.is_loop, NowUs());

            const ScopedMutex guard(lock_);

            if(const auto ready = CheckReady(); !ready)
                return std::unexpected(ready.error());

            replay_ = std::move(buffer);
            is_replay_running_ = true;
            reported_underruns_ = 0;

            return static_cast<uint32_t>(replay_->Room());
        }

        case CanMonitorReplayOp::CHUNK: {
            const ScopedMutex guard(lock_);

            if(const auto ready = CheckReady(); !ready)
                return std::unexpected(ready.error());

            if(!replay_ || !is_replay_running_)
                return std::unexpected(CanMonitorError::BAD_SEQ);

            switch(replay_->AddChunk(request.seq, request.data, NowUs())) {
                case CanReplayAdd::OK:
                    ScheduleReplay(K_NO_WAIT);
                    return static_cast<uint32_t>(replay_->Room());
                case CanReplayAdd::NO_ROOM:
                    return std::unexpected(CanMonitorError::NO_ROOM);
                case CanReplayAdd::MALFORMED:
                    return std::unexpected(CanMonitorError::INVALID);
                case CanReplayAdd::BAD_STATE:
                case CanReplayAdd::BAD_SEQ:
                    break;
            }

            return std::unexpected(CanMonitorError::BAD_SEQ);
        }

        case CanMonitorReplayOp::STOP:
            StopReplay();

            return CAPACITY;

        case CanMonitorReplayOp::FINISH: {
            const ScopedMutex guard(lock_);

            if(const auto ready = CheckReady(); !ready)
                return std::unexpected(ready.error());

            if(!replay_ || !is_replay_running_)
                return std::unexpected(CanMonitorError::BAD_SEQ);

            replay_->Finish();
            ScheduleReplay(K_NO_WAIT);

            return static_cast<uint32_t>(replay_->Room());
        }
    }

    return std::unexpected(CanMonitorError::INVALID);
}

// Stops the periodic frames and the replay, and reports what was not sent. Only an armed transmitter has
// anything to stop.
void CanSessionTransmitter::Disarm(CanTxEvent reason) {
    uint64_t not_sent = 0;
    uint64_t skipped = 0;
    std::vector<std::unique_ptr<PeriodicSlot>> old_slots;

    {
        const ScopedMutex guard(lock_);

        if(!is_armed_)
            return;

        is_armed_ = false;

        for(const auto& slot : slots_) {
            slot->is_active = false;
            if(slot->has_count)
                not_sent += slot->remaining;
        }
        old_slots = std::move(slots_);
        slots_.clear();

        if(replay_ && is_replay_running_)
            not_sent += replay_->PendingFrames();
        is_replay_running_ = false;

        skipped = skipped_;
        skipped_ = 0;
    }

    // The work items may be running; they find their slot inactive and leave.
    for(const auto& slot : old_slots)
        CancelSync(slot->work.work);
    old_slots.clear();

    StopReplay();
    CancelSync(skip_work_.work);

    if(skipped != 0)
        sink_(CanTxEvent::FRAMES_SKIPPED, skipped);

    sink_(reason, not_sent);
}

} // namespace eerie_leap::domain::can_monitor_domain::transmit
