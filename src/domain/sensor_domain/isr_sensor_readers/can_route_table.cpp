#include <zephyr/logging/log.h>

#include "domain/canbus_domain/utilities/can_signal_codec.h"
#include "domain/sensor_domain/models/sensor_type_traits.h"

#include "can_route_table.h"

LOG_MODULE_REGISTER(can_route_table_logger);

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;
using eerie_leap::domain::canbus_domain::utilities::CanSignalCodec;

CanRoute& CanRouteTable::GetOrAddRoute(uint8_t bus_channel, uint32_t frame_id) {
    for(auto& route : routes_) {
        if(route.bus_channel == bus_channel && route.frame_id == frame_id)
            return route;
    }

    routes_.push_back(CanRoute{.bus_channel = bus_channel, .frame_id = frame_id});

    return routes_.back();
}

void CanRouteTable::Build(const SensorGeneration& generation, const SignalResolver& resolve_signal) {
    routes_.clear();

    for(const auto& runtime : generation.runtimes) {
        const auto& configuration = runtime.GetSensor().configuration;
        const auto traits = configuration.GetTraits();

        if(!traits.uses_canbus || runtime.update_method != SensorReadingUpdateMethod::ISR || configuration.canbus_source == nullptr)
            continue;

        const auto& source = *configuration.canbus_source;

        CanSignalBinding binding{.runtime = &runtime, .signal = nullptr};

        if(traits.source == SensorSourceKind::CAN_SIGNAL) {
            binding.signal = resolve_signal(source.bus_channel, source.frame_id, source.signal_name_hash);

            if(binding.signal == nullptr) {
                LOG_ERR("Sensor %s: CAN signal %s is not configured on bus %u frame 0x%08X.",
                    runtime.GetSensor().id.c_str(), source.signal_name.c_str(), source.bus_channel, source.frame_id);
                continue;
            }
        }

        GetOrAddRoute(source.bus_channel, source.frame_id).bindings.push_back(std::move(binding));
    }
}

void CanRouteTable::Dispatch(
    const CanRoute& route,
    const CanFrame& frame,
    ITimeService& time_service,
    const ProcessSensorCallback& callback) {

    if(frame.data.empty())
        return;

    for(const auto& binding : route.bindings) {
        SensorReading reading(binding.runtime->sensor.get());
        reading.source = ReadingSource::ISR;
        reading.timestamp = time_service.GetCurrentTime();
        reading.status = ReadingStatus::RAW;

        if(binding.signal == nullptr) {
            // The frame itself is the reading; the frame store keeps a copy.
            reading.can_frame = &frame;
        } else {
            const auto value = CanSignalCodec::Decode(*binding.signal, frame.data);

            if(value.has_value()) {
                reading.value = value.value();
                reading.raw_value = value.value();
            } else {
                reading.SetError(ReadingError::READER_FAILED);
            }
        }

        callback(*binding.runtime, reading);
    }
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
