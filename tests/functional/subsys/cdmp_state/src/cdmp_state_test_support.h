#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/cdmp/models/cdmp_message.h"
#include "subsys/cdmp/services/cdmp_service.h"

namespace cdmp_state_test {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::cdmp::models;
using namespace eerie_leap::subsys::cdmp::services;
using namespace eerie_leap::subsys::cdmp::utilities;

using Bytes = std::vector<uint8_t>;

// The scripted peer answers discovery as device 1, so the unit under test claims device 2.
inline constexpr uint8_t PEER_ID = 1;
inline constexpr uint32_t PEER_UID = 0x22222222;
inline constexpr uint8_t OWN_ID = 2;
inline constexpr uint32_t OWN_UID = 0x11111111;

inline constexpr uint8_t TYPE_A = 0x10;
inline constexpr uint8_t TYPE_B = 0x11;

inline constexpr int FRAME_TIMEOUT_MS = 1000;
// Well past the time a frame takes through the loopback controller.
inline constexpr int NO_FRAME_TIMEOUT_MS = 100;
// Three discovery backoffs (800 ms) plus ten unanswered claims (500 ms), with room to spare.
inline constexpr int STARTUP_TIMEOUT_MS = 3000;

inline constexpr uint32_t MANAGEMENT = CdmpCanIdManager::MANAGEMENT_OFFSET;
inline constexpr uint32_t HEARTBEAT = CdmpCanIdManager::HEARTBEAT_OFFSET;
inline constexpr uint32_t COMMAND = CdmpCanIdManager::COMMAND_OFFSET;
inline constexpr uint32_t COMMAND_RESPONSE = CdmpCanIdManager::COMMAND_RESPONSE_OFFSET;
inline constexpr uint32_t STATE_CHANGE = CdmpCanIdManager::STATE_CHANGE_OFFSET;
inline constexpr uint32_t STATE_CHANGE_RESPONSE = CdmpCanIdManager::STATE_CHANGE_RESPONSE_OFFSET;

inline const device* LoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// A failing assertion skips the harness teardown, so the controller is stopped separately.
inline void ResetLoopbackDevice() {
    can_stop(LoopbackDevice());
}

inline Bytes NotificationFrame(uint8_t source, uint8_t version, uint8_t state_type, const Bytes& data) {
    const CdmpStateChangeMessage message{
        .source_device_id = source,
        .version = version,
        .state_type = state_type,
        .data = CdmpStateData(data)};

    CdmpStateChangeMessage::Frame frame{};
    const size_t size = message.ToCanFrame(frame);

    return {frame.begin(), frame.begin() + size};
}

inline Bytes AcknowledgementFrame(uint8_t responder, uint8_t source, uint8_t version) {
    const auto frame = CdmpStateChangeResponseMessage{
        .responding_device_id = responder,
        .source_device_id = source,
        .version = version,
        .result_code = CdmpResultCode::SUCCESS}.ToCanFrame();

    return {frame.begin(), frame.end()};
}

inline Bytes SyncRequestFrame(uint8_t target, uint8_t transaction_id) {
    return CdmpCommandRequestMessage{
        .target_device_id = target,
        .command_code = std::to_underlying(CdmpServiceCommandCode::STATE_SYNC_REQUEST),
        .transaction_id = transaction_id,
        .data = {0, 0, 0, 0, 0}}.ToCanFrame();
}

struct Record {
    uint8_t state_type;
    uint8_t version;
    Bytes data;
};

// CRC-8 over the records in ascending type order, each as type, version, length and data.
inline uint8_t Digest(std::initializer_list<Record> records) {
    Bytes bytes;
    for(const auto& record : records) {
        bytes.push_back(record.state_type);
        bytes.push_back(record.version);
        bytes.push_back(static_cast<uint8_t>(record.data.size()));
        bytes.insert(bytes.end(), record.data.begin(), record.data.end());
    }

    return crc8_ccitt(0, bytes.data(), bytes.size());
}

struct RecordedFrame {
    uint32_t sequence; // Order of arrival across all CDMP identifiers.
    int64_t received_ms;
    Bytes data;
};

// Every CDMP frame on the bus, the unit's and the peer's, one queue per offset from the base.
class BusRecorder {
private:
    struct Channel {
        k_sem received{};
        std::deque<RecordedFrame> frames;
    };

