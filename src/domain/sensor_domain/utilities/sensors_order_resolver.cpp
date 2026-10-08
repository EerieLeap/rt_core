#include <stdexcept>
#include <string>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/expression_engine/expression_evaluator.h"

#include "sensors_order_resolver.h"

namespace eerie_leap::domain::sensor_domain::utilities {

using namespace eerie_leap::domain::sensor_domain::models;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::subsys::expression_engine::ExpressionEvaluator;

void SensorsOrderResolver::AddSensor(std::shared_ptr<Sensor> sensor) {
    if(sensor->configuration.HasExpression()) {
        // Compiled only for its variable names; the runtime builds its own evaluator.
        auto evaluator = ExpressionEvaluator::Create(sensor->configuration.expression, Mrm::GetExtPmr());
        if(!evaluator.has_value())
            throw std::invalid_argument("Sensor " + std::string(sensor->id) + ": " + ExpressionEvaluator::Describe(evaluator.error()));

        std::unordered_set<std::string> sensor_ids;
        for(const std::string_view name : evaluator->GetVariableNames())
            sensor_ids.emplace(name);

        dependencies_.try_emplace(sensor->id, std::move(sensor_ids));
    } else {
        dependencies_.try_emplace(sensor->id, std::unordered_set<std::string>());
    }

    sensors_.try_emplace(sensor->id, std::move(sensor));
}

bool SensorsOrderResolver::HasCyclicDependency(
    std::string_view sensor_id,
    std::unordered_set<std::string_view>& visited,
    std::unordered_set<std::string_view>& temp) {

    if(temp.contains(sensor_id))
        return true;
    if(visited.contains(sensor_id))
        return false;

    temp.insert(sensor_id);

    for(const auto& dep : dependencies_.at(sensor_id)) {
        if(!sensors_.contains(dep)) {
            throw std::runtime_error("Sensor "
                + std::string(sensor_id)
                + " depends on non-existent sensor "
                + dep
                + ".");
        }

        if(HasCyclicDependency(dep, visited, temp))
            return true;
    }

    temp.erase(sensor_id);
    visited.insert(sensor_id);

    return false;
}

void SensorsOrderResolver::ResolveDependencies(
    std::string_view sensor_id,
    std::unordered_set<std::string_view>& visited,
    std::vector<std::shared_ptr<Sensor>>& ordered_sensors) {

    if(visited.contains(sensor_id))
        return;

    visited.insert(sensor_id);

    for(const auto& dep : dependencies_.at(sensor_id))
        ResolveDependencies(dep, visited, ordered_sensors);

    ordered_sensors.push_back(sensors_.at(sensor_id));
}

std::vector<std::shared_ptr<Sensor>> SensorsOrderResolver::GetProcessingOrder() {
    std::unordered_set<std::string_view> visited;
    std::unordered_set<std::string_view> temp;

    for(const auto& [sensor_id, _] : sensors_) {
        if(!visited.contains(sensor_id)) {
            if(HasCyclicDependency(sensor_id, visited, temp)) {
                throw std::runtime_error("Cyclic dependency detected in sensor "
                    + std::string(sensor_id)
                    + ".");
            }
        }
    }

    visited.clear();
    std::vector<std::shared_ptr<Sensor>> ordered_sensors;

    for(const auto& [sensor_id, _] : sensors_)
        ResolveDependencies(sensor_id, visited, ordered_sensors);

    return ordered_sensors;
}

} // namespace eerie_leap::domain::sensor_domain::utilities
