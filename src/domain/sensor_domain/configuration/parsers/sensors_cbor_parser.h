#pragma once

#include <vector>
#include <memory>
#include <span>
#include <eerie_memory.hpp>

#include "subsys/fs/services/i_fs_service.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "domain/sensor_domain/models/sensor.h"

namespace eerie_leap::domain::sensor_domain::configuration::parsers {

using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::domain::sensor_domain::models::Sensor;

class SensorsCborParser {
private:
    std::shared_ptr<IFsService> sd_fs_service_;

public:
    explicit SensorsCborParser(std::shared_ptr<IFsService> sd_fs_service);

    eerie_memory::pmr_unique_ptr<CborSensorsConfig> Serialize(
        const std::vector<std::shared_ptr<Sensor>>& sensors,
        uint32_t gpio_channel_count,
        uint32_t adc_channel_count);
    std::vector<std::shared_ptr<Sensor>> Deserialize(
        std::pmr::memory_resource* mr,
        const CborSensorsConfig& sensors_config,
        uint32_t gpio_channel_count,
        uint32_t adc_channel_count);
};

} // namespace eerie_leap::domain::sensor_domain::configuration::parsers
