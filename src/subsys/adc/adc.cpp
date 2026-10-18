#ifdef CONFIG_ADC

    #include <algorithm>
    #include <bit>
    #include <cerrno>

    #include <zephyr/kernel.h>
    #include <zephyr/logging/log.h>
    #include <zephyr/sys/util.h>

    #include "subsys/threading/scoped_mutex.h"

    #include "adc.h"

namespace eerie_leap::subsys::adc {

using eerie_leap::subsys::threading::ScopedMutex;

LOG_MODULE_REGISTER(adc_logger);

Adc::Adc(const AdcDTInfo& adc_dt_info) :
    references_mv_(adc_dt_info.references_mv),
    resolutions_(adc_dt_info.resolutions),
    oversampling_(adc_dt_info.oversampling),
    channel_configs_(adc_dt_info.channel_configs),
    adc_device_(adc_dt_info.adc_device) {

    k_mutex_init(&mutex_);
}

bool Adc::Initialize() {
    LOG_INF("Adc initialization started.");

    initialized_ = false;

    if(!device_is_ready(adc_device_)) {
        LOG_ERR("ADC device not ready");
        return false;
    }

    const size_t channel_count = channel_configs_.size();
    if(channel_count == 0) {
        LOG_ERR("No ADC channels configured in device tree");
        return false;
    }

    if(references_mv_.size() != channel_count
        || resolutions_.size() != channel_count
        || oversampling_.size() != channel_count) {
        LOG_ERR("ADC device tree info is inconsistent");
        return false;
    }

    scale_.assign(channel_count, 0.0F);
    local_by_id_.fill(0);
    uniform_sequence_ = true;
    uint32_t used_ids = 0;

    for(size_t i = 0U; i < channel_count; i++) {
        const uint8_t channel_id = channel_configs_[i].channel_id;
        if(channel_id >= MAX_CHANNEL_ID) {
            LOG_ERR("ADC channel #%zu id %u cannot be addressed in a sequence", i, channel_id);
            return false;
        }

        // Two channels on one id would share a mask bit, and one of them would never be converted.
        if((used_ids & BIT(channel_id)) != 0) {
            LOG_ERR("ADC channel #%zu repeats id %u", i, channel_id);
            return false;
        }
        used_ids |= BIT(channel_id);

        // A 0 resolution would make every conversion fail; more than 16 bits would not fit the buffer.
        if(resolutions_[i] == 0 || resolutions_[i] > 16) {
            LOG_ERR("ADC channel #%zu has an unsupported resolution %u", i, resolutions_[i]);
            return false;
        }

        // Samples are summed and scaled as unsigned codes; a differential channel produces signed ones.
        if(channel_configs_[i].differential) {
            LOG_ERR("ADC channel #%zu is differential, which is not supported", i);
            return false;
        }

        int err = adc_channel_setup(adc_device_, &channel_configs_[i]);
        if(err < 0) {
            LOG_ERR("Could not setup channel #%zu (%d)", i, err);
            return false;
        }

        // The driver's value wins over zephyr,vref-mv, as in adc_raw_to_x_dt_chan().
        uint16_t driver_mv = 0;
        if(adc_ref_get(adc_device_, channel_configs_[i].reference, &driver_mv) == 0)
            references_mv_[i] = driver_mv;

        if(references_mv_[i] == 0) {
            LOG_ERR("ADC reference for channel #%zu is not available", i);
            return false;
        }

        // adc_gain_invert() works on integers, so the gain is inverted on a 16.16 fixed-point one.
        int32_t gain_inverse = 1 << 16;
        err = adc_gain_invert(channel_configs_[i].gain, &gain_inverse);
        if(err < 0) {
            LOG_ERR("ADC gain for channel #%zu cannot be inverted (%d)", i, err);
            return false;
        }

        scale_[i] = (static_cast<float>(references_mv_[i]) / 1000.0F)
            * (static_cast<float>(gain_inverse) / static_cast<float>(1 << 16))
            / static_cast<float>(1U << resolutions_[i]);

        local_by_id_[channel_id] = static_cast<uint8_t>(i);
        uniform_sequence_ = uniform_sequence_
            && resolutions_[i] == resolutions_[0]
            && oversampling_[i] == oversampling_[0];
    }

    burst_buffer_.assign(channel_count * CONFIG_EERIE_LEAP_ADC_BURST_CHUNK, 0);
    accumulators_.assign(channel_count, 0);
    sequence_options_ = { .interval_us = 0, .callback = nullptr, .user_data = nullptr, .extra_samplings = 0 };
    capabilities_ = {};

    if(!uniform_sequence_)
        LOG_WRN("%s: channels differ in resolution or oversampling, multi-channel scans disabled", adc_device_->name);

    if(!Probe())
        return false;

    initialized_ = true;

    LOG_INF(
        "%s: %zu channels, burst=%s multi=%s oversampling=%u, ~%u us/conversion", adc_device_->name, channel_count,
        capabilities_.burst ? "yes" : "no", capabilities_.multi_channel ? "yes" : "no", oversampling_[0],
        capabilities_.us_per_conversion
    );

    return true;
}

// One single-channel conversion must succeed; it is timed. extra_samplings and a two-channel mask
// are optional driver features and only set the capability flags.
bool Adc::Probe() {
    const uint32_t first_mask = BIT(channel_configs_[0].channel_id);

    adc_sequence sequence = MakeSequence(0, first_mask, 1);
    uint32_t cycles = 0;
    int err = Read(sequence, &cycles);

    if(err < 0) {
        if(err == -ENOTSUP && oversampling_[0] != 0)
            LOG_ERR(
                "%s: the driver rejects oversampling %u from the device tree (%d)", adc_device_->name, oversampling_[0],
                err
            );
        else
            LOG_ERR("%s: probe conversion failed (%d)", adc_device_->name, err);

        return false;
    }

    capabilities_.us_per_conversion = k_cyc_to_us_near32(cycles);

    #if defined(CONFIG_ADC_ESP32) && !defined(CONFIG_ADC_ESP32_DMA)
    // The ESP32 oneshot driver rejects extra samplings and multi-channel sequences, logging an error
    // for each attempt, so the trials would only put errors into a clean boot log.
    capabilities_.burst = false;
    capabilities_.multi_channel = false;
    #else
    if(CONFIG_EERIE_LEAP_ADC_BURST_CHUNK >= 2) {
        sequence = MakeSequence(0, first_mask, 2);
        capabilities_.burst = Read(sequence) == 0;
    }

    if(channel_configs_.size() >= 2 && uniform_sequence_) {
        sequence = MakeSequence(0, first_mask | BIT(channel_configs_[1].channel_id), 1);
        capabilities_.multi_channel = Read(sequence) == 0;
    }
    #endif

    return true;
}

adc_sequence Adc::MakeSequence(int channel, uint32_t mask, uint16_t samplings) {
    sequence_options_.extra_samplings = samplings - 1;

    return adc_sequence{ .options = samplings > 1 ? &sequence_options_ : nullptr,
        .channels = mask,
        .buffer = burst_buffer_.data(),
        .buffer_size = burst_buffer_.size() * sizeof(uint16_t),
        .resolution = resolutions_[channel],
        .oversampling = oversampling_[channel],
        .calibrate = false };
}

// The ESP32 driver documents that a conversion may time out while the analog block is shared
// with the radio and expects the caller to retry; -EBUSY is treated the same way.
// cycles, when given, receives the duration of the last attempt only.
int Adc::Read(const adc_sequence& sequence, uint32_t* cycles) {
    int err = 0;

    for(int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        stats_.driver_calls++;

        const uint32_t start = cycles != nullptr ? k_cycle_get_32() : 0;
        err = adc_read(adc_device_, &sequence);
        if(cycles != nullptr)
            *cycles = k_cycle_get_32() - start;

        if(err == -ETIMEDOUT)
            stats_.timeouts++;

        if(err != -ETIMEDOUT && err != -EBUSY)
            break;
    }

    return err;
}

void Adc::MarkError(AdcSample& result, int err) {
    result.error = (err == -ETIMEDOUT || err == -EBUSY) ? AdcError::TIMEOUT : AdcError::DRIVER;
    result.errno_value = static_cast<int16_t>(err);

    stats_.errors++;
    stats_.last_errno = static_cast<int16_t>(err);
}

void Adc::Sample(const AdcSampleRequest& request) noexcept {
    ScopedMutex guard(mutex_);

    stats_.requests++;

    const auto samples = static_cast<uint16_t>(std::clamp<int>(request.samples, 1, CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX));
    const size_t count = ValidateRequest(request);

    uint16_t remaining = samples;
    if(capabilities_.multi_channel && capabilities_.burst) {
        const int err = SampleScan(request, count, remaining);

        // The driver cannot attribute a timeout to one channel and the retries are spent, so every
        // channel of the scan fails. Any other error leaves the completed samplings in the
        // accumulators, and per-channel reads take over the rest to isolate the failing channel.
        if(err == -ETIMEDOUT || err == -EBUSY) {
            for(size_t i = 0; i < count; i++)
                if(request.results[i].error == AdcError::NONE)
                    MarkError(request.results[i], err);

            remaining = 0;
        }
    }

    if(remaining > 0)
        SampleEach(request, count, remaining, capabilities_.burst ? CONFIG_EERIE_LEAP_ADC_BURST_CHUNK : 1);

    ResolveResults(request, count, samples);
}

// Resets every result and the accumulators of the requested channels, and marks the entries the
// request cannot be served for: a channel the device does not have, one not in ascending order,
// or any channel before a successful Initialize().
size_t Adc::ValidateRequest(const AdcSampleRequest& request) {
    const size_t count = std::min(request.channels.size(), request.results.size());
    int last_channel = -1;

    for(size_t i = 0; i < count; i++) {
        request.results[i] = {};

        const int channel = request.channels[i];
        if(channel >= GetChannelCount() || channel <= last_channel) {
            request.results[i].error = AdcError::CHANNEL_OUT_OF_RANGE;
            request.results[i].errno_value = -EINVAL;
            stats_.errors++;
            continue;
        }

        last_channel = channel;

        if(!initialized_) {
            MarkError(request.results[i], -ENODEV);
            continue;
        }

        accumulators_[channel] = 0;
    }

    return count;
}

// One adc_read() per chunk for every requested channel at once. The buffer holds one sample per
// channel per sampling, channels in ascending id order. remaining counts down the samplings still
// to do, so after an error it holds the failed chunk and everything after it.
int Adc::SampleScan(const AdcSampleRequest& request, size_t count, uint16_t& remaining) {
    uint32_t mask = 0;
    for(size_t i = 0; i < count; i++)
        if(request.results[i].error == AdcError::NONE)
            mask |= BIT(channel_configs_[request.channels[i]].channel_id);

    if(mask == 0) {
        remaining = 0;
        return 0;
    }

    const int first_channel = local_by_id_[std::countr_zero(mask)];
    const auto active = static_cast<uint32_t>(std::popcount(mask));

    while(remaining > 0) {
        const auto chunk = static_cast<uint16_t>(std::min<int>(remaining, CONFIG_EERIE_LEAP_ADC_BURST_CHUNK));

        const adc_sequence sequence = MakeSequence(first_channel, mask, chunk);
        int err = Read(sequence);
        if(err < 0)
            return err;

        const uint16_t* sample = burst_buffer_.data();
        for(uint16_t s = 0; s < chunk; s++)
            for(uint32_t bits = mask; bits != 0; bits &= bits - 1)
                accumulators_[local_by_id_[std::countr_zero(bits)]] += *sample++;

        stats_.conversions += chunk * active;
        remaining -= chunk;
    }

    return 0;
}

// One channel at a time, chunk_size samplings per adc_read() (1 for drivers without bursts),
// adding to what the accumulators already hold. An error stops that channel and moves on to the next one.
void Adc::SampleEach(const AdcSampleRequest& request, size_t count, uint16_t samples, uint16_t chunk_size) {
    for(size_t i = 0; i < count; i++) {
        if(request.results[i].error != AdcError::NONE)
            continue;

        const int channel = request.channels[i];
        const uint32_t mask = BIT(channel_configs_[channel].channel_id);

        uint16_t remaining = samples;
        while(remaining > 0) {
            const auto chunk = static_cast<uint16_t>(std::min(remaining, chunk_size));

            const adc_sequence sequence = MakeSequence(channel, mask, chunk);
            int err = Read(sequence);
            if(err < 0) {
                MarkError(request.results[i], err);
                break;
            }

            for(uint16_t s = 0; s < chunk; s++)
                accumulators_[channel] += burst_buffer_[s];

            stats_.conversions += chunk;
            remaining -= chunk;
        }
    }
}

void Adc::ResolveResults(const AdcSampleRequest& request, size_t count, uint16_t samples) const {
    for(size_t i = 0; i < count; i++) {
        if(request.results[i].error != AdcError::NONE)
            continue;

        const int channel = request.channels[i];
        request.results[i].volts = static_cast<float>(accumulators_[channel]) * (scale_[channel] / samples);
    }
}

int Adc::GetChannelCount() const {
    return static_cast<int>(channel_configs_.size());
}

AdcCapabilities Adc::GetCapabilities() const {
    return capabilities_;
}

AdcStats Adc::GetStats() const {
    ScopedMutex guard(mutex_);

    return stats_;
}

} // namespace eerie_leap::subsys::adc

#endif // CONFIG_ADC
