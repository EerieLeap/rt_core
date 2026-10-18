#pragma once

#include <memory>
#include <span>
#include <vector>

#include "i_adc.h"
#include "i_adc_manager.h"

namespace eerie_leap::subsys::adc {

// The unit's ADCs in alias order; global channel indices run over them in that order.
class AdcManager : public IAdcManager {
private:
    std::vector<std::shared_ptr<IAdc>> adcs_;

public:
    explicit AdcManager(std::vector<std::shared_ptr<IAdc>> adcs);

    bool Initialize() override;

    [[nodiscard]] int GetAdcCount() const override;
    [[nodiscard]] int GetChannelCount() const override;
    [[nodiscard]] std::span<const std::shared_ptr<IAdc>> GetAdcs() const override;
    AdcChannelHandle Resolve(int channel) override;
};

} // namespace eerie_leap::subsys::adc
