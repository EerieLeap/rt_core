#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "subsys/adc/adc_types.h"
#include "subsys/adc/i_adc.h"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::subsys::adc::AdcChannelHandle;
using eerie_leap::subsys::adc::AdcSample;
using eerie_leap::subsys::adc::AdcSampleRequest;
using eerie_leap::subsys::adc::IAdc;

// The ADC channels of one rate group, acquired together at the start of each tick: one Sample()
// per ADC covering every channel the group reads on it.
//
// Slots are indexed by local channel. An ADC's slots are sized to its channel count when it is
// first reserved and never resized, so the pointer Reserve() returns stays on its channel whatever
// is reserved afterwards, and sensors on one channel share one slot and one conversion. The request
// covers the reserved channels only, ascending; the driver fills a packed buffer, one entry per
// requested channel, which Acquire() copies into the slots. Each reserved channel keeps the global
// index it was reserved under, the number sensors and users know it by, for reporting.
class AdcSampleSet {
private:
    struct AdcEntry {
        IAdc* adc = nullptr;
        std::vector<uint8_t> channels;    // Reserved local channels, ascending, unique.
        std::vector<int> global_channels; // Parallel to channels.
        std::vector<AdcSample> results;   // One slot per local channel of the ADC.
        uint16_t samples = 0;             // The largest count any reservation asked for.

        std::vector<AdcSample> packed_results; // One per reserved channel; built by Seal().
        AdcSampleRequest request;              // Over channels and packed_results; built by Seal().
    };

    std::vector<AdcEntry> adcs_;
    bool is_sealed_ = false;

public:
    // One ADC of the set as reporting sees it: what each Acquire() asks of it and what it returned.
    struct AdcView {
        const IAdc* adc;
        std::span<const uint8_t> channels;    // Reserved local channels, ascending.
        std::span<const int> global_channels; // Parallel to channels: the global index of each.
        std::span<const AdcSample> results;   // Indexed by local channel; written by Acquire().
        uint16_t samples;
    };

    AdcSampleSet() = default;

    // Moving keeps every buffer, so slots and requests stay valid; a copy would not.
    AdcSampleSet(const AdcSampleSet&) = delete;
    AdcSampleSet& operator=(const AdcSampleSet&) = delete;
    AdcSampleSet(AdcSampleSet&&) = default;
    AdcSampleSet& operator=(AdcSampleSet&&) = default;
    ~AdcSampleSet() = default;

    // Construction time only. `channel` is the global channel `handle` was resolved from. The
    // returned slot is valid for the set's lifetime and is written by Acquire(). Throws
    // std::invalid_argument for a handle without an ADC or with a channel the ADC does not have, or
    // for a local channel already reserved under another global channel, std::logic_error once the
    // set is sealed.
    const AdcSample* Reserve(int channel, AdcChannelHandle handle, uint16_t samples);

    // Builds the requests; nothing is allocated afterwards.
    void Seal();

    // One Sample() per ADC; every slot carries its own status afterwards. Does nothing until sealed.
    void Acquire() noexcept;

    /** @return The number of ADCs sampled per Acquire(). */
    [[nodiscard]] size_t GetAdcCount() const {
        return adcs_.size();
    }

    // index < GetAdcCount(), in the order the ADCs were first reserved. Results are only stable
    // on the thread that calls Acquire().
    [[nodiscard]] AdcView GetAdc(size_t index) const;

    /** @return The number of distinct channels sampled per Acquire(), over every ADC. */
    [[nodiscard]] size_t GetChannelCount() const;
};

} // namespace eerie_leap::domain::sensor_domain::services
