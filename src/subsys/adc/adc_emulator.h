#pragma once

#ifdef CONFIG_ADC_EMUL

    #include <vector>

    #include "adc.h"

namespace eerie_leap::subsys::adc {

// Drives the emulated ADC with a random value before each read, so the stimulus always lands on
// the device and channel that Adc::Sample() converts. The channel hooks exist for tests only.
class AdcEmulator : public Adc {
private:
    std::vector<bool> externally_driven_;

public:
    explicit AdcEmulator(const AdcDTInfo& adc_dt_info) :
        Adc(adc_dt_info),
        externally_driven_(adc_dt_info.channel_configs.size(), false) {}

    void Sample(const AdcSampleRequest& request) noexcept override;

    // An externally driven channel keeps whatever a test installed through the adc_emul API
    // (a value function, a constant) instead of the random stimulus.
    void SetExternallyDriven(int channel, bool driven = true);

    // Replaces the probed capabilities, e.g. to force the per-sample loop path. Call after Initialize().
    void OverrideCapabilities(const AdcCapabilities& capabilities);
};

} // namespace eerie_leap::subsys::adc

#endif // CONFIG_ADC_EMUL
