#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace eerie_leap::subsys::bluetooth {

/** @brief Notifications on one characteristic of the connected central. */
class IBleNotifier {
public:
    virtual ~IBleNotifier() = default;

    /** @brief Largest notification payload; 0 while the central is not subscribed. */
    [[nodiscard]] virtual size_t GetMaxNotificationSize() const = 0;

    /**
     * @brief Notifies @p data without blocking; the data is copied before it returns.
     * @return 0, -ENOMEM while no buffer is free, or another negative errno if it cannot be sent.
     */
    virtual int Notify(std::span<const uint8_t> data) = 0;
};

} // namespace eerie_leap::subsys::bluetooth
