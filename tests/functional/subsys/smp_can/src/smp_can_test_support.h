#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/ztest.h>

#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/smp/smp_header.h"
#include "subsys/smp/smp_packet.h"
#include "subsys/smp/can/smp_can_framer.h"
#include "subsys/smp/can/smp_can_id_layout.h"
#include "subsys/smp/can/smp_can_reassembler.h"
#include "subsys/smp/can/smp_can_transport.h"

namespace smp_can_test {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::smp;
using namespace eerie_leap::subsys::smp::can;
using eerie_leap::subsys::threading::WorkQueueThread;

using Bytes = std::vector<uint8_t>;

inline constexpr uint8_t OWN_ADDRESS = 0x10;
// The harness answers for four peers, 0x20-0x23.
inline constexpr uint8_t FIRST_PEER = 0x20;
inline constexpr size_t PEER_COUNT = 4;
inline constexpr uint32_t BITRATE = 500000;
inline constexpr uint8_t BUS_SHARE_PERCENT = 25;
inline constexpr int RESPONSE_TIMEOUT_MS = 1000;

inline const device* LoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// A failing assertion skips the harness teardown, so the controller is stopped separately.
inline void ResetLoopbackDevice() {
    can_stop(LoopbackDevice());
}

inline Bytes MakePacket(SmpOperation operation, uint16_t group, uint8_t command_id, uint8_t sequence, const Bytes& body) {
    const SmpHeader header{
        .operation = operation,
        .version = 1,
        .length = static_cast<uint16_t>(body.size()),
        .group = group,
        .sequence = sequence,
        .command_id = command_id,
    };

    Bytes packet(SmpHeader::SIZE);
    header.Encode(std::span<uint8_t, SmpHeader::SIZE>(packet.data(), SmpHeader::SIZE));
    packet.insert(packet.end(), body.begin(), body.end());

    return packet;
}

inline Bytes CborText(std::string_view text) {
    Bytes bytes;
    if(text.size() < 24) {
        bytes.push_back(static_cast<uint8_t>(0x60 | text.size()));
    } else {
        bytes.push_back(0x78);
        bytes.push_back(static_cast<uint8_t>(text.size()));
    }
    bytes.insert(bytes.end(), text.begin(), text.end());

    return bytes;
}

// os group (0), echo (0): {"d": text}
inline Bytes MakeEchoRequest(std::string_view text, uint8_t sequence) {
    Bytes body = {0xA1, 0x61, 'd'};
    auto value = CborText(text);
    body.insert(body.end(), value.begin(), value.end());

    return MakePacket(SmpOperation::WRITE, 0, 0, sequence, body);
}

inline bool Contains(const Bytes& data, const Bytes& part) {
    return std::search(data.begin(), data.end(), part.begin(), part.end()) != data.end();
}

inline bool IsEchoResponse(const std::optional<Bytes>& packet, std::string_view text, uint8_t sequence) {
    if(!packet.has_value())
        return false;

    auto header = SmpHeader::Parse(*packet);
    if(!header.has_value() || header->operation != SmpOperation::WRITE_RESPONSE
        || header->group != 0 || header->command_id != 0 || header->sequence != sequence) {

        return false;
    }

    Bytes expected = {0x61, 'r'};
    auto value = CborText(text);
    expected.insert(expected.end(), value.begin(), value.end());

    return Contains(*packet, expected);
}

inline std::vector<Bytes> MakeFrames(std::span<const uint8_t> packet, SmpCanFrameFormat format = {}) {
    std::vector<Bytes> frames;
    SmpCanFramer framer(packet, format);

    while(!framer.IsDone()) {
        std::array<uint8_t, SmpCanFrameFormat::MAX_FRAME_SIZE> frame{};
        const size_t size = framer.Encode(frame);
        frames.emplace_back(frame.begin(), frame.begin() + size);
        framer.Advance();
    }

    return frames;
}

inline SmpPacket ToSmpPacket(const Bytes& bytes) {
    SmpPacket packet = AllocateSmpPacket();
    zassert_not_null(packet.get(), "SMP pool exhausted");
    net_buf_add_mem(packet.get(), bytes.data(), bytes.size());

    return packet;
}

inline std::string LongText(size_t size) {
    std::string text(size, ' ');
    for(size_t i = 0; i < size; i++)
        text[i] = static_cast<char>('a' + i % 26);

    return text;
}

class RecordingSink : public ISmpResponseSink {
private:
    k_sem received_{};
    k_mutex lock_{};
    std::vector<std::pair<uint8_t, Bytes>> responses_;

public:
    RecordingSink() {
        k_sem_init(&received_, 0, K_SEM_MAX_LIMIT);
        k_mutex_init(&lock_);
    }

