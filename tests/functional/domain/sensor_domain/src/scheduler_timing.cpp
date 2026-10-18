// The rate group scheduler against the clock: delivered periods, overruns, run order, one ADC
// request per tick and pause/resume. The timing suite runs on native_sim only, where code takes no
// simulated time: a tick costs exactly what its processor spends in k_busy_wait(), and every
// deadline lands on a tick edge, so periods and phases come out exact rather than statistical.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/processors/i_reading_processor.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/services/processing_scheduler_service.h"
#include "domain/sensor_domain/services/rate_group.h"
#include "domain/sensor_domain/services/rate_group_scheduler.h"
#include "subsys/threading/work_queue_thread.h"
#include "utilities/string/string_helpers.h"

#include "processing_test_support.h"

using namespace eerie_leap::utilities::string;
using namespace eerie_leap::subsys::threading;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::processors;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::services;

using namespace processing_test_support;

namespace {

struct Commit {
    uint32_t sensor_id_hash;
    int64_t timestamp_ms; // The tick's shared timestamp: uptime at the start of the group's tick.
};

// The last stage of the pipeline: records every reading in commit order, and makes one sensor's
// readings cost a fixed time, which is the tick's whole cost on native_sim. Processing thread
// only; the test reads it while the scheduler is paused or stopped, or while it preempts a tick
// that is past its commit.
class RecordingProcessor : public IReadingProcessor {
public:
    std::vector<Commit> commits;
    uint32_t costly_sensor_id_hash = 0;
    uint32_t cost_us = 0;
    bool throw_once = false; // The next reading throws out of the pipeline once it is recorded.

    RecordingProcessor() {
        commits.reserve(512);
    }

    void Process(const SensorRuntime& runtime, SensorReading& reading) override {
        const auto timestamp = reading.timestamp.value();

        if(commits.size() < commits.capacity()) {
            commits.push_back(
                { .sensor_id_hash = runtime.sensor->id_hash,
                    .timestamp_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count() }
            );
        }

        if(cost_us > 0 && runtime.sensor->id_hash == costly_sensor_id_hash)
            k_busy_wait(cost_us);

        if(throw_once) {
            throw_once = false;
            throw std::runtime_error("RecordingProcessor throws once.");
        }
    }

    [[nodiscard]] std::vector<int64_t> GetTimestamps(std::string_view sensor_id) const {
        const uint32_t id_hash = StringHelpers::GetHash(sensor_id);

        std::vector<int64_t> timestamps;
        for(const auto& commit : commits) {
            if(commit.sensor_id_hash == id_hash)
                timestamps.push_back(commit.timestamp_ms);
        }

        return timestamps;
    }
};

// A ProcessingSchedulerService on its own work queue over a hand-built generation, so the test sees
// the rate group stats and owns the pipeline. Members go in reverse: the service before the queue.
struct SchedulerRig {
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorGeneration> generation;
    std::shared_ptr<RecordingProcessor> recorder;
    std::shared_ptr<ReadingPipeline> pipeline;
    std::shared_ptr<WorkQueueThread> work_queue_thread;
    std::shared_ptr<ProcessingSchedulerService> scheduler_service;
};

SchedulerRig MakeSchedulerRig(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    std::shared_ptr<IAdcManager> adc_manager = nullptr,
    std::shared_ptr<const AdcConfiguration> adc_configuration = nullptr
) {

    SchedulerRig rig;
    rig.time_service = MakeTimeService();
    rig.frame = std::make_shared<SensorReadingsFrame>();

    rig.generation = SensorPipelineBuilder(nullptr, rig.frame)
                         .Build(std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));
    rig.generation->adc_configuration = std::move(adc_configuration);

    rig.recorder = std::make_shared<RecordingProcessor>();
    const auto processors = std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>();
    processors->push_back(rig.recorder);
    rig.pipeline = std::make_shared<ReadingPipeline>(rig.frame, processors, rig.time_service);

    rig.work_queue_thread = std::make_shared<WorkQueueThread>(
        "scheduler_timing", CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_STACK_SIZE,
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_PRIORITY
    );
    zassert_true(rig.work_queue_thread->Initialize());

