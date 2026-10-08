#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <zephyr/logging/log.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"
#include "domain/script_domain/utilities/global_fuctions_registry.h"

#include "sensor_pipeline_builder.h"

namespace eerie_leap::domain::sensor_domain::runtime {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::voltage_interpolator;
using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::domain::script_domain::utilities::GlobalFunctionsRegistry;

LOG_MODULE_REGISTER(sensor_pipeline_builder_logger);

SensorPipelineBuilder::SensorPipelineBuilder(
    std::shared_ptr<IFsService> sd_fs_service,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame)
        : sd_fs_service_(std::move(sd_fs_service)),
        sensor_readings_frame_(std::move(sensor_readings_frame)) {}

void SensorPipelineBuilder::BuildInterpolator(SensorRuntime& runtime) const {
    const auto& configuration = runtime.sensor->configuration;
    if(!configuration.HasInterpolation())
        return;

    auto* mr = Mrm::GetExtPmr();
    auto calibration_table = make_shared_pmr<std::pmr::vector<CalibrationData>>(
        mr, std::pmr::vector<CalibrationData>(configuration.calibration_table.begin(), configuration.calibration_table.end(), mr));

    switch(configuration.interpolation_method) {
    case InterpolationMethod::LINEAR:
        runtime.voltage_interpolator = make_unique_pmr<LinearVoltageInterpolator>(mr, calibration_table);
        break;

    case InterpolationMethod::CUBIC_SPLINE:
        runtime.voltage_interpolator = make_unique_pmr<CubicSplineVoltageInterpolator>(mr, calibration_table);
        break;

    default:
        throw std::invalid_argument("Sensor " + std::string(runtime.sensor->id) + " uses an unsupported interpolation method.");
    }
}

void SensorPipelineBuilder::BuildExpression(SensorRuntime& runtime) const {
    const auto& configuration = runtime.sensor->configuration;
    if(!configuration.HasExpression())
        return;

    auto* mr = Mrm::GetExtPmr();
    auto evaluator = ExpressionEvaluator::Create(configuration.expression, mr);
    if(!evaluator.has_value()) {
        throw std::invalid_argument(
            "Sensor " + std::string(runtime.sensor->id) + ": " + ExpressionEvaluator::Describe(evaluator.error()));
    }

    runtime.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(mr, std::move(*evaluator));

    // Variables are bound to the frame's value slots now, so nothing is resolved per sample and
    // the addresses stay valid for as long as this generation runs.
    for(const std::string_view name : runtime.expression_evaluator->GetVariableNames()) {
        const auto slot = sensor_readings_frame_->FindSlot(StringHelpers::GetHash(name));
        if(!slot.has_value()) {
            throw std::invalid_argument(
                "Sensor " + std::string(runtime.sensor->id) + " depends on non-existent sensor " + std::string(name) + ".");
        }

        runtime.expression_evaluator->BindVariable(name, sensor_readings_frame_->GetValueAddress(slot.value()));
        runtime.input_slots.push_back(slot.value());
    }
}

void SensorPipelineBuilder::BuildScript(SensorGeneration& generation, SensorRuntime& runtime) const {
    const auto& configuration = runtime.sensor->configuration;
    if(!configuration.HasScript())
        return;

    if(sd_fs_service_ == nullptr || !sd_fs_service_->IsAvailable() || !sd_fs_service_->Exists(configuration.script_path)) {
        LOG_WRN("Sensor %s: script %s is not available.", runtime.sensor->id.c_str(), configuration.script_path.c_str());
        return;
    }

    const size_t script_size = sd_fs_service_->GetFileSize(configuration.script_path).value_or(0);
    if(script_size == 0)
        return;

    std::pmr::vector<uint8_t> buffer(script_size, Mrm::GetExtPmr());

    size_t out_len = 0;
    if(!sd_fs_service_->ReadFile(configuration.script_path, buffer.data(), script_size, out_len)) {
        LOG_ERR("Sensor %s: failed to read script %s.", runtime.sensor->id.c_str(), configuration.script_path.c_str());
        return;
    }

    // One state serves every scripted sensor of the generation; the sensor functions are
    // registered once, each script gets its own environment.
    if(generation.script_host == nullptr) {
        auto host = make_shared_pmr<LuaScript>(Mrm::GetExtPmr(), LuaScript::CreateExt());
        if(!host->IsValid()) {
            LOG_ERR("Sensor %s: no Lua state for script %s.", runtime.sensor->id.c_str(), configuration.script_path.c_str());
            return;
        }

        GlobalFunctionsRegistry::RegisterGetSensorValue(*host, *sensor_readings_frame_);
        GlobalFunctionsRegistry::RegisterUpdateSensorValue(*host, *sensor_readings_frame_);

        generation.script_host = std::move(host);
    }

    auto& script = runtime.script;
    script.host = generation.script_host;

    auto guard = script.host->Lock();
    script.environment = script.host->LoadChunk(std::span<const uint8_t>(buffer.data(), out_len), runtime.sensor->id.c_str());
    if(script.environment == LuaScript::kNoRef) {
        LOG_ERR("Sensor %s: script %s failed to load.", runtime.sensor->id.c_str(), configuration.script_path.c_str());
        return;
    }

    script.create_value = script.host->FindFunction(script.environment, SensorScript::kCreateValueFunction);
    script.post_process = script.host->FindFunction(script.environment, SensorScript::kPostProcessFunction);
}

// Slots follow the configuration order, so a slot index is also the runtime's index.
void SensorPipelineBuilder::LinkDependents(SensorGeneration& generation) const {
    auto& runtimes = generation.runtimes;
    const size_t count = runtimes.size();

    std::vector<std::vector<size_t>> direct(count);
    for(size_t i = 0; i < count; i++) {
        if(runtimes[i].update_method != SensorReadingUpdateMethod::DEPENDENT)
            continue;

        for(const auto input : runtimes[i].input_slots)
            direct[input.index].push_back(i);
    }

    // The transitive closure in processing order evaluates every dependent once, after its inputs.
    std::vector<bool> reached(count);
    std::vector<size_t> pending;
    for(size_t i = 0; i < count; i++) {
        std::fill(reached.begin(), reached.end(), false);
        pending.assign(direct[i].begin(), direct[i].end());

        while(!pending.empty()) {
            const size_t index = pending.back();
            pending.pop_back();

            if(reached[index])
                continue;

            reached[index] = true;
            pending.insert(pending.end(), direct[index].begin(), direct[index].end());
        }

        for(size_t j = 0; j < count; j++) {
            if(reached[j])
                runtimes[i].dependents.push_back(&runtimes[j]);
        }
    }
}

std::shared_ptr<SensorGeneration> SensorPipelineBuilder::Build(
    std::shared_ptr<const std::vector<std::shared_ptr<Sensor>>> sensors) const {

    auto generation = std::make_shared<SensorGeneration>();
    generation->sensors = std::move(sensors);
    generation->id = sensor_readings_frame_->Configure(*generation->sensors);
    generation->runtimes.reserve(generation->sensors->size());

    for(const auto& sensor : *generation->sensors) {
        SensorRuntime runtime;
        runtime.sensor = sensor;
        runtime.slot = sensor_readings_frame_->FindSlot(sensor->id_hash).value();
        runtime.update_method = sensor->configuration.GetReadingUpdateMethod();

        BuildInterpolator(runtime);
        BuildExpression(runtime);
        BuildScript(*generation, runtime);

        generation->runtimes.push_back(std::move(runtime));
    }

    LinkDependents(*generation);

    return generation;
}

} // namespace eerie_leap::domain::sensor_domain::runtime
