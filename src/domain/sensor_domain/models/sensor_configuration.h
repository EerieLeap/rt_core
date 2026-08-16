#pragma once

#include <memory_resource>
#include <optional>
#include <eerie_memory.hpp>

#include "utilities/voltage_interpolator/i_voltage_interpolator.h"
#include "subsys/math_parser/expression_evaluator.h"
#include "subsys/lua_script/lua_script.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"

#include "sensor_type.h"
#include "sensor_reading_update_method.h"

namespace eerie_leap::domain::sensor_domain::models {

using eerie_leap::utilities::voltage_interpolator::IVoltageInterpolator;
using eerie_leap::subsys::math_parser::ExpressionEvaluator;
using eerie_leap::subsys::lua_script::LuaScript;
using eerie_leap::domain::sensor_domain::models::sources::CanbusSource;

struct SensorConfiguration {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    SensorType type = SensorType::NONE;

    std::optional<uint32_t> channel = std::nullopt;
    std::pmr::string connection_string;
    std::pmr::string script_path;
    // TODO: make optional
    std::optional<int> sampling_rate_ms = std::nullopt;

    eerie_memory::pmr_unique_ptr<IVoltageInterpolator> voltage_interpolator = nullptr;
    eerie_memory::pmr_unique_ptr<ExpressionEvaluator> expression_evaluator = nullptr;
    std::shared_ptr<LuaScript> lua_script = nullptr;

    // connection_string data source decomposition objects
    eerie_memory::pmr_unique_ptr<CanbusSource> canbus_source = nullptr;

    SensorConfiguration(
        std::allocator_arg_t, allocator_type alloc)
            : connection_string(alloc),
            script_path(alloc),
            alloc_(alloc) {}

    SensorConfiguration(const SensorConfiguration&) = delete;
	SensorConfiguration& operator=(const SensorConfiguration&) noexcept = default;
	SensorConfiguration& operator=(SensorConfiguration&&) noexcept = default;
	SensorConfiguration(SensorConfiguration&&) noexcept = default;
	~SensorConfiguration() = default;

    SensorConfiguration(SensorConfiguration&& other, allocator_type alloc) noexcept
        : type(other.type),
        channel(other.channel),
        connection_string(other.connection_string, alloc),
        script_path(other.script_path, alloc),
        sampling_rate_ms(other.sampling_rate_ms),
        voltage_interpolator(std::move(other.voltage_interpolator)),
        expression_evaluator(std::move(other.expression_evaluator)),
        lua_script(other.lua_script),
        canbus_source(std::move(other.canbus_source)) {}

    SensorReadingUpdateMethod GetReadingUpdateMethod() const {
        if(sampling_rate_ms.has_value())
            return SensorReadingUpdateMethod::SCHEDULER;
        else if(type == SensorType::CANBUS_RAW || type == SensorType::CANBUS_ANALOG || type == SensorType::CANBUS_INDICATOR)
            return SensorReadingUpdateMethod::ISR;
        else if(type == SensorType::PHYSICAL_INDICATOR)
            return SensorReadingUpdateMethod::ISR;

        return SensorReadingUpdateMethod::NONE;
    }

    void UpdateConnectionString() {
        if(type == SensorType::CANBUS_RAW || type == SensorType::CANBUS_ANALOG || type == SensorType::CANBUS_INDICATOR)
            connection_string = canbus_source->ToConnectionString();
        else
            connection_string = "";
    }

    void UnwrapConnectionString() {
        if(type == SensorType::CANBUS_RAW || type == SensorType::CANBUS_ANALOG || type == SensorType::CANBUS_INDICATOR)
            canbus_source = eerie_memory::make_unique_pmr<CanbusSource>(alloc_, CanbusSource::FromConnectionString(alloc_, connection_string));
    }

private:
    allocator_type alloc_;
};

} // namespace eerie_leap::domain::sensor_domain::models
