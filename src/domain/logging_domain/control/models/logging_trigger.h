#pragma once

#include <cstdint>

#include "subsys/event_bus/event_binding.h"

namespace eerie_leap::domain::logging_domain::control::models {

using eerie_leap::subsys::event_bus::EventBinding;

// A logging trigger: the value its binding names, read as a bool, is the wanted logging state. A
// momentary input is made a switch by its sensor (IndicatorInputMode::Toggle), so a trigger has no
// mode.
struct LoggingTrigger {
    EventBinding binding;
    // CDMP UID (CdmpDeviceInfo::uid) of the unit it starts and stops; 0 (ILoggingControl::ALL_UNITS) is
    // every unit that can log.
    uint32_t target_uid = 0;
};

} // namespace eerie_leap::domain::logging_domain::control::models
