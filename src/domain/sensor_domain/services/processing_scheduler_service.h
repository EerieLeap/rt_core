#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"

#include "rate_group.h"
#include "rate_group_scheduler.h"

namespace eerie_leap::domain::sensor_domain::services {

namespace threading = eerie_leap::subsys::threading;

using threading::ServiceBase;
using threading::ServiceState;
using threading::WorkQueueThread;
using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::sensor_domain::runtime::SensorGeneration;
using eerie_leap::domain::sensor_domain::sensor_readers::SensorReaderFactory;
using eerie_leap::domain::sensor_domain::processors::ReadingPipeline;

// Runs the polled sensors of a generation: one rate group per distinct sampling rate, all of them
// on one scheduler with fixed-phase deadlines. A group's tick acquires its ADC channels first, then
// reads its sensors in processing order with one timestamp, so a derived sensor in the group sees
// this tick's inputs.
class ProcessingSchedulerService final : public ServiceBase<> {
private:
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory_;

    // The scheduler's readers point into the generation, so it is declared after it and goes first.
    std::shared_ptr<const SensorGeneration> generation_;
    RateGroupScheduler scheduler_;

    std::vector<std::unique_ptr<RateGroup>> CreateRateGroups() const;

    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    ProcessingSchedulerService(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<ReadingPipeline> pipeline);

    // The generation to run; set while stopped, before Start().
    void SetGeneration(std::shared_ptr<const SensorGeneration> generation);

    [[nodiscard]] bool IsPausable() const noexcept override { return true; }

    /** @return The number of rate groups of the generation running or paused. */
    [[nodiscard]] size_t GetRateGroupCount() const { return scheduler_.GetGroupCount(); }

    /** @return Per rate group, in run order: ticks, overruns and tick durations. Safe from any thread. */
    [[nodiscard]] std::vector<RateGroupStatsSnapshot> GetRateGroupStats() const { return scheduler_.GetStats(); }
};

} // namespace eerie_leap::domain::sensor_domain::services
