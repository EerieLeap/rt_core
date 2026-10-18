#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"

#include "rate_group_scheduler.h"

// Deadlines are absolute uptime ticks, which the kernel only takes with 64-bit timeouts.
#ifndef CONFIG_TIMEOUT_64BIT
    #error "The rate group scheduler needs CONFIG_TIMEOUT_64BIT for absolute deadlines."
#endif

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;
using eerie_leap::subsys::adc::AdcError;
using eerie_leap::subsys::threading::ScopedMutex;

LOG_MODULE_DECLARE(processing_service_logger);

RateGroupScheduler::RateGroupScheduler(
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<ReadingPipeline> pipeline,
    std::shared_ptr<ITimeService> time_service
) :
    work_queue_thread_(std::move(work_queue_thread)),
    pipeline_(std::move(pipeline)),
    time_service_(std::move(time_service)) {

    k_mutex_init(&groups_mutex_);
}

// The groups and their readers go after the tick that uses them.
RateGroupScheduler::~RateGroupScheduler() {
    Stop();
}

// What a tick of the group spends in the ADC drivers, from the per-conversion time each ADC's probe
// measured. Exact for the ESP32 oneshot driver; drivers that burst or scan cover several
// conversions per call, so it is an upper bound for them.
static uint32_t EstimateSamplingUs(const AdcSampleSet& adc_samples) {
    uint64_t estimate_us = 0;

    for(size_t i = 0; i < adc_samples.GetAdcCount(); i++) {
        const auto adc = adc_samples.GetAdc(i);
        estimate_us += static_cast<uint64_t>(adc.channels.size()) * adc.samples
            * adc.adc->GetCapabilities().us_per_conversion;
    }

    return static_cast<uint32_t>(std::min<uint64_t>(estimate_us, UINT32_MAX));
}

void RateGroupScheduler::SetGroups(std::vector<std::unique_ptr<RateGroup>> groups) {
    for(const auto& group : groups) {
        if(group == nullptr || group->period_ms <= 0)
            throw std::invalid_argument("A rate group needs a period of at least 1 ms.");
    }

    Stop();

    std::ranges::sort(groups, {}, [](const auto& group) {
        return group->period_ms;
    });

    for(auto& group : groups) {
        // In 64 bits: at 10 000 ticks/s a period above ~59 hours overflows the 32-bit conversion.
        group->period_ticks = static_cast<k_ticks_t>(k_ms_to_ticks_ceil64(static_cast<uint64_t>(group->period_ms)));

        // Deadlines are whole ticks, so a period that is not one runs slow by up to a tick.
        const uint64_t period_us = k_ticks_to_us_floor64(static_cast<uint64_t>(group->period_ticks));
        if(period_us != static_cast<uint64_t>(group->period_ms) * 1000) {
            LOG_WRN(
                "Rate group %d ms: runs every %llu us, the period is not a whole number of system ticks.",
                group->period_ms, static_cast<unsigned long long>(period_us)
            );
        }

        const uint32_t estimate_us = EstimateSamplingUs(group->adc_samples);
        LOG_INF(
            "Rate group %d ms: %zu sensors, %zu ADC channels, ~%u us of sampling per tick", group->period_ms,
            group->entries.size(), group->adc_samples.GetChannelCount(), estimate_us
        );

        // The other half is for the readers, the other groups and the threads above processing.
        if(estimate_us > static_cast<uint64_t>(group->period_ms) * 1000 / 2)
            LOG_WRN(
                "Rate group %d ms: ~%u us of sampling is more than half the period.", group->period_ms, estimate_us
            );
    }

    ScopedMutex guard(groups_mutex_);
    groups_ = std::move(groups);
}

void RateGroupScheduler::ClearGroups() {
    Stop();

    ScopedMutex guard(groups_mutex_);
    groups_.clear();
}

