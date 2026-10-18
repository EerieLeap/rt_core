#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "subsys/cdmp/models/cdmp_device.h"

namespace eerie_leap::subsys::cdmp::models {

/** @brief The data bytes of a shared state record, stored without allocation. */
class CdmpStateData {
public:
    /** @brief A notification keeps the 8 bytes of a classic frame, 3 of them its header (§4.3). */
    static constexpr size_t MAX_SIZE = 5;

private:
    std::array<uint8_t, MAX_SIZE> bytes_{};
    uint8_t size_ = 0;

public:
    constexpr CdmpStateData() = default;

    /** @throws std::invalid_argument if @p data is longer than MAX_SIZE. */
    explicit CdmpStateData(std::span<const uint8_t> data) {
        if(data.size() > MAX_SIZE)
            throw std::invalid_argument("State data is longer than 5 bytes");

        std::ranges::copy(data, bytes_.begin());
        size_ = static_cast<uint8_t>(data.size());
    }

    [[nodiscard]] std::span<const uint8_t> GetData() const {
        return { bytes_.data(), size_ };
    }
    [[nodiscard]] size_t GetSize() const {
        return size_;
    }
    [[nodiscard]] bool IsEmpty() const {
        return size_ == 0;
    }

    bool operator==(const CdmpStateData& other) const {
        return std::ranges::equal(GetData(), other.GetData());
    }
};

/** @brief Another unit's record of one state type, as CdmpStateService::GetRemoteStates() copies it. */
struct CdmpRemoteState {
    uint8_t device_id = CdmpDevice::DEVICE_ID_UNASSIGNED; ///< The unit that owns the record.
    CdmpStateData data;
    /// Not confirmed since its owner restarted or its digest differed: it keeps its last value until
    /// the owner sends the record again or a heartbeat digest matches.
    bool is_stale = false;
};

} // namespace eerie_leap::subsys::cdmp::models
