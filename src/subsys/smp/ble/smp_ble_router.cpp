#include <algorithm>
#include <cerrno>
#include <stdexcept>

#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <mgmt/mcumgr/transport/smp_internal.h>
#include <zcbor_encode.h>

#include "smp_ble_router.h"

LOG_MODULE_REGISTER(smp_ble_router, LOG_LEVEL_INF);

namespace eerie_leap::subsys::smp::ble {

namespace {

// MCUmgr copies it from a request to its response.
struct SmpBleUserData {
    uint8_t route;
    uint8_t generation;
};

static_assert(sizeof(SmpBleUserData) <= CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE);

SmpBleUserData& UserData(net_buf& packet) {
    return *static_cast<SmpBleUserData*>(net_buf_user_data(&packet));
}

constexpr size_t MAX_PACKET_SIZE = CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;

} // namespace

std::atomic<SmpBleRouter*> SmpBleRouter::instance_ = nullptr;

SmpBleRouter::SmpBleRouter(std::shared_ptr<IBleNotifier> notifier, std::shared_ptr<ISmpForwarder> forwarder)
    : notifier_(std::move(notifier)), forwarder_(std::move(forwarder)) {

    SmpBleRouter* expected = nullptr;
    if(!instance_.compare_exchange_strong(expected, this))
        throw std::logic_error("Only one SmpBleRouter may exist");

    k_fifo_init(&tx_queue_);
    k_work_init_delayable(&tx_work_, TxWorkHandler);
}

SmpBleRouter::~SmpBleRouter() {
    k_work_sync sync;

    // Once MCUmgr is done with our requests, it no longer calls Output().
    if(is_initialized_) {
        k_work_cancel_sync(&smp_transport_.work, &sync);
        smp_rx_clear(&smp_transport_);
    }

    k_work_cancel_delayable_sync(&tx_work_, &sync);
    instance_.store(nullptr);

    DropTx();
    ResetRx();
}

bool SmpBleRouter::Initialize() {
    if(is_initialized_)
        return true;

    smp_transport_.functions.output = Output;
    smp_transport_.functions.get_mtu = GetMtu;

    if(smp_transport_init(&smp_transport_) != 0) {
        LOG_ERR("Failed to initialize the SMP transport.");
        return false;
    }

    is_initialized_ = true;

    return true;
}

void SmpBleRouter::OnReceive(std::span<const uint8_t> data) {
    const int64_t now_ms = k_uptime_get();
    if(rx_head_size_ > 0 && now_ms - rx_last_ms_ > CONFIG_EERIE_LEAP_SMP_BLE_RX_TIMEOUT_MS) {
        rx_dropped_++;
        ResetRx();
    }
    rx_last_ms_ = now_ms;

    // A write may end one packet and start the next.
    while(!data.empty()) {
        if(rx_head_size_ < rx_head_.size()) {
            const size_t size = std::min(rx_head_.size() - rx_head_size_, data.size());
            std::copy_n(data.begin(), size, rx_head_.begin() + rx_head_size_);
            rx_head_size_ += size;
            data = data.subspan(size);

            if(rx_head_size_ < rx_head_.size())
                return;

            OnHeader();
        } else {
            const size_t size = std::min(rx_remaining_, data.size());
            if(!rx_discarding_)
                net_buf_add_mem(rx_packet_.get(), data.data(), size);

            rx_remaining_ -= size;
            data = data.subspan(size);
        }

        if(rx_remaining_ == 0) {
            const uint8_t route = rx_head_[0];
            const bool is_discarded = rx_discarding_;
            SmpPacket packet = std::move(rx_packet_);
            ResetRx();

            if(!is_discarded)
                Dispatch(route, std::move(packet));
        }
    }
}

void SmpBleRouter::OnHeader() {
    const auto header = SmpHeader::Parse(std::span(rx_head_).subspan(1));
    rx_remaining_ = header->length;
    rx_discarding_ = true;

    if(!header->IsRequest() || header->GetPacketSize() > MAX_PACKET_SIZE) {
        rx_dropped_++;
        return;
    }

    rx_packet_ = AllocateSmpPacket();
    if(rx_packet_ == nullptr) {
        rx_dropped_++;
        return;
    }

    net_buf_add_mem(rx_packet_.get(), rx_head_.data() + 1, SmpHeader::SIZE);
    rx_discarding_ = false;
}

void SmpBleRouter::ResetRx() {
    rx_head_size_ = 0;
    rx_packet_.reset();
    rx_remaining_ = 0;
    rx_discarding_ = false;
}

void SmpBleRouter::Dispatch(uint8_t route, SmpPacket packet) {
    const uint8_t address = forwarder_ != nullptr ? forwarder_->GetAddress() : 0;

    if(route == LOCAL_ROUTE || route == address) {
        if(!is_initialized_) {
            rx_dropped_++;
            return;
        }

        UserData(*packet) = {.route = route, .generation = generation_.load()};
        smp_rx_req(&smp_transport_, packet.release());
        return;
    }

    if(forwarder_ != nullptr && forwarder_->IsReachable(route)) {
        if(!forwarder_->Forward(route, std::move(packet)))
            rx_dropped_++;

        return;
    }

    Reject(route, std::move(packet));
}

void SmpBleRouter::Reject(uint8_t route, SmpPacket packet) {
    SmpHeader header = *SmpHeader::Parse({packet->data, packet->len});
    header.operation = header.operation == SmpOperation::READ
        ? SmpOperation::READ_RESPONSE
        : SmpOperation::WRITE_RESPONSE;
    header.flags = 0;

    // The request is no longer needed, so its buffer carries the answer.
    net_buf_reset(packet.get());
    uint8_t* body = packet->data + SmpHeader::SIZE;

    zcbor_state_t zse[2];
    zcbor_new_encode_state(zse, ZCBOR_ARRAY_SIZE(zse), body, net_buf_tailroom(packet.get()) - SmpHeader::SIZE, 1);

    const bool ok = zcbor_map_start_encode(zse, 1)
        && zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, MGMT_ERR_ENOENT)
        && zcbor_map_end_encode(zse, 1);

