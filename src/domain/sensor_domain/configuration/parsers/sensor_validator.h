#pragma once

#include "subsys/fs/services/i_fs_service.h"
#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/sensor_domain/models/sensor.h"

namespace eerie_leap::domain::sensor_domain::configuration::parsers {

using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorConfiguration;

class SensorValidator {
private:
    static void ValidateId(const std::vector<std::shared_ptr<Sensor>>& sensors);

    static void ValidateMetadata(const std::vector<std::shared_ptr<Sensor>>& sensors);
    static void ValidateName(std::string_view sensor_id, const std::pmr::string& name);
    static void ValidateUnit(std::string_view sensor_id, const std::pmr::string& unit);
    static void ValidateDescription(std::string_view sensor_id, const std::pmr::string& description);

    static void ValidateSensorConfiguration(
        const std::vector<std::shared_ptr<Sensor>>& sensors,
        IFsService* sd_fs_service,
        uint32_t gpio_channel_count,
        uint32_t adc_channel_count);
    static void ValidateType(std::string_view sensor_id, const SensorConfiguration& sensor_configuration);
    static void ValidateChannel(
        std::string_view sensor_id,
        const SensorConfiguration& sensor_configuration,
        uint32_t gpio_channel_count,
        uint32_t adc_channel_count);
    static void ValidateConnectionString(std::string_view sensor_id, const SensorConfiguration& sensor_configuration);
    static void ValidateScriptPath(std::string_view sensor_id, const SensorConfiguration& sensor_configuration, IFsService* sd_fs_service);
    static void ValidateSamplingRateMs(std::string_view sensor_id, const SensorConfiguration& sensor_configuration);
    static void ValidateInterpolationMethod(std::string_view sensor_id, const SensorConfiguration& sensor_configuration);
    static void ValidateCalibrationTable(std::string_view sensor_id, const SensorConfiguration& sensor_configuration);
    static void ValidateExpression(
        std::string_view sensor_id,
        const SensorConfiguration& sensor_configuration,
        const std::vector<std::shared_ptr<Sensor>>& sensors);

public:
    /// The most points a calibration table holds, the schema's 2*50 (sensors_config.cddl); the decoder does not count them.
    static constexpr size_t MAX_CALIBRATION_POINTS = 50;

    static void Validate(const std::vector<std::shared_ptr<Sensor>>& sensors, IFsService* sd_fs_service, uint32_t gpio_channel_count, uint32_t adc_channel_count);

    /**
     * @brief Checks the CAN sensors against a CAN bus configuration: each one's bus must be configured,
     *        and a signal sensor's frame and signal must be defined on it. A raw sensor reads every
     *        frame with its ID, so it needs only the bus. Expects validated sensors (canbus_source set).
     */
    static void ValidateCanbusSources(
        const std::vector<std::shared_ptr<Sensor>>& sensors,
        const CanbusConfiguration& canbus_configuration);
};

} // namespace eerie_leap::domain::sensor_domain::configuration::parsers
