// A disabled CAN message keeps its configuration, but the frame router treats it as absent: a signal
// sensor bound to it gets no route and never a reading. A raw sensor on the same frame ID needs no
// message definition and still gets the frame.
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"
#include "domain/canbus_domain/models/can_channel_configuration.h"
#include "domain/canbus_domain/models/can_message_configuration.h"
#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/sensor_domain/isr_sensor_readers/can_frame_router.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "subsys/canbus/can_frame.h"
#include "subsys/canbus/can_id.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/work_queue_thread.h"

#include "processing_test_support.h"

using namespace eerie_memory;
using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using namespace eerie_leap::domain::sensor_domain::runtime;

using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::CanFrameRouter;
using eerie_leap::subsys::threading::WorkQueueThread;

using namespace processing_test_support;

namespace {

constexpr uint8_t K_BUS = 0;
constexpr uint32_t ENABLED_FRAME_ID = 0x100;
constexpr uint32_t DISABLED_FRAME_ID = 0x200;
constexpr int TIMEOUT_MS = 1000;

const device* LoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// CanbusService opens the channel in normal mode, in which the loopback controller hands no frame
// back, so the controller is switched to loopback mode under it.
void SwitchToLoopbackMode() {
    zassert_ok(can_stop(LoopbackDevice()));
    zassert_ok(can_set_mode(LoopbackDevice(), CAN_MODE_LOOPBACK));
    zassert_ok(can_start(LoopbackDevice()));
}

void SendFrame(const CanbusProxy& canbus, uint32_t frame_id, uint8_t first_byte) {
    const std::array<uint8_t, 8> data{ first_byte };

    zassert_ok(canbus->SendFrame(CanId::Standard(frame_id), data, K_MSEC(100)));
}

bool WaitForValue(const SensorReadingsFrame& frame, std::string_view sensor_id, float value) {
    const int64_t deadline = k_uptime_get() + TIMEOUT_MS;

    while(frame.TryGetReadingValue(sensor_id) != value) {
        if(k_uptime_get() >= deadline)
            return false;

        k_msleep(5);
    }

    return true;
}

// A failing assertion skips the destructors, so the controller is stopped for the next test.
void ResetLoopbackDevice(void*) {
    can_stop(LoopbackDevice());
}

// An 8-byte message whose first byte is the unsigned signal "sig".
std::shared_ptr<CanMessageConfiguration> MakeMessage(uint32_t frame_id, std::string_view name, bool is_enabled) {
    auto message = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message->frame_id = frame_id;
    message->is_enabled = is_enabled;
    message->name = name;
    message->message_size = 8;

    CanSignalConfiguration signal(std::allocator_arg, Mrm::GetDefaultPmr());
    signal.start_bit = 0;
    signal.size_bits = 8;
    signal.is_signed = false;
    signal.SetName("sig");
    message->signal_configurations.push_back(std::move(signal));

    return message;
}

// Two messages with the same signal on one classical channel; the second one is the one switched off.
CanbusConfiguration MakeCanbusConfiguration(bool is_second_message_enabled) {
    CanbusConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());

    CanChannelConfiguration channel(std::allocator_arg, Mrm::GetDefaultPmr());
    channel.type = CanbusType::CLASSICAL_CAN;
    channel.bus_channel = K_BUS;
    channel.bitrate = 500000;
    channel.message_configurations.push_back(MakeMessage(ENABLED_FRAME_ID, "EL_FRAME_0", true));
    channel.message_configurations.push_back(MakeMessage(DISABLED_FRAME_ID, "EL_FRAME_1", is_second_message_enabled));
    configuration.channel_configurations.emplace(K_BUS, std::move(channel));

    return configuration;
}

std::shared_ptr<CanbusConfigurationManager> MakeCanbusConfigurationManager(std::shared_ptr<FsService> fs_service) {
    auto manager = std::make_shared<CanbusConfigurationManager>(
        std::make_unique<CborConfigurationService<CborCanbusConfig>>("canbus_config", std::move(fs_service)), nullptr
    );
    zassert_true(manager->Update(MakeCanbusConfiguration(false)), "The CAN configuration is valid");

    return manager;
}

std::shared_ptr<Sensor> MakeCanSensor(std::string_view id, SensorType type, uint32_t frame_id) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = type;
    sensor->configuration.canbus_source = type == SensorType::CANBUS_RAW
        ? make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), K_BUS, frame_id)
        : make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), K_BUS, frame_id, "sig");
    sensor->configuration.UpdateConnectionString();

    return sensor;
}

} // namespace

ZTEST_SUITE(disabled_can_message_routes, NULL, NULL, NULL, ResetLoopbackDevice, NULL);