    void OnResponse(uint8_t source, SmpPacket packet) override {
        k_mutex_lock(&lock_, K_FOREVER);
        responses_.emplace_back(source, Bytes(packet->data, packet->data + packet->len));
        k_mutex_unlock(&lock_);

        k_sem_give(&received_);
    }

    std::optional<std::pair<uint8_t, Bytes>> Wait(int timeout_ms = RESPONSE_TIMEOUT_MS) {
        if(k_sem_take(&received_, K_MSEC(timeout_ms)) != 0)
            return std::nullopt;

        k_mutex_lock(&lock_, K_FOREVER);
        auto response = responses_.front();
        responses_.erase(responses_.begin());
        k_mutex_unlock(&lock_);

        return response;
    }
};

// Stands in for the other units: reassembles what the transport sends to 0x20-0x23.
class Peers {
private:
    struct Peer {
        std::array<uint8_t, CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE> buffer{};
        SmpCanReassembler reassembler;
        std::vector<Bytes> packets;
        k_sem received{};
    };

    std::array<Peer, PEER_COUNT> peers_;
    k_mutex lock_{};
    uint32_t frame_count_ = 0;
    uint32_t can_fd_frame_count_ = 0;
    uint32_t foreign_source_count_ = 0;
    uint8_t last_source_ = 0;
    int64_t last_frame_ms_ = 0;

public:
    Peers() {
        k_mutex_init(&lock_);
        for(auto& peer : peers_) {
            peer.reassembler.Attach(peer.buffer);
            k_sem_init(&peer.received, 0, K_SEM_MAX_LIMIT);
        }
    }

    void OnFrame(const CanFrame& frame) {
        const uint8_t target = SmpCanIdLayout::GetTarget(frame.id);
        Peer& peer = peers_.at(target - FIRST_PEER);

        k_mutex_lock(&lock_, K_FOREVER);

        frame_count_++;
        if(frame.is_can_fd)
            can_fd_frame_count_++;
        last_frame_ms_ = k_uptime_get();
        last_source_ = SmpCanIdLayout::GetSource(frame.id);
        if(last_source_ != OWN_ADDRESS)
            foreign_source_count_++;

        if(SmpCanReassembler::IsStartFrame(frame.data))
            peer.reassembler.Attach(peer.buffer, SmpCanFrameFormat(frame.is_can_fd));

        const bool is_complete =
            peer.reassembler.Accept(frame.data, last_frame_ms_) == SmpCanReassembler::Result::COMPLETE;
        if(is_complete)
            peer.packets.emplace_back(peer.buffer.begin(), peer.buffer.begin() + peer.reassembler.GetLength());

        k_mutex_unlock(&lock_);

        if(is_complete)
            k_sem_give(&peer.received);
    }

    std::optional<Bytes> WaitPacket(uint8_t address, int timeout_ms = RESPONSE_TIMEOUT_MS) {
        Peer& peer = peers_.at(address - FIRST_PEER);
        if(k_sem_take(&peer.received, K_MSEC(timeout_ms)) != 0)
            return std::nullopt;

        k_mutex_lock(&lock_, K_FOREVER);
        Bytes packet = std::move(peer.packets.front());
        peer.packets.erase(peer.packets.begin());
        k_mutex_unlock(&lock_);

        return packet;
    }

