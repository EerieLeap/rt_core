#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::processors {

class IReadingProcessor {
public:
    virtual ~IReadingProcessor() = default;

    virtual void ProcessReading(const uint32_t sensor_id_hash) = 0;
};

} // namespace eerie_leap::domain::sensor_domain::processors
