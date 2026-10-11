#include <zephyr/logging/log.h>

#include "dt_logger.h"

#include "dt_adc.h"

// One AdcDTInfo per adc<idx> alias, channels in device-tree child order.
#define ADC_DT_INFO_ADD(idx) \
    IF_ENABLED(DT_HAS_ALIAS(adc ## idx), ( \
        adc_infos_.value().push_back(AdcDTInfo { \
            .adc_device = DEVICE_DT_GET(ADC_NODE(idx)), \
            .channel_configs = {DT_FOREACH_CHILD_SEP(ADC_NODE(idx), ADC_CHANNEL_CFG_DT, (,))}, \
            .references_mv = {DT_FOREACH_CHILD_SEP(ADC_NODE(idx), ADC_CHANNEL_VREF, (,))}, \
            .resolutions = {DT_FOREACH_CHILD_SEP(ADC_NODE(idx), ADC_CHANNEL_RESOLUTION, (,))}, \
            .oversampling = {DT_FOREACH_CHILD_SEP(ADC_NODE(idx), ADC_CHANNEL_OVERSAMPLING, (,))} \
        }); \
        LOG_INF("ADC" #idx " initialized."); \
    ))

namespace eerie_leap::subsys::device_tree {

LOG_MODULE_DECLARE(dt_logger);

std::optional<std::vector<AdcDTInfo>> DtAdc::adc_infos_ = std::nullopt;

void DtAdc::Initialize() {
#if DT_HAS_ALIAS(adc0) || DT_HAS_ALIAS(adc1) || DT_HAS_ALIAS(adc2) || DT_HAS_ALIAS(adc3)
    adc_infos_ = std::make_optional<std::vector<AdcDTInfo>>();
#endif

    ADC_DT_INFO_ADD(0)
    ADC_DT_INFO_ADD(1)
    ADC_DT_INFO_ADD(2)
    ADC_DT_INFO_ADD(3)
}

} // namespace eerie_leap::subsys::device_tree
