#include <stdexcept>

#include "domain/sensor_domain/models/reading_error.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "sensor_reader_physical_analog.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::domain::sensor_domain::models;

// Everything Read() dereferences is checked here, so the tick path has nothing left to throw.
SensorReaderPhysicalAnalog::SensorReaderPhysicalAnalog(
    std::shared_ptr<ITimeService> time_service,
    const SensorRuntime& runtime,
    const AdcSample* sample,
    std::shared_ptr<const AdcCalibrator> calibrator
) :
    SensorReaderBase(std::move(time_service), runtime),
    sample_(sample),
    calibrator_(std::move(calibrator)) {

    if(GetSensor().configuration.type != SensorType::PHYSICAL_ANALOG)
        throw std::runtime_error("Unsupported sensor type");

    if(sample_ == nullptr || calibrator_ == nullptr)
        throw std::invalid_argument("Analog reader needs an ADC sample slot and a calibrator");

    if(runtime_->voltage_interpolator == nullptr)
        throw std::invalid_argument("Sensor has no interpolator");
}

SensorReading SensorReaderPhysicalAnalog::Read(const TickContext& tick) {
    SensorReading reading = CreateReading(tick);

    if(sample_->error != AdcError::NONE) {
        reading.SetError(ReadingError::READER_FAILED);
        return reading;
    }

    reading.adc_voltage = sample_->volts;

    const float voltage_calibrated = calibrator_->InterpolateToCalibratedRange(sample_->volts);
    reading.calibrated_voltage = voltage_calibrated;

    reading.value = runtime_->voltage_interpolator->Interpolate(voltage_calibrated, true);
    reading.raw_value = reading.value;

    reading.status = ReadingStatus::INTERPOLATED;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
