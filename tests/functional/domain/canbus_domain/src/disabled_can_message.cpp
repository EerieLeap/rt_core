// A disabled CAN message keeps its configuration (send interval, script path, signals) and is stored
// as disabled, but it is never sent and its script is never loaded.
#include <memory>
#include <string_view>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/cbor/cbor_serializer.h"
#include "configuration/services/cbor_configuration_service.h"
#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"
#include "domain/canbus_domain/configuration/parsers/canbus_configuration_cbor_parser.h"
#include "domain/canbus_domain/models/can_channel_configuration.h"
#include "domain/canbus_domain/models/can_message_configuration.h"
#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/canbus_domain/services/canbus_scheduler_service.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/canbus/can_frame.h"
#include "subsys/canbus/can_id.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "utilities/memory/memory_resource_manager.h"

using namespace eerie_leap::domain::canbus_domain::models;

using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::canbus_domain::configuration::parsers::CanbusConfigurationCborParser;
using eerie_leap::domain::canbus_domain::services::CanbusSchedulerService;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::CanId;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::utilities::memory::Mrm;

namespace {

constexpr uint8_t K_BUS = 0;
constexpr uint32_t ENABLED_FRAME_ID = 0x100;
constexpr uint32_t DISABLED_FRAME_ID = 0x200;
constexpr int SEND_INTERVAL_MS = 10;

const device* LoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// A failing assertion skips the destructors, so the controller is stopped for the next test.
void ResetLoopbackDevice(void*) {
    can_stop(LoopbackDevice());
}

// CanbusService opens the channel in normal mode, in which the loopback controller hands no frame
// back, so the controller is switched to loopback mode under it.
void SwitchToLoopbackMode() {
    zassert_ok(can_stop(LoopbackDevice()));
    zassert_ok(can_set_mode(LoopbackDevice(), CAN_MODE_LOOPBACK));
    zassert_ok(can_start(LoopbackDevice()));
}

std::shared_ptr<FsService> MakeFormattedFsService() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    zassert_true(fs_service->Format());

    return fs_service;
}

std::shared_ptr<CanMessageConfiguration> MakeMessage(uint32_t frame_id, std::string_view name, bool is_enabled) {
    auto message = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message->frame_id = frame_id;
    message->is_enabled = is_enabled;
    message->name = name;
    message->message_size = 8;
    message->send_interval_ms = SEND_INTERVAL_MS;

    return message;
}

// One classical channel with an enabled and a disabled message, both with the same send interval.
CanbusConfiguration MakeConfiguration() {
    CanbusConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());

    CanChannelConfiguration channel(std::allocator_arg, Mrm::GetDefaultPmr());
    channel.type = CanbusType::CLASSICAL_CAN;
    channel.bus_channel = K_BUS;
    channel.bitrate = 500000;
    channel.message_configurations.push_back(MakeMessage(ENABLED_FRAME_ID, "EL_FRAME_0", true));
    channel.message_configurations.push_back(MakeMessage(DISABLED_FRAME_ID, "EL_FRAME_1", false));
    configuration.channel_configurations.emplace(K_BUS, std::move(channel));

    return configuration;
}

std::shared_ptr<CanbusConfigurationManager> MakeConfigurationManager(std::shared_ptr<FsService> fs_service) {
    auto manager = std::make_shared<CanbusConfigurationManager>(
        std::make_unique<CborConfigurationService<CborCanbusConfig>>("canbus_config", std::move(fs_service)), nullptr
    );
    zassert_true(manager->Update(MakeConfiguration()), "The CAN configuration is valid");

    return manager;
}

// Counts the frames of one ID that the loopback controller hands back.
class FrameCounter {
private:
    atomic_t count_ = ATOMIC_INIT(0);
    k_sem received_{};

public:
    FrameCounter() {
        k_sem_init(&received_, 0, K_SEM_MAX_LIMIT);
    }

    void Count() {
        atomic_inc(&count_);
        k_sem_give(&received_);
    }

    [[nodiscard]] bool Wait(int timeout_ms) {
        return k_sem_take(&received_, K_MSEC(timeout_ms)) == 0;
    }

    [[nodiscard]] int GetCount() const {
        return static_cast<int>(atomic_get(&count_));
    }
};

} // namespace

ZTEST_SUITE(disabled_can_message, NULL, NULL, NULL, ResetLoopbackDevice, NULL);

