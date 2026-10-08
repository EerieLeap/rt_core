#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#include "subsys/canbus/can_frame.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"

#include "can_route_table.h"
#include "i_isr_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::canbus_domain::services::CanbusService;

// Feeds the CAN sensors of a generation: one bus handler per route (bus, frame ID), one pending
// frame per route, one work item for the whole router.
//
// The CAN thread copies a frame into its route's pending slot and submits the work item, which is a
// no-op while it is already queued. The work item then decodes every route with a pending frame.
// Frames arriving faster than the work queue drains are coalesced per route, newest wins, and
// nothing is allocated per frame.
class CanFrameRouter {
private:
    struct PendingWork {
        k_work work;
        CanFrameRouter* router;
    };

    struct RouteState {
        std::shared_ptr<CanbusProxy> canbus;
        int handler_id = 0;
        k_spinlock lock{};
        CanFrame frame{};
        bool has_frame = false;
    };

    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<CanbusService> canbus_service_;
    ProcessSensorCallback process_sensor_callback_;

    CanRouteTable table_;
    std::vector<RouteState> states_;   // Parallel to the table's routes; fixed after Attach().

    PendingWork pending_work_{};
    k_work_sync work_sync_{};

    static void WorkHandler(k_work* work);

    void OnFrameReceived(size_t route_index, const CanFrame& frame);
    void ProcessPendingFrames() noexcept;

public:
    CanFrameRouter(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<CanbusService> canbus_service,
        ProcessSensorCallback process_sensor_callback);
    ~CanFrameRouter();

    CanFrameRouter(const CanFrameRouter&) = delete;
    CanFrameRouter& operator=(const CanFrameRouter&) = delete;

    /**
     * @brief Builds the routes of @p generation and registers one bus handler per route.
     * @return The number of routes listening.
     */
    size_t Attach(const SensorGeneration& generation);

    /** @brief Removes the bus handlers and waits for a running dispatch; nothing refers to the generation afterwards. */
    void Detach();

    [[nodiscard]] size_t GetRouteCount() const { return table_.GetRoutes().size(); }
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
