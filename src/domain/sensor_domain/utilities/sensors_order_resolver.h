#pragma once

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "domain/sensor_domain/models/sensor.h"

namespace eerie_leap::domain::sensor_domain::utilities {

using eerie_leap::domain::sensor_domain::models::Sensor;

class SensorsOrderResolver {
private:
    std::unordered_map<std::string_view, std::unordered_set<std::string>> dependencies_;
    std::unordered_map<std::string_view, std::shared_ptr<Sensor>> sensors_;

    bool HasCyclicDependency(
        std::string_view sensor_id,
        std::unordered_set<std::string_view>& visited,
        std::unordered_set<std::string_view>& temp
    );

    void ResolveDependencies(
        std::string_view sensor_id,
        std::unordered_set<std::string_view>& visited,
        std::vector<std::shared_ptr<Sensor>>& ordered_sensors
    );

public:
    void AddSensor(const std::shared_ptr<Sensor>& sensor);
    std::vector<std::shared_ptr<Sensor>> GetProcessingOrder();
};

} // namespace eerie_leap::domain::sensor_domain::utilities
