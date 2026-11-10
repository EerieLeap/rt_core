#include "can_monitor_sender.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <span>

#include <zephyr/logging/log.h>

#include "domain/can_monitor_domain/utilities/can_notification_cutter.h"
#include "domain/can_monitor_domain/utilities/can_record_reader.h"
#include "domain/can_monitor_domain/utilities/can_record_writer.h"
#include "subsys/threading/scoped_mutex.h"

namespace eerie_leap::domain::can_monitor_domain::services {

LOG_MODULE_DECLARE(can_monitor_logger, CONFIG_EERIE_LEAP_CAN_MONITOR_LOG_LEVEL);

using namespace eerie_leap::domain::can_monitor_domain::utilities;
using eerie_leap::subsys::threading::ScopedMutex;

namespace {

constexpr int64_t SECOND_MS = 1000;
/// Notifications one pass sends at most, so a busy stream can't keep the thread from looking at its events.
constexpr int MAX_SENDS_PER_PASS = 32;
/// A refused notify with some of ours in flight waits for a completion; this is the fallback.
constexpr int64_t COMPLETION_FALLBACK_MS = 100;

constexpr CanStatistics Subtract(const CanStatistics& now, const CanStatistics& before) {
    CanStatistics delta;
    delta.frames_received = now.frames_received - before.frames_received;
    delta.frames_sent = now.frames_sent - before.frames_sent;
    delta.busy_us = now.busy_us - before.busy_us;
    delta.filtered = now.filtered - before.filtered;
    for(size_t i = 0; i < delta.errors.size(); ++i)
        delta.errors[i] = now.errors[i] - before.errors[i];
    for(size_t i = 0; i < delta.lost.size(); ++i)
        delta.lost[i] = now.lost[i] - before.lost[i];

    return delta;
}

} // namespace

CanMonitorSender::CanMonitorSender(CanMonitorLink link) :
    link_(std::move(link)) {
    k_mutex_init(&slots_lock_);
}

CanMonitorSender::~CanMonitorSender() {
    Stop();
    thread_.reset();
}

bool CanMonitorSender::Start() {
    if(thread_ && thread_->IsRunning())
        return true;

    is_stopping_ = false;

    if(!thread_) {
        thread_ = std::make_unique<Thread>(
            "can_monitor_tx", this, CONFIG_EERIE_LEAP_CAN_MONITOR_SENDER_STACK_SIZE,
            CONFIG_EERIE_LEAP_CAN_MONITOR_SENDER_PRIORITY, false
        );

        if(!thread_->Initialize()) {
            thread_.reset();
            return false;
        }
    }

    return thread_->Start();
}

void CanMonitorSender::Stop() {
    if(!thread_)
        return;

    is_stopping_ = true;
    wake_.Wake();
    thread_->Join();
}

bool CanMonitorSender::IsRunning() const {
    return thread_ && thread_->IsRunning();
}

void CanMonitorSender::Attach(std::unique_ptr<CanMonitorSession> session) {
    auto slot = std::make_unique<Slot>();
    slot->running_us = session->GetStartTimeUs();
    slot->next_statistics_ms = k_uptime_get() + STATISTICS_PERIOD_MS;
    slot->session = std::move(session);

    {
        const ScopedMutex guard(slots_lock_);
        slots_.push_back(std::move(slot));
    }

    wake_.Wake();
}

void CanMonitorSender::Discard(const CanMonitorSession* session) {
    const ScopedMutex guard(slots_lock_);

    for(auto& slot : slots_) {
        if(slot->session.get() == session)
            slot->is_done = true;
    }

    Sweep();
}

void CanMonitorSender::End(const CanMonitorSession* session, CanEndReason reason) {
    {
        const ScopedMutex guard(slots_lock_);

        for(auto& slot : slots_) {
            if(slot->session.get() != session || slot->is_ending)
                continue;

            slot->is_ending = true;
            slot->end_reason = reason;
            slot->drain_deadline_ms = k_uptime_get() + DRAIN_TIMEOUT_MS;
        }
    }

    wake_.Wake();
}

size_t CanMonitorSender::GetSessionCount() {
    const ScopedMutex guard(slots_lock_);

    return slots_.size();
}

void CanMonitorSender::ReleaseInFlight() {
    uint32_t current = in_flight_.load(std::memory_order_relaxed);
    while(current > 0 && !in_flight_.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
}

void CanMonitorSender::OnNotificationSent() {
    ReleaseInFlight();
    retry_allowed_.store(true, std::memory_order_relaxed);
    wake_.Wake();
}

void CanMonitorSender::OnDisconnected() {
    was_disconnected_.store(true, std::memory_order_relaxed);
    wake_.Wake();
}

size_t CanMonitorSender::GetNotifyMax() const {
    if(!link_.notifier)
        return 0;

    return std::min(link_.notifier->GetMaxNotificationSize(), MAX_NOTIFICATION_SIZE);
}

bool CanMonitorSender::IsSmpBusy() const {
    return link_.is_smp_busy && link_.is_smp_busy();
}

// Called with slots_lock_ held. Notifications in flight never report after a disconnect, a built one has nowhere to go, and a session
// that is ending can't drain.
void CanMonitorSender::HandleEvents() {
    if(!was_disconnected_.exchange(false, std::memory_order_relaxed))
        return;

    in_flight_.store(0, std::memory_order_relaxed);
    built_.is_valid = false;

    for(auto& slot : slots_) {
        if(slot->is_ending)
            slot->is_done = true;
    }

    Sweep();
}

void CanMonitorSender::Sweep() {
    for(auto& slot : slots_) {
        if(slot->is_done && built_.slot == slot.get())
            built_.is_valid = false;
    }

    std::erase_if(slots_, [](const std::unique_ptr<Slot>& slot) {
        return slot->is_done;
    });
}

void CanMonitorSender::ThreadEntry() {
    while(!is_stopping_.load(std::memory_order_relaxed)) {
        int64_t timeout_ms = 0;

        try {
            timeout_ms = RunPass();
        } catch(const std::exception& e) {
            LOG_ERR("CAN monitor sender: %s", e.what());
            timeout_ms = IDLE_POLL_MS;
        }

        k_sem_take(&wake_.semaphore, K_MSEC(static_cast<int32_t>(timeout_ms)));
    }
}

// One look at every session; returns how long the thread may sleep until something is due.
int64_t CanMonitorSender::RunPass() {
    const ScopedMutex guard(slots_lock_);

    HandleEvents();

    const size_t notify_max = GetNotifyMax();
    wake_.fill_threshold.store(
        notify_max > CAN_NOTIFICATION_HEADER_SIZE ? static_cast<uint32_t>(notify_max - CAN_NOTIFICATION_HEADER_SIZE)
                                                  : UINT32_MAX,
        std::memory_order_relaxed
    );

    int64_t now = k_uptime_get();
    int64_t next = now + SECOND_MS;
    bool is_busy = false;

    // Nothing built can go to a link that is gone, and its frames stay in the ring.
    if(notify_max == 0)
        built_.is_valid = false;

    for(int sends = 0; sends < MAX_SENDS_PER_PASS; ++sends) {
        now = k_uptime_get();
        next = now + (slots_.empty() ? SECOND_MS : IDLE_POLL_MS);
        bool is_progress = false;

        if(built_.is_valid) {
            is_progress = RetryBuilt(now, next);
            if(built_.is_valid)
                break;
        }

        const bool can_send = CanSend(now, notify_max, next);
        const size_t count = slots_.size();
        for(size_t i = 0; i < count; ++i) {
            Slot& slot = *slots_[(round_robin_ + i) % count];
            if(slot.is_done)
                continue;

            is_progress |= Serve(slot, now, notify_max, can_send, next);

            // The link has no buffer: nothing else goes before this one.
            if(built_.is_valid)
                break;
        }

        ++round_robin_;
        Sweep();

        if(!is_progress)
            break;

        is_busy = sends + 1 == MAX_SENDS_PER_PASS;
    }

    if(built_.is_valid)
        next = std::min(next, retry_at_ms_);

    if(is_busy)
        return 0;

    return std::clamp<int64_t>(next - k_uptime_get(), 0, SECOND_MS);
}

bool CanMonitorSender::CanSend(int64_t now_ms, size_t notify_max, int64_t& next_ms) const {
    if(notify_max <= CAN_NOTIFICATION_HEADER_SIZE)
        return false;

    if(IsSmpBusy()) {
        next_ms = std::min(next_ms, now_ms + SMP_RECHECK_MS);
        return false;
    }

    return in_flight_.load(std::memory_order_relaxed) < CONFIG_EERIE_LEAP_CAN_MONITOR_IN_FLIGHT;
}

// @return whether anything happened that warrants another look right away.
bool CanMonitorSender::Serve(Slot& slot, int64_t now_ms, size_t notify_max, bool can_send, int64_t& next_ms) {
    ByteRing& ring = slot.session->GetRing();

    if(slot.is_ending) {
        // No link to drain into, or it has not drained in time.
        if(notify_max == 0 || slot.end_reason == CanEndReason::DISCONNECTED || now_ms >= slot.drain_deadline_ms) {
            slot.is_done = true;
            return true;
        }

        next_ms = std::min(next_ms, slot.drain_deadline_ms);
    } else if(notify_max > CAN_NOTIFICATION_HEADER_SIZE && slot.statistics_size == 0) {
        if(now_ms >= slot.next_statistics_ms)
            StageStatistics(slot, now_ms);
        else
            next_ms = std::min(next_ms, slot.next_statistics_ms);
    }

    const size_t waiting_bytes = ring.Size();
    if(waiting_bytes == 0 && slot.statistics_size == 0 && !slot.is_ending) {
        slot.first_waiting_ms = -1;
        return false;
    }

    if(slot.first_waiting_ms < 0)
        slot.first_waiting_ms = now_ms;

    const size_t capacity = notify_max > CAN_NOTIFICATION_HEADER_SIZE ? notify_max - CAN_NOTIFICATION_HEADER_SIZE : 0;
    const int64_t flush_at_ms = slot.first_waiting_ms + CONFIG_EERIE_LEAP_CAN_MONITOR_FLUSH_MS;
    const bool is_due = slot.is_ending || (capacity > 0 && waiting_bytes >= capacity) || now_ms >= flush_at_ms;

    if(!is_due) {
        next_ms = std::min(next_ms, flush_at_ms);
        return false;
    }

    if(!can_send)
        return false;

    const bool is_built = Build(slot, notify_max);
    if(!built_.is_valid)
        return is_built;

    return Submit(now_ms);
}

// The totals the session counted so far less those of the previous record; the cause arrays are in the
// statistics record's order.
CanStatistics CanMonitorSender::TakeStatistics(Slot& slot) {
    const CanMonitorSession& session = *slot.session;
    const CanRecordWriter& writer = session.GetWriter();

    CanStatistics totals = slot.reported;
    totals.frames_received = slot.received.Update(session.GetFramesReceived());
    totals.frames_sent = slot.sent.Update(session.GetFramesSent());
    totals.busy_us = slot.busy.Update(session.GetBusyUs());
    totals.filtered = slot.filtered.Update(session.GetFiltered());

    const auto lost = [&](CanGapCause cause) {
        return slot.lost[static_cast<size_t>(cause) - 1].Update(writer.LostCount(cause));
    };
    totals.lost[CanStatistics::LOST_CONTROLLER] = lost(CanGapCause::CONTROLLER);
    totals.lost[CanStatistics::LOST_RECEIVE_QUEUE] = lost(CanGapCause::RECEIVE_QUEUE);
    totals.lost[CanStatistics::LOST_RING] = lost(CanGapCause::RING) + slot.sender_lost;
    totals.lost[CanStatistics::LOST_ECHOES] = lost(CanGapCause::ECHO_POOL);

    const CanStatistics delta = Subtract(totals, slot.reported);
    slot.reported = totals;

    return delta;
}

void CanMonitorSender::StageStatistics(Slot& slot, int64_t now_ms) {
    slot.statistics_size = EncodeStatistics(slot.statistics_record, 0, TakeStatistics(slot));

    // A late pass doesn't owe the missed seconds.
    slot.next_statistics_ms += STATISTICS_PERIOD_MS;
    if(slot.next_statistics_ms <= now_ms)
        slot.next_statistics_ms = now_ms + STATISTICS_PERIOD_MS;
}

// Fills built_ from the slot: header, the staged statistics, whole ring records and, for a session that
// ends once the ring is empty, the final statistics and the end record. The ring is only read.
// @return whether a notification was built or ring records were skipped.
bool CanMonitorSender::Build(Slot& slot, size_t notify_max) {
    const size_t limit = std::min(notify_max, MAX_NOTIFICATION_SIZE);
    const size_t capacity = limit - CAN_NOTIFICATION_HEADER_SIZE;
    ByteRing& ring = slot.session->GetRing();

    Built& built = built_;
    built.is_valid = false;
    built.slot = &slot;
    built.ring_offset = 0;
    built.ring_bytes = 0;
    built.dt_sum_us = 0;
    built.is_final = false;

    size_t position = CAN_NOTIFICATION_HEADER_SIZE;
    size_t records = 0;
    bool is_skipped = false;

    if(slot.statistics_size != 0 && position + slot.statistics_size <= limit) {
        std::memcpy(&built.data[position], slot.statistics_record.data(), slot.statistics_size);
        position += slot.statistics_size;
        ++records;
        slot.statistics_size = 0;
    }

    // The producer has stopped: losses it never got to write as a gap go in behind its records.
    if(slot.is_ending)
        slot.session->FlushLossesAfterStop();

    CanNotificationCut cut;
    for(;;) {
        const size_t available = ring.Size();
        if(available == 0)
            break;

        const size_t room = limit - position;
        const size_t peeked = std::min(available, room + CAN_MAX_RECORD_SIZE);
        const std::span<uint8_t> window = std::span(scratch_).first(peeked);
        ring.Peek(0, window);

        cut = CanNotificationCutter::Cut(window, room, CAN_NOTIFICATION_MAX_RECORDS - records);

        if(cut.malformed && cut.records == 0) {
            // Cannot happen with a writer that puts whole records in: drop what is there and go on.
            LOG_ERR(
                "CAN monitor ring of session %u holds a malformed record; dropping %zu bytes", slot.session->GetToken(),
                available
            );
            ring.Consume(available);
            ++slot.sender_lost;
            is_skipped = true;
            cut = {};
            break;
        }

        if(cut.first_too_large) {
            // Larger than a whole notification: skipped here, at a boundary, its time still counted.
            if(cut.first_record_size > capacity) {
                ring.Consume(cut.first_record_size);
                slot.running_us += cut.first_record_dt_us;
                ++slot.sender_lost;
                is_skipped = true;
                continue;
            }

            // It only doesn't fit behind the records already in; the next notification takes it.
            cut = {};
        }

        break;
    }

    built.ring_offset = position;
    if(cut.records != 0) {
        std::memcpy(&built.data[position], scratch_.data(), cut.bytes);
        position += cut.bytes;
        records += cut.records;
        built.ring_bytes = cut.bytes;
        built.dt_sum_us = cut.dt_sum_us;
    }

    // The ring is empty with this notification: the final statistics record and the end record follow.
    if(slot.is_ending && ring.Size() == built.ring_bytes) {
        if(!slot.is_final_staged && slot.statistics_size == 0) {
            slot.statistics_size = EncodeStatistics(slot.statistics_record, 0, TakeStatistics(slot));
            slot.is_final_staged = true;
        }

        std::array<uint8_t, CAN_MAX_RECORD_SIZE> end_record{};
        const size_t end_size = EncodeEnd(end_record, 0, slot.end_reason);
        const size_t statistics_size = slot.statistics_size;
        const size_t needed = statistics_size != 0 ? 2 : 1;

        if(slot.is_final_staged
            && position + statistics_size + end_size <= limit
            && records + needed <= CAN_NOTIFICATION_MAX_RECORDS) {
            std::memcpy(&built.data[position], slot.statistics_record.data(), statistics_size);
            position += statistics_size;
            std::memcpy(&built.data[position], end_record.data(), end_size);
            position += end_size;
            records += needed;
            slot.statistics_size = 0;
            built.is_final = true;
        }
    }

    if(records == 0)
        return is_skipped;

    EncodeNotificationHeader(
        std::span(built.data), slot.session->GetPort(), slot.sequence, slot.running_us, static_cast<uint8_t>(records)
    );
    built.size = position;
    built.is_valid = true;

    return true;
}

// @return false when the link had no buffer: built_ stays for a retry.
bool CanMonitorSender::Submit(int64_t now_ms) {
    // Cleared before the attempt, so a completion that lands during it allows the next one.
    retry_allowed_.store(false, std::memory_order_relaxed);
    in_flight_.fetch_add(1, std::memory_order_relaxed);

    const int result = link_.notifier->Notify(std::span<const uint8_t>(built_.data).first(built_.size));
    if(result == 0) {
        Commit(now_ms);
        return true;
    }

    ReleaseInFlight();

    if(result == -ENOMEM) {
        retry_at_ms_ = now_ms
            + (in_flight_.load(std::memory_order_relaxed) == 0 ? NO_BUFFER_RETRY_MS : COMPLETION_FALLBACK_MS);
        return false;
    }

    Drop(now_ms);

    return true;
}

bool CanMonitorSender::RetryBuilt(int64_t now_ms, int64_t& next_ms) {
    if(!retry_allowed_.load(std::memory_order_relaxed) && now_ms < retry_at_ms_) {
        next_ms = std::min(next_ms, retry_at_ms_);
        return false;
    }

    if(IsSmpBusy()) {
        next_ms = std::min(next_ms, now_ms + SMP_RECHECK_MS);
        return false;
    }

    if(in_flight_.load(std::memory_order_relaxed) >= CONFIG_EERIE_LEAP_CAN_MONITOR_IN_FLIGHT)
        return false;

    return Submit(now_ms);
}

void CanMonitorSender::Commit(int64_t now_ms) {
    Slot& slot = *built_.slot;
    ByteRing& ring = slot.session->GetRing();

    ring.Consume(built_.ring_bytes);
    slot.running_us += built_.dt_sum_us;
    ++slot.sequence;
    slot.first_waiting_ms = (ring.Size() > 0 || slot.statistics_size > 0) ? now_ms : -1;

    if(built_.is_final)
        slot.is_done = true;

    built_.is_valid = false;
}

// The link refused the notification for good: its frames are gone, and `seq` moves on so the client sees
// the hole.
void CanMonitorSender::Drop(int64_t now_ms) {
    Slot& slot = *built_.slot;

    size_t frames = 0;
    for(size_t offset = 0; offset < built_.ring_bytes;) {
        const CanRecordInspection record = InspectRecord(
            std::span<const uint8_t>(built_.data).subspan(built_.ring_offset + offset, built_.ring_bytes - offset)
        );
        if(record.status != CanRecordStatus::OK)
            break;

        if(record.kind == CanRecordKind::FRAME || record.kind == CanRecordKind::CHANGED_BYTES)
            ++frames;

        offset += record.size;
    }

    slot.sender_lost += frames;
    LOG_WRN("CAN monitor dropped a notification of session %u (%zu frames)", slot.session->GetToken(), frames);

    Commit(now_ms);
}

} // namespace eerie_leap::domain::can_monitor_domain::services
