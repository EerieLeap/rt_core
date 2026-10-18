#pragma once

#include <memory>
#include <optional>

#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_thread.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;

/**
 * @brief Writer role: keeps the LOGGING feature that this unit sends on CDMP equal to whether its
 * log volume is available.
 *
 * Senders skip a unit without the bit, so a DAQ with no SD card is not asked to log. The first value
 * is set in the constructor, so the first heartbeat after the COM service starts already carries it;
 * after that the volume is checked every POLL_INTERVAL_MS, and the next heartbeat (within 3 s)
 * carries a change. Destruction clears the bit.
 *
 * It polls on the configuration work queue, not the CDMP one: IFsService::IsAvailable() takes the
 * filesystem mutex, which the writer holds across SD syncs, and a heartbeat must not wait for it.
 * A write failure or a full volume does not clear the bit; the logging status reports those. The bit
 * says only that the unit has a writer and a mounted volume.
 */
class CanbusComLoggingAvailabilityReporter {
public:
    static constexpr int POLL_INTERVAL_MS = 1000;

private:
    std::shared_ptr<IFsService> sd_fs_service_;
    std::shared_ptr<CanbusComService> canbus_com_service_;
    bool is_available_ = false;
    std::optional<WorkQueueTask<CanbusComLoggingAvailabilityReporter>> task_;

    void Update();
    WorkQueueTaskResult Poll();

public:
    /** @throws std::invalid_argument without the filesystem service or the COM service. */
    CanbusComLoggingAvailabilityReporter(
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<IFsService> sd_fs_service,
        std::shared_ptr<CanbusComService> canbus_com_service
    );
    /** @brief Waits for a poll in progress, then clears LOGGING. */
    ~CanbusComLoggingAvailabilityReporter();

    CanbusComLoggingAvailabilityReporter(const CanbusComLoggingAvailabilityReporter&) = delete;
    CanbusComLoggingAvailabilityReporter& operator=(const CanbusComLoggingAvailabilityReporter&) = delete;
};

} // namespace eerie_leap::domain::logging_domain::control