void RateGroupScheduler::Start() {
    // The owner's transitions are serialized, so nothing replaces the groups meanwhile; the lock
    // keeps the deadlines it writes off a concurrent stats read all the same.
    ScopedMutex guard(groups_mutex_);

    if(groups_.empty())
        return;

    if(!task_.has_value())
        task_.emplace(work_queue_thread_->CreateTask(ProcessTick, this));

    const int64_t now = k_uptime_ticks();
    for(auto& group : groups_) {
        group->next_deadline = now + group->period_ticks;
#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
        group->period_window.Restart();
#endif
    }

#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
    next_stats_log_ = now
        + static_cast<int64_t>(k_sec_to_ticks_ceil64(CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS_LOG_PERIOD_S));
#endif

    // The dispatcher reschedules with the last result when a tick throws. A new task has
    // reschedule = false there, and so has one whose tick raced the last Stop(): a throw in the
    // first tick would stop every group. Seeded, it is retried on the next system tick.
    const k_timeout_t first_deadline = K_TIMEOUT_ABS_TICKS(GetEarliestDeadline());
    task_->SetLastResult({ .reschedule = true, .delay = first_deadline });

    atomic_set(&is_running_, 1);
    task_->Reschedule(first_deadline);
}

void RateGroupScheduler::Stop() {
    atomic_clear(&is_running_);

    if(!task_.has_value())
        return;

    // A tick in progress finishes and, seeing the flag, schedules no other; the loop takes a
    // reschedule that raced the flag.
    while(task_->Cancel())
        k_sleep(K_MSEC(1));
}

size_t RateGroupScheduler::GetGroupCount() const {
    ScopedMutex guard(groups_mutex_);

    return groups_.size();
}

std::vector<RateGroupStatsSnapshot> RateGroupScheduler::GetStats() const {
    ScopedMutex guard(groups_mutex_);

    std::vector<RateGroupStatsSnapshot> stats;
    stats.reserve(groups_.size());
    for(const auto& group : groups_)
        stats.push_back(group->GetStatsSnapshot());

    return stats;
}

int64_t RateGroupScheduler::GetEarliestDeadline() const {
    int64_t earliest = INT64_MAX;
    for(const auto& group : groups_)
        earliest = std::min(earliest, group->next_deadline);

    return earliest;
}

WorkQueueTaskResult RateGroupScheduler::ProcessTick(RateGroupScheduler* scheduler) {
    return scheduler->Tick();
}

WorkQueueTaskResult RateGroupScheduler::Tick() {
    if(!atomic_get(&is_running_))
        return { .reschedule = false };

    const int64_t now = k_uptime_ticks();

    // Ascending period: at coincident deadlines the fastest group runs first.
    for(auto& group_ptr : groups_) {
        RateGroup& group = *group_ptr;
        if(group.next_deadline > now)
            continue;

        // Advanced before the group runs: should an exception escape the group, the dispatcher's
        // retry finds it not due rather than running it again ahead of its phase.
        group.next_deadline += group.period_ticks;

        RunGroup(group);

        // A tick that ran past the next deadline keeps the phase: the missed periods are skipped
        // and counted rather than run late, back to back.
        const int64_t after = k_uptime_ticks();
        if(group.next_deadline <= after) {
            const int64_t missed = (after - group.next_deadline) / group.period_ticks + 1;
            group.next_deadline += missed * group.period_ticks;
            group.stats.RecordOverruns(static_cast<uint32_t>(missed));
        }
    }

#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
    // After the due groups, so the log's own time never counts as a tick; it shows in the next
    // interval's period maximum instead.
    if(now >= next_stats_log_) {
        LogStats();
        next_stats_log_ = now
            + static_cast<int64_t>(
                k_sec_to_ticks_ceil64(CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS_LOG_PERIOD_S)
            );
    }
#endif

    // Stop() started while the groups ran: leave nothing behind for its cancel to chase.
    if(!atomic_get(&is_running_))
        return { .reschedule = false };

    // A deadline that passed while the groups ran is clamped to the next tick by the kernel.
    return { .reschedule = true, .delay = K_TIMEOUT_ABS_TICKS(GetEarliestDeadline()) };
}

#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
static RateGroupCycles GetRateGroupCycles() {
    #ifdef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
    return k_cycle_get_64();
    #else
    return k_cycle_get_32();
    #endif
}

static uint32_t CyclesToUs(uint64_t cycles) {
    return static_cast<uint32_t>(std::min<uint64_t>(k_cyc_to_us_floor64(cycles), UINT32_MAX));
}
#endif

