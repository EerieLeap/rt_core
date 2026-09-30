#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace eerie_leap::subsys::smp::ble {

/** @brief The BLE side of SmpBleRouter: notifications on the `smp` characteristic of the central. */
class ISmpBleLink {
public:
    virtual ~ISmpBleLink() = default;

    /** @brief Largest notification payload; 0 while no central is subscribed. */
    [[nodiscard]] virtual size_t GetMaxNotificationSize() const = 0;

    /**
     * @brief Notifies @p fragment without blocking; the data is copied before it returns.
     * @return 0, -ENOMEM while no buffer is free, or another negative errno if it cannot be sent.
     */
    virtual int Notify(std::span<const uint8_t> fragment) = 0;
};

} // namespace eerie_leap::subsys::smp::ble