    uint32_t GetFrameCount() {
        k_mutex_lock(&lock_, K_FOREVER);
        const uint32_t count = frame_count_;
        k_mutex_unlock(&lock_);

        return count;
    }

    uint32_t GetCanFdFrameCount() {
        k_mutex_lock(&lock_, K_FOREVER);
        const uint32_t count = can_fd_frame_count_;
        k_mutex_unlock(&lock_);

        return count;
    }

    uint32_t GetForeignSourceCount() {
        k_mutex_lock(&lock_, K_FOREVER);
        const uint32_t count = foreign_source_count_;
        k_mutex_unlock(&lock_);

        return count;
    }

    int64_t GetLastFrameMs() {
        k_mutex_lock(&lock_, K_FOREVER);
        const int64_t last_frame_ms = last_frame_ms_;
        k_mutex_unlock(&lock_);

        return last_frame_ms;
    }

    uint8_t GetLastSource() {
        k_mutex_lock(&lock_, K_FOREVER);
        const uint8_t last_source = last_source_;
        k_mutex_unlock(&lock_);

        return last_source;
    }
};

class Harness {
private:
    static constexpr uint32_t PEER_FILTER_MASK = SmpCanIdLayout::TARGET_FILTER_MASK & ~(0x03U << 8);

    int peer_handler_id_ = -1;

public:
    SmpCanIdLayout layout;
    SmpCanFrameFormat format;
    std::shared_ptr<CanbusProxy> canbus;
    std::shared_ptr<WorkQueueThread> work_queue;
    std::unique_ptr<SmpCanTransport> transport;
    Peers peers;

    explicit Harness(CanbusType type = CanbusType::CLASSICAL_CAN, uint32_t data_bitrate = 0)
        : format(type == CanbusType::CANFD) {

        auto running_canbus = std::make_unique<Canbus>(
            CanbusConfig(LoopbackDevice(), type, BITRATE, data_bitrate, CAN_MODE_LOOPBACK));
        zassert_true(running_canbus->Initialize(), "Canbus::Initialize() failed");
        zassert_true(running_canbus->Start(), "Canbus::Start() failed");
        canbus = std::make_shared<CanbusProxy>(std::move(running_canbus));

        work_queue = std::make_shared<WorkQueueThread>("smp_can_test_wq", 4096, 5);
        zassert_true(work_queue->Initialize());

        transport = std::make_unique<SmpCanTransport>(work_queue);
        zassert_true(transport->Initialize());
        transport->Configure(canbus, layout.GetBase(), BUS_SHARE_PERCENT);
        zassert_true(transport->Bind(OWN_ADDRESS));

        peer_handler_id_ = (*canbus)->RegisterFrameReceivedHandler(
            CanFilter(layout.GetTargetFilterId(FIRST_PEER), PEER_FILTER_MASK, true),
            [this](const CanFrame& frame) { peers.OnFrame(frame); });
        zassert_true(peer_handler_id_ > 0, "Peer filter rejected: %d", peer_handler_id_);
    }

    ~Harness() {
        (*canbus)->RemoveFrameReceivedHandler(peer_handler_id_);
        transport.reset();
        work_queue.reset();
        canbus.reset();
    }

    void SendFrame(uint8_t source, uint8_t target, const Bytes& frame) {
        const int result = (*canbus)->SendFrame(
            CanId::Extended(layout.MakeId(target, source)), frame, K_MSEC(100));
        zassert_equal(result, 0, "SendFrame failed: %d", result);
    }

    void SendPacket(uint8_t source, const Bytes& packet, uint8_t target = OWN_ADDRESS) {
        for(const auto& frame : MakeFrames(packet, format))
            SendFrame(source, target, frame);
    }
};

} // namespace smp_can_test