ZTEST(disabled_can_message, test_a_disabled_message_is_stored_and_reloaded_as_disabled) {
    const auto manager = MakeConfigurationManager(MakeFormattedFsService());

    // Decoded from flash, not the instance Update() kept.
    const auto reloaded = manager->Get(true);
    zassert_not_null(reloaded.get());

    const auto& messages = reloaded->channel_configurations.at(K_BUS).message_configurations;
    zassert_equal(messages.size(), 2);
    zassert_true(messages.at(0)->is_enabled);
    zassert_false(messages.at(1)->is_enabled);
    zassert_equal(messages.at(1)->send_interval_ms, SEND_INTERVAL_MS, "A disabled message keeps its send interval");

    // What a client exports carries the flag too.
    CborSerializer<CborCanbusConfig> serializer;
    const auto exported = manager->GetCborConfiguration();
    auto cbor_config = serializer.Deserialize(exported);
    zassert_not_null(cbor_config.get());

    const auto& message_configs = cbor_config->CborCanChannelConfig_m.at(0).CborCanMessageConfig_m;
    zassert_true(message_configs.at(0).is_enabled);
    zassert_false(message_configs.at(1).is_enabled);
}

ZTEST(disabled_can_message, test_the_script_of_a_disabled_message_is_not_loaded) {
    const auto fs_service = MakeFormattedFsService();

    static constexpr std::string_view SCRIPT = "function process_can_frame(frame_id, data) return data end";
    zassert_true(fs_service->WriteFile("frame.lua", SCRIPT.data(), SCRIPT.size()));

    auto configuration = MakeConfiguration();
    for(const auto& message : configuration.channel_configurations.at(K_BUS).message_configurations)
        message->script_path = "frame.lua";

    CanbusConfigurationCborParser parser(fs_service);
    const auto cbor_config = parser.Serialize(configuration);
    auto parsed = parser.Deserialize(Mrm::GetDefaultPmr(), *cbor_config);

    const auto& messages = parsed->channel_configurations.at(K_BUS).message_configurations;
    zassert_not_null(messages.at(0)->lua_script.get(), "The enabled message loads its script");
    zassert_is_null(messages.at(1)->lua_script.get(), "A disabled message never sends, so its script is not loaded");
    zassert_true(messages.at(1)->script_path == "frame.lua", "A disabled message keeps its script path");
}

ZTEST(disabled_can_message, test_the_scheduler_does_not_send_a_disabled_message) {
    FrameCounter enabled_frames;
    FrameCounter disabled_frames;

    const auto configuration_manager = MakeConfigurationManager(MakeFormattedFsService());
    const auto canbus_service = std::make_shared<CanbusService>(
        [](uint8_t bus_channel) {
            return bus_channel == K_BUS ? LoopbackDevice() : nullptr;
        },
        configuration_manager
    );

    const auto canbus = canbus_service->GetCanbus(K_BUS);
    zassert_not_null(canbus.get(), "The channel is open");
    SwitchToLoopbackMode();

    const int enabled_handler_id = (*canbus)->RegisterFrameReceivedHandler(
        CanId::Standard(ENABLED_FRAME_ID), [&enabled_frames](const CanFrame&) {
            enabled_frames.Count();
        }
    );
    const int disabled_handler_id = (*canbus)->RegisterFrameReceivedHandler(
        CanId::Standard(DISABLED_FRAME_ID), [&disabled_frames](const CanFrame&) {
            disabled_frames.Count();
        }
    );
    zassert_true(enabled_handler_id > 0, "Frame handler rejected: %d", enabled_handler_id);
    zassert_true(disabled_handler_id > 0, "Frame handler rejected: %d", disabled_handler_id);

    const auto scheduler = std::make_shared<CanbusSchedulerService>(
        configuration_manager, canbus_service, std::make_shared<SensorReadingsFrame>()
    );
    zassert_true(scheduler->Initialize());
    zassert_true(scheduler->Start());

    // Both tasks would start together with the same interval, and frames come back in the order they
    // were sent, so a disabled message that was sent shows up before the third enabled frame.
    for(int i = 0; i < 3; i++)
        zassert_true(enabled_frames.Wait(1000), "The enabled message is sent every %d ms", SEND_INTERVAL_MS);

    zassert_equal(disabled_frames.GetCount(), 0, "The disabled message was sent");

    zassert_true(scheduler->Stop());
    zassert_true((*canbus)->RemoveFrameReceivedHandler(enabled_handler_id));
    zassert_true((*canbus)->RemoveFrameReceivedHandler(disabled_handler_id));
}