    k_mutex lock_{};
    uint32_t next_sequence_ = 0;
    std::array<Channel, STATE_CHANGE_RESPONSE + 1> channels_;

public:
    using Match = std::function<bool(const Bytes& frame)>;

    BusRecorder() {
        k_mutex_init(&lock_);
        for(auto& channel : channels_)
            k_sem_init(&channel.received, 0, K_SEM_MAX_LIMIT);
    }

    void Add(uint32_t offset, const CanFrame& frame) {
        Channel& channel = channels_.at(offset);

        k_mutex_lock(&lock_, K_FOREVER);
        channel.frames.push_back({
            .sequence = next_sequence_++,
            .received_ms = k_uptime_get(),
            .data = Bytes(frame.data.begin(), frame.data.end())});
        k_mutex_unlock(&lock_);

        k_sem_give(&channel.received);
    }

    /** @brief The next frame on @p offset that @p match accepts; the frames before it are dropped. */
    std::optional<RecordedFrame> Wait(uint32_t offset, const Match& match, int timeout_ms = FRAME_TIMEOUT_MS) {
        Channel& channel = channels_.at(offset);
        const int64_t deadline = k_uptime_get() + timeout_ms;

        for(int64_t remaining = timeout_ms; remaining >= 0; remaining = deadline - k_uptime_get()) {
            if(k_sem_take(&channel.received, K_MSEC(remaining)) != 0)
                break;

            k_mutex_lock(&lock_, K_FOREVER);
            RecordedFrame frame = std::move(channel.frames.front());
            channel.frames.pop_front();
            k_mutex_unlock(&lock_);

            if(match(frame.data))
                return frame;
        }

        return std::nullopt;
    }

    void Clear() {
        for(uint32_t offset = 0; offset < channels_.size(); offset++) {
            while(k_sem_take(&channels_[offset].received, K_NO_WAIT) == 0) {
                k_mutex_lock(&lock_, K_FOREVER);
                channels_[offset].frames.pop_front();
                k_mutex_unlock(&lock_);
            }
        }
    }
};

inline BusRecorder::Match IsManagement(CdmpManagementMessageType type) {
    return [type](const Bytes& frame) { return !frame.empty() && frame[0] == std::to_underlying(type); };
}

// Notifications and acknowledgements carry their sender in byte 0.
inline BusRecorder::Match IsFrom(uint8_t device_id) {
    return [device_id](const Bytes& frame) { return !frame.empty() && frame[0] == device_id; };
}

inline BusRecorder::Match IsSyncRequestTo(uint8_t target) {
    return [target](const Bytes& frame) {
        return frame.size() >= 3
            && frame[0] == target
            && frame[1] == std::to_underlying(CdmpServiceCommandCode::STATE_SYNC_REQUEST);
    };
}

/** @brief A CdmpService on the loopback controller, and a peer scripted on the same bus. */
class Harness {
private:
    std::vector<int> recorder_handler_ids_;

public:
    CdmpCanIdManager ids;
    std::shared_ptr<CanbusProxy> canbus;
    std::shared_ptr<CdmpService> unit;
    BusRecorder bus;

