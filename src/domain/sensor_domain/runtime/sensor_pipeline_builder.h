#pragma once

#include <memory>
#include <vector>

#include "subsys/fs/services/i_fs_service.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

#include "sensor_runtime.h"

namespace eerie_leap::domain::sensor_domain::runtime {

using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

// Turns a validated sensors configuration into a SensorGeneration: configures the frame's slots,
// builds the interpolators, expressions and scripts, and binds expression variables to the
// frame's value slots. Everything a sample needs is allocated here, once.
class SensorPipelineBuilder {
private:
    std::shared_ptr<IFsService> sd_fs_service_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

    void BuildInterpolator(SensorRuntime& runtime) const;
    void BuildExpression(SensorRuntime& runtime) const;
    void BuildScript(SensorRuntime& runtime) const;

public:
    SensorPipelineBuilder(
        std::shared_ptr<IFsService> sd_fs_service,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);

    /**
     * @brief Builds the generation for @p sensors, which must be validated and in processing order.
     * @throws std::invalid_argument when an expression references a sensor that is not configured.
     */
    std::shared_ptr<SensorGeneration> Build(std::shared_ptr<const std::vector<std::shared_ptr<Sensor>>> sensors) const;
};

} // namespace eerie_leap::domain::sensor_domain::runtime
