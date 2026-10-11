#pragma once

#include <memory>
#include <vector>
#include <functional>
#include <optional>

#include "subsys/device_tree/adc_dt_info.h"

#include "i_adc.h"
#include "i_adc_manager.h"
#include "adc.h"
#include "adc_emulator.h"
#include "adc_simulator.h"
#include "adc_manager.h"

namespace eerie_leap::subsys::adc {

using eerie_leap::subsys::device_tree::AdcDTInfo;

class AdcFactory {
private:
    using DtAdcProvider = std::function<std::optional<std::vector<AdcDTInfo>>&()>;
    DtAdcProvider dt_adc_provider_;

public:
    AdcFactory(DtAdcProvider dt_adc_provider) : dt_adc_provider_(dt_adc_provider) {}

    // One device per device-tree ADC: emulated, then real; a simulator when there is none.
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

        if(adcs.empty())
            adcs.push_back(std::make_shared<AdcSimulator>());

        return std::make_shared<AdcManager>(std::move(adcs));
    }
};

}  // namespace eerie_leap::subsys::adc
