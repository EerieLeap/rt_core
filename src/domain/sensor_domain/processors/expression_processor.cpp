#include <cmath>
#include <memory>

#include "domain/sensor_domain/models/sensor_type_traits.h"

#include "expression_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using namespace eerie_leap::domain::sensor_domain::models;

ExpressionProcessor::ExpressionProcessor(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame) :
    sensor_readings_frame_(std::move(sensor_readings_frame)) {}

// Returns the result, or nothing after recording the failure on the reading.
std::optional<float> ExpressionProcessor::Evaluate(
    const SensorRuntime& runtime, std::optional<float> x, SensorReading& reading) const {

    // Evaluating against a sensor that has no value yet would read NaN, or 0 through a comparison.
    if(!sensor_readings_frame_->AreValuesAvailable(runtime.input_slots)) {
        reading.SetError(ReadingError::INPUT_UNAVAILABLE);
        return std::nullopt;
    }

    // Nothing means an unbound variable or a missing x; both are build-time defects of this
    // generation rather than a property of the sample.
    const auto value = runtime.expression_evaluator->Evaluate(x);
    if(!value.has_value()) {
        reading.SetError(ReadingError::EXPRESSION_FAILED);
        return std::nullopt;
    }

    if(std::isnan(*value)) {
        reading.SetError(ReadingError::EXPRESSION_NOT_A_NUMBER);
        return std::nullopt;
    }

    return value;
}

void ExpressionProcessor::Process(const SensorRuntime& runtime, SensorReading& reading) {
    const auto traits = runtime.GetSensor().configuration.GetTraits();
    if(!traits.allows_expression)
        return;

    if(reading.status > ReadingStatus::INTERPOLATED) {
        reading.SetError(ReadingError::WRONG_STATE);
        return;
    }

    const bool has_evaluator = runtime.expression_evaluator != nullptr;

    if(traits.source == SensorSourceKind::EXPRESSION) {
        if(!has_evaluator) {
            reading.SetError(ReadingError::EXPRESSION_FAILED);
            return;
        }

        const auto value = Evaluate(runtime, std::nullopt, reading);
        if(!value.has_value())
            return;

        reading.value = value;
    } else if(has_evaluator) {
        // Sources other than a script always have a value to feed in; a script may not.
        if(!reading.value.has_value() && traits.source != SensorSourceKind::SCRIPT) {
            reading.SetError(ReadingError::NO_VALUE);
            return;
        }

        std::optional<float> x = reading.value;
        if(x.has_value() && traits.value_kind == SensorValueKind::INDICATOR)
            x = x.value() != 0.0F ? 1.0F : 0.0F;

        const auto value = Evaluate(runtime, x, reading);
        if(!value.has_value())
            return;

        reading.value = value;
    }

    // Indicators are stored as 0 or 1.
    if(traits.value_kind == SensorValueKind::INDICATOR && reading.value.has_value())
        reading.value = reading.value.value() != 0.0F ? 1.0F : 0.0F;

    reading.status = ReadingStatus::EXPRESSION_EVALUATED;
}

} // namespace eerie_leap::domain::sensor_domain::processors
