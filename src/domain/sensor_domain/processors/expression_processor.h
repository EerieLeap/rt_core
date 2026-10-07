#pragma once

#include <memory>
#include <optional>

#include "subsys/math_parser/expression_evaluator.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/processors/i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::subsys::math_parser::ExpressionEvaluator;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

class ExpressionProcessor : public IReadingProcessor {
private:
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

    bool AreInputsAvailable(const ExpressionEvaluator& expression_evaluator) const;
    std::optional<float> Evaluate(ExpressionEvaluator& expression_evaluator, std::optional<float> x, SensorReading& reading) const;

public:
    explicit ExpressionProcessor(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);

    void Process(const Sensor& sensor, SensorReading& reading) override;
};

} // namespace eerie_leap::domain::sensor_domain::processors
