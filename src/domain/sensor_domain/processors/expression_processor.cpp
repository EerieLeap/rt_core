#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#include "expression_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using namespace eerie_leap::domain::sensor_domain::models;

ExpressionProcessor::ExpressionProcessor(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame) :
    sensor_readings_frame_(std::move(sensor_readings_frame)) {}

// Evaluating against a sensor that has no value yet would read NaN, or 0 through a comparison.
void ExpressionProcessor::EnsureInputsAvailable(const ExpressionEvaluator& expression_evaluator) const {
    for(const auto& sensor_id : expression_evaluator.GetVariableNames()) {
        if(sensor_id == "x")
            continue;

        if(!sensor_readings_frame_->TryGetReadingValue(sensor_id).has_value())
            throw std::runtime_error("Input sensor " + sensor_id + " has no value yet.");
    }
}

float ExpressionProcessor::Evaluate(ExpressionEvaluator& expression_evaluator, std::optional<float> x) const {
    EnsureInputsAvailable(expression_evaluator);

    const float value = expression_evaluator.Evaluate(x);
    if(std::isnan(value))
        throw std::runtime_error("Expression result is not a number.");

    return value;
}

void ExpressionProcessor::ProcessReading(const uint32_t sensor_id_hash) {
    auto reading_optioanl = sensor_readings_frame_->TryGetReading(sensor_id_hash);
    if(!reading_optioanl)
        return;
    auto reading = std::move(reading_optioanl.value());

    try {
        if(reading.status > ReadingStatus::INTERPOLATED)
            throw std::invalid_argument("Reading is in wrong state");

        const auto& configuration = reading.sensor->configuration;
        auto* expression_evaluator = configuration.expression_evaluator.get();

        switch(configuration.type) {
        case SensorType::PHYSICAL_ANALOG:
        case SensorType::CANBUS_ANALOG: {
            float value = reading.value.value();

            if(expression_evaluator != nullptr)
                value = Evaluate(*expression_evaluator, value);

            reading.value = value;
            break;
        }

        case SensorType::PHYSICAL_INDICATOR:
        case SensorType::CANBUS_INDICATOR: {
            // Indicators are evaluated and stored as 0 or 1.
            bool value = reading.value.value();

            if(expression_evaluator != nullptr)
                value = Evaluate(*expression_evaluator, static_cast<float>(value));

            reading.value = value;
            break;
        }

        case SensorType::VIRTUAL_ANALOG:
        case SensorType::VIRTUAL_INDICATOR:
            if(expression_evaluator == nullptr)
                throw std::invalid_argument("Virtual sensor has no expression");

            reading.value = Evaluate(*expression_evaluator);
            break;

        case SensorType::USER_ANALOG:
        case SensorType::USER_INDICATOR:
            if(expression_evaluator != nullptr)
                reading.value = Evaluate(*expression_evaluator, reading.value);
            break;

        default:
            return;
        }

        reading.status = ReadingStatus::EXPRESSION_EVALUATED;
    } catch (const std::exception& e) {
        reading.status = ReadingStatus::ERROR;
        reading.error_message = e.what();
    }

    sensor_readings_frame_->AddOrUpdateReading(reading);
}

} // namespace eerie_leap::domain::sensor_domain::processors
