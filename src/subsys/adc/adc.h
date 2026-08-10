#pragma once

#ifdef CONFIG_ADC

#include <memory>
#include <cstdint>
#include <vector>
#include <optional>
#include <unordered_set>
#include <zephyr/drivers/adc.h>

#include "subsys/device_tree/adc_dt_info.h"

#include "i_adc.h"

namespace eerie_leap::subsys::adc {

using eerie_leap::subsys::device_tree::AdcDTInfo;

class Adc : public IAdc {
private:
    std::vector<uint32_t> references_mv_;
    std::vector<uint8_t> resolutions_;

protected:
    std::vector<adc_channel_cfg> channel_configs_;
    const device* adc_device_;
    std::vector<adc_sequence> sequences_;
    adc_sequence_options sequence_options_;
    uint32_t samples_buffer_ = 0;
    std::unordered_set<uint8_t> available_channels_;
    uint16_t samples_ = 0;

public:
    explicit Adc(const AdcDTInfo& adc_dt_info) :
        references_mv_(adc_dt_info.references_mv),
        resolutions_(adc_dt_info.resolutions),
        channel_configs_(adc_dt_info.channel_configs),
        adc_device_(adc_dt_info.adc_device) {}

    bool Initialize() override;
    void UpdateConfiguration(uint16_t samples) override;
    float ReadChannel(int channel) override;
    int GetChannelCount() override;
};

}  // namespace eerie_leap::subsys::adc

#endif // CONFIG_ADC
