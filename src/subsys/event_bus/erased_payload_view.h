#pragma once

#include <cstdint>
#include <functional>

#include "event.h"

namespace eerie_leap::subsys::event_bus {

// Reads a payload whose key enum the caller does not know, so a subscription can be built
// from configuration. Non-owning and only valid for the duration of the handler call.
class ErasedPayloadView {
public:
    virtual ~ErasedPayloadView() = default;

    virtual const EventData* Find(uint32_t key) const = 0;
};

using ErasedEventHandler = std::function<void(const ErasedPayloadView&)>;

// Evaluated where a typed filter is, under the subscriber lock and before the handler, so an
// event a subscriber does not want costs it nothing. Empty means accept everything.
using ErasedEventFilter = std::function<bool(const ErasedPayloadView&)>;

} // namespace eerie_leap::subsys::event_bus
