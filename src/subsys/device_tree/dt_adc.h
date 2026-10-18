#pragma once

#include <optional>
#include <vector>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "subsys/device_tree/adc_dt_info.h"

#define ADC_NODE(idx) DT_ALIAS(adc##idx)
#define ADC_CHANNEL_VREF(node_id) DT_PROP_OR(node_id, zephyr_vref_mv, 0)
#define ADC_CHANNEL_RESOLUTION(node_id) DT_PROP_OR(node_id, zephyr_resolution, 0)
#define ADC_CHANNEL_OVERSAMPLING(node_id) DT_PROP_OR(node_id, zephyr_oversampling, 0)

namespace eerie_leap::subsys::device_tree {

class DtAdc {
private:
    static std::optional<std::vector<AdcDTInfo>> adc_infos_;

    DtAdc() = default;

public:
    static void Initialize();

    static std::optional<std::vector<AdcDTInfo>>& Get() {
        return adc_infos_;
    }
};

} // namespace eerie_leap::subsys::device_tree