    Harness() {
        auto running_canbus = std::make_unique<Canbus>(
            CanbusConfig(LoopbackDevice(), CanbusType::CLASSICAL_CAN, 500000, 0, CAN_MODE_LOOPBACK));
        zassert_true(running_canbus->Initialize(), "Canbus::Initialize() failed");
        zassert_true(running_canbus->Start(), "Canbus::Start() failed");
        canbus = std::make_shared<CanbusProxy>(std::move(running_canbus));

        for(uint32_t offset = MANAGEMENT; offset <= STATE_CHANGE_RESPONSE; offset++) {
            const int handler_id = (*canbus)->RegisterFrameReceivedHandler(
                CanId::Standard(ids.GetBaseCanId() + offset),
                [this, offset](const CanFrame& frame) { bus.Add(offset, frame); });
            zassert_true(handler_id > 0, "Frame handler rejected: %d", handler_id);
            recorder_handler_ids_.push_back(handler_id);
        }

        unit = std::make_shared<CdmpService>(CdmpDeviceType::DISPLAY, OWN_UID);
        zassert_true(unit->Initialize());
        unit->Configure(canbus);
    }

    ~Harness() {
        unit->Stop();
        unit.reset();

        for(int handler_id : recorder_handler_ids_)
            (*canbus)->RemoveFrameReceivedHandler(handler_id);
    }

    std::shared_ptr<CdmpStateService> State() const { return unit->GetStateService(); }

    void SendAsPeer(uint32_t offset, const Bytes& frame) {
        const int result = (*canbus)->SendFrame(CanId::Standard(ids.GetBaseCanId() + offset), frame, K_MSEC(100));
        zassert_equal(result, 0, "SendFrame failed: %d", result);
    }

    bool WaitForStatus(CdmpDeviceStatus status, int timeout_ms = STARTUP_TIMEOUT_MS) const {
        const int64_t deadline = k_uptime_get() + timeout_ms;

        while(unit->GetDevice()->GetStatus() != status) {
            if(k_uptime_get() >= deadline)
                return false;

            k_msleep(5);
        }

        return true;
    }

    /**
     * @brief Starts the unit with the peer answering its discovery and accepting its claim.
     *
     * The unit goes online as device 2 with the peer in its table, and broadcasts its sync request.
     */
    void BringOnline() {
        zassert_true(unit->Start());

        zassert_true(bus.Wait(MANAGEMENT, IsManagement(CdmpManagementMessageType::DISCOVERY_REQUEST)).has_value());
        SendAsPeer(MANAGEMENT, CdmpDiscoveryResponseMessage{
            .device_id = PEER_ID,
            .uid = PEER_UID,
            .device_type = CdmpDeviceType::LOGGER}.ToCanFrame());

        auto claim = bus.Wait(MANAGEMENT, IsManagement(CdmpManagementMessageType::ID_CLAIM), STARTUP_TIMEOUT_MS);
        zassert_true(claim.has_value(), "The unit claims an ID after discovery");
        zassert_equal(CdmpIdClaimRequestMessage::FromCanFrame(claim->data).claiming_device_id, OWN_ID);
        SendAsPeer(MANAGEMENT, CdmpIdClaimResponseMessage{
            .responding_device_id = PEER_ID,
            .claiming_device_id = OWN_ID,
            .claiming_device_uid = OWN_UID,
            .result = CdmpIdClaimResult::ACCEPT}.ToCanFrame());

        zassert_true(WaitForStatus(CdmpDeviceStatus::ONLINE));
        zassert_equal(unit->GetDevice()->GetDeviceId(), OWN_ID);
    }

    /**
     * @brief Lets the unit's start finish: the loopback controller hands it its own broadcast sync
     * request, which it answers like any other, after its stagger.
     */
    void Settle() {
        zassert_true(bus.Wait(COMMAND, IsSyncRequestTo(CdmpDevice::DEVICE_ID_BROADCAST)).has_value(),
            "Going online broadcasts STATE_SYNC_REQUEST");
        k_msleep(NO_FRAME_TIMEOUT_MS);
    }

    std::optional<CdmpStateChangeMessage> WaitForNotification(int timeout_ms = FRAME_TIMEOUT_MS) {
        auto frame = bus.Wait(STATE_CHANGE, IsFrom(OWN_ID), timeout_ms);
        if(!frame.has_value())
            return std::nullopt;

        return CdmpStateChangeMessage::FromCanFrame(frame->data);
    }
};

} // namespace cdmp_state_test
