#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "subsys/canbus/can_frame.h"

#include "sensor.h"
#include "reading_source.h"
#include "reading_status.h"
#include "reading_error.h"

namespace eerie_leap::domain::sensor_domain::models {

using eerie_leap::subsys::canbus::CanFrame;

// A sample on its way through the pipeline and, once committed, the latest state of a sensor.
// Trivially copyable on purpose: readings are copied into and out of SensorReadingsFrame on every
// sample, and that must not allocate.
struct SensorReading {
    // The configured sensor. Valid for as long as the configuration the pipeline was started with;
    // the frame is cleared before that configuration is replaced. Consumers that only need the
    // identity use sensor_id_hash.
    const Sensor* sensor = nullptr;
    uint32_t sensor_id_hash = 0;

    std::optional<float> value;
    std::optional<float> raw_value;   ///< The value before the expression was applied.
    std::optional<float> voltage;     ///< Calibrated ADC voltage, PHYSICAL_ANALOG only.
    std::optional<std::chrono::system_clock::time_point> timestamp;

    ReadingSource source = ReadingSource::NONE;
    ReadingStatus status = ReadingStatus::UNINITIALIZED;
    ReadingError error = ReadingError::NONE;

    // CANBUS_RAW only, non-owning: the frame behind the reading. Set by a reader for the duration of
    // its call and by a consumer for the duration of its snapshot; the store keeps its own copy and
    // never hands this pointer out.
    const CanFrame* can_frame = nullptr;

    SensorReading() = default;

    explicit SensorReading(const Sensor* sensor)
        : sensor(sensor), sensor_id_hash(sensor != nullptr ? sensor->id_hash : 0) {}

    [[nodiscard]] bool HasError() const { return status == ReadingStatus::ERROR; }

    void SetError(ReadingError reading_error) {
        status = ReadingStatus::ERROR;
        error = reading_error;
    }
};

static_assert(std::is_trivially_copyable_v<SensorReading>, "Readings are copied on every sample and must not allocate.");

} // namespace eerie_leap::domain::sensor_domain::models
