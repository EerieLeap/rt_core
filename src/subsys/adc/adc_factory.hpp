#pragma once

#include <memory>
#include <vector>
#include <functional>
#include <optional>

#include "subsys/device_tree/adc_dt_info.h"

#include "i_adc_manager.h"
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

    std::shared_ptr<IAdcManager> Create() {
#ifdef CONFIG_ADC_EMUL
        if(dt_adc_provider_ != nullptr && dt_adc_provider_().has_value())
            return std::make_shared<AdcEmulatorManager>(dt_adc_provider_().value());
#endif

#ifdef CONFIG_ADC
        if(dt_adc_provider_ != nullptr && dt_adc_provider_().has_value())
            return std::make_shared<AdcManager>(dt_adc_provider_().value());
#endif

        return std::make_shared<AdcSimulatorManager>();
    }
};

}  // namespace eerie_leap::subsys::adc
