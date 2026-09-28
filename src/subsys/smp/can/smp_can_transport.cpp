#include <algorithm>
#include <cerrno>
#include <span>
#include <stdexcept>

#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <mgmt/mcumgr/transport/smp_internal.h>

#include "subsys/canbus/can_id.h"
#include "subsys/canbus/can_filter.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/smp/smp_header.h"

#include "smp_can_transport.h"

LOG_MODULE_REGISTER(smp_can_transport, LOG_LEVEL_INF);

namespace eerie_leap::subsys::smp::can {

using eerie_leap::subsys::canbus::CanId;
using eerie_leap::subsys::canbus::CanFilter;
using eerie_leap::subsys::canbus::CanbusConfig;
using eerie_leap::subsys::canbus::CanbusType;
using eerie_leap::subsys::threading::ScopedMutex;

namespace {

// The peer is the source of a received packet and the target of a sent one.
// MCUmgr copies it from a request to its response.
struct SmpCanUserData {
    uint8_t peer;
};

static_assert(sizeof(SmpCanUserData) <= CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE);

SmpCanUserData& UserData(net_buf& packet) {
    return *static_cast<SmpCanUserData*>(net_buf_user_data(&packet));
}

int64_t UptimeUs() {
    return static_cast<int64_t>(k_ticks_to_us_floor64(k_uptime_ticks()));
}

} // namespace

std::atomic<SmpCanTransport*> SmpCanTransport::instance_ = nullptr;

SmpCanTransport::SmpCanTransport(std::shared_ptr<WorkQueueThread> work_queue_thread)
    : work_queue_thread_(std::move(work_queue_thread)) {

    SmpCanTransport* expected = nullptr;
    if(!instance_.compare_exchange_strong(expected, this))
        throw std::logic_error("Only one SmpCanTransport may exist");

    k_mutex_init(&bind_lock_);
    k_fifo_init(&tx_queue_);
}

SmpCanTransport::~SmpCanTransport() {
    Unbind();

    if(tx_task_.has_value()) {
        k_work_sync sync;
        k_work_cancel_sync(&smp_transport_.work, &sync);
    }

    instance_.store(nullptr);

    if(tx_task_.has_value())
        tx_task_->Cancel();

    DropTx();
}

bool SmpCanTransport::Initialize() {
    if(tx_task_.has_value())
        return true;

    smp_transport_.functions.output = Output;
    smp_transport_.functions.get_mtu = GetMtu;

    if(smp_transport_init(&smp_transport_) != 0) {
        LOG_ERR("Failed to initialize the SMP transport.");
        return false;
    }

    tx_task_ = work_queue_thread_->CreateTask(
        [](SmpCanTransport* transport) { return transport->ProcessTx(); }, this);

    return true;
}

void SmpCanTransport::Configure(std::shared_ptr<CanbusProxy> canbus, uint32_t id_base, uint8_t bus_share_percent) {
    if(bus_share_percent == 0 || bus_share_percent > 100)
        throw std::invalid_argument("SMP bus share must be 1-100 %");

    ScopedMutex guard(bind_lock_);

    UnbindLocked();

    canbus_ = std::move(canbus);
    layout_ = SmpCanIdLayout(id_base);
    bus_share_percent_ = bus_share_percent;
    pacer_frame_time_ns_ = 0;
}

bool SmpCanTransport::Bind(uint8_t address) {
    if(!SmpCanIdLayout::IsValidAddress(address) || !tx_task_.has_value())
        return false;

    ScopedMutex guard(bind_lock_);

    if(address_ == address)
        return true;

    UnbindLocked();

    auto* canbus = canbus_ != nullptr ? canbus_->Get() : nullptr;
    if(canbus == nullptr) {
        LOG_ERR("Cannot bind SMP over CAN without a CAN bus.");
        return false;
    }

    const CanFilter filter(layout_.GetTargetFilterId(address), SmpCanIdLayout::TARGET_FILTER_MASK, true);
    const int handler_id = canbus->RegisterFrameReceivedHandler(
        filter, [this](const CanFrame& frame) { OnFrame(frame); });

    if(handler_id < 0) {
        LOG_ERR("Failed to register the SMP over CAN filter: %d", handler_id);
        return false;
    }

    handler_id_ = handler_id;
    bound_canbus_ = canbus;
    address_.store(address);

    LOG_INF("SMP over CAN bound to address %u", address);

    return true;
}

void SmpCanTransport::Unbind() {
    ScopedMutex guard(bind_lock_);

    UnbindLocked();
}

void SmpCanTransport::UnbindLocked() {
    if(handler_id_ < 0)
        return;

    address_.store(0);

    // Once removed, the RX thread is out of the handler and the slots are ours.
    // A reconfigured bus is a new instance, and the old handler went with the old one.
    if(canbus_ != nullptr && canbus_->Get() == bound_canbus_)
        bound_canbus_->RemoveFrameReceivedHandler(handler_id_);
    handler_id_ = -1;
    bound_canbus_ = nullptr;

    for(auto& slot : rx_slots_)
        ReleaseRxSlot(slot);

    smp_rx_clear(&smp_transport_);

    // Waits for a TX run that still saw the old address, then lets the TX task drop the queue itself.
    tx_task_->Cancel();
    tx_task_->Schedule();

    LOG_INF("SMP over CAN unbound");
}

void SmpCanTransport::SetResponseSink(std::shared_ptr<ISmpResponseSink> response_sink) {
    k_spinlock_key_t key = k_spin_lock(&sink_lock_);
    response_sink_.swap(response_sink);
    k_spin_unlock(&sink_lock_, key);
}

bool SmpCanTransport::Forward(uint8_t target, SmpPacket packet) {
    return Enqueue(target, std::move(packet));
}

int SmpCanTransport::Output(net_buf* packet) {
    SmpPacket owned(packet);

    SmpCanTransport* transport = instance_.load();
    if(transport == nullptr)
        return -ENODEV;

    const uint8_t target = UserData(*owned).peer;

    return transport->Enqueue(target, std::move(owned)) ? 0 : -ENOTCONN;
}

uint16_t SmpCanTransport::GetMtu(const net_buf* /*packet*/) {
    return CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
}

bool SmpCanTransport::Enqueue(uint8_t target, SmpPacket packet) {
    const uint8_t address = address_.load();

    if(packet == nullptr || packet->len == 0 || address == 0
        || !SmpCanIdLayout::IsValidAddress(target) || target == address) {

        tx_dropped_++;
        return false;
    }

    UserData(*packet).peer = target;
    k_fifo_put(&tx_queue_, packet.release());
    tx_task_->Schedule();

    return true;
}

WorkQueueTaskResult SmpCanTransport::ProcessTx() {
    const uint8_t address = address_.load();
    auto* canbus = address != 0 && canbus_ != nullptr ? canbus_->Get() : nullptr;

    // Canbus falls back to classic CAN on a controller without FD, so its type is authoritative.
    const CanbusConfig config = canbus != nullptr ? canbus->GetConfig() : CanbusConfig(nullptr, CanbusType::NONE, 0);
    const SmpCanFrameFormat format(config.type == CanbusType::CANFD);

    // The bitrate is 0 while the controller is still detecting it.
    const uint32_t frame_time_ns = format.GetFrameTimeNs(config.bitrate, config.data_bitrate);
    if(frame_time_ns == 0) {
        DropTx();
        return {};
    }

    const int64_t now_us = UptimeUs();
    if(frame_time_ns != pacer_frame_time_ns_) {
        pacer_ = SmpCanPacer(frame_time_ns, bus_share_percent_, CONFIG_EERIE_LEAP_SMP_CAN_TX_BURST_FRAMES);
        pacer_.Reset(now_us);
        pacer_frame_time_ns_ = frame_time_ns;
    }
    pacer_.Update(now_us);

    while(true) {
        if(tx_packet_ == nullptr) {
            tx_packet_ = SmpPacket(static_cast<net_buf*>(k_fifo_get(&tx_queue_, K_NO_WAIT)));
            if(tx_packet_ == nullptr)
                return {};

            tx_framer_ = SmpCanFramer({tx_packet_->data, tx_packet_->len}, format);
        }

        // Waiting for half a burst keeps the wake-ups to a few hundred per second.
        if(!pacer_.CanSend()) {
            const uint32_t frames = std::max(CONFIG_EERIE_LEAP_SMP_CAN_TX_BURST_FRAMES / 2, 1);
            return {.reschedule = true, .delay = K_USEC(pacer_.GetWaitUs(frames))};
        }

        std::array<uint8_t, SmpCanFrameFormat::MAX_FRAME_SIZE> frame{};
        const size_t frame_size = tx_framer_.Encode(frame);
        const CanId frame_id = CanId::Extended(layout_.MakeId(UserData(*tx_packet_).peer, address));

        const int result = canbus->SendFrame(frame_id, std::span(frame.data(), frame_size), K_NO_WAIT);
        if(result == -EAGAIN)
            return {.reschedule = true, .delay = K_USEC(TX_RETRY_DELAY_US)};

        if(result != 0) {
            LOG_WRN("Dropping an SMP packet, CAN send failed: %d", result);
            tx_packet_.reset();
            tx_dropped_++;
            continue;
        }

        pacer_.OnSent();
        tx_framer_.Advance();
        if(tx_framer_.IsDone())
            tx_packet_.reset();
    }
}

void SmpCanTransport::DropTx() {
    if(tx_packet_ != nullptr) {
        tx_packet_.reset();
        tx_dropped_++;
    }

    while(auto* packet = static_cast<net_buf*>(k_fifo_get(&tx_queue_, K_NO_WAIT))) {
        smp_packet_free(packet);
        tx_dropped_++;
    }
}

void SmpCanTransport::OnFrame(const CanFrame& frame) {
    if(!frame.is_extended || frame.is_remote_request)
        return;

    const uint8_t source = SmpCanIdLayout::GetSource(frame.id);
    if(!SmpCanIdLayout::IsValidAddress(source))
        return;

    const int64_t now_ms = k_uptime_get();
    ReleaseExpiredRxSlots(now_ms);

    // A packet is read in the format of its start frame.
    const SmpCanFrameFormat format(frame.is_can_fd);

    RxSlot* slot = FindRxSlot(source);
    if(slot != nullptr && slot->reassembler.GetFormat() != format) {
        rx_dropped_++;
        ReleaseRxSlot(*slot);
        slot = nullptr;
    }

    if(slot == nullptr) {
        if(!SmpCanReassembler::IsStartFrame(frame.data))
            return;

        slot = AcquireRxSlot(source, format);
        if(slot == nullptr) {
            rx_dropped_++;
            return;
        }
    }

    switch(slot->reassembler.Accept(frame.data, now_ms)) {
        case SmpCanReassembler::Result::IN_PROGRESS:
            return;

        case SmpCanReassembler::Result::COMPLETE: {
            net_buf_add(slot->packet.get(), slot->reassembler.GetLength());
            SmpPacket packet = std::move(slot->packet);
            ReleaseRxSlot(*slot);
            Deliver(source, std::move(packet));
            return;
        }

        case SmpCanReassembler::Result::DROPPED:
            rx_dropped_++;
            ReleaseRxSlot(*slot);
            return;

        case SmpCanReassembler::Result::IGNORED:
            ReleaseRxSlot(*slot);
            return;
    }
}

SmpCanTransport::RxSlot* SmpCanTransport::FindRxSlot(uint8_t source) {
    auto it = std::ranges::find(rx_slots_, source, &RxSlot::source);

    return it != rx_slots_.end() ? &*it : nullptr;
}

SmpCanTransport::RxSlot* SmpCanTransport::AcquireRxSlot(uint8_t source, SmpCanFrameFormat format) {
    RxSlot* slot = FindRxSlot(0);
    if(slot == nullptr)
        return nullptr;

    slot->packet = AllocateSmpPacket();
    if(slot->packet == nullptr)
        return nullptr;

    slot->source = source;
    slot->reassembler.Attach({slot->packet->data, net_buf_tailroom(slot->packet.get())}, format);

    return slot;
}

void SmpCanTransport::ReleaseRxSlot(RxSlot& slot) {
    slot.source = 0;
    slot.packet.reset();
    slot.reassembler.Reset();
}

void SmpCanTransport::ReleaseExpiredRxSlots(int64_t now_ms) {
    for(auto& slot : rx_slots_) {
        if(slot.source != 0 && slot.reassembler.IsExpired(now_ms)) {
            rx_dropped_++;
            ReleaseRxSlot(slot);
        }
    }
}

void SmpCanTransport::Deliver(uint8_t source, SmpPacket packet) {
    const auto header = SmpHeader::Parse({packet->data, packet->len});

    if(header->IsRequest()) {
        UserData(*packet).peer = source;
        smp_rx_req(&smp_transport_, packet.release());
        return;
    }

    if(header->IsResponse()) {
        k_spinlock_key_t key = k_spin_lock(&sink_lock_);
        auto response_sink = response_sink_;
        k_spin_unlock(&sink_lock_, key);

        if(response_sink != nullptr) {
            response_sink->OnResponse(source, std::move(packet));
            return;
        }
    }

    rx_dropped_++;
}

} // namespace eerie_leap::subsys::smp::can
