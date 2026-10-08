#include <algorithm>
#include <utility>

#include <zephyr/kernel.h>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/cbor/cbor_helpers.hpp"
#include "domain/sensor_domain/utilities/sensors_order_resolver.h"
#include "sensor_validator.h"

#include "sensors_cbor_parser.h"

namespace eerie_leap::domain::sensor_domain::configuration::parsers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::models;

SensorsCborParser::SensorsCborParser(std::shared_ptr<IFsService> sd_fs_service)
    : sd_fs_service_(std::move(sd_fs_service)) {}

// Serializes plain configuration data; nothing executable is built or touched here.
pmr_unique_ptr<CborSensorsConfig> SensorsCborParser::Serialize(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    uint32_t gpio_channel_count,
    uint32_t adc_channel_count) {

    // NOTE: UpdateConnectionString() must be called before validation
    for(auto& sensor : sensors)
        sensor->configuration.UpdateConnectionString();
    SensorValidator::Validate(sensors, sd_fs_service_.get(), gpio_channel_count, adc_channel_count);

    auto sensors_config = make_unique_pmr<CborSensorsConfig>(Mrm::GetExtPmr());
    SensorsOrderResolver order_resolver;

    for(const auto& sensor : sensors) {

        CborSensorConfig sensor_config(std::allocator_arg, Mrm::GetExtPmr());

        sensor_config.id = CborHelpers::ToZcborString(sensor->id);
        sensor_config.configuration.type = std::to_underlying(sensor->configuration.type);

        if(sensor->configuration.channel.has_value()) {
            sensor_config.configuration.channel_present = true;
            sensor_config.configuration.channel = sensor->configuration.channel.value();
        } else {
            sensor_config.configuration.channel_present = false;
        }

        sensor_config.configuration.connection_string = CborHelpers::ToZcborString(sensor->configuration.connection_string);
        sensor_config.configuration.script_path = CborHelpers::ToZcborString(sensor->configuration.script_path);
        sensor_config.configuration.sampling_rate_ms = sensor->configuration.sampling_rate_ms.has_value() && sensor->configuration.sampling_rate_ms.value() > 0
            ? sensor->configuration.sampling_rate_ms.value()
            : -1;

        sensor_config.configuration.interpolation_method = static_cast<uint32_t>(sensor->configuration.interpolation_method);
        if(sensor->configuration.HasInterpolation()) {
            sensor_config.configuration.calibration_table_present = true;

            // Stored sorted; the caller's table is left as it is.
            for(const auto& calibration_data : sensor->configuration.calibration_table) {
                sensor_config.configuration.calibration_table.float32float.push_back({
                    .float32float_key = calibration_data.voltage,
                    .float32float = calibration_data.value});
            }

            std::ranges::sort(
                sensor_config.configuration.calibration_table.float32float,
                [](const auto& a, const auto& b) { return a.float32float_key < b.float32float_key; });
        } else {
            sensor_config.configuration.calibration_table_present = false;
        }

        if(sensor->configuration.HasExpression()) {
            sensor_config.configuration.expression_present = true;
            sensor_config.configuration.expression = CborHelpers::ToZcborString(sensor->configuration.expression);
        } else {
            sensor_config.configuration.expression_present = false;
        }

        sensor_config.metadata.unit = CborHelpers::ToZcborString(sensor->metadata.unit);
        sensor_config.metadata.name = CborHelpers::ToZcborString(sensor->metadata.name);

        sensor_config.metadata.description = CborHelpers::ToZcborString(sensor->metadata.description);

        sensors_config->CborSensorConfig_m.push_back(std::move(sensor_config));

        order_resolver.AddSensor(sensor);
    }

    // Validate dependencies
    order_resolver.GetProcessingOrder();

    return sensors_config;
}

// Produces plain configuration data in processing order. Interpreters and scripts are built
// from it by SensorPipelineBuilder when the pipeline starts.
std::vector<std::shared_ptr<Sensor>> SensorsCborParser::Deserialize(
    std::pmr::memory_resource* mr,
    const CborSensorsConfig& sensors_config,
    uint32_t gpio_channel_count,
    uint32_t adc_channel_count) {

    SensorsOrderResolver order_resolver;

    for(const auto& sensor_config : sensors_config.CborSensorConfig_m) {
        auto sensor = make_shared_pmr<Sensor>(mr, CborHelpers::ToPmrString(mr, sensor_config.id));

        sensor->configuration.type = static_cast<SensorType>(sensor_config.configuration.type);

        if(sensor_config.configuration.channel_present)
            sensor->configuration.channel = sensor_config.configuration.channel;
        else
            sensor->configuration.channel = std::nullopt;

        sensor->configuration.connection_string = CborHelpers::ToPmrString(mr, sensor_config.configuration.connection_string);
        sensor->configuration.UnwrapConnectionString();

        sensor->configuration.script_path = CborHelpers::ToPmrString(mr, sensor_config.configuration.script_path);

        sensor->configuration.sampling_rate_ms = sensor_config.configuration.sampling_rate_ms > 0
            ? std::optional<int>(sensor_config.configuration.sampling_rate_ms)
            : std::nullopt;

        auto interpolation_method = static_cast<InterpolationMethod>(sensor_config.configuration.interpolation_method);
        if(interpolation_method != InterpolationMethod::NONE && sensor_config.configuration.calibration_table_present) {
            sensor->configuration.interpolation_method = interpolation_method;

            auto& calibration_table = sensor->configuration.calibration_table;
            calibration_table.reserve(sensor_config.configuration.calibration_table.float32float.size());
            for(const auto& calibration_data : sensor_config.configuration.calibration_table.float32float) {
                calibration_table.push_back({
                    .voltage = calibration_data.float32float_key,
                    .value = calibration_data.float32float});
            }

            // Interpolators binary search the table, and an imported one may be stored unordered.
            std::ranges::sort(
                calibration_table,
                [](const CalibrationData& a, const CalibrationData& b) {
                    return a.voltage < b.voltage;
                });
        } else {
            sensor->configuration.interpolation_method = InterpolationMethod::NONE;
        }

        if(sensor_config.configuration.expression_present)
            sensor->configuration.expression = CborHelpers::ToPmrString(mr, sensor_config.configuration.expression);

        sensor->metadata.unit = CborHelpers::ToPmrString(mr, sensor_config.metadata.unit);
        sensor->metadata.name = CborHelpers::ToPmrString(mr, sensor_config.metadata.name);

        sensor->metadata.description = CborHelpers::ToPmrString(mr, sensor_config.metadata.description);

        order_resolver.AddSensor(std::move(sensor));
    }

    auto sensors = order_resolver.GetProcessingOrder();
    SensorValidator::Validate(sensors, sd_fs_service_.get(), gpio_channel_count, adc_channel_count);

    return sensors;
}

} // namespace eerie_leap::domain::sensor_domain::configuration::parsers
