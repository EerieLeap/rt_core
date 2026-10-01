#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#include "subsys/bluetooth/i_ble_notifier.h"
#include "subsys/smp/smp_header.h"
#include "subsys/smp/smp_packet.h"
#include "subsys/smp/i_smp_forwarder.h"
#include "subsys/smp/i_smp_response_sink.h"

namespace eerie_leap::subsys::smp::ble {

using eerie_leap::subsys::bluetooth::IBleNotifier;

/**
 * @brief Routes SMP between a BLE central and the local SMP server or, through a forwarder, other units.
 *
 * Every packet on the link is `[route][SMP packet]` and may span several writes or notifications;
 * the SMP length marks where it ends. A request's route is its destination: 0 or this unit's own
 * address reaches the local SMP server, another reachable address is forwarded, and anything else
 * is answered with MGMT_ERR_ENOENT. A response carries the route of the unit that answered.
 *
 * Responses are notified from the system work queue, where notifications never block. Only one
 * instance may exist, because MCUmgr's output callback carries no context.
 */
class SmpBleRouter : public ISmpResponseSink {
public:
    /// Version of the `smp` characteristic protocol, reported by GetInfo().
    static constexpr uint8_t PROTOCOL_VERSION = 1;
    static constexpr uint8_t LOCAL_ROUTE = 0;
    static constexpr size_t INFO_SIZE = 4;
    /// Retry delay when the link has no free notification buffer and none of ours is pending.
    static constexpr int32_t TX_RETRY_DELAY_MS = 10;

private:
    static std::atomic<SmpBleRouter*> instance_;

    std::shared_ptr<IBleNotifier> notifier_;
    std::shared_ptr<ISmpForwarder> forwarder_;
    smp_transport smp_transport_{};
    bool is_initialized_ = false;

    // Bumped on disconnect, so packets of an earlier connection are dropped.
    std::atomic<uint8_t> generation_ = 0;

    // Owned by the thread that calls OnReceive() and OnDisconnected().
    std::array<uint8_t, 1 + SmpHeader::SIZE> rx_head_{};
    size_t rx_head_size_ = 0;
    SmpPacket rx_packet_;
    size_t rx_remaining_ = 0;
    bool rx_discarding_ = false;
    int64_t rx_last_ms_ = 0;

    // Owned by tx_work_ on the system work queue.
    k_fifo tx_queue_{};
    k_work_delayable tx_work_{};
    SmpPacket tx_packet_;
    size_t tx_offset_ = 0;
    std::array<uint8_t, CONFIG_EERIE_LEAP_SMP_BLE_MAX_FIRST_FRAGMENT_SIZE> tx_first_fragment_{};

    std::atomic<uint32_t> rx_dropped_ = 0;
    std::atomic<uint32_t> tx_dropped_ = 0;

    static int Output(net_buf* packet);
    static uint16_t GetMtu(const net_buf* packet);
    static void TxWorkHandler(k_work* work);

    void ResetRx();
    void OnHeader();
    void Dispatch(uint8_t route, SmpPacket packet);
    void Reject(uint8_t route, SmpPacket packet);
    void EnqueueTx(uint8_t route, uint8_t generation, SmpPacket packet);
    void ProcessTx();
    void DropTx();

public:
    /**
     * @param notifier Notifications on the `smp` characteristic.
     * @param forwarder Reaches the other units; nullptr on a unit that only serves itself.
     */
    SmpBleRouter(std::shared_ptr<IBleNotifier> notifier, std::shared_ptr<ISmpForwarder> forwarder);
    ~SmpBleRouter() override;

    SmpBleRouter(const SmpBleRouter&) = delete;
    SmpBleRouter& operator=(const SmpBleRouter&) = delete;
    SmpBleRouter(SmpBleRouter&&) = delete;
    SmpBleRouter& operator=(SmpBleRouter&&) = delete;

    /** @brief Registers with MCUmgr. */
    bool Initialize();

    /** @brief Takes the data of one write to the `smp` characteristic; never blocks. */
    void OnReceive(std::span<const uint8_t> data);
    /** @brief Drops partial and pending packets; call from the thread that calls OnReceive(). */
    void OnDisconnected();
    /** @brief A notification left, so a buffer may be free again. */
    void OnNotificationSent();

    /** @brief Notifies a response from another unit; never blocks. */
    void OnResponse(uint8_t source, SmpPacket packet) override;

    /** @brief The `info` characteristic: `[version][max packet size: u16 LE][requests in flight]`. */
    static std::array<uint8_t, INFO_SIZE> GetInfo();

    /** @brief Received packets dropped: malformed, oversized, not a request, no buffer or not forwardable. */
    [[nodiscard]] uint32_t GetRxDroppedCount() const { return rx_dropped_; }
    /** @brief Responses dropped before being fully notified. */
    [[nodiscard]] uint32_t GetTxDroppedCount() const { return tx_dropped_; }
};

} // namespace eerie_leap::subsys::smp::ble