    rig.scheduler_service = std::make_shared<ProcessingSchedulerService>(
        rig.time_service, std::make_shared<SensorReaderFactory>(rig.time_service, nullptr, std::move(adc_manager)),
        rig.work_queue_thread, rig.pipeline
    );
    zassert_true(rig.scheduler_service->Initialize());
    rig.scheduler_service->SetGeneration(rig.generation);

    return rig;
}

// Both helpers serve the native_sim timing suite only; the other boards build it out.
[[maybe_unused]] int64_t TicksToMs(int64_t ticks) {
    return static_cast<int64_t>(k_ticks_to_ms_floor64(static_cast<uint64_t>(ticks)));
}

// An absolute wake-up: a relative sleep would be rounded up by a tick and could land on a deadline.
// Every offset below falls between two deadlines, so what ran before it is never in doubt.
[[maybe_unused]] void SleepUntil(int64_t start_ticks, int offset_ms) {
    k_sleep(K_TIMEOUT_ABS_TICKS(start_ticks + static_cast<int64_t>(k_ms_to_ticks_ceil64(offset_ms))));
}

template<typename TFunction>
bool ThrowsInvalidArgument(TFunction function) {
    try {
        function();
    } catch(const std::invalid_argument&) {
        return true;
    }

    return false;
}

} // namespace

ZTEST_SUITE(rate_group_scheduler, NULL, NULL, NULL, NULL, NULL);

ZTEST(rate_group_scheduler, test_SetGroups_rejects_a_period_below_1_ms_and_keeps_the_groups) {
    const auto time_service = MakeTimeService();
    const auto frame = std::make_shared<SensorReadingsFrame>();
    const auto pipeline = std::make_shared<ReadingPipeline>(
        frame, std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>(), time_service
    );

    // Never started, so the queue does not have to run.
    RateGroupScheduler scheduler(
        std::make_shared<WorkQueueThread>("rate_group_scheduler", 4096, 7), pipeline, time_service
    );

    std::vector<std::unique_ptr<RateGroup>> groups;
    groups.push_back(std::make_unique<RateGroup>());
    groups.back()->period_ms = 40;
    groups.push_back(std::make_unique<RateGroup>());
    groups.back()->period_ms = 20;
    scheduler.SetGroups(std::move(groups));

    const auto stats = scheduler.GetStats();
    zassert_equal(scheduler.GetGroupCount(), 2);
    zassert_equal(stats.size(), 2);
    zassert_equal(stats[0].period_ms, 20, "Run order is ascending period");
    zassert_equal(stats[1].period_ms, 40);
    zassert_equal(stats[0].ticks, 0);
    zassert_equal(stats[0].overruns, 0);

    std::vector<std::unique_ptr<RateGroup>> zero_period;
    zero_period.push_back(std::make_unique<RateGroup>());
    zassert_true(ThrowsInvalidArgument([&] {
        scheduler.SetGroups(std::move(zero_period));
    }));

    std::vector<std::unique_ptr<RateGroup>> null_group;
    null_group.push_back(nullptr);
    zassert_true(ThrowsInvalidArgument([&] {
        scheduler.SetGroups(std::move(null_group));
    }));

    zassert_equal(scheduler.GetGroupCount(), 2, "A rejected set leaves the current groups in place");

    scheduler.ClearGroups();
    zassert_equal(scheduler.GetGroupCount(), 0);
    zassert_equal(scheduler.GetStats().size(), 0);
}

