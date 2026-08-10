#pragma once

#include <cstdint>

namespace eerie_leap::subsys::adc {

class IAdc {
public:
    virtual ~IAdc() = default;

    virtual bool Initialize() = 0;
    virtual void UpdateConfiguration(uint16_t samples) = 0;
    virtual float ReadChannel(int channel) = 0;
    virtual int GetChannelCount() = 0;
};

}  // namespace eerie_leap::subsys::adc
