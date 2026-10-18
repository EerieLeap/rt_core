#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "domain/sensor_domain/processors/reading_pipeline.hpp"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"

#include "rate_group.h"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::domain::sensor_domain::processors::ReadingPipeline;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::ITimeService;

// Runs rate groups on one delayable work item against absolute deadlines. A tick runs every group
// whose deadline has come, fastest first, then sleeps until the earliest next deadline. A group
// keeps its phase: neither the tick's duration nor the kernel's rounding of a relative timeout
// adds to its period, and a tick that runs past the next deadline skips and counts what it missed.
class RateGroupScheduler {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<ReadingPipeline> pipeline_;
    std::shared_ptr<ITimeService> time_service_;

    // Ascending period. Only replaced while no tick is scheduled, so the tick reads it unlocked;
    // the mutex keeps the stats readers off a vector that is being replaced.
    std::vector<std::unique_ptr<RateGroup>> groups_;
    mutable k_mutex groups_mutex_;

    // Created on the first Start(): the work queue does not run before the owner is initialized.
    std::optional<WorkQueueTask<RateGroupScheduler>> task_;
    atomic_t is_running_ = ATOMIC_INIT(0);

#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
    int64_t next_stats_log_ = 0;

    void LogStats();
#endif

    static WorkQueueTaskResult ProcessTick(RateGroupScheduler* scheduler);
    WorkQueueTaskResult Tick();
    void RunGroup(RateGroup& group);
    [[nodiscard]] int64_t GetEarliestDeadline() const;

public:
    RateGroupScheduler(
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<ReadingPipeline> pipeline,
        std::shared_ptr<ITimeService> time_service
    );
    ~RateGroupScheduler();

    // The work item points at the scheduler.
    RateGroupScheduler(const RateGroupScheduler&) = delete;
    RateGroupScheduler& operator=(const RateGroupScheduler&) = delete;
    RateGroupScheduler(RateGroupScheduler&&) = delete;
    RateGroupScheduler& operator=(RateGroupScheduler&&) = delete;

    // Stops, then takes the groups of a new generation: sorts them into run order (ascending
    // period), derives each one's period in ticks, logs its sampling budget and warns when the
    // period is not a whole number of system ticks, which rounds it up. Throws
    // std::invalid_argument for a period below 1 ms, leaving the current groups in place.
    void SetGroups(std::vector<std::unique_ptr<RateGroup>> groups);

    // Stops, then drops the groups and their readers.
    void ClearGroups();

    // Every group's first deadline is one period from now, so a resume never catches up on the
    // time it was paused. Throws std::runtime_error when the work queue is not running.
    void Start();

    // Cancels the tick and waits for one in progress; nothing samples once it returns.
    void Stop();

    /** @return The number of rate groups set. */
    [[nodiscard]] size_t GetGroupCount() const;

    /** @return One snapshot per rate group, in run order. Safe from any thread. */
    [[nodiscard]] std::vector<RateGroupStatsSnapshot> GetStats() const;

#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
    // The channel part of a group's stats line: " ch<global> <volts> V", or " ch<global> err
    // <errno>" for a failed channel, per channel of the set from the `first`th on, in the set's
    // order. Writes whole channels while they fit, at least one (cut short when `line` cannot hold
    // it), and always terminates `line`. Allocates nothing.
    // @return The index after the last channel written; GetChannelCount() once all are written.
    static size_t FormatChannels(const AdcSampleSet& adc_samples, size_t first, std::span<char> line);
#endif
};

} // namespace eerie_leap::domain::sensor_domain::services
