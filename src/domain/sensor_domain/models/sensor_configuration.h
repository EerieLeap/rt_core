#pragma once

#include <eerie_memory.hpp>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/interpolation_method.h"

#include "indicator_input_mode.h"
#include "sensor_reading_update_method.h"
#include "sensor_type.h"
#include "sensor_type_traits.h"

namespace eerie_leap::domain::sensor_domain::models {

using eerie_leap::domain::sensor_domain::models::sources::CanbusSource;
using eerie_leap::utilities::voltage_interpolator::CalibrationData;
using eerie_leap::utilities::voltage_interpolator::InterpolationMethod;

// Plain data, as persisted. The interpreters built from it (interpolator, expression, script)
// live in SensorRuntime for one configuration generation.
struct SensorConfiguration {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    SensorType type = SensorType::NONE;

    std::optional<uint32_t> channel = std::nullopt;
    std::pmr::string connection_string;
    std::pmr::string script_path;
    std::optional<int> sampling_rate_ms = std::nullopt;

    InterpolationMethod interpolation_method = InterpolationMethod::NONE;
    std::pmr::vector<CalibrationData> calibration_table; // Sorted by voltage.
    std::pmr::string expression;                         // Empty for none.

    // Indicator types only. A change of the input counts once it has held this long; 0 = at once.
    uint32_t debounce_ms = 0;
    IndicatorInputMode input_mode = IndicatorInputMode::Level;

    // connection_string data source decomposition objects
    eerie_memory::PmrUniquePtr<CanbusSource> canbus_source = nullptr;

    SensorConfiguration(std::allocator_arg_t, allocator_type alloc) :
        connection_string(alloc),
        script_path(alloc),
        calibration_table(alloc),
        expression(alloc),
        alloc_(alloc) {}

    SensorConfiguration(const SensorConfiguration&) = delete;
    // Not assignable: the allocator member cannot be assigned.
    SensorConfiguration& operator=(const SensorConfiguration&) = delete;
    SensorConfiguration& operator=(SensorConfiguration&&) = delete;
    SensorConfiguration(SensorConfiguration&&) noexcept = default;
    ~SensorConfiguration() = default;

    SensorConfiguration(SensorConfiguration&& other, allocator_type alloc) noexcept :
        type(other.type),
        channel(other.channel),
        connection_string(other.connection_string, alloc),
        script_path(other.script_path, alloc),
        sampling_rate_ms(other.sampling_rate_ms),
        interpolation_method(other.interpolation_method),
        calibration_table(other.calibration_table, alloc),
        expression(other.expression, alloc),
        debounce_ms(other.debounce_ms),
        input_mode(other.input_mode),
        canbus_source(std::move(other.canbus_source)),
        alloc_(alloc) {}

    [[nodiscard]] SensorTypeTraits GetTraits() const {
        return SensorTypeTraits::Of(type);
    }

    [[nodiscard]] bool HasExpression() const {
        return !expression.empty();
    }
    [[nodiscard]] bool HasInterpolation() const {
        return interpolation_method != InterpolationMethod::NONE;
    }
    [[nodiscard]] bool HasScript() const {
        return !script_path.empty();
    }
    [[nodiscard]] bool HasConditioning() const {
        return debounce_ms != 0 || input_mode != IndicatorInputMode::Level;
    }

    SensorReadingUpdateMethod GetReadingUpdateMethod() const {
        const auto traits = GetTraits();

        if(sampling_rate_ms.has_value())
            return SensorReadingUpdateMethod::SCHEDULER;
        if(traits.source == SensorSourceKind::EXPRESSION)
            return SensorReadingUpdateMethod::DEPENDENT;
        if(traits.is_isr_driven)
            return SensorReadingUpdateMethod::ISR;

        return SensorReadingUpdateMethod::NONE;
    }

    void UpdateConnectionString() {
        if(GetTraits().uses_canbus && canbus_source != nullptr)
            connection_string = canbus_source->ToConnectionString();
        else
            connection_string = "";
    }

    void UnwrapConnectionString() {
        if(GetTraits().uses_canbus)
            canbus_source = eerie_memory::make_unique_pmr<CanbusSource>(
                alloc_, CanbusSource::FromConnectionString(alloc_, connection_string)
            );
    }

private:
    allocator_type alloc_;
};

} // namespace eerie_leap::domain::sensor_domain::models