// Deadlines are whole system ticks: a period that is not one is rounded up, and the stats report
// what the group runs at. The conversion is 64-bit, so a period of days does not wrap.
ZTEST(rate_group_scheduler, test_SetGroups_reports_the_period_the_group_runs_at) {
    const auto time_service = MakeTimeService();
    const auto frame = std::make_shared<SensorReadingsFrame>();
    const auto pipeline = std::make_shared<ReadingPipeline>(
        frame, std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>(), time_service
    );

    RateGroupScheduler scheduler(
        std::make_shared<WorkQueueThread>("rate_group_scheduler", 4096, 7), pipeline, time_service
    );

    // 429 496 730 ms is 4 294 967 300 ticks at 10 000 ticks/s, which a 32-bit conversion wraps to 4.
    constexpr int kLongPeriodMs = 429496730;

    std::vector<std::unique_ptr<RateGroup>> groups;
    for(const int period_ms : { 20, 25, kLongPeriodMs }) {
        groups.push_back(std::make_unique<RateGroup>());
        groups.back()->period_ms = period_ms;
    }
    scheduler.SetGroups(std::move(groups));

    const auto stats = scheduler.GetStats();
    zassert_equal(stats.size(), 3);

    const uint64_t tick_us = k_ticks_to_us_ceil64(1);
    zassert_equal(stats[0].period_us, 20000, "20 ms is whole ticks at 100 and 10 000 ticks/s");
    zassert_true(
        stats[1].period_us >= 25000 && stats[1].period_us < 25000 + tick_us, "25 ms runs every %llu us",
        static_cast<unsigned long long>(stats[1].period_us)
    );
    if(CONFIG_SYS_CLOCK_TICKS_PER_SEC == 100)
        zassert_equal(stats[1].period_us, 30000, "Rounded up to the next 10 ms tick");

    zassert_equal(stats[2].period_ms, kLongPeriodMs);
    zassert_equal(
        stats[2].period_us, static_cast<uint64_t>(kLongPeriodMs) * 1000, "The long period runs every %llu us",
        static_cast<unsigned long long>(stats[2].period_us)
    );
}

ZTEST(rate_group_scheduler, test_Start_without_groups_schedules_nothing) {
    const auto rig = MakeSchedulerRig({});

    zassert_true(rig.scheduler_service->Start());
    zassert_equal(rig.scheduler_service->GetRateGroupCount(), 0);
    zassert_equal(rig.scheduler_service->GetRateGroupStats().size(), 0);
    zassert_true(rig.scheduler_service->Stop());
}

#ifdef CONFIG_BOARD_NATIVE_SIM

ZTEST_SUITE(scheduler_timing, NULL, NULL, NULL, NULL, NULL);

// Before Phase 4 a group re-armed a relative K_MSEC(20) when its tick ended. The kernel rounds a
// relative timeout taken between tick edges up by one tick, so the next start was the third 10 ms
// tick edge after the 5 ms tick began: 30 ms on native_sim, where the round-up swallows the 5 ms;
// with a tick shorter than the work (the ESP32's 100 us) it is 20 + 5 + 0.1 ms. Either way the tick
// and the round-up added to every period. Absolute deadlines add neither.
ZTEST(scheduler_timing, test_delivered_period_equals_the_configured_period) {
    constexpr int kPeriodMs = 20;
    constexpr size_t kTicks = 100;

    const auto rig = MakeSchedulerRig({ MakeVirtualSensor("timed", "1", kPeriodMs) });
    rig.recorder->costly_sensor_id_hash = StringHelpers::GetHash("timed");
    rig.recorder->cost_us = 5000;

    const int64_t start_ticks = k_uptime_ticks();
    zassert_true(rig.scheduler_service->Start());
    SleepUntil(start_ticks, kPeriodMs * kTicks + kPeriodMs / 2);
    zassert_true(rig.scheduler_service->Pause());

    const auto stats = rig.scheduler_service->GetRateGroupStats();
    const auto timestamps = rig.recorder->GetTimestamps("timed");
    zassert_true(rig.scheduler_service->Stop());

    zassert_equal(timestamps.size(), kTicks, "%zu ticks", timestamps.size());
    zassert_equal(
        timestamps.front(), TicksToMs(start_ticks + k_ms_to_ticks_ceil32(kPeriodMs)),
        "The first tick is one period after Start()"
    );

    int64_t max_deviation_ms = 0;
    for(size_t i = 1; i < timestamps.size(); i++) {
        const int64_t period_ms = timestamps[i] - timestamps[i - 1];
        max_deviation_ms = std::max(max_deviation_ms, std::abs(period_ms - kPeriodMs));
    }

    const int64_t total_ms = timestamps.back() - timestamps.front();
    zassert_equal(
        total_ms, kPeriodMs * static_cast<int64_t>(kTicks - 1), "Mean period %lld/%zu ms", total_ms, kTicks - 1
    );
    zassert_true(max_deviation_ms <= TicksToMs(1), "Max deviation %lld ms", max_deviation_ms);
    zassert_equal(max_deviation_ms, 0, "native_sim has no jitter to explain one");

    zassert_equal(stats.size(), 1);
    zassert_equal(stats[0].ticks, kTicks);
    zassert_equal(stats[0].overruns, 0);
    zassert_true(
        stats[0].last_us >= 5000 && stats[0].max_us < 10000, "Tick last %u us, max %u us", stats[0].last_us,
        stats[0].max_us
    );
}

