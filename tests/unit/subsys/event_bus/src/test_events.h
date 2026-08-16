#pragma once

#include <cstdint>

namespace event_bus_tests {

enum class TestEventType : uint32_t {
    Alpha = 0,
    Beta = 1,
    Gamma = 2
};

enum class TestPayloadType : uint32_t {
    Value = 0,
    Label = 1,
    Flag = 2
};

} // namespace event_bus_tests
