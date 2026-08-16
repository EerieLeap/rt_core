#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <algorithm>
#include <initializer_list>
#include <ranges>
#include <span>
#include <type_traits>

namespace eerie_leap::subsys::canbus {

// CAN FD payload limit. Deliberately not CAN_MAX_DLEN: that one follows
// CONFIG_CAN_FD_MODE and would silently truncate stored or replayed FD frames.
inline constexpr std::size_t CAN_FRAME_MAX_DATA_LENGTH = 64;

// Fixed capacity payload: frames are decoded in the RX bottom half and copied
// into readings, so the hot path has to stay allocation free.
class CanFramePayload {
private:
    using Storage = std::array<uint8_t, CAN_FRAME_MAX_DATA_LENGTH>;

    Storage data_{};
    uint8_t size_ = 0;

public:
    using value_type = uint8_t;
    using size_type = std::size_t;
    using iterator = Storage::iterator;
    using const_iterator = Storage::const_iterator;

    constexpr CanFramePayload() = default;

    CanFramePayload(std::initializer_list<uint8_t> bytes) { // NOLINT(google-explicit-constructor)
        Assign(std::span<const uint8_t>(bytes.begin(), bytes.size()));
    }

    template <std::ranges::contiguous_range Range>
        requires std::is_same_v<std::remove_cvref_t<std::ranges::range_value_t<Range>>, uint8_t>
              && (!std::is_same_v<std::remove_cvref_t<Range>, CanFramePayload>)
    CanFramePayload(const Range& bytes) { // NOLINT(google-explicit-constructor)
        Assign(std::span<const uint8_t>(std::ranges::data(bytes), std::ranges::size(bytes)));
    }

    // Oversized input is truncated instead of rejected so the frame stays usable.
    void Assign(std::span<const uint8_t> bytes) {
        size_ = static_cast<uint8_t>(std::min(bytes.size(), CAN_FRAME_MAX_DATA_LENGTH));
        std::copy_n(bytes.begin(), size_, data_.begin());
        std::fill(data_.begin() + size_, data_.end(), 0);
    }

    void Resize(size_type size) {
        auto clamped = static_cast<uint8_t>(std::min(size, CAN_FRAME_MAX_DATA_LENGTH));
        if(clamped < size_)
            std::fill(data_.begin() + clamped, data_.begin() + size_, 0);

        size_ = clamped;
    }

    void Clear() { Resize(0); }

    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] static constexpr size_type capacity() noexcept { return CAN_FRAME_MAX_DATA_LENGTH; }

    [[nodiscard]] const uint8_t* data() const noexcept { return data_.data(); }
    [[nodiscard]] uint8_t* data() noexcept { return data_.data(); }

    [[nodiscard]] const_iterator begin() const noexcept { return data_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return data_.begin() + size_; }
    [[nodiscard]] iterator begin() noexcept { return data_.begin(); }
    [[nodiscard]] iterator end() noexcept { return data_.begin() + size_; }

    [[nodiscard]] uint8_t operator[](size_type index) const { return data_[index]; }
    [[nodiscard]] uint8_t& operator[](size_type index) { return data_[index]; }

    operator std::span<const uint8_t>() const noexcept { // NOLINT(google-explicit-constructor)
        return {data_.data(), size_};
    }

    [[nodiscard]] bool operator==(const CanFramePayload& other) const noexcept {
        return size_ == other.size_ && std::equal(begin(), end(), other.begin());
    }
};

}  // namespace eerie_leap::subsys::canbus
