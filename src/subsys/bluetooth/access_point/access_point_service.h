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

#include "subsys/bluetooth/access_point/fast_link_policy.h"
#include "subsys/bluetooth/i_ble_notifier.h"

namespace eerie_leap::subsys::bluetooth::access_point {

// Base UUID: e7a1b2c3-d4e5-6f78-9a0b-c1d2e3f40000
#define BT_UUID_ACCESS_POINT_ENCODE(id) BT_UUID_128_ENCODE(0xe7a1b2c3, 0xd4e5, 0x6f78, 0x9a0b, 0xc1d2e3f40000 + (id))

#define BT_UUID_ACCESS_POINT_SERVICE_VAL BT_UUID_ACCESS_POINT_ENCODE(0)

/**
 * @brief GATT service of an access point: `smp` (SMP with a route prefix), `live`, `info` and `bus`
 * (the CAN monitor's stream).
 *
 * While SMP writes arrive, or while a hold (HoldFast()) is held, the connection is asked for a
 * short interval and the 2M PHY; some time after the last of them it is asked for a long interval
 * again. When the short request leaves an interval above 15 ms, one request for 15 to 30 ms follows
 * (FastLinkPolicy).
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
        /// A `bus` notification left, on the system work queue. SetBusSentHandler() replaces it.
        std::function<void()> on_bus_sent;
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

    static std::atomic<AccessPointService*> instance;
    static const bt_gatt_attr ATTRIBUTES[];
    // Placed in the iterable section by its definition, after Zephyr's GATT and GAP services.
    static const bt_gatt_service_static GATT_SERVICE;

    Callbacks callbacks_;
    std::array<uint8_t, MAX_INFO_SIZE> info_{};
    size_t info_size_ = 0;
    std::shared_ptr<Notifier> smp_notifier_;
    std::shared_ptr<Notifier> live_notifier_;
    std::shared_ptr<Notifier> bus_notifier_;

    // Held by BusSentCallback() while on_bus_sent runs, so replacing it waits for a running call.
    k_mutex bus_sent_mutex_{};

    // Events come from the BT RX thread, the system work queue and the callers of HoldFast(). The
    // lock covers the policy and the work calls that carry out its decisions, so those cannot be
    // reordered between threads (an idle work left scheduled after the last release would keep the
    // link fast for ever). The Bluetooth calls themselves run in the work handlers.
    k_spinlock policy_lock_{};
    FastLinkPolicy policy_;
    k_work fast_work_{};
    k_work fallback_work_{};
    k_work_delayable idle_work_{};
    int connected_handler_id_ = -1;
    int disconnected_handler_id_ = -1;
    int parameters_updated_handler_id_ = -1;

    static ssize_t SmpWriteCallback(
        bt_conn* conn,
        const bt_gatt_attr* attr,
        const void* buf,
        uint16_t len,
        uint16_t offset,
        uint8_t flags
    );
    static ssize_t InfoReadCallback(bt_conn* conn, const bt_gatt_attr* attr, void* buf, uint16_t len, uint16_t offset);
    static void SmpSentCallback(bt_conn* conn, void* user_data);
    static void LiveSentCallback(bt_conn* conn, void* user_data);
    static void BusSentCallback(bt_conn* conn, void* user_data);
    static void FastWorkHandler(k_work* work);
    static void FallbackWorkHandler(k_work* work);
    static void IdleWorkHandler(k_work* work);

    void OnSmpActivity();
    void OnParametersUpdated(uint16_t interval);
    /// Carries out the work calls of a decision; call with policy_lock_ held.
    void ApplyLocked(const FastLinkPolicy::Decision& decision);
    void RequestFast();
    static void RequestFallback();
    void RequestIdle();

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

    [[nodiscard]] std::shared_ptr<IBleNotifier> GetSmpNotifier() const {
        return smp_notifier_;
    }
    [[nodiscard]] std::shared_ptr<IBleNotifier> GetLiveNotifier() const {
        return live_notifier_;
    }
    [[nodiscard]] std::shared_ptr<IBleNotifier> GetBusNotifier() const {
        return bus_notifier_;
    }

    /**
     * @brief Takes a hold on the fast link; callable from any thread, never blocks.
     *
     * Holds are counted. While any is held the fast parameters and the 2M PHY stay requested (at the
     * first hold, and again on a new connection) and SMP traffic does not start the idle work. Every
     * call needs one ReleaseFast(); the CAN monitor holds from the opening of a session to its end.
     */
    void HoldFast();
    /** @brief Gives a hold back; the idle work starts IDLE_TIMEOUT_MS after the last one. */
    void ReleaseFast();

    /**
     * @brief Replaces on_bus_sent, which runs on the system work queue.
     *
     * Safe against a completion running at the same moment: the call waits for a running handler to
     * return, so once it has returned with an empty handler the old one is no longer running. It must
     * not wait for something the handler waits for, and a handler may replace itself.
     */
    void SetBusSentHandler(std::function<void()> handler);
};

} // namespace eerie_leap::subsys::bluetooth::access_point
