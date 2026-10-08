#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "utilities/string/string_helpers.h"
#include "subsys/math_parser/expression_evaluator.h"
#include "domain/sensor_domain/models/sensor_type_traits.h"

#include "sensor_validator.h"

namespace eerie_leap::domain::sensor_domain::configuration::parsers {

using namespace eerie_leap::domain::sensor_domain::models;

using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::subsys::math_parser::ExpressionEvaluator;

static void InvalidSensorConfiguration(std::string_view sensor_id, std::string_view message) {
    throw std::invalid_argument(
        "Invalid Sensor configuration. Sensor ID: "
        + std::string(sensor_id)
        + ". "
        + std::string(message));
}

static void InvalidMetadataConfiguration(std::string_view sensor_id, std::string_view message) {
    throw std::invalid_argument(
        "Invalid Sensor metadata configuration. Sensor ID: "
        + std::string(sensor_id)
        + ". "
        + std::string(message));
}

void SensorValidator::Validate(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    IFsService* sd_fs_service,
    uint32_t gpio_channel_count,
    uint32_t adc_channel_count) {

    if(sensors.size() > CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT) {
        throw std::invalid_argument(
            "Invalid Sensors configuration. "
            + std::to_string(sensors.size())
            + " sensors configured, at most "
            + std::to_string(CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT)
            + " are supported.");
    }

    ValidateId(sensors);
    ValidateMetadata(sensors);
    ValidateSensorConfiguration(sensors, sd_fs_service, gpio_channel_count, adc_channel_count);
}

void SensorValidator::ValidateId(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    std::unordered_set<std::string_view> ids;
    // Readings, events, bindings and logging refer to a sensor by this hash, so it has to be unique too.
    std::unordered_map<uint32_t, std::string_view> id_hashes;

    for(const auto& sensor : sensors) {
        const std::string_view sensor_id = sensor->id;

        if(sensor_id.empty())
            InvalidSensorConfiguration(sensor_id, "Sensor ID cannot be empty.");

        static constexpr std::string_view valid_symbols = "_";

        if(!std::isalpha(sensor_id[0]) && valid_symbols.find(sensor_id[0]) == std::string_view::npos)
            InvalidSensorConfiguration(sensor_id, "Sensor ID must start with a letter or an underscore.");

        if(!std::ranges::all_of(sensor_id, [](char c) {
            return std::isalnum(c) || valid_symbols.find(c) != std::string_view::npos;})) {

            InvalidSensorConfiguration(sensor_id, "Sensor ID must contain only letters, digits, and underscores.");
        }

        if(!ids.insert(sensor_id).second)
            InvalidSensorConfiguration(sensor_id, "Duplicate sensor ID.");

        const auto [it, inserted] = id_hashes.try_emplace(StringHelpers::GetHash(sensor_id), sensor_id);
        if(!inserted) {
            InvalidSensorConfiguration(sensor_id,
                "Sensor ID hash collides with sensor " + std::string(it->second) + ", rename one of them.");
        }
    }
}

void SensorValidator::ValidateMetadata(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    for(const auto& sensor : sensors) {
        ValidateName(sensor->id, sensor->metadata.name);
        ValidateUnit(sensor->id, sensor->metadata.unit);
        ValidateDescription(sensor->id, sensor->metadata.description);
    }
}

void SensorValidator::ValidateName(std::string_view sensor_id, const std::pmr::string& name) {
    if(name.size() > 32)
        InvalidMetadataConfiguration(sensor_id, "Sensor name must be less than 32 characters.");
}

void SensorValidator::ValidateUnit(std::string_view sensor_id, const std::pmr::string& unit) {
    if(unit.size() > 32)
        InvalidMetadataConfiguration(sensor_id, "Sensor unit must be less than 32 characters.");
}

void SensorValidator::ValidateDescription(std::string_view sensor_id, const std::pmr::string& description) {
    if(description.size() > 128)
        InvalidMetadataConfiguration(sensor_id, "Sensor description must be less than 128 characters.");
}

void SensorValidator::ValidateSensorConfiguration(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    IFsService* sd_fs_service,
    uint32_t gpio_channel_count,
    uint32_t adc_channel_count) {

    for(const auto& sensor : sensors) {
        ValidateType(sensor->id, sensor->configuration);
        ValidateChannel(sensor->id, sensor->configuration, gpio_channel_count, adc_channel_count);
        ValidateConnectionString(sensor->id, sensor->configuration);
        ValidateScriptPath(sensor->id, sensor->configuration, sd_fs_service);
        ValidateSamplingRateMs(sensor->id, sensor->configuration);
        ValidateInterpolationMethod(sensor->id, sensor->configuration);
        ValidateExpression(sensor->id, sensor->configuration);
    }
}

void SensorValidator::ValidateType(std::string_view sensor_id, const SensorConfiguration& sensor_configuration) {
    if(sensor_configuration.type == SensorType::NONE || !IsSensorTypeValid(sensor_configuration.type))
        InvalidSensorConfiguration(sensor_id, "Invalid sensor type.");
}

void SensorValidator::ValidateChannel(
    std::string_view sensor_id,
    const SensorConfiguration& sensor_configuration,
    uint32_t gpio_channel_count,
    uint32_t adc_channel_count) {

    const auto traits = sensor_configuration.GetTraits();

    if(!traits.requires_channel) {
        if(sensor_configuration.channel.has_value())
            InvalidSensorConfiguration(sensor_id, "Channel value is not supported for this sensor type.");

        return;
    }

    if(!sensor_configuration.channel.has_value())
        InvalidSensorConfiguration(sensor_id, "Sensor channel is not set.");

    const uint32_t channel_count = traits.source == SensorSourceKind::GPIO ? gpio_channel_count : adc_channel_count;

    if(sensor_configuration.channel.value() >= channel_count)
        InvalidSensorConfiguration(sensor_id, "Channel value is out of range.");
}

void SensorValidator::ValidateConnectionString(std::string_view sensor_id, const SensorConfiguration& sensor_configuration) {
    const auto traits = sensor_configuration.GetTraits();

    if(!traits.uses_canbus) {
        if(!sensor_configuration.connection_string.empty())
            InvalidSensorConfiguration(sensor_id, "Connection string is not supported for this sensor type.");

        return;
    }

    if(sensor_configuration.connection_string.empty() || sensor_configuration.canbus_source == nullptr)
        InvalidSensorConfiguration(sensor_id, "Connection string cannot be empty.");

    if(traits.requires_signal_name && sensor_configuration.canbus_source->signal_name.empty())
        InvalidSensorConfiguration(sensor_id, "Sensor must have CAN bus signal name.");
}

void SensorValidator::ValidateScriptPath(std::string_view sensor_id, const SensorConfiguration& sensor_configuration, IFsService* sd_fs_service) {
    if(sd_fs_service == nullptr)
        return;

    if(!sensor_configuration.script_path.empty()) {
        if(!sd_fs_service->Exists(sensor_configuration.script_path))
            InvalidSensorConfiguration(sensor_id, "Invalid sensor script path.");
    }
}

// Mirrors SensorConfiguration::GetReadingUpdateMethod(): a sensor whose update method
// resolves to NONE would be accepted and then never produce a reading.
void SensorValidator::ValidateSamplingRateMs(std::string_view sensor_id, const SensorConfiguration& sensor_configuration) {
    const auto traits = sensor_configuration.GetTraits();
    const bool has_sampling_rate = sensor_configuration.sampling_rate_ms.has_value();

    if(has_sampling_rate && sensor_configuration.sampling_rate_ms.value() <= 0)
        InvalidSensorConfiguration(sensor_id, "Invalid sampling rate value.");

    if(!traits.allows_sampling_rate && has_sampling_rate)
        InvalidSensorConfiguration(sensor_id, "CAN bus sensors are updated by received frames and do not support a sampling rate.");

    if(traits.requires_sampling_rate && !has_sampling_rate)
        InvalidSensorConfiguration(sensor_id, "Sensor must have a sampling rate.");
}

void SensorValidator::ValidateInterpolationMethod(std::string_view sensor_id, const SensorConfiguration& sensor_configuration) {
    const auto traits = sensor_configuration.GetTraits();

    if(!traits.requires_interpolation) {
        if(sensor_configuration.HasInterpolation() || !sensor_configuration.calibration_table.empty())
            InvalidSensorConfiguration(sensor_id, "Sensor does not support interpolation.");

        return;
    }

    if(!sensor_configuration.HasInterpolation())
        InvalidSensorConfiguration(sensor_id, "Sensor must have interpolation method.");

    if(sensor_configuration.calibration_table.size() < 2)
        InvalidSensorConfiguration(sensor_id, "Calibration table must have at least 2 points.");
}

void SensorValidator::ValidateExpression(std::string_view sensor_id, const SensorConfiguration& sensor_configuration) {
    const auto traits = sensor_configuration.GetTraits();

    if(!traits.allows_expression && sensor_configuration.HasExpression())
        InvalidSensorConfiguration(sensor_id, "Sensor does not support expression evaluator.");

    if(traits.requires_expression && !sensor_configuration.HasExpression())
        InvalidSensorConfiguration(sensor_id, "Sensor must have expression evaluator.");

    if(!sensor_configuration.HasExpression())
        return;

    // Parsed here so an invalid expression is reported at save time with its message.
    size_t input_count = 0;
    try {
        ExpressionEvaluator expression_evaluator(std::string(sensor_configuration.expression));

        for(const auto& name : expression_evaluator.GetVariableNames())
            input_count += name == "x" ? 0 : 1;
    } catch(const std::invalid_argument& e) {
        InvalidSensorConfiguration(sensor_id, e.what());
    }

    // A virtual sensor is either polled or evaluated when an input commits; with neither it never updates.
    if(traits.source == SensorSourceKind::EXPRESSION && !sensor_configuration.sampling_rate_ms.has_value() && input_count == 0)
        InvalidSensorConfiguration(sensor_id, "Sensor must have a sampling rate or depend on another sensor.");
}

} // namespace eerie_leap::domain::sensor_domain::configuration::parsers
