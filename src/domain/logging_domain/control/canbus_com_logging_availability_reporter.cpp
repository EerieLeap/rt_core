#include <stdexcept>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "canbus_com_logging_availability_reporter.h"

namespace eerie_leap::domain::logging_domain::control {

LOG_MODULE_REGISTER(canbus_com_logging_availability_reporter_logger);

CanbusComLoggingAvailabilityReporter::CanbusComLoggingAvailabilityReporter(
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<IFsService> sd_fs_service,
    std::shared_ptr<CanbusComService> canbus_com_service
) :
    sd_fs_service_(std::move(sd_fs_service)),
    canbus_com_service_(std::move(canbus_com_service)) {

    if(work_queue_thread == nullptr || sd_fs_service_ == nullptr || canbus_com_service_ == nullptr)
        throw std::invalid_argument(
            "The availability reporter needs a work queue, the log volume and the CAN COM service."
        );

    Update();

    task_ = work_queue_thread->CreateTask(
        [](CanbusComLoggingAvailabilityReporter* reporter) {
            return reporter->Poll();
        },
        this
    );
    task_->Schedule(K_MSEC(POLL_INTERVAL_MS));
}

CanbusComLoggingAvailabilityReporter::~CanbusComLoggingAvailabilityReporter() {
    // Drops a pending poll and waits for one in progress.
    if(task_.has_value())
        task_->Cancel();

    canbus_com_service_->SetLoggingAvailable(false);
}

void CanbusComLoggingAvailabilityReporter::Update() {
    const bool is_available = sd_fs_service_->IsAvailable();

    // The first call always sets, so the COM service is told even when the volume is missing.
    if(is_available != is_available_ || !task_.has_value()) {
        LOG_INF(
            "Log volume %s: the LOGGING feature is %s.", is_available ? "available" : "not available",
            is_available ? "set" : "cleared"
        );
    }

    is_available_ = is_available;
    canbus_com_service_->SetLoggingAvailable(is_available);
}

WorkQueueTaskResult CanbusComLoggingAvailabilityReporter::Poll() {
    Update();

    return { .reschedule = true, .delay = K_MSEC(POLL_INTERVAL_MS) };
}

} // namespace eerie_leap::domain::logging_domain::control
