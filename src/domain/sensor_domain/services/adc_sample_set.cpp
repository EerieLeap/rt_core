#include <algorithm>
#include <stdexcept>

#include "adc_sample_set.h"

namespace eerie_leap::domain::sensor_domain::services {

const AdcSample* AdcSampleSet::Reserve(int channel, AdcChannelHandle handle, uint16_t samples) {
    if(is_sealed_)
        throw std::logic_error("ADC sample set is sealed.");

    if(handle.adc == nullptr)
        throw std::invalid_argument("ADC channel handle has no ADC.");

    const int channel_count = handle.adc->GetChannelCount();
    if(handle.local_channel >= channel_count)
        throw std::invalid_argument("ADC channel out of range.");

    auto entry = std::ranges::find(adcs_, handle.adc, &AdcEntry::adc);
    if(entry == adcs_.end()) {
        AdcEntry added;
        added.adc = handle.adc;
        added.channels.reserve(channel_count);
        added.global_channels.reserve(channel_count);
        added.results.resize(channel_count);

        // Moving an entry keeps its buffers, so the slots handed out so far stay valid.
        adcs_.push_back(std::move(added));
        entry = adcs_.end() - 1;
    }

    // A new ADC has no channels yet, so a throw below never leaves an empty entry behind.
    const auto position = std::ranges::lower_bound(entry->channels, handle.local_channel);
    const auto index = position - entry->channels.begin();
    if(position == entry->channels.end() || *position != handle.local_channel) {
        entry->global_channels.insert(entry->global_channels.begin() + index, channel);
        entry->channels.insert(position, handle.local_channel);
    } else if(entry->global_channels[index] != channel) {
        throw std::invalid_argument("ADC channel already reserved under another global channel.");
    }

    entry->samples = std::max(entry->samples, samples);

    return &entry->results[handle.local_channel];
}

void AdcSampleSet::Seal() {
    for(auto& entry : adcs_) {
        entry.packed_results.resize(entry.channels.size());
        entry.request = {
            .channels = entry.channels,
            .results = entry.packed_results,
            .samples = entry.samples
        };
    }

    is_sealed_ = true;
}

void AdcSampleSet::Acquire() noexcept {
    if(!is_sealed_)
        return;

    for(auto& entry : adcs_) {
        entry.adc->Sample(entry.request);

        for(size_t i = 0; i < entry.channels.size(); i++)
            entry.results[entry.channels[i]] = entry.packed_results[i];
    }
}

AdcSampleSet::AdcView AdcSampleSet::GetAdc(size_t index) const {
    const AdcEntry& entry = adcs_[index];

    return {
        .adc = entry.adc,
        .channels = entry.channels,
        .global_channels = entry.global_channels,
        .results = entry.results,
        .samples = entry.samples
    };
}

size_t AdcSampleSet::GetChannelCount() const {
    size_t count = 0;
    for(const auto& entry : adcs_)
        count += entry.channels.size();

    return count;
}

} // namespace eerie_leap::domain::sensor_domain::services
