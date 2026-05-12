#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <chrono>

#include "utilities/guid/guid.hpp"

#include "sensor.h"
#include "reading_source.h"
#include "reading_status.h"
#include "reading_metadata.h"

namespace eerie_leap::domain::sensor_domain::models {

using time_point = std::chrono::system_clock::time_point;

using eerie_leap::utilities::guid::Guid;

// NOTE: SensorReading is a non allocator-aware type due to
// complications overweighting benefits, especially because of the
// variant type in the ReadingMetadata, which requres too much work
// for correct allocator management, on top of that it's not clear
// if it makes any sense to make CanFrame allocator-aware.
// Review implementation in case of memory shotage.
struct SensorReading {
    const Guid id;
    const std::shared_ptr<Sensor> sensor;
    std::optional<float> value;
    std::optional<time_point> timestamp;
    ReadingSource source = ReadingSource::NONE;
    ReadingStatus status = ReadingStatus::UNINITIALIZED;
    std::optional<std::string> error_message = std::nullopt;
    ReadingMetadata metadata;

    SensorReading(const Guid id, std::shared_ptr<Sensor> sensor)
        : id(id), sensor(std::move(sensor)) {}

    ~SensorReading() = default;
};

}

// namespace eerie_leap::domain::sensor_domain::models
