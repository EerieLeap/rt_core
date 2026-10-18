#pragma once

#include <memory>
#include <span>

#include "adc_types.h"
#include "i_adc.h"

namespace eerie_leap::subsys::adc {

class IAdcManager {
public:
    virtual ~IAdcManager() = default;

    virtual bool Initialize() = 0;

    [[nodiscard]] virtual int GetAdcCount() const = 0;
    // Device-tree total over every ADC.
    [[nodiscard]] virtual int GetChannelCount() const = 0;
    [[nodiscard]] virtual std::span<const std::shared_ptr<IAdc>> GetAdcs() const = 0;

    // Maps a global channel index (ADCs in alias order, channels in device-tree order) to its ADC.
    // Throws std::invalid_argument when out of range; construction time only.
    virtual AdcChannelHandle Resolve(int channel) = 0;
};

} // namespace eerie_leap::subsys::adc
