#pragma once

#include <memory>

#include "domain/adc_domain/utilities/adc_calibrator.h"
#include "sensor_reader_base.h"
#include "subsys/adc/adc_types.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::domain::adc_domain::utilities::AdcCalibrator;
using eerie_leap::subsys::adc::AdcSample;

// Reads the slot its rate group acquired for this tick; never touches the ADC itself.
class SensorReaderPhysicalAnalog : public SensorReaderBase {
private:
    const AdcSample* sample_; // Owned by the rate group's AdcSampleSet, which outlives the reader.
    std::shared_ptr<const AdcCalibrator> calibrator_;

public:
    SensorReaderPhysicalAnalog(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime,
        const AdcSample* sample,
        std::shared_ptr<const AdcCalibrator> calibrator
    );

    ~SensorReaderPhysicalAnalog() override = default;

    // Never throws: a failed conversion gives a READER_FAILED reading.
    SensorReading Read(const TickContext& tick) override;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
