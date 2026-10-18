#pragma once

#include <memory>

#include "subsys/adc/i_adc_manager.h"
#include "subsys/gpio/i_gpio.h"
#include "subsys/time/i_time_service.h"

#include "domain/adc_domain/models/adc_configuration.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/services/adc_sample_set.h"
#include "i_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::domain::adc_domain::models::AdcConfiguration;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;
using eerie_leap::domain::sensor_domain::services::AdcSampleSet;
using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::subsys::time::ITimeService;

// What the readers of one rate group are built against: the group's sample set, where analog
// readers reserve their slots, and the generation's ADC configuration (null without one).
struct RateGroupBuildContext {
    AdcSampleSet& adc_samples;
    const AdcConfiguration* adc_configuration;
};

// Creates the polled reader for a sensor's source kind; null when the unit lacks that source.
class SensorReaderFactory {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<IGpio> gpio_;
    std::shared_ptr<IAdcManager> adc_manager_;

    std::unique_ptr<ISensorReader> CreatePhysicalAnalog(const SensorRuntime& runtime, RateGroupBuildContext& context);

public:
    SensorReaderFactory(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<IGpio> gpio,
        // PHYSICAL_ANALOG sensors get no reader without it.
        std::shared_ptr<IAdcManager> adc_manager
    );

    virtual ~SensorReaderFactory() = default;

    // Construction time only. Throws std::invalid_argument when the sensor names an ADC channel
    // the unit or the ADC configuration does not have.
    std::unique_ptr<ISensorReader> Create(const SensorRuntime& runtime, RateGroupBuildContext& context);
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
