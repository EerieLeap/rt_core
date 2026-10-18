#include <stdexcept>

#include <zephyr/logging/log.h>

#include "adc_manager.h"

namespace eerie_leap::subsys::adc {

LOG_MODULE_REGISTER(adc_manager);

AdcManager::AdcManager(std::vector<std::shared_ptr<IAdc>> adcs) :
    adcs_(std::move(adcs)) {
    for(const auto& adc : adcs_)
        if(adc == nullptr)
            throw std::invalid_argument("ADC cannot be null.");
}

bool AdcManager::Initialize() {
    for(auto& adc : adcs_) {
        if(!adc->Initialize()) {
            LOG_ERR("ADC initialization failed.");
            return false;
        }
    }

    LOG_INF("ADC Manager initialized. %d channels configured.", GetChannelCount());

    return true;
}

int AdcManager::GetAdcCount() const {
    return static_cast<int>(adcs_.size());
}

int AdcManager::GetChannelCount() const {
    int count = 0;
    for(const auto& adc : adcs_)
        count += adc->GetChannelCount();

    return count;
}

std::span<const std::shared_ptr<IAdc>> AdcManager::GetAdcs() const {
    return adcs_;
}

AdcChannelHandle AdcManager::Resolve(int channel) {
    int channel_index = channel;

    if(channel_index >= 0) {
        for(const auto& adc : adcs_) {
            if(channel_index < adc->GetChannelCount())
                return { .adc = adc.get(), .local_channel = static_cast<uint8_t>(channel_index) };

            channel_index -= adc->GetChannelCount();
        }
    }

    throw std::invalid_argument("ADC channel out of range.");
}

} // namespace eerie_leap::subsys::adc
