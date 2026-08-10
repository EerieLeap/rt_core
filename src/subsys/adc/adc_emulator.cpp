#ifdef CONFIG_ADC_EMUL

#include <stdexcept>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/random/random.h>

#include "adc_emulator.h"

namespace eerie_leap::subsys::adc {

LOG_MODULE_REGISTER(adc_emulator_logger);

float AdcEmulator::ReadChannel(int channel) {
    if(channel < 0 || channel >= GetChannelCount())
        throw std::invalid_argument("ADC channel out of range.");

    uint32_t raw = sys_rand32_get();
    auto input_mv = static_cast<uint16_t>((static_cast<uint64_t>(raw) * 3301) >> 32);

    int err = adc_emul_const_value_set(adc_device_, channel_configs_[channel].channel_id, input_mv);
    if(err < 0) {
        LOG_ERR("Could not set constant value (%d).", err);
        return 0;
    }

    return Adc::ReadChannel(channel);
}

AdcEmulatorManager::AdcEmulatorManager(std::vector<AdcDTInfo> adc_infos)
    : AdcManager(std::move(adc_infos),
        [](const AdcDTInfo& adc_info) { return std::make_shared<AdcEmulator>(adc_info); }) {}

}  // namespace eerie_leap::subsys::adc

#endif // CONFIG_ADC_EMUL
