#pragma once

#include <cstdint>

#include "subsys/event_bus/event_binding.h"

namespace eerie_leap::domain::logging_domain::control::models {

using eerie_leap::subsys::event_bus::EventBinding;

// Binds logging control to an event, as PropertyBinding binds a widget property: a logging trigger.
// The value `event` names, read as a bool, is the wanted logging state. A momentary input is made a
// switch by its sensor (IndicatorInputMode::Toggle), so a trigger has no mode.
struct LoggingControlBinding {
    EventBinding event;
    // CDMP UID (CdmpDeviceInfo::uid) of the unit it starts and stops; 0 (ILoggingControl::ALL_UNITS) is
    // every unit that can log.
    uint32_t target_uid = 0;
};

} // namespace eerie_leap::domain::logging_domain::control::models