void RateGroupScheduler::RunGroup(RateGroup& group) {
    const uint32_t start_cycles = k_cycle_get_32();

#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
    group.period_window.RecordStart(GetRateGroupCycles());
#endif

    // Every ADC channel of the group, back to back, before any reader looks at its slot.
    group.adc_samples.Acquire();

    const TickContext tick{ time_service_->GetCurrentTime() };

    for(auto& entry : group.entries) {
        const SensorRuntime& runtime = *entry.runtime;

        SensorReading reading(runtime.sensor.get());
        reading.source = ReadingSource::PROCESSING;
        reading.timestamp = tick.timestamp;

        try {
            reading = entry.reader->Read(tick);
        } catch(const std::exception& e) {
            reading.SetError(ReadingError::READER_FAILED);

            LOG_DBG("Error reading sensor: %s, Error: %s", runtime.GetSensor().id.c_str(), e.what());
        }

        pipeline_->Process(runtime, reading);

        if(reading.HasError())
            LOG_DBG("Sensor %s: %s", runtime.GetSensor().id.c_str(), ToString(reading.error).data());
    }

    // Unsigned subtraction: a wrap of the 32-bit counter within one tick is harmless.
    group.stats.RecordTick(k_cyc_to_us_floor32(k_cycle_get_32() - start_cycles));
}

#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
// Volts to the nearest millivolt through integer formatting, so the line does not depend on the
// C library's floating-point printf.
static int FormatChannel(char* buffer, size_t size, int channel, const AdcSample& sample) {
    if(sample.error != AdcError::NONE)
        return snprintf(buffer, size, " ch%d err %d", channel, sample.errno_value);

    const long millivolts = std::lround(sample.volts * 1000.0F);
    const long magnitude = millivolts < 0 ? -millivolts : millivolts;

    return snprintf(
        buffer, size, " ch%d %s%ld.%03ld V", channel, millivolts < 0 ? "-" : "", magnitude / 1000, magnitude % 1000
    );
}

size_t RateGroupScheduler::FormatChannels(const AdcSampleSet& adc_samples, size_t first, std::span<char> line) {

    if(line.empty())
        return first;

    line[0] = '\0';
    size_t length = 0;
    size_t index = 0;

    for(size_t i = 0; i < adc_samples.GetAdcCount(); i++) {
        const auto adc = adc_samples.GetAdc(i);

        for(size_t j = 0; j < adc.channels.size(); j++, index++) {
            if(index < first)
                continue;

            char item[48];
            const int written = FormatChannel(item, sizeof(item), adc.global_channels[j], adc.results[adc.channels[j]]);
            const size_t item_length = written > 0 ? std::min(static_cast<size_t>(written), sizeof(item) - 1) : 0;

            // The next channel goes on the next line; a line holds at least one.
            if(length > 0 && length + item_length >= line.size())
                return index;

            const size_t copied = std::min(item_length, line.size() - 1 - length);
            std::memcpy(line.data() + length, item, copied);
            length += copied;
            line[length] = '\0';
        }
    }

    return index;
}

// Ticks and overruns count from the group's start, the period from the previous log line. The
// channel line shows the latest result of every ADC channel the group samples by its global index,
// the number sensors are configured with, so a hardware check can see readings flow; a group with
// more channels than one line holds continues on further lines.
void RateGroupScheduler::LogStats() {
    for(auto& group_ptr : groups_) {
        RateGroup& group = *group_ptr;
        const RateGroupStatsSnapshot stats = group.GetStatsSnapshot();
        RateGroupPeriodWindow& window = group.period_window;

        const bool has_periods = window.periods > 0;
        LOG_INF(
            "Rate group %d ms: %u ticks, %u overruns, tick last/max %u/%u us, period avg/min/max %u/%u/%u us",
            group.period_ms, stats.ticks, stats.overruns, stats.last_us, stats.max_us,
            has_periods ? CyclesToUs(window.sum_cycles / window.periods) : 0,
            has_periods ? CyclesToUs(window.min_cycles) : 0, has_periods ? CyclesToUs(window.max_cycles) : 0
        );

        window.StartInterval();

        const size_t channel_count = group.adc_samples.GetChannelCount();
        for(size_t next = 0; next < channel_count;) {
            char line[128];
            next = FormatChannels(group.adc_samples, next, line);

            LOG_INF("Rate group %d ms:%s", group.period_ms, line);
        }
    }
}
#endif

} // namespace eerie_leap::domain::sensor_domain::services
