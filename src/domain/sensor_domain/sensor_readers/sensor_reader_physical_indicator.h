#pragma once

#include <memory>

#include "subsys/gpio/i_gpio.h"
#include "sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::subsys::gpio::IGpio;

class SensorReaderPhysicalIndicator : public SensorReaderBase {
private:
    std::shared_ptr<IGpio> gpio_;

public:
    SensorReaderPhysicalIndicator(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime,
        std::shared_ptr<IGpio> gpio);

    ~SensorReaderPhysicalIndicator() override = default;

    SensorReading Read() override;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
