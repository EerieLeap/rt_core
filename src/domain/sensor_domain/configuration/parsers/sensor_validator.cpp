#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "utilities/string/string_helpers.h"
#include "utilities/memory/memory_resource_manager.h"
#include "subsys/expression_engine/expression_evaluator.h"
#include "domain/sensor_domain/models/sensor_type_traits.h"

#include "sensor_validator.h"

namespace eerie_leap::domain::sensor_domain::configuration::parsers {

using namespace eerie_leap::domain::sensor_domain::models;

using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::subsys::expression_engine::ExpressionEvaluator;

[[noreturn]] static void InvalidSensorConfiguration(std::string_view sensor_id, std::string_view message) {
    throw std::invalid_argument(
        "Invalid Sensor configuration. Sensor ID: "
        + std::string(sensor_id)
        + ". "
        + std::string(message));
}

[[noreturn]] static void InvalidMetadataConfiguration(std::string_view sensor_id, std::string_view message) {
    throw std::invalid_argument(
        "Invalid Sensor metadata configuration. Sensor ID: "
        + std::string(sensor_id)
        + ". "
        + std::string(message));
}

// The channels a unit has of one kind, for the out-of-range message.
static std::string ChannelRangeText(std::string_view kind, uint32_t channel_count) {
    if(channel_count == 0)
        return "This unit has no " + std::string(kind) + " channels.";

    if(channel_count == 1)
        return "This unit has only " + std::string(kind) + " channel 0.";

    return "This unit has " + std::string(kind) + " channels 0 to " + std::to_string(channel_count - 1) + ".";
}

// In hex, as CAN tools, the CAN bus validator and the app show frame IDs.
static std::string FrameIdText(uint32_t frame_id) {
    char text[sizeof("0xFFFFFFFF")];
    snprintf(text, sizeof(text), "0x%" PRIX32, frame_id);

    return text;
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
        InvalidMetadataConfiguration(sensor_id, "Sensor name must be at most 32 bytes.");
}

void SensorValidator::ValidateUnit(std::string_view sensor_id, const std::pmr::string& unit) {
    if(unit.size() > 32)
        InvalidMetadataConfiguration(sensor_id, "Sensor unit must be at most 32 bytes.");
}

void SensorValidator::ValidateDescription(std::string_view sensor_id, const std::pmr::string& description) {
    if(description.size() > 128)
        InvalidMetadataConfiguration(sensor_id, "Sensor description must be at most 128 bytes.");
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
        ValidateConditioning(sensor->id, sensor->configuration);
        ValidateExpression(*sensor, sensors);
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

    const bool is_gpio = traits.source == SensorSourceKind::GPIO;
    const uint32_t channel_count = is_gpio ? gpio_channel_count : adc_channel_count;

    if(sensor_configuration.channel.value() >= channel_count) {
        InvalidSensorConfiguration(sensor_id,
            "Channel value is out of range. " + ChannelRangeText(is_gpio ? "GPIO" : "ADC", channel_count));
    }
}

void SensorValidator::ValidateConnectionString(std::string_view sensor_id, const SensorConfiguration& sensor_configuration) {
    const auto traits = sensor_configuration.GetTraits();

    if(!traits.uses_canbus) {
        if(!sensor_configuration.connection_string.empty())
            InvalidSensorConfiguration(sensor_id, "Connection string is not supported for this sensor type.");

        return;
    }

    // The text rather than canbus_source: a stored configuration is parsed into it after validation.
    const auto source = CanbusSource::Parse(sensor_configuration.connection_string);
    if(!source.has_value())
        InvalidSensorConfiguration(sensor_id, source.error());

    if(traits.requires_signal_name && source->signal_name.empty())
        InvalidSensorConfiguration(sensor_id, "Sensor must have CAN bus signal name.");
}

// Only against a mounted card: a stored configuration is validated when it loads, and rejecting it
// while the card is out would run the unit on defaults. A missing script is logged when the
// pipeline is built and the sensor runs without it.
void SensorValidator::ValidateScriptPath(std::string_view sensor_id, const SensorConfiguration& sensor_configuration, IFsService* sd_fs_service) {
    if(sd_fs_service == nullptr || !sd_fs_service->IsAvailable())
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

    ValidateCalibrationTable(sensor_id, sensor_configuration);
}

// Both interpolators binary search the table and the spline divides by the distance between
// neighbouring voltages, so the voltages must be finite and distinct. On the wire the table is a
// map keyed by voltage, and the decoder lets a repeated key through. The table is not ordered yet.
void SensorValidator::ValidateCalibrationTable(std::string_view sensor_id, const SensorConfiguration& sensor_configuration) {
    const auto& calibration_table = sensor_configuration.calibration_table;

    if(calibration_table.size() > MAX_CALIBRATION_POINTS) {
        InvalidSensorConfiguration(sensor_id,
            "Calibration table must have at most " + std::to_string(MAX_CALIBRATION_POINTS) + " points.");
    }

    const bool is_finite = std::ranges::all_of(calibration_table, [](const CalibrationData& point) {
        return std::isfinite(point.voltage) && std::isfinite(point.value);
    });

    if(!is_finite)
        InvalidSensorConfiguration(sensor_id, "Calibration table must hold finite numbers.");

    for(size_t i = 1; i < calibration_table.size(); i++) {
        for(size_t j = 0; j < i; j++) {
            if(calibration_table[i].voltage == calibration_table[j].voltage)
                InvalidSensorConfiguration(sensor_id, "Calibration voltages must be unique.");
        }
    }
}

// Conditioning works on a 0/1 value, so only indicators take it.
void SensorValidator::ValidateConditioning(std::string_view sensor_id, const SensorConfiguration& sensor_configuration) {
    if(!IsIndicatorInputModeValid(sensor_configuration.input_mode))
        InvalidSensorConfiguration(sensor_id, "Invalid indicator input mode.");

    if(sensor_configuration.GetTraits().value_kind != SensorValueKind::INDICATOR) {
        if(sensor_configuration.debounce_ms != 0)
            InvalidSensorConfiguration(sensor_id, "Debounce is only supported for indicator sensors.");

        if(sensor_configuration.input_mode != IndicatorInputMode::Level)
            InvalidSensorConfiguration(sensor_id, "Input mode is only supported for indicator sensors.");
    }

    if(sensor_configuration.debounce_ms > MAX_DEBOUNCE_MS)
        InvalidSensorConfiguration(sensor_id, "Debounce must be at most " + std::to_string(MAX_DEBOUNCE_MS) + " ms.");
}

void SensorValidator::ValidateExpression(const Sensor& sensor, const std::vector<std::shared_ptr<Sensor>>& sensors) {
    const std::string_view sensor_id = sensor.id;
    const auto& sensor_configuration = sensor.configuration;
    const auto traits = sensor_configuration.GetTraits();

    if(!traits.allows_expression && sensor_configuration.HasExpression())
        InvalidSensorConfiguration(sensor_id, "Sensor does not support expression evaluator.");

    if(traits.requires_expression && !sensor_configuration.HasExpression())
        InvalidSensorConfiguration(sensor_id, "Sensor must have expression evaluator.");

    if(!sensor_configuration.HasExpression())
        return;

    // Compiled here so an invalid expression is reported at save time with its position, and an
    // unknown variable as a validation error instead of a failure when the pipeline is built.
    auto evaluator = ExpressionEvaluator::Create(sensor_configuration.expression, Mrm::GetExtPmr());
    if(!evaluator.has_value())
        InvalidSensorConfiguration(sensor_id, ExpressionEvaluator::Describe(evaluator.error()));

    // Without an input the evaluation has no x, fails every time, and the sensor never gets a value.
    if(!traits.has_input && evaluator->UsesInput())
        InvalidSensorConfiguration(sensor_id, "Sensor has no input value, the expression cannot use x.");

    size_t input_count = 0;
    for(const std::string_view name : evaluator->GetVariableNames()) {
        const auto input = std::ranges::find_if(sensors,
            [name](const auto& candidate) { return std::string_view(candidate->id) == name; });

        if(input == sensors.end())
            InvalidSensorConfiguration(sensor_id, "Expression references unknown sensor " + std::string(name) + ".");

        // A disabled input never gets a value, so the expression would never evaluate.
        if(sensor.is_enabled && !(*input)->is_enabled)
            InvalidSensorConfiguration(sensor_id, "Expression references disabled sensor " + std::string(name) + ".");

        input_count++;
    }

    // A virtual sensor is either polled or evaluated when an input commits; with neither it never updates.
    if(traits.source == SensorSourceKind::EXPRESSION && !sensor_configuration.sampling_rate_ms.has_value() && input_count == 0)
        InvalidSensorConfiguration(sensor_id, "Sensor must have a sampling rate or depend on another sensor.");
}

void SensorValidator::ValidateCanbusSources(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    const CanbusConfiguration& canbus_configuration) {

    for(const auto& sensor : sensors) {
        const auto& sensor_configuration = sensor->configuration;
        const auto traits = sensor_configuration.GetTraits();

        if(!traits.uses_canbus || sensor_configuration.canbus_source == nullptr)
            continue;

        const auto& source = *sensor_configuration.canbus_source;
        const std::string bus = std::to_string(source.bus_channel);

        const auto channel = canbus_configuration.channel_configurations.find(source.bus_channel);
        if(channel == canbus_configuration.channel_configurations.end())
            InvalidSensorConfiguration(sensor->id, "CAN bus " + bus + " is not configured.");

        if(traits.source != SensorSourceKind::CAN_SIGNAL)
            continue;

        // Looked up as the frame router resolves the signal when the pipeline is built.
        const auto& messages = channel->second.message_configurations;
        const auto message = std::ranges::find_if(messages, [&source](const auto& message_configuration) {
            return message_configuration != nullptr && message_configuration->frame_id == source.frame_id;
        });

        if(message == messages.end()) {
            InvalidSensorConfiguration(sensor->id,
                "CAN bus " + bus + " has no message with frame ID " + FrameIdText(source.frame_id) + ".");
        }

        // The frame router treats a disabled message as absent.
        if(sensor->is_enabled && !(*message)->is_enabled) {
            InvalidSensorConfiguration(sensor->id,
                "CAN message " + FrameIdText(source.frame_id) + " on bus " + bus + " is disabled.");
        }

        if((*message)->TryGetSignal(source.signal_name_hash) == nullptr) {
            InvalidSensorConfiguration(sensor->id,
                "CAN message " + FrameIdText(source.frame_id) + " on bus " + bus
                + " has no signal " + std::string(source.signal_name) + ".");
        }
    }
}

} // namespace eerie_leap::domain::sensor_domain::configuration::parsers
