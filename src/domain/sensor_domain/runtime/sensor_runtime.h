#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <eerie_memory.hpp>

#include "utilities/voltage_interpolator/i_voltage_interpolator.h"
#include "subsys/math_parser/expression_evaluator.h"
#include "subsys/lua_script/lua_script.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_slot.h"
#include "domain/sensor_domain/models/sensor_reading_update_method.h"

namespace eerie_leap::domain::sensor_domain::runtime {

using eerie_leap::utilities::voltage_interpolator::IVoltageInterpolator;
using eerie_leap::subsys::math_parser::ExpressionEvaluator;
using eerie_leap::subsys::lua_script::LuaScript;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorSlot;
using eerie_leap::domain::sensor_domain::models::SensorReadingUpdateMethod;

// Everything executable that a sensor needs, built from its configuration for one generation.
// Owned by SensorGeneration; readers and tasks refer to it for as long as the generation runs.
struct SensorRuntime {
    std::shared_ptr<Sensor> sensor;
    SensorSlot slot;
    SensorReadingUpdateMethod update_method = SensorReadingUpdateMethod::NONE;

    eerie_memory::pmr_unique_ptr<IVoltageInterpolator> voltage_interpolator = nullptr;
    eerie_memory::pmr_unique_ptr<ExpressionEvaluator> expression_evaluator = nullptr;
    std::shared_ptr<LuaScript> lua_script = nullptr;

    // Slots of the sensors the expression reads, resolved when the generation was built.
    std::vector<SensorSlot> input_slots;

    [[nodiscard]] const Sensor& GetSensor() const { return *sensor; }
};

// One sensors configuration turned into a running pipeline: runtimes in processing order over
// the frame's slots. Replaced as a whole when the configuration changes.
struct SensorGeneration {
    uint32_t id = 0;
    std::shared_ptr<const std::vector<std::shared_ptr<Sensor>>> sensors;   // Keeps the configuration alive.
    std::vector<SensorRuntime> runtimes;

    [[nodiscard]] const SensorRuntime* Find(uint32_t sensor_id_hash) const {
        for(const auto& runtime : runtimes) {
            if(runtime.sensor->id_hash == sensor_id_hash)
                return &runtime;
        }

        return nullptr;
    }
};

} // namespace eerie_leap::domain::sensor_domain::runtime
