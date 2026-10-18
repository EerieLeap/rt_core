#pragma once

#ifdef CONFIG_ADC

    #include <array>
    #include <cstddef>
    #include <cstdint>
    #include <vector>

    #include <zephyr/drivers/adc.h>
    #include <zephyr/kernel.h>

    #include "subsys/device_tree/adc_dt_info.h"

    #include "i_adc.h"

namespace eerie_leap::subsys::adc {

using eerie_leap::subsys::device_tree::AdcDTInfo;

// One hardware ADC: device-tree facts and the driver capabilities probed at Initialize().
// Holds nothing of the user configuration; a request says how many conversions to average.
// Sample() is serialized by a mutex and allocates nothing.
class Adc : public IAdc {
private:
    static constexpr int MAX_RETRIES = 2;        // Further attempts after -ETIMEDOUT or -EBUSY.
    static constexpr size_t MAX_CHANNEL_ID = 32; // Width of adc_sequence.channels.

    std::vector<uint32_t> references_mv_;
    std::vector<uint8_t> resolutions_;
    std::vector<uint8_t> oversampling_;
    std::vector<float> scale_; // Raw count to ADC-pin volts, per channel.
    std::array<uint8_t, MAX_CHANNEL_ID> local_by_id_{};
    bool uniform_sequence_ = false; // Every channel shares resolution and oversampling.
    bool initialized_ = false;      // Set once the probe has succeeded.

    // Sized once in Initialize(): channel_count × CONFIG_EERIE_LEAP_ADC_BURST_CHUNK samples,
    // and one accumulator per channel.
    std::vector<uint16_t> burst_buffer_;
    std::vector<uint32_t> accumulators_;
    adc_sequence_options sequence_options_{};

    mutable k_mutex mutex_{};
    AdcStats stats_{};

    adc_sequence MakeSequence(int channel, uint32_t mask, uint16_t samplings);
    int Read(const adc_sequence& sequence, uint32_t* cycles = nullptr);
    void MarkError(AdcSample& result, int err);
    bool Probe();

    size_t ValidateRequest(const AdcSampleRequest& request);
    int SampleScan(const AdcSampleRequest& request, size_t count, uint16_t& remaining);
    void SampleEach(const AdcSampleRequest& request, size_t count, uint16_t samples, uint16_t chunk_size);
    void ResolveResults(const AdcSampleRequest& request, size_t count, uint16_t samples) const;

protected:
    std::vector<adc_channel_cfg> channel_configs_;
    const device* adc_device_;
    AdcCapabilities capabilities_{};

public:
    explicit Adc(const AdcDTInfo& adc_dt_info);

    // Owns a kernel mutex, which a copy would duplicate together with its lock state.
    Adc(const Adc&) = delete;
    Adc& operator=(const Adc&) = delete;
    Adc(Adc&&) = delete;
    Adc& operator=(Adc&&) = delete;
    ~Adc() override = default;

    bool Initialize() override;
    void Sample(const AdcSampleRequest& request) noexcept override;
    [[nodiscard]] int GetChannelCount() const override;
    [[nodiscard]] AdcCapabilities GetCapabilities() const override;
    [[nodiscard]] AdcStats GetStats() const override;
};

} // namespace eerie_leap::subsys::adc

#endif // CONFIG_ADC
