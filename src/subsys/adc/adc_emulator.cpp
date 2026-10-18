#ifdef CONFIG_ADC_EMUL

    #include <zephyr/drivers/adc/adc_emul.h>
    #include <zephyr/logging/log.h>
    #include <zephyr/random/random.h>

    #include "adc_emulator.h"

namespace eerie_leap::subsys::adc {

LOG_MODULE_REGISTER(adc_emulator_logger);

void AdcEmulator::Sample(const AdcSampleRequest& request) noexcept {
    const size_t count = std::min(request.channels.size(), request.results.size());

    for(size_t i = 0; i < count; i++) {
        const int channel = request.channels[i];
        if(channel >= GetChannelCount() || externally_driven_[channel])
            continue;

        uint32_t raw = sys_rand32_get();
        auto input_mv = static_cast<uint16_t>((static_cast<uint64_t>(raw) * 3301) >> 32);

        int err = adc_emul_const_value_set(adc_device_, channel_configs_[channel].channel_id, input_mv);
        if(err < 0)
            LOG_ERR("Could not set constant value for channel %d (%d)", channel, err);
    }

    Adc::Sample(request);
}

void AdcEmulator::SetExternallyDriven(int channel, bool driven) {
    if(channel < 0 || channel >= GetChannelCount())
        return;

    externally_driven_[channel] = driven;
}

void AdcEmulator::OverrideCapabilities(const AdcCapabilities& capabilities) {
    capabilities_ = capabilities;
}

} // namespace eerie_leap::subsys::adc

#endif // CONFIG_ADC_EMUL
