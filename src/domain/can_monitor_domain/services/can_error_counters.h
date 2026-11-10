#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include <zephyr/device.h>

namespace eerie_leap::domain::can_monitor_domain::services {

/** @brief The controller's counts, as it keeps them since it was started; each wraps at 2^32. */
struct CanErrorCounts {
    /// Bit, stuff, CRC, form and acknowledgement errors, in the statistics record's order.
    std::array<uint32_t, 5> errors{};
    /// Frames the controller dropped because its receive buffer was full.
    uint32_t rx_overruns = 0;
};

/**
 * @brief Where a session gets Zephyr's error counts and receive overruns, so that tests can stub them.
 *
 * Read() is called from the bus's receive thread (for the overruns) and from the threads that make
 * statistics, so it takes no lock and never blocks.
 */
class ICanErrorCounters {
public:
    virtual ~ICanErrorCounters() = default;

    [[nodiscard]] virtual CanErrorCounts Read() const = 0;
};

/** @brief Reads `can_stats_get_*` of a controller; all zeros without CONFIG_CAN_STATS. */
class DeviceCanErrorCounters final : public ICanErrorCounters {
private:
    const device* device_;

public:
    explicit DeviceCanErrorCounters(const device* device) :
        device_(device) {}

    [[nodiscard]] CanErrorCounts Read() const override;
};

/** @brief Makes the counters of a controller; the service's default makes DeviceCanErrorCounters. */
using CanErrorCountersFactory = std::function<std::shared_ptr<ICanErrorCounters>(const device*)>;

} // namespace eerie_leap::domain::can_monitor_domain::services
