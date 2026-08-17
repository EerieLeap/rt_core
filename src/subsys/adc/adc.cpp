#ifdef CONFIG_ADC

#include <stdexcept>
#include <numeric>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/kernel.h>

#include "adc.h"

namespace eerie_leap::subsys::adc {

LOG_MODULE_REGISTER(adc_logger);

bool Adc::Initialize() {
    LOG_INF("Adc initialization started.");

	if(!device_is_ready(adc_device_)) {
		LOG_ERR("ADC device not ready");
		return false;
	}

    if(channel_configs_.size() == 0) {
        LOG_ERR("No ADC channels configured in device tree");
        return false;
    }

	for(size_t i = 0U; i < channel_configs_.size(); i++) {
		int err = adc_channel_setup(adc_device_, &channel_configs_[i]);
		if(err < 0) {
			LOG_ERR("Could not setup channel #%d (%d)\n", i, err);
			return false;
		}

        if(channel_configs_[i].reference == ADC_REF_INTERNAL)
		    references_mv_[i] = adc_ref_internal(adc_device_);

        if(references_mv_[i] == 0) {
            LOG_ERR("ADC reference for channel %d is not available", i);
            return false;
        }

        available_channels_.insert(channel_configs_[i].channel_id);
	}

    LOG_DBG("ADC initialized with %d channels", channel_configs_.size());

    return true;
}

void Adc::UpdateConfiguration(uint16_t samples) {
    samples_ = samples;

	// NOTE: ESP32-S3's ADC doens't support sampling,
	// I'm not sure how commonly it is supported by other ADCs
	// thus currently sampling is done manually

	sequence_options_ = {
		.interval_us = 0,
		.extra_samplings = 0
	};

	sequences_.clear();
	for(size_t i = 0U; i < channel_configs_.size(); i++) {
		sequences_.push_back({
			.options     = &sequence_options_,
			.buffer      = &samples_buffer_,
			.buffer_size = sizeof(samples_buffer_),
			.resolution  = resolutions_[i],
		});
	}
}

float Adc::ReadChannel(int channel) {
    if(samples_ == 0)
        throw std::runtime_error("ADC config is not set.");

    if(channel < 0 || channel >= GetChannelCount())
		throw std::invalid_argument("ADC channel out of range.");

	sequences_[channel].channels = BIT(channel_configs_[channel].channel_id);

	uint64_t accumulator = 0;
	int err = 0;

    for(int i = 0; i < samples_; i++) {
		err += adc_read(adc_device_, &sequences_[channel]);
		accumulator += samples_buffer_;
	}

    if(err < 0) {
        LOG_ERR("Can not read channel: %d, error: (%d)\n", channel, err);
		return 0;
    }

	auto val_mv = static_cast<int32_t>(accumulator / samples_);
	err = adc_raw_to_millivolts(references_mv_[channel], channel_configs_[channel].gain, sequences_[channel].resolution, &val_mv);

	if(err < 0) {
		LOG_ERR("ADC conversion for channel %d to mV failed (%d)\n", channel, err);
		return 0;
	}

    return (float)val_mv / 1000.0f;
}

int Adc::GetChannelCount() {
    return channel_configs_.size();
}

}  // namespace eerie_leap::subsys::adc

#endif // CONFIG_ADC
