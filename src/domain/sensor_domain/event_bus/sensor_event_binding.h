#pragma once

#include <utility>

#include "subsys/event_bus/event_binding.h"

#include "sensor_payload_type.h"

namespace eerie_leap::domain::sensor_domain::event_bus {

using eerie_leap::subsys::event_bus::EventBinding;

// Whether @p binding selects one sensor by its SensorId, as every binding to the Sensors channel
// must. Without a selector it would receive every sensor's value and make SensorEventsPublisher
// publish every reading, which the publisher's acceptance cache exists to avoid; a selector on
// another key would leave that cache, which is keyed by sensor, answering from a sensor's first
// reading. Header only, for the validators.
inline bool SelectsSensor(const EventBinding& binding) {
    return binding.HasSelector() && binding.selector_key == std::to_underlying(SensorPayloadType::SensorId);
}

} // namespace eerie_leap::domain::sensor_domain::event_bus
