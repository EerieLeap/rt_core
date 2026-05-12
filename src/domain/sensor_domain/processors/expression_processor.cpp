#include <memory>
#include <stdexcept>

#include "expression_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using namespace eerie_leap::domain::sensor_domain::models;

ExpressionProcessor::ExpressionProcessor(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame) :
    sensor_readings_frame_(std::move(sensor_readings_frame)) {}

void ExpressionProcessor::ProcessReading(const size_t sensor_id_hash) {
    auto reading_optioanl = sensor_readings_frame_->TryGetReading(sensor_id_hash);
    if(!reading_optioanl)
        return;
    auto reading = std::move(reading_optioanl.value());

    try {
        if(reading.status > ReadingStatus::INTERPOLATED)
            throw std::invalid_argument("Reading is in wrong state");

        if(reading.sensor->configuration.type == SensorType::PHYSICAL_ANALOG) {
            float value = reading.value.value();

            if(reading.sensor->configuration.expression_evaluator != nullptr)
                value = reading.sensor->configuration.expression_evaluator->Evaluate(value);

            reading.value = value;
            reading.status = ReadingStatus::EXPRESSION_EVALUATED;
        } else if(reading.sensor->configuration.type == SensorType::VIRTUAL_ANALOG) {
            reading.value = reading.sensor->configuration.expression_evaluator->Evaluate();
            reading.status = ReadingStatus::EXPRESSION_EVALUATED;
        } else if(reading.sensor->configuration.type == SensorType::PHYSICAL_INDICATOR) {
            bool value = reading.value.value();

            if(reading.sensor->configuration.expression_evaluator != nullptr)
                value = reading.sensor->configuration.expression_evaluator->Evaluate(value);

            reading.value = value;
            reading.status = ReadingStatus::EXPRESSION_EVALUATED;
        } else if(reading.sensor->configuration.type == SensorType::VIRTUAL_INDICATOR) {
            reading.value = reading.sensor->configuration.expression_evaluator->Evaluate();
            reading.status = ReadingStatus::EXPRESSION_EVALUATED;
        } else if(reading.sensor->configuration.type == SensorType::CANBUS_ANALOG) {
            float value = reading.value.value();

            if(reading.sensor->configuration.expression_evaluator != nullptr)
                value = reading.sensor->configuration.expression_evaluator->Evaluate(value);

            reading.value = value;
            reading.status = ReadingStatus::EXPRESSION_EVALUATED;
        } else if(reading.sensor->configuration.type == SensorType::CANBUS_INDICATOR) {
            bool value = reading.value.value();

            if(reading.sensor->configuration.expression_evaluator != nullptr)
                value = reading.sensor->configuration.expression_evaluator->Evaluate(value);

            reading.value = value;
            reading.status = ReadingStatus::EXPRESSION_EVALUATED;
        } else if(reading.sensor->configuration.type == SensorType::USER_ANALOG ||
            reading.sensor->configuration.type == SensorType::USER_INDICATOR) {

            if(reading.sensor->configuration.expression_evaluator != nullptr)
                reading.value = reading.sensor->configuration.expression_evaluator->Evaluate(reading.value);

            reading.status = ReadingStatus::EXPRESSION_EVALUATED;
        } else {
            return;
        }

        sensor_readings_frame_->AddOrUpdateReading(reading);
    } catch (const std::exception& e) {
        reading.status = ReadingStatus::ERROR;
        reading.error_message = e.what();
    }
}

} // namespace eerie_leap::domain::sensor_domain::processors
