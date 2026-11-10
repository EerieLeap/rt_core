#pragma once

#include <cstdint>
#include <vector>

#include <zephyr/spinlock.h>

#include "adc_random_signal.h"
#include "i_adc.h"

namespace eerie_leap::subsys::adc {

struct AdcRandomizerConfig {
    int channel_count = 4;
    float volts_max = 3.3F;
    uint32_t move_min_ms = 200;
    uint32_t move_max_ms = 4000;
    uint32_t hold_max_ms = 2000;
    uint32_t seed = 0; // 0: from the entropy source, so each boot differs.
};

// A bench and development ADC: every channel is a random but plausible voltage that wanders
// between targets (AdcRandomSignal), so a sensor connected to it moves like a real one. Take the
// place of the hardware or follow it, see AdcFactory.
class AdcRandomizer : public IAdc {
private:
    mutable k_spinlock lock_{};
    std::vector<AdcRandomSignal> signals_;
    AdcStats stats_{};

public:
    explicit AdcRandomizer(const AdcRandomizerConfig& config);
    ~AdcRandomizer() override = default;

    bool Initialize() override;
    void Sample(const AdcSampleRequest& request) noexcept override;
    [[nodiscard]] int GetChannelCount() const override;
    [[nodiscard]] AdcCapabilities GetCapabilities() const override;
    [[nodiscard]] AdcStats GetStats() const override;
};

} // namespace eerie_leap::subsys::adc
