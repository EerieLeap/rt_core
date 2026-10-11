#pragma once

#include "adc_types.h"

namespace eerie_leap::subsys::adc {

class IAdc {
public:
    virtual ~IAdc() = default;

    virtual bool Initialize() = 0;

    // Never throws and never allocates; every result carries its own status.
    virtual void Sample(const AdcSampleRequest& request) noexcept = 0;

    [[nodiscard]] virtual int GetChannelCount() const = 0;
    [[nodiscard]] virtual AdcCapabilities GetCapabilities() const = 0;
    [[nodiscard]] virtual AdcStats GetStats() const = 0;
};

}  // namespace eerie_leap::subsys::adc
