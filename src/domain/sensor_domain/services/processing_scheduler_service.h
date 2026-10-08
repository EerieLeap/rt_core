#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"

#include "rate_group_task.hpp"

namespace eerie_leap::domain::sensor_domain::services {

namespace threading = eerie_leap::subsys::threading;

using threading::ServiceBase;
using threading::ServiceState;
using threading::WorkQueueThread;
using threading::WorkQueueTaskResult;
using eerie_leap::domain::sensor_domain::runtime::SensorGeneration;
using eerie_leap::domain::sensor_domain::sensor_readers::SensorReaderFactory;
using eerie_leap::domain::sensor_domain::processors::ReadingPipeline;

// Runs the polled sensors of a generation: one periodic task per distinct sampling rate, whose
// sensors are read in processing order so a derived sensor in the group sees this tick's inputs.
class ProcessingSchedulerService final : public ServiceBase<> {
private:
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<ReadingPipeline> pipeline_;

    std::vector<threading::WorkQueueTask<RateGroupTask>> work_queue_tasks_;
    std::shared_ptr<const SensorGeneration> generation_;

    void StartTasks();
    void CancelTasks();
    std::vector<std::unique_ptr<RateGroupTask>> CreateRateGroups() const;
    static WorkQueueTaskResult ProcessRateGroup(RateGroupTask* task);

    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    ProcessingSchedulerService(
        std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<ReadingPipeline> pipeline);

    // The generation to run; set while stopped, before Start().
    void SetGeneration(std::shared_ptr<const SensorGeneration> generation);

    [[nodiscard]] bool IsPausable() const noexcept override { return true; }

    /** @return The number of rate groups (timers) running. */
    [[nodiscard]] size_t GetRateGroupCount() const { return work_queue_tasks_.size(); }
};

} // namespace eerie_leap::domain::sensor_domain::services