// A 30 ms tick in a 20 ms group: each tick ends past the next deadline, which is skipped and
// counted, and the group runs again on its own phase, two periods after the last start.
ZTEST(scheduler_timing, test_overruns_are_counted_and_the_phase_is_kept) {
    constexpr int kPeriodMs = 20;

    const auto rig = MakeSchedulerRig({ MakeVirtualSensor("overrun", "1", kPeriodMs) });
    rig.recorder->costly_sensor_id_hash = StringHelpers::GetHash("overrun");
    rig.recorder->cost_us = 30000;

    const int64_t start_ticks = k_uptime_ticks();
    zassert_true(rig.scheduler_service->Start());
    SleepUntil(start_ticks, 1010);
    zassert_true(rig.scheduler_service->Pause());

    const auto stats = rig.scheduler_service->GetRateGroupStats();
    const auto timestamps = rig.recorder->GetTimestamps("overrun");
    zassert_true(rig.scheduler_service->Stop());

    zassert_equal(stats.size(), 1);
    const uint32_t ticks = stats[0].ticks;
    zassert_true(ticks >= 20, "%u ticks", ticks);
    zassert_equal(timestamps.size(), ticks);
    zassert_true(stats[0].overruns >= ticks - 1, "%u overruns in %u ticks", stats[0].overruns, ticks);
    zassert_true(stats[0].max_us >= 30000, "Tick max %u us", stats[0].max_us);

    // Every start on the phase Start() set: a whole number of periods from the first deadline.
    const int64_t phase_ms = TicksToMs(start_ticks + k_ms_to_ticks_ceil32(kPeriodMs));
    zassert_equal(timestamps.front(), phase_ms);
    for(size_t i = 0; i < timestamps.size(); i++)
        zassert_equal(
            (timestamps[i] - phase_ms) % kPeriodMs, 0, "tick %zu at %lld ms is off the phase", i, timestamps[i]
        );

    for(size_t i = 1; i < timestamps.size(); i++)
        zassert_equal(
            timestamps[i] - timestamps[i - 1], 2 * kPeriodMs, "tick %zu: the missed deadline is skipped, not run late",
            i
        );
}

// The 40 ms sensor comes first in the configuration, so its group is built first; the scheduler
// still runs the 20 ms group first whenever both are due.
ZTEST(scheduler_timing, test_faster_groups_run_first_at_coincident_deadlines) {
    const auto rig = MakeSchedulerRig(
        {
            MakeVirtualSensor("slow", "1", 40),
            MakeVirtualSensor("fast", "2", 20),
        }
    );

    const int64_t start_ticks = k_uptime_ticks();
    zassert_true(rig.scheduler_service->Start());
    SleepUntil(start_ticks, 1010);
    zassert_true(rig.scheduler_service->Pause());

    const auto stats = rig.scheduler_service->GetRateGroupStats();
    const auto commits = rig.recorder->commits;
    zassert_true(rig.scheduler_service->Stop());

    zassert_equal(stats.size(), 2);
    zassert_equal(stats[0].period_ms, 20, "Run order is ascending period");
    zassert_equal(stats[1].period_ms, 40);
    zassert_equal(stats[0].ticks, 50);
    zassert_equal(stats[1].ticks, 25);

    const uint32_t fast = StringHelpers::GetHash("fast");
    const uint32_t slow = StringHelpers::GetHash("slow");

    size_t coincident = 0;
    for(size_t i = 0; i < commits.size(); i++) {
        if(commits[i].sensor_id_hash != slow)
            continue;

        bool fast_before = false;
        bool fast_after = false;
        for(size_t j = 0; j < commits.size(); j++) {
            if(commits[j].sensor_id_hash == fast && commits[j].timestamp_ms == commits[i].timestamp_ms) {
                fast_before |= j < i;
                fast_after |= j > i;
            }
        }

        zassert_true(fast_before, "No 20 ms tick before the 40 ms tick at %lld ms", commits[i].timestamp_ms);
        zassert_false(fast_after, "The 20 ms tick at %lld ms ran after the 40 ms one", commits[i].timestamp_ms);
        coincident++;
    }

    zassert_equal(coincident, 25, "Every 40 ms deadline is also a 20 ms one");
}

