#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "domain/canbus_com_domain/services/i_network_state_transport.h"
#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"
#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/canbus_domain/services/canbus_service.h"

#include "cdmp_loopback_harness.h"
#include "cdmp_test_features.h"

using eerie_leap::tests::LOGGER_FEATURES;

// A CanbusComService on the loopback controller and a peer scripted on its bus, shared by the
// suites that exercise the COM domain: canbus_com_domain and logging_control.
namespace canbus_com_loopback_test {

using namespace cdmp_loopback_test;

using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::domain::canbus_com_domain::services::INetworkStateTransport;
using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::domain::canbus_domain::models::CanChannelConfiguration;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::utilities::memory::Mrm;

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
    bool StartUnit() override {
        return com->Start();
    }
    [[nodiscard]] CdmpDeviceStatus GetUnitStatus() const override {
        return com->GetNetworkInfo()->GetDeviceInfo().status;
    }
    [[nodiscard]] uint8_t GetUnitDeviceId() const override {
        return com->GetNetworkInfo()->GetDeviceInfo().device_id;
    }

public:
    std::shared_ptr<CanbusService> canbus_service;
    std::shared_ptr<CanbusComService> com;

    Harness() {
        DtFs::InitInternalFs();
        auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        zassert_true(fs_service->Format());

        auto configuration_manager = std::make_shared<CanbusConfigurationManager>(
            std::make_unique<CborConfigurationService<CborCanbusConfig>>("canbus_config", fs_service), nullptr
        );
        zassert_true(configuration_manager->Update(ComConfiguration()));

        canbus_service = std::make_shared<CanbusService>(
            [](uint8_t bus_channel) {
                return bus_channel == COM_CHANNEL ? LoopbackDevice() : nullptr;
            },
            configuration_manager
        );
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
    INetworkStateTransport& Transport() {
        return *com;
    }

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
        SendAsPeer(
            MANAGEMENT,
            CdmpIdClaimRequestMessage{ .claiming_device_id = device_id,
                .uid = uid,
                .protocol_version = CdmpDevice::GetCurrentProtocolVersion() }
                .ToCanFrame()
        );
        SendAsPeer(HEARTBEAT, HeartbeatFrame(device_id, 0, LOGGER_FEATURES));

        const int64_t deadline = k_uptime_get() + FRAME_TIMEOUT_MS;
        while(!IsInDeviceTable(device_id)) {
            zassert_true(k_uptime_get() < deadline, "Device %d joins on its first heartbeat", device_id);
            k_msleep(5);
        }
    }

    /** @brief Waits for the unit's acknowledgement of @p source's notification @p version. */
    bool WaitForAcknowledgement(uint8_t source, uint8_t version) {
        return bus
            .Wait(
                STATE_CHANGE_RESPONSE,
                [source, version](const Bytes& frame) {
                    return frame.size() >= 3 && frame[0] == OWN_ID && frame[1] == source && frame[2] == version;
                }
            )
            .has_value();
    }
};

} // namespace canbus_com_loopback_test
