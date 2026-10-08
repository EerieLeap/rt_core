#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "subsys/canbus/can_frame.h"
#include "subsys/time/i_time_service.h"
#include "domain/canbus_domain/models/can_signal_configuration.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"

#include "i_isr_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::canbus_domain::models::CanSignalConfiguration;
using eerie_leap::domain::sensor_domain::runtime::SensorGeneration;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;

// A sensor fed by one CAN frame: a decoded signal, or the frame itself for CANBUS_RAW.
struct CanSignalBinding {
    const SensorRuntime* runtime;
    std::shared_ptr<const CanSignalConfiguration> signal;   // Null for a raw frame sensor.
};

// Every sensor fed by one CAN ID on one bus. A frame is decoded once per route, however many
// sensors read it, and the bus sees one handler per route.
struct CanRoute {
    uint8_t bus_channel = 0;
    uint32_t frame_id = 0;
    std::vector<CanSignalBinding> bindings;
};

class CanRouteTable {
public:
    // Looks a signal up by bus, frame ID and name hash; null when it is not configured.
    using SignalResolver = std::function<std::shared_ptr<const CanSignalConfiguration>(uint8_t, uint32_t, uint32_t)>;

private:
    std::vector<CanRoute> routes_;

    CanRoute& GetOrAddRoute(uint8_t bus_channel, uint32_t frame_id);

public:
    /**
     * @brief Groups the generation's event-driven CAN sensors by bus and frame ID.
     *
     * A signal sensor whose signal is not configured is left out and logged.
     */
    void Build(const SensorGeneration& generation, const SignalResolver& resolve_signal);

    [[nodiscard]] std::span<const CanRoute> GetRoutes() const { return routes_; }

    /** @brief Produces one reading per sensor of @p route from @p frame and hands each to @p callback. */
    static void Dispatch(
        const CanRoute& route,
        const CanFrame& frame,
        ITimeService& time_service,
        const ProcessSensorCallback& callback);
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