// Two sensors on channel 0 share one slot: one request per tick, `samples` conversions, not twice.
ZTEST(scheduler_timing, test_one_sample_per_channel_per_tick) {
    constexpr uint16_t kSamples = 8;

    const auto adc = MakeEmulator();
    const auto adc_manager = MakeAdcManager(adc);
    const auto adc_configuration = MakeAdcConfiguration(
        adc_manager->GetChannelCount(), kSamples, MakeCalibrator({ { 0.0F, 0.0F }, { 5.0F, 5.0F } })
    );

    const auto rig = MakeSchedulerRig(
        {
            MakeAnalogSensor("first", 0, 20),
            MakeAnalogSensor("second", 0, 20),
        },
        adc_manager, adc_configuration
    );

    const int64_t start_ticks = k_uptime_ticks();
    zassert_true(rig.scheduler_service->Start());
    const auto before = adc->GetStats();
    // Past one stats log period, so the stats scenario logs the group's channel too.
    SleepUntil(start_ticks, 1010);
    zassert_true(rig.scheduler_service->Pause());

    const auto after = adc->GetStats();
    const auto stats = rig.scheduler_service->GetRateGroupStats();
    zassert_true(rig.scheduler_service->Stop());

    zassert_equal(stats.size(), 1);
    zassert_equal(stats[0].sensors, 2);
    zassert_equal(stats[0].adc_channels, 1);
    zassert_equal(stats[0].ticks, 50);

    zassert_equal(after.requests - before.requests, stats[0].ticks, "One Sample() per tick");
    zassert_equal(
        after.conversions - before.conversions, stats[0].ticks * kSamples, "%u conversions in %u ticks",
        after.conversions - before.conversions, stats[0].ticks
    );
    zassert_equal(after.errors, before.errors);

    zassert_equal(rig.recorder->GetTimestamps("first").size(), stats[0].ticks);
    zassert_equal(rig.recorder->GetTimestamps("second").size(), stats[0].ticks);
}

// A pause cancels the tick; a resume starts every group on a fresh phase one period out, so the
// paused time is neither run nor counted as overruns.
ZTEST(scheduler_timing, test_pause_and_resume_realign_the_deadlines) {
    constexpr int kPeriodMs = 20;

    const auto rig = MakeSchedulerRig({ MakeVirtualSensor("paused", "1", kPeriodMs) });
    const k_ticks_t period_ticks = k_ms_to_ticks_ceil32(kPeriodMs);

    const int64_t start_ticks = k_uptime_ticks();
    zassert_true(rig.scheduler_service->Start());
    SleepUntil(start_ticks, 210);
    zassert_true(rig.scheduler_service->Pause());

    const int64_t pause_ms = k_uptime_get();
    const size_t commits_at_pause = rig.recorder->commits.size();
    const uint32_t ticks_at_pause = rig.scheduler_service->GetRateGroupStats().at(0).ticks;
    zassert_equal(commits_at_pause, 10);
    zassert_equal(ticks_at_pause, 10);
    zassert_equal(rig.recorder->commits.front().timestamp_ms, TicksToMs(start_ticks + period_ticks));

    k_msleep(100);
    zassert_equal(rig.recorder->commits.size(), commits_at_pause, "No tick during the pause");
    zassert_equal(rig.scheduler_service->GetRateGroupStats().at(0).ticks, ticks_at_pause);

    const int64_t resume_ticks = k_uptime_ticks();
    zassert_true(rig.scheduler_service->Resume());
    SleepUntil(resume_ticks, 110);
    zassert_true(rig.scheduler_service->Pause());

    const auto stats = rig.scheduler_service->GetRateGroupStats();
    const auto commits = rig.recorder->commits;
    zassert_true(rig.scheduler_service->Stop());

    for(size_t i = 0; i < commits_at_pause; i++)
        zassert_true(commits[i].timestamp_ms <= pause_ms);

    zassert_equal(commits.size(), commits_at_pause + 5);
    zassert_equal(
        commits[commits_at_pause].timestamp_ms, TicksToMs(resume_ticks + period_ticks),
        "The first tick after the resume is one period out"
    );
    for(size_t i = commits_at_pause + 1; i < commits.size(); i++)
        zassert_equal(commits[i].timestamp_ms - commits[i - 1].timestamp_ms, kPeriodMs);

    zassert_equal(stats.at(0).ticks, 15);
    zassert_equal(stats.at(0).overruns, 0, "The pause is not an overrun");
}

