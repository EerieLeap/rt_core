#pragma once

#include <algorithm>
#include <cstdint>
#include <eerie_memory.hpp>
#include <memory>
#include <ranges>
#include <vector>

#include "domain/adc_domain/models/adc_configuration.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading_update_method.h"
#include "domain/sensor_domain/models/sensor_slot.h"
#include "domain/sensor_domain/processors/indicator_conditioner.h"
#include "domain/sensor_domain/runtime/sensor_script.h"
#include "subsys/expression_engine/expression_evaluator.h"
#include "subsys/lua_script/lua_script.h"
#include "utilities/voltage_interpolator/i_voltage_interpolator.h"

namespace eerie_leap::domain::sensor_domain::runtime {

using eerie_leap::domain::adc_domain::models::AdcConfiguration;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReadingUpdateMethod;
using eerie_leap::domain::sensor_domain::models::SensorSlot;
using eerie_leap::domain::sensor_domain::models::SensorSourceKind;
using eerie_leap::domain::sensor_domain::models::SensorValueKind;
using eerie_leap::domain::sensor_domain::processors::IndicatorConditioner;
using eerie_leap::domain::sensor_domain::processors::IndicatorSwitchState;
using eerie_leap::subsys::expression_engine::ExpressionEvaluator;
using eerie_leap::subsys::lua_script::LuaScript;
using eerie_leap::utilities::voltage_interpolator::IVoltageInterpolator;

// Everything executable that a sensor needs, built from its configuration for one generation.
// Owned by SensorGeneration; readers and tasks refer to it for as long as the generation runs.
struct SensorRuntime {
    std::shared_ptr<Sensor> sensor;
    SensorSlot slot;
    SensorReadingUpdateMethod update_method = SensorReadingUpdateMethod::NONE;

    eerie_memory::PmrUniquePtr<IVoltageInterpolator> voltage_interpolator = nullptr;
    eerie_memory::PmrUniquePtr<ExpressionEvaluator> expression_evaluator = nullptr;
    SensorScript script;

    // Slots of the sensors the expression reads, resolved when the generation was built.
    std::vector<SensorSlot> input_slots;

    // A USER sensor that nothing of its own gives a value (no create_sensor_value, no
    // post_process_sensor_value, no expression): its slot's latest value, NaN for none, which
    // scripts write with update_sensor_value. Its ticks carry that value. Null for every other sensor.
    const float* written_value = nullptr;

    // DEPENDENT sensors that read this one, directly or through other dependents, in processing
    // order. Evaluated right after a reading of this sensor is committed.
    std::vector<const SensorRuntime*> dependents;

    // Indicators with a debounce or Toggle only. Its state changes as readings arrive, on the
    // processing work queue. On the default heap rather than PSRAM: it holds a kernel timeout, as
    // the readers' work items do.
    std::unique_ptr<IndicatorConditioner> conditioner = nullptr;

    [[nodiscard]] const Sensor& GetSensor() const {
        return *sensor;
    }

    // A control indicator the generation runs, not a disabled one: it has no input until its control
    // publishes, so the start gives it a released one.
    [[nodiscard]] bool IsControlIndicator() const {
        if(update_method != SensorReadingUpdateMethod::ISR)
            return false;

        const auto traits = sensor->configuration.GetTraits();

        return traits.source == SensorSourceKind::CONTROL && traits.value_kind == SensorValueKind::INDICATOR;
    }
};

// One sensors configuration turned into a running pipeline: runtimes in processing order over
// the frame's slots. Replaced as a whole when the configuration changes.
struct SensorGeneration {
    uint32_t id = 0;
    std::shared_ptr<const std::vector<std::shared_ptr<Sensor>>> sensors; // Keeps the configuration alive.
    std::vector<SensorRuntime> runtimes;
    std::shared_ptr<LuaScript> script_host; // One Lua state for every scripted sensor; null without scripts.

    // The ADC configuration the analog readers were built with, null on a unit without one. Keeps
    // their calibrators alive; a new ADC configuration takes effect with the next generation.
    std::shared_ptr<const AdcConfiguration> adc_configuration;

    [[nodiscard]] const SensorRuntime* Find(uint32_t sensor_id_hash) const {
        for(const auto& runtime : runtimes) {
            if(runtime.sensor->id_hash == sensor_id_hash)
                return &runtime;
        }

        return nullptr;
    }

    /** @return Whether the start has a value to commit: a Toggle switch, or a control indicator's released input. */
    [[nodiscard]] bool HasStartValues() const {
        return std::ranges::any_of(runtimes, [](const SensorRuntime& runtime) {
            return (runtime.conditioner != nullptr && runtime.conditioner->IsSwitch()) || runtime.IsControlIndicator();
        });
    }

    // Drops every held indicator change and waits for a settle in progress. A settle commits into
    // the sensors after it, which may schedule theirs, so going in processing order leaves none
    // scheduled. Only while the sources are stopped; never from the processing work queue.
    void CancelSettles() {
        for(auto& runtime : runtimes) {
            if(runtime.conditioner != nullptr)
                runtime.conditioner->CancelSettle();
        }
    }

    // Stops the window of every held indicator change for a pause, keeping what is left of it; in
    // processing order, as CancelSettles(). Only while the sources are paused.
    void SuspendSettles() {
        for(auto& runtime : runtimes) {
            if(runtime.conditioner != nullptr)
                runtime.conditioner->SuspendSettle();
        }
    }

    // Runs what was left of every suspended window, before the sources resume. In reverse
    // processing order: a settle that runs at once reaches only the sensors after it, which are
    // done by then.
    void ResumeSettles() {
        for(auto& runtime : std::views::reverse(runtimes)) {
            if(runtime.conditioner != nullptr)
                runtime.conditioner->ResumeSettle();
        }
    }

    // The switch of every Toggle indicator, for the next generation. Once the settles are cancelled.
    [[nodiscard]] std::vector<IndicatorSwitchState> GetSwitchStates() const {
        std::vector<IndicatorSwitchState> switch_states;

        for(const auto& runtime : runtimes) {
            if(runtime.conditioner == nullptr || !runtime.conditioner->IsSwitch())
                continue;

            const auto& configuration = runtime.sensor->configuration;
            switch_states.push_back(
                { .sensor_id_hash = runtime.sensor->id_hash,
                    .type = configuration.type,
                    .debounce_ms = configuration.debounce_ms,
                    .input_mode = configuration.input_mode,
                    .is_on = runtime.conditioner->IsSwitchOn() }
            );
        }

        return switch_states;
    }
};

} // namespace eerie_leap::domain::sensor_domain::runtime
