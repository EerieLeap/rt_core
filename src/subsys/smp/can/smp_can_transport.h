#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/threading/work_queue_task.h"
#include "subsys/smp/smp_packet.h"
#include "subsys/smp/i_smp_forwarder.h"
#include "subsys/smp/i_smp_response_sink.h"

#include "smp_can_id_layout.h"
#include "smp_can_framer.h"
#include "smp_can_reassembler.h"
#include "smp_can_pacer.h"

namespace eerie_leap::subsys::smp::can {

using eerie_leap::subsys::canbus::Canbus;
using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;

/**
 * @brief MCUmgr transport over CAN, as specified in the CDMP README, section 5.
 *
 * Requests addressed to this unit go to the local SMP server, responses to the response sink.
 * Frames are classic or CAN FD, following the type of the bus. Sending is paced to a share of the
 * bus and never blocks. Only one instance may exist, because MCUmgr's output callback carries no
 * context.
 */
class SmpCanTransport : public ISmpForwarder {
private:
    struct RxSlot {
        uint8_t source = 0; // 0 while free
        SmpPacket packet;
        SmpCanReassembler reassembler{CONFIG_EERIE_LEAP_SMP_CAN_RX_TIMEOUT_MS};
    };

    static std::atomic<SmpCanTransport*> instance_;

    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    smp_transport smp_transport_{};

    // Serialises Configure(), Bind() and Unbind(). Never taken by the RX or TX paths.
    k_mutex bind_lock_{};
    Canbus* bound_canbus_ = nullptr;
    int handler_id_ = -1;
    std::atomic<uint8_t> address_ = 0;

    // Written only while unbound; read by the TX task after it sees a bound address.
    std::shared_ptr<CanbusProxy> canbus_;
    SmpCanIdLayout layout_;
    uint8_t bus_share_percent_ = 25;

    // Owned by the Canbus RX thread while bound.
    std::array<RxSlot, CONFIG_EERIE_LEAP_SMP_CAN_RX_SLOTS> rx_slots_;

    k_spinlock sink_lock_{};
    std::shared_ptr<ISmpResponseSink> response_sink_;

    // Owned by tx_task_ on the work queue.
    k_fifo tx_queue_{};
    std::optional<WorkQueueTask<SmpCanTransport>> tx_task_;
    SmpPacket tx_packet_;
    SmpCanFramer tx_framer_;
    SmpCanPacer pacer_;
    uint32_t pacer_frame_time_ns_ = 0;

    std::atomic<uint32_t> rx_dropped_ = 0;
    std::atomic<uint32_t> tx_dropped_ = 0;

    static int Output(net_buf* packet);
    static uint16_t GetMtu(const net_buf* packet);

    bool Enqueue(uint8_t target, SmpPacket packet);
    WorkQueueTaskResult ProcessTx();
    void DropTx();

    void OnFrame(const CanFrame& frame);
    RxSlot* FindRxSlot(uint8_t source);
    RxSlot* AcquireRxSlot(uint8_t source, SmpCanFrameFormat format);
    void ReleaseRxSlot(RxSlot& slot);
    void ReleaseExpiredRxSlots(int64_t now_ms);
    void Deliver(uint8_t source, SmpPacket packet);

    void UnbindLocked();

public:
    /// Retry delay when the controller's TX queue is full.
    static constexpr int64_t TX_RETRY_DELAY_US = 1000;

    /**
     * @param work_queue_thread Runs the paced sending; the CDMP work queue in the application.
     * @throws std::logic_error if another instance exists.
     */
    explicit SmpCanTransport(std::shared_ptr<WorkQueueThread> work_queue_thread);
    ~SmpCanTransport() override;

    SmpCanTransport(const SmpCanTransport&) = delete;
    SmpCanTransport& operator=(const SmpCanTransport&) = delete;
    SmpCanTransport(SmpCanTransport&&) = delete;
    SmpCanTransport& operator=(SmpCanTransport&&) = delete;

    /** @brief Registers with MCUmgr; needs a running work queue. */
    bool Initialize();

    /**
     * @brief Unbinds and applies new settings from the next Bind().
     * @param id_base 29-bit base, see SmpCanIdLayout.
     * @param bus_share_percent Cap on the bus load of the sent frames, 1-100.
     * @throws std::invalid_argument for an invalid base or share.
     */
    void Configure(std::shared_ptr<CanbusProxy> canbus, uint32_t id_base, uint8_t bus_share_percent);

    /**
     * @brief Accepts frames addressed to @p address and sends with it as the source.
     *
     * Rebinding to another address unbinds first.
     *
     * @return false for an invalid address, before Initialize(), without a bus, or when the CAN
     *         filter is rejected.
     */
    bool Bind(uint8_t address);
    /** @brief Stops receiving and drops packets in progress, queued requests and queued sends. */
    void Unbind();
    /** @brief The bound address, 0 while unbound. */
    [[nodiscard]] uint8_t GetAddress() const { return address_; }

    /** @brief Sets where responses to forwarded requests go; nullptr drops them. */
    void SetResponseSink(std::shared_ptr<ISmpResponseSink> response_sink);

    /** @brief Queues a packet for @p target; fails while unbound or for an invalid or own address. */
    bool Forward(uint8_t target, SmpPacket packet) override;

    /** @brief Received packets dropped: gaps, timeouts, malformed frames, no free slot or buffer. */
    [[nodiscard]] uint32_t GetRxDroppedCount() const { return rx_dropped_; }
    /** @brief Packets that could not be queued or were dropped before being fully sent. */
    [[nodiscard]] uint32_t GetTxDroppedCount() const { return tx_dropped_; }
};

} // namespace eerie_leap::subsys::smp::can
