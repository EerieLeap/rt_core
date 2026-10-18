#include <algorithm>
#include <exception>

#include <zephyr/logging/log.h>

#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "reading_pipeline.hpp"

#include "indicator_conditioner.h"

// A held change settles at an absolute uptime tick, which the kernel only takes with 64-bit timeouts.
#ifndef CONFIG_TIMEOUT_64BIT
    #error "The indicator conditioner needs CONFIG_TIMEOUT_64BIT for absolute deadlines."
#endif

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;

LOG_MODULE_REGISTER(indicator_conditioner_logger);

IndicatorConditioner::IndicatorConditioner(uint32_t debounce_ms, IndicatorInputMode input_mode, bool is_switch_on) :
    debounce_ticks_(static_cast<k_ticks_t>(k_ms_to_ticks_ceil64(debounce_ms))),
    input_mode_(input_mode),
    is_switch_on_(is_switch_on) {

    settle_work_.conditioner = this;
    k_work_init_delayable(&settle_work_.work, SettleWorkHandler);
}

// The owner has cancelled already (SensorsProcessingService::DoStop()); this covers a generation
// that never ran under one.
IndicatorConditioner::~IndicatorConditioner() {
    CancelSettle();
}

bool IndicatorConditioner::Offer(ReadingPipeline& pipeline, const SensorRuntime& runtime, SensorReading& reading) {
    // An error, or a reading without a value, says nothing about the input: it is committed as it
    // is and leaves the window and the switch alone.
    if(reading.HasError() || !reading.value.has_value())
        return true;

    // Any number but 0 is on: a post-process script may have replaced the expression's 0 or 1.
    const bool input = reading.value.value() != 0.0F;

    // Nothing to settle against at the start of a generation, and no press either: a button held
    // while processing starts does not flip the switch.
    if(!has_input_) {
        has_input_ = true;
        input_ = input;
        Apply(reading);

        return true;
    }

    // Back at the conditioned input before the window ended: the change never happened. Offer()
    // runs on the queue the settle runs on, so it is not running, and a cancel takes an expired
    // one off the queue.
    if(input == input_) {
        if(has_pending_) {
            has_pending_ = false;
            k_work_cancel_delayable(&settle_work_.work);
        }

        Apply(reading);

        return true;
    }

    const int64_t now = k_uptime_ticks();

    // Further readings of the pending value do not restart the window. A queue that refuses the
    // settle is being torn down with the generation.
    if(!has_pending_) {
        has_pending_ = true;
        deadline_ = now + debounce_ticks_;

        k_work_q* work_queue = pipeline.GetWorkQueue();
        if(deadline_ > now && work_queue != nullptr)
            k_work_reschedule_for_queue(work_queue, &settle_work_.work, K_TIMEOUT_ABS_TICKS(deadline_));
    }

    // No window, or a reading after it ended: a polled sample, or one queued ahead of the settle.
    if(now >= deadline_) {
        has_pending_ = false;
        k_work_cancel_delayable(&settle_work_.work);

        Accept(input);
        Apply(reading);

        return true;
    }

    held_ = reading;
    held_.can_frame = nullptr; // Only valid during the source's call.
    pipeline_ = &pipeline;
    runtime_ = &runtime;

    return false;
}

void IndicatorConditioner::CancelSettle() {
    k_work_cancel_delayable_sync(&settle_work_.work, &settle_sync_);

    // Nothing runs on the conditioner now, so the next reading starts a fresh window.
    has_pending_ = false;
}

void IndicatorConditioner::SuspendSettle() {
    k_work_cancel_delayable_sync(&settle_work_.work, &settle_sync_);

    // A window that ended before the settle ran has nothing left and settles at once on resume.
    if(has_pending_)
        remaining_ticks_ = std::max<int64_t>(deadline_ - k_uptime_ticks(), 0);
}

// The held change stays pending, so the first reading after the resume confirms or cancels it as
// any reading does; a source that sends nothing more settles it on the timer.
void IndicatorConditioner::ResumeSettle() {
    if(!has_pending_)
        return;

    deadline_ = k_uptime_ticks() + remaining_ticks_;

    k_work_q* work_queue = pipeline_ != nullptr ? pipeline_->GetWorkQueue() : nullptr;
    if(work_queue != nullptr) {
        k_work_reschedule_for_queue(
            work_queue, &settle_work_.work, remaining_ticks_ > 0 ? K_TIMEOUT_ABS_TICKS(deadline_) : K_NO_WAIT
        );
    }
}

void IndicatorConditioner::SettleWorkHandler(k_work* work) {
    auto* settle_work = CONTAINER_OF(k_work_delayable_from_work(work), SettleWork, work);
    settle_work->conditioner->Settle();
}

// The input has held the pending value for the window. The commit carries the time it settled,
// which keeps the sensor's timestamps in order after an error committed during the window.
// Exceptions must not unwind into the work queue's C dispatch.
void IndicatorConditioner::Settle() noexcept {
    // A reading that confirmed or cancelled the change could not take a settle that was
    // already running off the queue.
    if(!has_pending_)
        return;

    has_pending_ = false;
    Accept(!input_);

    try {
        SensorReading reading = held_;
        reading.timestamp = pipeline_->GetCurrentTime();
        Apply(reading);

        pipeline_->Commit(*runtime_, reading);
    } catch(const std::exception& e) {
        LOG_ERR("Sensor %s: settled change not committed: %s", runtime_->GetSensor().id.c_str(), e.what());
    } catch(...) {
        LOG_ERR("Sensor %s: settled change not committed.", runtime_->GetSensor().id.c_str());
    }
}

void IndicatorConditioner::Accept(bool input) {
    if(IsSwitch() && input && !input_)
        is_switch_on_ = !is_switch_on_;

    input_ = input;
}

// The value only: raw_value keeps what the source read.
void IndicatorConditioner::Apply(SensorReading& reading) const {
    reading.value = (IsSwitch() ? is_switch_on_ : input_) ? 1.0F : 0.0F;
}

} // namespace eerie_leap::domain::sensor_domain::processors