namespace {

// Halts the scheduler at 30 ms, halfway through the 15 ms tick that started at 20 ms: the ztest
// thread runs above processing, so the halt arrives while the tick runs, and waits for it. Then
// starts it again with the first tick after the restart throwing out of the pipeline, which the
// dispatcher must retry rather than take as the end of every group.
void HaltDuringTickThenRestart(bool is_stop) {
    constexpr int kPeriodMs = 20;

    const auto rig = MakeSchedulerRig({ MakeVirtualSensor("halted", "1", kPeriodMs) });
    rig.recorder->costly_sensor_id_hash = StringHelpers::GetHash("halted");
    rig.recorder->cost_us = 15000;
    const k_ticks_t period_ticks = k_ms_to_ticks_ceil32(kPeriodMs);

    const int64_t start_ticks = k_uptime_ticks();
    zassert_true(rig.scheduler_service->Start());
    SleepUntil(start_ticks, 30);
    // Uptime only moves on tick edges and the tick ends between two, at 35 ms: the halt is timed
    // on the cycle counter.
    const uint32_t wake_cycles = k_cycle_get_32();

    // The tick has committed its reading and is still in its busy wait.
    zassert_equal(rig.recorder->commits.size(), 1);
    zassert_equal(rig.scheduler_service->GetRateGroupStats().at(0).ticks, 0, "The tick is in progress");

    if(is_stop) {
        zassert_true(rig.scheduler_service->Stop());
    } else {
        zassert_true(rig.scheduler_service->Pause());
        zassert_equal(
            rig.scheduler_service->GetRateGroupStats().at(0).ticks, 1,
            "The tick in progress ended before Pause() returned"
        );
    }

    const uint32_t halt_us = k_cyc_to_us_floor32(k_cycle_get_32() - wake_cycles);
    zassert_true(halt_us >= 5000, "Returned %u us after 30 ms, before the tick in progress ended", halt_us);

    k_msleep(100);
    zassert_equal(rig.recorder->commits.size(), 1, "No tick after the halt returned");
    if(!is_stop)
        zassert_equal(rig.scheduler_service->GetRateGroupStats().at(0).ticks, 1);

    rig.recorder->cost_us = 0;
    rig.recorder->throw_once = true;

    const int64_t restart_ticks = k_uptime_ticks();
    if(is_stop) {
        rig.scheduler_service->SetGeneration(rig.generation);
        zassert_true(rig.scheduler_service->Start());
    } else {
        zassert_true(rig.scheduler_service->Resume());
    }
    SleepUntil(restart_ticks, 110);
    zassert_true(rig.scheduler_service->Pause());

    const auto stats = rig.scheduler_service->GetRateGroupStats();
    const auto commits = rig.recorder->commits;
    zassert_true(rig.scheduler_service->Stop());

    zassert_false(rig.recorder->throw_once, "The first tick after the restart threw");
    zassert_equal(
        commits.size(), 1 + 5, "%zu commits: a throw in the first tick after the restart stopped the group",
        commits.size()
    );
    zassert_equal(
        commits[1].timestamp_ms, TicksToMs(restart_ticks + period_ticks),
        "The first tick after the restart is one period out"
    );
    for(size_t i = 2; i < commits.size(); i++)
        zassert_equal(commits[i].timestamp_ms - commits[i - 1].timestamp_ms, kPeriodMs);

    // The tick that threw is not counted; a stop rebuilt the groups, a pause kept the tick before it.
    zassert_equal(stats.at(0).ticks, is_stop ? 4 : 1 + 4);
    zassert_equal(stats.at(0).overruns, 0);
}

} // namespace

ZTEST(scheduler_timing, test_pause_during_a_tick_waits_for_it_and_resume_survives_a_throw) {
    HaltDuringTickThenRestart(false);
}

ZTEST(scheduler_timing, test_stop_during_a_tick_waits_for_it_and_start_survives_a_throw) {
    HaltDuringTickThenRestart(true);
}

#endif // CONFIG_BOARD_NATIVE_SIM
