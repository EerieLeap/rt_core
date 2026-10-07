#include <cmath>
#include <memory>
#include <stdexcept>

#include "expression_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using namespace eerie_leap::domain::sensor_domain::models;

ExpressionProcessor::ExpressionProcessor(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame) :
    sensor_readings_frame_(std::move(sensor_readings_frame)) {}

// Evaluating against a sensor that has no value yet would read NaN, or 0 through a comparison.
bool ExpressionProcessor::AreInputsAvailable(const ExpressionEvaluator& expression_evaluator) const {
    for(const auto& sensor_id : expression_evaluator.GetVariableNames()) {
        if(sensor_id == "x")
            continue;

        if(!sensor_readings_frame_->TryGetReadingValue(sensor_id).has_value())
            return false;
    }

    return true;
}

// Returns the result, or nothing after recording the failure on the reading.
std::optional<float> ExpressionProcessor::Evaluate(
    ExpressionEvaluator& expression_evaluator, std::optional<float> x, SensorReading& reading) const {

    if(!AreInputsAvailable(expression_evaluator)) {
        reading.SetError(ReadingError::INPUT_UNAVAILABLE);
        return std::nullopt;
    }

    float value = 0.0F;
    try {
        value = expression_evaluator.Evaluate(x);
    } catch(const std::exception&) {
        reading.SetError(ReadingError::EXPRESSION_FAILED);
        return std::nullopt;
    }

    if(std::isnan(value)) {
        reading.SetError(ReadingError::EXPRESSION_NOT_A_NUMBER);
        return std::nullopt;
    }

    return value;
}

void ExpressionProcessor::Process(const Sensor& sensor, SensorReading& reading) {
    if(reading.status > ReadingStatus::INTERPOLATED) {
        reading.SetError(ReadingError::WRONG_STATE);
        return;
    }

    const auto& configuration = sensor.configuration;
    auto* expression_evaluator = configuration.expression_evaluator.get();

    switch(configuration.type) {
    case SensorType::PHYSICAL_ANALOG:
    case SensorType::CANBUS_ANALOG: {
        if(!reading.value.has_value()) {
            reading.SetError(ReadingError::NO_VALUE);
            return;
        }

        if(expression_evaluator != nullptr) {
            const auto value = Evaluate(*expression_evaluator, reading.value, reading);
            if(!value.has_value())
                return;

            reading.value = value;
        }
        break;
    }

    case SensorType::PHYSICAL_INDICATOR:
    case SensorType::CANBUS_INDICATOR: {
        if(!reading.value.has_value()) {
            reading.SetError(ReadingError::NO_VALUE);
            return;
        }

        // Indicators are evaluated and stored as 0 or 1.
        bool value = reading.value.value() != 0.0F;

        if(expression_evaluator != nullptr) {
            const auto result = Evaluate(*expression_evaluator, static_cast<float>(value), reading);
            if(!result.has_value())
                return;

            value = result.value() != 0.0F;
        }

        reading.value = value ? 1.0F : 0.0F;
        break;
    }

    case SensorType::VIRTUAL_ANALOG:
    case SensorType::VIRTUAL_INDICATOR: {
        if(expression_evaluator == nullptr) {
            reading.SetError(ReadingError::EXPRESSION_FAILED);
            return;
        }

        const auto value = Evaluate(*expression_evaluator, std::nullopt, reading);
        if(!value.has_value())
            return;

        reading.value = value;
        break;
    }

    case SensorType::USER_ANALOG:
    case SensorType::USER_INDICATOR: {
        if(expression_evaluator != nullptr) {
            const auto value = Evaluate(*expression_evaluator, reading.value, reading);
            if(!value.has_value())
                return;

            reading.value = value;
        }
        break;
    }

    default:
        return;
    }

    reading.status = ReadingStatus::EXPRESSION_EVALUATED;
}

} // namespace eerie_leap::domain::sensor_domain::processors
