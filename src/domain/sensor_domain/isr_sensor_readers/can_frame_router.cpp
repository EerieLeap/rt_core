#include <exception>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "subsys/canbus/can_id.h"

#include "can_frame_router.h"

LOG_MODULE_REGISTER(can_frame_router_logger);

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::canbus::CanId;
using eerie_leap::domain::canbus_domain::models::CanSignalConfiguration;

CanFrameRouter::CanFrameRouter(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CanbusService> canbus_service,
    ProcessSensorCallback process_sensor_callback)
        : time_service_(std::move(time_service)),
        work_queue_thread_(std::move(work_queue_thread)),
        canbus_service_(std::move(canbus_service)),
        process_sensor_callback_(std::move(process_sensor_callback)) {

    pending_work_.router = this;
    k_work_init(&pending_work_.work, WorkHandler);
}

CanFrameRouter::~CanFrameRouter() {
    Detach();
}

size_t CanFrameRouter::Attach(const SensorGeneration& generation) {
    Detach();

    table_.Build(generation, [this](uint8_t bus_channel, uint32_t frame_id, uint32_t signal_name_hash)
        -> std::shared_ptr<const CanSignalConfiguration> {

        auto message_configuration = canbus_service_->GetMessageConfiguration(bus_channel, frame_id);
        if(message_configuration == nullptr)
            return nullptr;

        const auto* signal_configuration = message_configuration->TryGetSignal(signal_name_hash);
        if(signal_configuration == nullptr)
            return nullptr;

        // Aliases the owning message configuration, so that stays alive too.
        return std::shared_ptr<const CanSignalConfiguration>(std::move(message_configuration), signal_configuration);
    });

    const auto routes = table_.GetRoutes();
    states_.resize(routes.size());

    size_t attached = 0;
    for(size_t i = 0; i < routes.size(); i++) {
        const auto& route = routes[i];
        auto& state = states_[i];

        state.canbus = canbus_service_->GetCanbus(route.bus_channel);
        const auto* channel_configuration = canbus_service_->GetChannelConfiguration(route.bus_channel);
        if(state.canbus == nullptr || !state.canbus->IsValid() || channel_configuration == nullptr) {
            LOG_ERR("CAN bus %u is not available; %zu sensors on frame 0x%08X stay silent.",
                route.bus_channel, route.bindings.size(), route.frame_id);
            continue;
        }

        // Handler removal is serialised against dispatch, so `this` outlives every call.
        const int handler_id = (*state.canbus)->RegisterFrameReceivedHandler(
            CanId{route.frame_id, channel_configuration->is_extended_id},
            [this, i](const CanFrame& frame) { OnFrameReceived(i, frame); });

        if(handler_id <= 0) {
            LOG_ERR("Failed to register the CAN handler for bus %u frame 0x%08X: %d.", route.bus_channel, route.frame_id, handler_id);
            continue;
        }

        state.handler_id = handler_id;
        attached++;

        LOG_INF("CAN route bus %u frame 0x%08X feeds %zu sensors.", route.bus_channel, route.frame_id, route.bindings.size());
    }

    return attached;
}

void CanFrameRouter::Detach() {
    for(auto& state : states_) {
        if(state.handler_id > 0 && state.canbus != nullptr) {
            if(auto* canbus = state.canbus->Get(); canbus != nullptr)
                canbus->RemoveFrameReceivedHandler(state.handler_id);
        }

        state.handler_id = 0;
    }

    // Waits for a running dispatch; nothing submits afterwards since the handlers are gone.
    k_work_cancel_sync(&pending_work_.work, &work_sync_);

    states_.clear();
    table_ = CanRouteTable{};
}

// CAN thread. Keeps only the newest frame of a route while the work queue is behind.
void CanFrameRouter::OnFrameReceived(size_t route_index, const CanFrame& frame) {
    if(route_index >= states_.size() || frame.data.empty())
        return;

    auto& state = states_[route_index];

    K_SPINLOCK(&state.lock) {
        state.frame = frame;
        state.has_frame = true;
    }

    // Returns 0 while already queued; a stopping queue rejects the submission and the frame is dropped.
    try {
        if(k_work_submit_to_queue(work_queue_thread_->GetWorkQueue(), &pending_work_.work) < 0)
            LOG_DBG("CAN frame ID 0x%08X dropped: work queue unavailable.", frame.id);
    } catch(const std::exception& e) {
        LOG_DBG("CAN frame ID 0x%08X dropped: %s", frame.id, e.what());
    }
}

void CanFrameRouter::WorkHandler(k_work* work) {
    auto* pending_work = CONTAINER_OF(work, PendingWork, work);
    pending_work->router->ProcessPendingFrames();
}

// Exceptions must not unwind into the work queue's C dispatch.
void CanFrameRouter::ProcessPendingFrames() noexcept {
    const auto routes = table_.GetRoutes();

    for(size_t i = 0; i < routes.size() && i < states_.size(); i++) {
        CanFrame frame;
        bool has_frame = false;

        K_SPINLOCK(&states_[i].lock) {
            has_frame = states_[i].has_frame;
            if(has_frame) {
                frame = states_[i].frame;
                states_[i].has_frame = false;
            }
        }

        if(!has_frame)
            continue;

        try {
            CanRouteTable::Dispatch(routes[i], frame, *time_service_, process_sensor_callback_);
        } catch(const std::exception& e) {
            LOG_ERR("CAN frame ID 0x%08X processing failed: %s", frame.id, e.what());
        } catch(...) {
            LOG_ERR("CAN frame ID 0x%08X processing failed.", frame.id);
        }
    }
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
