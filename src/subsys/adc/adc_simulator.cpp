#include <stdexcept>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "adc_simulator.h"

namespace eerie_leap::subsys::adc {

LOG_MODULE_REGISTER(adc_simulator_logger);

bool AdcSimulator::Initialize() {
    LOG_INF("Adc Simulator initialization started.");

    LOG_INF("Adc Simulator initialized successfully.");

    return true;
}

void AdcSimulator::UpdateConfiguration(uint16_t samples) {
    samples_ = samples;

    LOG_INF("Adc configuration updated.");
}

float AdcSimulator::ReadChannel(int channel) {
    if(samples_ == 0)
        throw std::runtime_error("ADC config is not set.");

    if(channel < 0 || channel >= GetChannelCount())
        throw std::invalid_argument("ADC channel out of range.");

    uint32_t raw = sys_rand32_get();
    float random_value = (raw / static_cast<float>(UINT32_MAX)) * 3.3F;

    return random_value;
}

int AdcSimulator::GetChannelCount() {
    return 8;
}

AdcSimulatorManager::AdcSimulatorManager()
    : adc_(std::make_shared<AdcSimulator>()), adc_configuration_(nullptr) {}

bool AdcSimulatorManager::IsChannelValid(int channel) {
    return adc_configuration_ != nullptr
        && channel >= 0
        && channel < adc_configuration_->channel_configurations->size()
        && channel < GetChannelCount();
}

bool AdcSimulatorManager::Initialize() {
    return adc_->Initialize();
}

void AdcSimulatorManager::UpdateConfiguration(std::shared_ptr<AdcConfiguration> adc_configuration) {
    if(adc_configuration == nullptr)
        throw std::invalid_argument("ADC configuration cannot be null.");

    adc_configuration_ = std::move(adc_configuration);

    adc_->UpdateConfiguration(adc_configuration_->samples);
}

std::shared_ptr<AdcChannelConfiguration> AdcSimulatorManager::GetChannelConfiguration(int channel) {
    if(!IsChannelValid(channel))
        throw std::invalid_argument("ADC channel out of range.");

    return adc_configuration_->channel_configurations->at(channel);
}

std::function<float ()> AdcSimulatorManager::GetChannelReader(int channel) {
    if(!IsChannelValid(channel))
        throw std::invalid_argument("ADC channel out of range.");

    return [this, channel]() { return adc_->ReadChannel(channel); };
}

int AdcSimulatorManager::GetAdcCount() {
    return 1;
}

int AdcSimulatorManager::GetChannelCount() {
    return adc_->GetChannelCount();
}

void AdcSimulatorManager::UpdateSamplesCount(int samples) {
    adc_->UpdateConfiguration(samples);
}

void AdcSimulatorManager::ResetSamplesCount() {
    if(adc_configuration_ == nullptr)
        throw std::runtime_error("ADC config is not set.");

    adc_->UpdateConfiguration(adc_configuration_->samples);
}

}  // namespace eerie_leap::subsys::adc
