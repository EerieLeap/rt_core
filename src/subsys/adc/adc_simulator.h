#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "i_adc.h"
#include "i_adc_manager.h"
#include "models/adc_configuration.h"

namespace eerie_leap::subsys::adc {

using eerie_leap::subsys::adc::models::AdcConfiguration;
using eerie_leap::subsys::adc::models::AdcChannelConfiguration;

class AdcSimulator : public IAdc {
private:
    uint16_t samples_ = 0;

public:
    AdcSimulator() = default;
    ~AdcSimulator() = default;

    bool Initialize() override;
    void UpdateConfiguration(uint16_t samples) override;
    float ReadChannel(int channel) override;
    int GetChannelCount() override;
};

class AdcSimulatorManager : public IAdcManager {
private:
    std::shared_ptr<IAdc> adc_;
    std::shared_ptr<AdcConfiguration> adc_configuration_;

    bool IsChannelValid(int channel);

public:
    AdcSimulatorManager();

    bool Initialize() override;
    void UpdateConfiguration(std::shared_ptr<AdcConfiguration> adc_configuration) override;
    std::shared_ptr<AdcChannelConfiguration> GetChannelConfiguration(int channel) override;
    std::function<float ()> GetChannelReader(int channel) override;
    int GetAdcCount() override;
    int GetChannelCount() override;
    void UpdateSamplesCount(int samples) override;
    void ResetSamplesCount() override;
};

}  // namespace eerie_leap::subsys::adc
