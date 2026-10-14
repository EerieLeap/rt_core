#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"

#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"
#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "domain/canbus_com_domain/services/i_network_state_transport.h"
#include "domain/canbus_com_domain/states/canbus_com_state_data.h"
#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "domain/canbus_com_domain/states/i_canbus_com_state.h"

#include "cdmp_loopback_harness.h"

namespace canbus_com_test {

using namespace cdmp_loopback_test;
using namespace eerie_leap::domain::canbus_com_domain::states;

using eerie_leap::utilities::memory::Mrm;
using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::domain::canbus_domain::models::CanChannelConfiguration;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::domain::canbus_com_domain::services::INetworkStateTransport;

inline constexpr uint8_t COM_CHANNEL = 0;
inline constexpr const char* CDMP_WORK_QUEUE_NAME = "cdmp_work_queue";

/** @brief One classical channel at 500 kbit/s that carries CDMP, with the default CAN IDs. */
inline CanbusConfiguration ComConfiguration() {
    CanbusConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());

    CanChannelConfiguration channel(std::allocator_arg, Mrm::GetDefaultPmr());
    channel.type = CanbusType::CLASSICAL_CAN;
    channel.bus_channel = COM_CHANNEL;
    channel.bitrate = 500000;
    configuration.channel_configurations.emplace(COM_CHANNEL, std::move(channel));
    configuration.com_configuration.bus_channel = COM_CHANNEL;

    return configuration;
}

/**
 * @brief CanbusService opens the channel in normal mode, in which the loopback controller hands no
 * frame back, so the controller is switched to loopback mode under it.
 */
inline void SwitchToLoopbackMode() {
    const device* controller = LoopbackDevice();

    zassert_ok(can_stop(controller));
    zassert_ok(can_set_mode(controller, CAN_MODE_LOOPBACK));
    zassert_ok(can_start(controller));
}

/**
 * @brief A CanbusComService on the loopback controller, built as the CAN controller builds it from a
 * stored configuration, and a peer scripted on the same bus.
 */
class Harness : public CdmpLoopbackHarness {
protected:
    bool StartUnit() override { return com->Start(); }
    [[nodiscard]] CdmpDeviceStatus GetUnitStatus() const override { return com->GetNetworkInfo()->GetDeviceInfo().status; }
    [[nodiscard]] uint8_t GetUnitDeviceId() const override { return com->GetNetworkInfo()->GetDeviceInfo().device_id; }

public:
    std::shared_ptr<CanbusService> canbus_service;
    std::shared_ptr<CanbusComService> com;

    Harness() {
        DtFs::InitInternalFs();
        auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        zassert_true(fs_service->Format());

        auto configuration_manager = std::make_shared<CanbusConfigurationManager>(
            std::make_unique<CborConfigurationService<CborCanbusConfig>>("canbus_config", fs_service), nullptr);
        zassert_true(configuration_manager->Update(ComConfiguration()));

        canbus_service = std::make_shared<CanbusService>(
            [](uint8_t bus_channel) { return bus_channel == COM_CHANNEL ? LoopbackDevice() : nullptr; },
            configuration_manager);
        zassert_not_null(canbus_service->GetComCanbus().get(), "The COM channel is open");

        SwitchToLoopbackMode();
        AttachBus(canbus_service->GetComCanbus());

        com = std::make_shared<CanbusComService>(canbus_service);
        zassert_true(com->Initialize());
    }

    ~Harness() override {
        com.reset();
        canbus_service.reset();
    }

    /** @brief The COM service as the state bridge sees it. */
    INetworkStateTransport& Transport() { return *com; }

    [[nodiscard]] bool IsInDeviceTable(uint8_t device_id) const {
        std::array<CdmpDeviceInfo, 8> devices{};
        const size_t count = com->GetNetworkInfo()->GetNetworkDevices(devices);

        for(const auto& device : std::span(devices).first(count)) {
            if(device.device_id == device_id && device.status == CdmpDeviceStatus::ONLINE)
                return true;
        }

        return false;
    }

