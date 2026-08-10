#pragma once

#ifdef CONFIG_ADC_EMUL

#include <vector>

#include "adc.h"
#include "adc_manager.h"

namespace eerie_leap::subsys::adc {

// Drives the emulated ADC with a random value before each read, so the stimulus
// always lands on the same device and channel that Adc::ReadChannel samples.
class AdcEmulator : public Adc {
public:
    explicit AdcEmulator(const AdcDTInfo& adc_dt_info) : Adc(adc_dt_info) {}

    float ReadChannel(int channel) override;
};

class AdcEmulatorManager : public AdcManager {
public:
    explicit AdcEmulatorManager(std::vector<AdcDTInfo> adc_infos);
};

}  // namespace eerie_leap::subsys::adc

#endif // CONFIG_ADC_EMUL
