#if defined(CONFIG_ADC) || defined(CONFIG_ADC_EMUL)

#include <zephyr/logging/log.h>

#include "adc.h"

#include "adc_manager.h"

namespace eerie_leap::subsys::adc {

LOG_MODULE_REGISTER(adc_manager);

AdcManager::AdcManager(std::vector<AdcDTInfo> adc_infos)
    : AdcManager(std::move(adc_infos),
        [](const AdcDTInfo& adc_info) { return std::make_shared<Adc>(adc_info); }) {}

AdcManager::AdcManager(std::vector<AdcDTInfo> adc_infos, const AdcProvider& adc_provider) {
    adc_infos_ = std::move(adc_infos);

    for(const auto& adc_info : adc_infos_)
        adcs_.push_back(adc_provider(adc_info));
}

bool AdcManager::IsChannelValid(int channel) {
    return adc_configuration_ != nullptr
        && channel >= 0
        && channel < adc_configuration_->channel_configurations->size()
        && channel < GetChannelCount();
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

void AdcManager::UpdateConfiguration(std::shared_ptr<AdcConfiguration> adc_configuration) {
    if(adc_configuration == nullptr)
        throw std::invalid_argument("ADC configuration cannot be null.");

    adc_configuration_ = std::move(adc_configuration);

    for(auto& adc : adcs_)
        adc->UpdateConfiguration(adc_configuration_->samples);

    LOG_INF("Adc configuration updated.");
}

std::shared_ptr<AdcChannelConfiguration> AdcManager::GetChannelConfiguration(int channel) {
    if(!IsChannelValid(channel))
        throw std::invalid_argument("ADC channel out of range.");

    return adc_configuration_->channel_configurations->at(channel);
}

std::pair<IAdc*, int> AdcManager::GetAdcForChannel(int channel) {
    if(!IsChannelValid(channel))
        throw std::invalid_argument("ADC channel out of range.");

    int channel_index = channel;
    for(auto& adc : adcs_) {
        if(channel_index < adc->GetChannelCount())
            return std::make_pair(adc.get(), channel_index);
        channel_index -= adc->GetChannelCount();
    }

    throw std::invalid_argument("ADC channel out of range.");
}

std::function<float ()> AdcManager::GetChannelReader(int channel) {
    IAdc* adc;
    int channel_index;
    std::tie(adc, channel_index) = GetAdcForChannel(channel);

    return [adc, channel_index]() { return adc->ReadChannel(channel_index); };
}

int AdcManager::GetAdcCount() {
    return adcs_.size();
}

int AdcManager::GetChannelCount() {
    int count = 0;
    for(auto& adc_info : adc_infos_)
        count += adc_info.channel_configs.size();

    return count;
}

void AdcManager::UpdateSamplesCount(int samples) {
    for(auto& adc : adcs_)
        adc->UpdateConfiguration(samples);
}

void AdcManager::ResetSamplesCount() {
    if(adc_configuration_ == nullptr)
        throw std::runtime_error("ADC config is not set.");

    for(auto& adc : adcs_)
        adc->UpdateConfiguration(adc_configuration_->samples);
}

}  // namespace eerie_leap::subsys::adc

#endif // defined(CONFIG_ADC) || defined(CONFIG_ADC_EMUL)