    if(!ok) {
        rx_dropped_++;
        return;
    }

    header.length = static_cast<uint16_t>(zse->payload - body);
    header.Encode(std::span<uint8_t, SmpHeader::SIZE>(packet->data, SmpHeader::SIZE));
    net_buf_add(packet.get(), header.GetPacketSize());

    EnqueueTx(route, generation_.load(), std::move(packet));
}

void SmpBleRouter::OnDisconnected() {
    ResetRx();
    generation_++;

    if(is_initialized_)
        smp_rx_clear(&smp_transport_);

    k_work_reschedule(&tx_work_, K_NO_WAIT);
}

void SmpBleRouter::OnNotificationSent() {
    k_work_reschedule(&tx_work_, K_NO_WAIT);
}

void SmpBleRouter::OnResponse(uint8_t source, SmpPacket packet) {
    if(packet == nullptr || packet->len == 0)
        return;

    EnqueueTx(source, generation_.load(), std::move(packet));
}

int SmpBleRouter::Output(net_buf* packet) {
    SmpPacket owned(packet);

    SmpBleRouter* router = instance_.load();
    if(router == nullptr)
        return -ENODEV;

    const SmpBleUserData user_data = UserData(*owned);
    router->EnqueueTx(user_data.route, user_data.generation, std::move(owned));

    return 0;
}

uint16_t SmpBleRouter::GetMtu(const net_buf* /*packet*/) {
    return MAX_PACKET_SIZE;
}

void SmpBleRouter::EnqueueTx(uint8_t route, uint8_t generation, SmpPacket packet) {
    UserData(*packet) = {.route = route, .generation = generation};
    k_fifo_put(&tx_queue_, packet.release());

    // Keeps a pending retry delay; OnNotificationSent() cuts it short.
    k_work_schedule(&tx_work_, K_NO_WAIT);
}

void SmpBleRouter::TxWorkHandler(k_work* /*work*/) {
    SmpBleRouter* router = instance_.load();
    if(router != nullptr)
        router->ProcessTx();
}

void SmpBleRouter::ProcessTx() {
    while(true) {
        if(tx_packet_ == nullptr) {
            tx_packet_ = SmpPacket(static_cast<net_buf*>(k_fifo_get(&tx_queue_, K_NO_WAIT)));
            if(tx_packet_ == nullptr)
                return;

            tx_offset_ = 0;
        }

        const SmpBleUserData user_data = UserData(*tx_packet_);
        const size_t max_size = notifier_->GetMaxNotificationSize();

        if(user_data.generation != generation_.load() || max_size == 0) {
            tx_packet_.reset();
            tx_dropped_++;
            continue;
        }

        // tx_offset_ counts the route byte, which only the first fragment carries.
        const size_t total = 1 + tx_packet_->len;
        std::span<const uint8_t> fragment;

        if(tx_offset_ == 0) {
            const size_t size = std::min({max_size, tx_first_fragment_.size(), total});
            tx_first_fragment_[0] = user_data.route;
            std::copy_n(tx_packet_->data, size - 1, tx_first_fragment_.begin() + 1);
            fragment = std::span(tx_first_fragment_.data(), size);
        } else {
            fragment = std::span(tx_packet_->data + tx_offset_ - 1, std::min(max_size, total - tx_offset_));
        }

        const int result = notifier_->Notify(fragment);
        if(result == -ENOMEM) {
            k_work_schedule(&tx_work_, K_MSEC(TX_RETRY_DELAY_MS));
            return;
        }

        if(result != 0) {
            LOG_WRN("Dropping an SMP response, notification failed: %d", result);
            tx_packet_.reset();
            tx_dropped_++;
            continue;
        }

        tx_offset_ += fragment.size();
        if(tx_offset_ == total)
            tx_packet_.reset();
    }
}

void SmpBleRouter::DropTx() {
    tx_packet_.reset();

    while(auto* packet = static_cast<net_buf*>(k_fifo_get(&tx_queue_, K_NO_WAIT)))
        smp_packet_free(packet);
}

std::array<uint8_t, SmpBleRouter::INFO_SIZE> SmpBleRouter::GetInfo() {
    return {
        PROTOCOL_VERSION,
        static_cast<uint8_t>(MAX_PACKET_SIZE & 0xFF),
        static_cast<uint8_t>(MAX_PACKET_SIZE >> 8),
        CONFIG_EERIE_LEAP_SMP_BLE_REQUESTS_IN_FLIGHT,
    };
}

} // namespace eerie_leap::subsys::smp::ble
