#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace eerie_leap::subsys::event_bus {

// Remembers per key whether a channel's subscribers accepted an event (IEventChannel::Accepts()),
// so a publisher runs the filters once per key and subscription change instead of once per event.
// The key is what the filters select on (a sensor id hash); an answer that depends on anything else
// in the event is cached for the first event only. Fixed capacity, never allocates, owned by one
// thread. Header only.
template<size_t TCapacity>
class AcceptanceCache {
    static_assert(TCapacity > 0);

private:
    struct Entry {
        uint32_t key = 0;
        bool is_accepted = false;
    };

    std::array<Entry, TCapacity> entries_ { };   // The first count_, sorted by key.
    size_t count_ = 0;
    uint32_t subscription_generation_ = 0;

    // The index of key's entry, or of where it would go.
    size_t LowerBound(uint32_t key) const {
        const auto it = std::lower_bound(entries_.begin(), entries_.begin() + count_, key,
            [](const Entry& entry, uint32_t value) { return entry.key < value; });

        return static_cast<size_t>(it - entries_.begin());
    }

public:
    // Forgets every answer when the subscriptions changed since they were given. Called with the
    // generation read after the events to judge were taken (see SensorEventsPublisher), so a change
    // that happened before they were taken is never answered from an older entry.
    void Revalidate(uint32_t subscription_generation) {
        if(subscription_generation == subscription_generation_)
            return;

        subscription_generation_ = subscription_generation;
        count_ = 0;
    }

    [[nodiscard]] std::optional<bool> Find(uint32_t key) const {
        const size_t index = LowerBound(key);
        if(index == count_ || entries_[index].key != key)
            return std::nullopt;

        return entries_[index].is_accepted;
    }

    // A known key is overwritten. A full cache forgets every answer first: keys no longer published
    // (sensors of an earlier configuration) make room, and the keys in use are asked once more each.
    void Store(uint32_t key, bool is_accepted) {
        size_t index = LowerBound(key);
        if(index != count_ && entries_[index].key == key) {
            entries_[index].is_accepted = is_accepted;
            return;
        }

        if(count_ == TCapacity) {
            count_ = 0;
            index = 0;
        }

        std::copy_backward(entries_.begin() + index, entries_.begin() + count_, entries_.begin() + count_ + 1);
        entries_[index] = Entry { .key = key, .is_accepted = is_accepted };
        count_++;
    }

    [[nodiscard]] size_t GetCount() const { return count_; }
};

} // namespace eerie_leap::subsys::event_bus
