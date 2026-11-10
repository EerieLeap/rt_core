#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "subsys/device_tree/adc_dt_info.h"

#include "adc.h"
#include "adc_emulator.h"
#include "adc_manager.h"
#include "adc_randomizer.h"
#include "adc_simulator.h"
#include "i_adc.h"
#include "i_adc_manager.h"

namespace eerie_leap::subsys::adc {

using eerie_leap::subsys::device_tree::AdcDTInfo;

class AdcFactory {
private:
    using DtAdcProvider = std::function<std::optional<std::vector<AdcDTInfo>>&()>;
    DtAdcProvider dt_adc_provider_;

public:
    explicit AdcFactory(DtAdcProvider dt_adc_provider) :
        dt_adc_provider_(std::move(dt_adc_provider)) {}

    // One device per device-tree ADC: emulated, then real. With CONFIG_EERIE_LEAP_ADC_RANDOMIZER a
    // randomizer follows them (its channels are numbered after the hardware); an AdcSimulator stands
    // in only when there is nothing else.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads dt_adc_provider_ when CONFIG_ADC or CONFIG_ADC_EMUL is set, which this build is not
    std::shared_ptr<IAdcManager> Create() {
        std::vector<std::shared_ptr<IAdc>> adcs;

#if defined(CONFIG_ADC_EMUL) || defined(CONFIG_ADC)
        if(dt_adc_provider_ != nullptr && dt_adc_provider_().has_value()) {
            for(const auto& adc_info : dt_adc_provider_().value()) {
    #ifdef CONFIG_ADC_EMUL
                adcs.push_back(std::make_shared<AdcEmulator>(adc_info));
    #else
                adcs.push_back(std::make_shared<Adc>(adc_info));
    #endif
            }
        }
#endif

#ifdef CONFIG_EERIE_LEAP_ADC_RANDOMIZER
        adcs.push_back(
            std::make_shared<AdcRandomizer>(AdcRandomizerConfig{
                .channel_count = CONFIG_EERIE_LEAP_ADC_RANDOMIZER_CHANNELS,
    #ifdef CONFIG_EERIE_LEAP_ADC_VOLTAGE_MAX_MV
                // The pin voltage that maps to the full sensor input.
                .volts_max = CONFIG_EERIE_LEAP_ADC_VOLTAGE_MAX_MV / 1000.0F,
    #else
                .volts_max = 3.3F,
    #endif
                .move_min_ms = CONFIG_EERIE_LEAP_ADC_RANDOMIZER_MOVE_MIN_MS,
                .move_max_ms = CONFIG_EERIE_LEAP_ADC_RANDOMIZER_MOVE_MAX_MS,
                .hold_max_ms = CONFIG_EERIE_LEAP_ADC_RANDOMIZER_HOLD_MAX_MS,
            })
        );
#endif

        if(adcs.empty())
            adcs.push_back(std::make_shared<AdcSimulator>());

        return std::make_shared<AdcManager>(std::move(adcs));
    }
};

} // namespace eerie_leap::subsys::adc