ZTEST(disabled_can_message_routes, test_a_signal_on_a_disabled_message_gets_no_route) {
    const auto canbus_configuration_manager = MakeCanbusConfigurationManager(MakeFormattedFsService());

    // Routes are resolved from the configuration alone; without a controller none of them listens.
    const auto canbus_service = std::make_shared<CanbusService>(
        [](uint8_t) -> const device* {
            return nullptr;
        },
        canbus_configuration_manager
    );

    const std::vector<std::shared_ptr<Sensor>> sensors{
        MakeCanSensor("on_enabled", SensorType::CANBUS_ANALOG, ENABLED_FRAME_ID),
        MakeCanSensor("on_disabled", SensorType::CANBUS_ANALOG, DISABLED_FRAME_ID),
    };

    const auto frame = std::make_shared<SensorReadingsFrame>();
    const auto generation = SensorPipelineBuilder(nullptr, frame)
                                .Build(std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));

    const auto work_queue_thread = std::make_shared<WorkQueueThread>("can_router_test", 4096, 6);
    zassert_true(work_queue_thread->Initialize());

    CanFrameRouter router(
        MakeTimeService(), work_queue_thread, canbus_service, [](const SensorRuntime&, SensorReading&) {}
    );

    zassert_equal(router.Attach(*generation), 0);
    zassert_equal(router.GetRouteCount(), 1, "Only the signal of the enabled message is routed");

    // The disabled message kept its signal, so switching it back on is all it takes.
    zassert_true(canbus_configuration_manager->Update(MakeCanbusConfiguration(true)));
    zassert_equal(router.Attach(*generation), 0);
    zassert_equal(router.GetRouteCount(), 2, "Both signals are routed once the message is enabled");

    router.Detach();
}

ZTEST(disabled_can_message_routes, test_a_frame_of_a_disabled_message_gives_its_signal_no_reading) {
    const auto fs_service = MakeFormattedFsService();
    const auto canbus_configuration_manager = MakeCanbusConfigurationManager(fs_service);

    const auto canbus_service = std::make_shared<CanbusService>(
        [](uint8_t bus_channel) {
            return bus_channel == K_BUS ? LoopbackDevice() : nullptr;
        },
        canbus_configuration_manager
    );

    const auto canbus = canbus_service->GetCanbus(K_BUS);
    zassert_not_null(canbus.get(), "The channel is open");
    SwitchToLoopbackMode();

    const std::vector<std::shared_ptr<Sensor>> sensors{
        MakeCanSensor("on_enabled", SensorType::CANBUS_ANALOG, ENABLED_FRAME_ID),
        MakeCanSensor("on_disabled", SensorType::CANBUS_ANALOG, DISABLED_FRAME_ID),
        MakeCanSensor("raw_on_disabled", SensorType::CANBUS_RAW, DISABLED_FRAME_ID),
    };

    // Stored without the check a client's apply makes, as when the message is disabled after the
    // sensors were configured.
    const auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 0, 0
    );
    zassert_true(sensors_configuration_manager->Update(sensors), "The sensors configuration is valid");

    const auto time_service = MakeTimeService();
    const auto frame = std::make_shared<SensorReadingsFrame>();

    const auto processing_service = std::make_shared<SensorsProcessingService>(
        sensors_configuration_manager, frame, time_service, nullptr,
        std::make_shared<SensorReaderFactory>(time_service, nullptr, nullptr), nullptr, canbus_service
    );
    zassert_true(processing_service->Initialize());
    zassert_true(processing_service->Start());

    SendFrame(*canbus, DISABLED_FRAME_ID, 7);
    SendFrame(*canbus, ENABLED_FRAME_ID, 1);
    zassert_true(WaitForValue(*frame, "on_enabled", 1.0F), "The signal of the enabled message is decoded");

    // The router dispatches one pass at a time, so once this frame lands the pass that took the
    // disabled message's frame is over.
    SendFrame(*canbus, ENABLED_FRAME_ID, 2);
    zassert_true(WaitForValue(*frame, "on_enabled", 2.0F));

    CanFrame raw_frame;
    zassert_true(
        frame->TryGetCanFrame(sensors[2]->id_hash, raw_frame),
        "A raw sensor on the disabled message's frame ID still gets the frame"
    );
    zassert_true(!raw_frame.data.empty());
    zassert_equal(raw_frame.data[0], 7);

    zassert_false(frame->TryGetReading("on_disabled").has_value(), "The signal of the disabled message got a reading");
    zassert_false(frame->TryGetReadingValue("on_disabled").has_value());

    zassert_true(processing_service->Stop());
}
