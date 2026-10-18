#pragma once

#include <cstdint>
#include <span>

namespace eerie_leap::subsys::adc {

enum class AdcError : uint8_t {
    NONE,
    CHANNEL_OUT_OF_RANGE,
    DRIVER,
    TIMEOUT
};

struct AdcSample {
    float volts = 0.0F; // Mean of the requested conversions in ADC-pin volts, full precision.
    AdcError error = AdcError::NONE;
    int16_t errno_value = 0; // -errno from the driver when error != NONE.
};

// Built once (per rate group or per calibration job); only the results change per call.
struct AdcSampleRequest {
    std::span<const uint8_t> channels; // Local channel indices, ascending (driver mask order).
    std::span<AdcSample> results;      // One per channel.
    uint16_t samples = 1;              // Conversions averaged per channel, 1..CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX.
};

struct AdcCapabilities {
    bool burst = false;             // extra_samplings accepted.
    bool multi_channel = false;     // More than one bit in adc_sequence.channels accepted.
    uint32_t us_per_conversion = 0; // Measured by the probe at Initialize().
};

struct AdcStats {
    uint32_t requests = 0;     // Sample() calls.
    uint32_t driver_calls = 0; // adc_read() calls, retries included.
    uint32_t conversions = 0;  // Conversions that reached an accumulator.
    uint32_t errors = 0;       // Channel results marked with an error.
    uint32_t timeouts = 0;     // Driver calls that returned -ETIMEDOUT.
    int16_t last_errno = 0;
};

class IAdc;

struct AdcChannelHandle {
    IAdc* adc = nullptr;
    uint8_t local_channel = 0;
};

} // namespace eerie_leap::subsys::adc