    /** @brief Adds another unit to the unit's device table: its ID claim, then its first heartbeat. */
    void Join(uint8_t device_id, uint32_t uid) {
        // Both reach the CDMP work queue in this order. A heartbeat without the claim would make the
        // unit rediscover the network, and on a loopback controller it would answer its own request.
        SendAsPeer(MANAGEMENT, CdmpIdClaimRequestMessage{
            .claiming_device_id = device_id,
            .uid = uid,
            .device_type = CdmpDeviceType::LOGGER,
            .protocol_version = CdmpDevice::GetCurrentProtocolVersion()}.ToCanFrame());
        SendAsPeer(HEARTBEAT, HeartbeatFrame(device_id));

        const int64_t deadline = k_uptime_get() + FRAME_TIMEOUT_MS;
        while(!IsInDeviceTable(device_id)) {
            zassert_true(k_uptime_get() < deadline, "Device %d joins on its first heartbeat", device_id);
            k_msleep(5);
        }
    }

    /** @brief Waits for the unit's acknowledgement of @p source's notification @p version. */
    bool WaitForAcknowledgement(uint8_t source, uint8_t version) {
        return bus.Wait(STATE_CHANGE_RESPONSE, [source, version](const Bytes& frame) {
            return frame.size() >= 3 && frame[0] == OWN_ID && frame[1] == source && frame[2] == version;
        }).has_value();
    }
};

/** @brief A state handler that keeps the records it read on each call, and where it ran. */
class HandlerProbe {
private:
    k_sem called_{};
    k_mutex lock_{};
    std::deque<std::vector<RemoteStateRecord>> snapshots_;
    INetworkStateTransport* transport_ = nullptr;
    CanbusComStateType state_type_{};
    int handler_id_ = -1;
    bool is_off_work_queue_ = false;
    bool is_other_type_ = false;

    void OnStateChanged(CanbusComStateType state_type) {
        const char* thread_name = k_thread_name_get(k_current_get());

        // Read as the importer will, from the handler, with room for every unit's record.
        std::array<RemoteStateRecord, INetworkStateTransport::MAX_REMOTE_RECORDS> records{};
        const size_t count = transport_->GetRemoteStates(state_type, records);

        k_mutex_lock(&lock_, K_FOREVER);
        if(thread_name == nullptr || std::strcmp(thread_name, CDMP_WORK_QUEUE_NAME) != 0)
            is_off_work_queue_ = true;
        if(state_type != state_type_)
            is_other_type_ = true;
        snapshots_.emplace_back(records.begin(), records.begin() + count);
        k_mutex_unlock(&lock_);

        k_sem_give(&called_);
    }

public:
    HandlerProbe() {
        k_sem_init(&called_, 0, K_SEM_MAX_LIMIT);
        k_mutex_init(&lock_);
    }

    void Register(INetworkStateTransport& transport, CanbusComStateType state_type) {
        transport_ = &transport;
        state_type_ = state_type;
        handler_id_ = transport.RegisterStateHandler(
            state_type, [this](CanbusComStateType type) { OnStateChanged(type); });
        zassert_true(handler_id_ >= 0, "RegisterStateHandler failed: %d", handler_id_);
    }

    void Unregister() {
        if(handler_id_ < 0)
            return;

        transport_->UnregisterStateHandler(handler_id_);
        handler_id_ = -1;
    }

    /** @brief The records the next call read, or nothing without a call in time. */
    std::optional<std::vector<RemoteStateRecord>> WaitForCall(int timeout_ms = FRAME_TIMEOUT_MS) {
        if(k_sem_take(&called_, K_MSEC(timeout_ms)) != 0)
            return std::nullopt;

        k_mutex_lock(&lock_, K_FOREVER);
        auto snapshot = std::move(snapshots_.front());
        snapshots_.pop_front();
        k_mutex_unlock(&lock_);

        return snapshot;
    }

    /** @brief The records the last of the calls made in time read, or nothing without a call. */
    std::optional<std::vector<RemoteStateRecord>> WaitForLastCall() {
        auto last = WaitForCall();

        // Changes that arrive together may share a call, so the calls are not counted.
        while(auto next = WaitForCall(NO_FRAME_TIMEOUT_MS))
            last = std::move(next);

        return last;
    }

    /** @brief Whether every call ran on the CDMP work queue, with the registered state type. */
    bool CallsWereAsRegistered() {
        k_mutex_lock(&lock_, K_FOREVER);
        const bool is_as_registered = !is_off_work_queue_ && !is_other_type_;
        k_mutex_unlock(&lock_);

        return is_as_registered;
    }
};

} // namespace canbus_com_test
