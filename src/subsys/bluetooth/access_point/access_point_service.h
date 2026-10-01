#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

#include "subsys/bluetooth/i_ble_notifier.h"

namespace eerie_leap::subsys::bluetooth::access_point {

// Base UUID: e7a1b2c3-d4e5-6f78-9a0b-c1d2e3f40000
#define BT_UUID_ACCESS_POINT_ENCODE(id) \
    BT_UUID_128_ENCODE(0xe7a1b2c3, 0xd4e5, 0x6f78, 0x9a0b, 0xc1d2e3f40000 + (id))

#define BT_UUID_ACCESS_POINT_SERVICE_VAL BT_UUID_ACCESS_POINT_ENCODE(0)

/**
 * @brief GATT service of an access point: `smp` (SMP with a route prefix), `live` and `info`.
 *
 * While SMP writes arrive the connection is asked for a short interval and the 2M PHY; after an
 * idle period it is asked for a long interval again.
 *
 * Only one instance may exist, because the GATT table is static.
 */
class AccessPointService {
public:
    struct Callbacks {
        /// A write to `smp`, on the BT RX thread.
        std::function<void(std::span<const uint8_t> data)> on_smp_write;
        /// An `smp` notification left, on the system work queue.
        std::function<void()> on_smp_sent;
        /// A `live` notification left, on the system work queue.
        std::function<void()> on_live_sent;
    };

    static constexpr size_t MAX_INFO_SIZE = 16;
    /// SMP idle time after which the long connection interval is requested.
    static constexpr int32_t IDLE_TIMEOUT_MS = 3000;

private:
    class Notifier : public IBleNotifier {
    private:
        const bt_gatt_attr* attribute_;
        bt_gatt_complete_func_t on_sent_;

    public:
        Notifier(const bt_gatt_attr* attribute, bt_gatt_complete_func_t on_sent);

        [[nodiscard]] size_t GetMaxNotificationSize() const override;
        int Notify(std::span<const uint8_t> data) override;
    };

    static std::atomic<AccessPointService*> instance_;
    static const bt_gatt_attr attributes_[];
    // Placed in the iterable section by its definition.
    static const bt_gatt_service_static gatt_service_;

    Callbacks callbacks_;
    std::array<uint8_t, MAX_INFO_SIZE> info_{};
    size_t info_size_ = 0;
    std::shared_ptr<Notifier> smp_notifier_;
    std::shared_ptr<Notifier> live_notifier_;

    std::atomic<bool> is_fast_ = false;
    k_work fast_work_{};
    k_work_delayable idle_work_{};
    int connected_handler_id_ = -1;
    int disconnected_handler_id_ = -1;

    static ssize_t SmpWriteCallback(
        bt_conn* conn, const bt_gatt_attr* attr, const void* buf, uint16_t len, uint16_t offset, uint8_t flags);
    static ssize_t InfoReadCallback(
        bt_conn* conn, const bt_gatt_attr* attr, void* buf, uint16_t len, uint16_t offset);
    static void SmpSentCallback(bt_conn* conn, void* user_data);
    static void LiveSentCallback(bt_conn* conn, void* user_data);
    static void FastWorkHandler(k_work* work);
    static void IdleWorkHandler(k_work* work);

    void OnSmpActivity();
    static void RequestConnectionParameters(bool is_fast);

public:
    /** @throws std::logic_error if another instance exists. */
    AccessPointService();
    ~AccessPointService();

    AccessPointService(const AccessPointService&) = delete;
    AccessPointService& operator=(const AccessPointService&) = delete;

    /**
     * @brief Starts serving the characteristics; call after Ble::Initialize().
     * @param info Value of `info`, up to MAX_INFO_SIZE bytes.
     */
    void Initialize(Callbacks callbacks, std::span<const uint8_t> info);

    [[nodiscard]] std::shared_ptr<IBleNotifier> GetSmpNotifier() const { return smp_notifier_; }
    [[nodiscard]] std::shared_ptr<IBleNotifier> GetLiveNotifier() const { return live_notifier_; }
};

} // namespace eerie_leap::subsys::bluetooth::access_point
