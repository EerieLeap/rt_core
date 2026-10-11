#pragma once

#include "i_adc.h"

namespace eerie_leap::subsys::adc {

// Stands in for the hardware on units without an ADC: eight channels of random volts.
class AdcSimulator : public IAdc {
private:
    static constexpr int CHANNEL_COUNT = 8;

    AdcStats stats_ {};

public:
    AdcSimulator() = default;
    ~AdcSimulator() override = default;

    bool Initialize() override;
    void Sample(const AdcSampleRequest& request) noexcept override;
    [[nodiscard]] int GetChannelCount() const override;
    [[nodiscard]] AdcCapabilities GetCapabilities() const override;
    [[nodiscard]] AdcStats GetStats() const override;
};

}  // namespace eerie_leap::subsys::adc
