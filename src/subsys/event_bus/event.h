#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "utilities/concepts/concepts.h"

namespace eerie_leap::subsys::event_bus {

namespace concepts = eerie_leap::utilities::concepts;

using EventData = std::variant<int, uint32_t, float, std::string, bool>;

// Payloads carry a handful of keys (four is the most any publisher sends today), so they live
// inline: no bucket array, no node per key, nothing to free. A string value still owns its
// characters; numeric payloads, which is what the sensor hot path sends, never touch the heap.
inline constexpr size_t k_max_payload_entries = 6;

// A small map with the subset of std::unordered_map's interface the bus and its users rely on,
// over a fixed array. Keys are unique; insertion order is kept. A payload that is already full
// overwrites its last entry instead of growing, which no publisher in the tree gets near.
template<concepts::EnumClassUint32 PayloadTypeEnum>
class InlinePayload {
public:
    using key_type = PayloadTypeEnum;
    using mapped_type = EventData;
    using value_type = std::pair<PayloadTypeEnum, EventData>;
    using iterator = value_type*;
    using const_iterator = const value_type*;

    static constexpr size_t capacity() { return k_max_payload_entries; }

private:
    std::array<value_type, k_max_payload_entries> entries_ { };
    uint8_t size_ = 0;

    // The slot for @p key: its entry when present, otherwise a fresh one (the last one when full).
    value_type& Slot(PayloadTypeEnum key) {
        if(auto it = find(key); it != end())
            return *it;

        if(size_ == k_max_payload_entries) {
            entries_[size_ - 1] = value_type { key, EventData { } };
            return entries_[size_ - 1];
        }

        entries_[size_] = value_type { key, EventData { } };
        return entries_[size_++];
    }

public:
    InlinePayload() = default;

    InlinePayload(std::initializer_list<value_type> entries) {
        for(const auto& entry : entries)
            insert_or_assign(entry.first, entry.second);
    }

    iterator begin() { return entries_.data(); }
    iterator end() { return entries_.data() + size_; }
    const_iterator begin() const { return entries_.data(); }
    const_iterator end() const { return entries_.data() + size_; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    iterator find(PayloadTypeEnum key) {
        for(iterator it = begin(); it != end(); ++it) {
            if(it->first == key)
                return it;
        }

        return end();
    }

    const_iterator find(PayloadTypeEnum key) const {
        for(const_iterator it = begin(); it != end(); ++it) {
            if(it->first == key)
                return it;
        }

        return end();
    }

    bool contains(PayloadTypeEnum key) const { return find(key) != end(); }

    EventData& at(PayloadTypeEnum key) {
        auto it = find(key);
        if(it == end())
            throw std::out_of_range("payload key");

        return it->second;
    }

    const EventData& at(PayloadTypeEnum key) const {
        auto it = find(key);
        if(it == end())
            throw std::out_of_range("payload key");

        return it->second;
    }

    EventData& operator[](PayloadTypeEnum key) { return Slot(key).second; }

    // Like std::unordered_map::emplace: keeps an existing entry.
    template<typename... Args>
    std::pair<iterator, bool> emplace(PayloadTypeEnum key, Args&&... args) {
        if(auto it = find(key); it != end())
            return { it, false };

        auto& slot = Slot(key);
        slot.second = EventData(std::forward<Args>(args)...);

        return { &slot, true };
    }

    std::pair<iterator, bool> insert(const value_type& entry) { return emplace(entry.first, entry.second); }

    std::pair<iterator, bool> insert_or_assign(PayloadTypeEnum key, EventData value) {
        const bool existed = contains(key);
        auto& slot = Slot(key);
        slot.second = std::move(value);

        return { &slot, !existed };
    }

    size_t erase(PayloadTypeEnum key) {
        auto it = find(key);
        if(it == end())
            return 0;

        for(iterator next = it + 1; next != end(); ++it, ++next)
            *it = std::move(*next);

        *it = value_type { };
        --size_;

        return 1;
    }

    void clear() {
        for(auto& entry : *this)
            entry = value_type { };

        size_ = 0;
    }

    // Kept for callers written against the map; the storage is fixed.
    void reserve(size_t) { }
};

template<concepts::EnumClassUint32 PayloadTypeEnum>
using EventPayload = InlinePayload<PayloadTypeEnum>;

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
struct Event {
    uint32_t source_id = 0;
    EventTypeEnum type;
    EventPayload<PayloadTypeEnum> payload;
};

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
using EventHandler = std::function<void(const Event<EventTypeEnum, PayloadTypeEnum>&)>;

} // namespace eerie_leap::subsys::event_bus
