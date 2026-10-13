#include <memory>
#include <utility>

#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_thread.h"
#include "domain/configuration_domain/services/configuration_service.h"

#include "controllers/management_controller.h"

#include "smp_groups_test_support.h"

using namespace smp_groups_test;

using eerie_leap::controllers::ManagementController;
using eerie_leap::domain::system_domain::smp::FilesMgmtGroup;
using eerie_leap::domain::system_domain::models::SensorCapabilities;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;

namespace {

constexpr uint32_t BUILD_NUMBER = 4321;

} // namespace

ZTEST_SUITE(management_controller, NULL, NULL, NULL, NULL, NULL);

ZTEST(management_controller, test_serves_the_config_and_device_groups) {
    auto work_queue = std::make_shared<WorkQueueThread>("mgmt_test_config_wq", 4096, 5);
    zassert_true(work_queue->Initialize());

    auto configuration_service = std::make_shared<ConfigurationService>();
    configuration_service->RegisterCborConfigurationManager(
        ConfigurationService::Type::Ui, std::make_shared<FakeConfigurationManager>());

    ManagementController controller(configuration_service, work_queue, nullptr, BUILD_NUMBER,
        SensorCapabilities{ .adc_channels = 0, .gpio_channels = 1, .sensors_max = 64 });
    zassert_equal(controller.Initialize(), 0);

    SmpTestClient client;

    const auto device = client.Read(SmpGroupId::DEVICE, 0);
    zassert_true(device.IsOk());
    zassert_equal(device.Value("build"), BUILD_NUMBER);
    zassert_false(device.Has("cdmp_id"), "No CDMP identity without network info");
    zassert_equal(device.Value("adc_channels"), 0);
    zassert_equal(device.Value("gpio_channels"), 1);
    zassert_equal(device.Value("sensors_max"), 64);

    const auto types = client.Read(SmpGroupId::CONFIG, 0);
    zassert_true(types.IsOk());
    zassert_equal(types.entries.size(), 1);
    zassert_equal(types.entries[0].at("type"), std::to_underlying(ConfigurationService::Type::Ui));
}

ZTEST(management_controller, test_serves_the_files_group) {
    auto work_queue = std::make_shared<WorkQueueThread>("mgmt_test_files_wq", 4096, 5);
    zassert_true(work_queue->Initialize());

    auto configuration_service = std::make_shared<ConfigurationService>();
    const uint8_t list = std::to_underlying(FilesMgmtGroup::Command::LIST);
    const auto request = CborMap().PutText("dir", "scripts").Build();

    {
        ManagementController controller(configuration_service, work_queue, nullptr, BUILD_NUMBER);
        zassert_equal(controller.Initialize(), 0);

        SmpTestClient client;
        const auto response = client.Read(SmpGroupId::FILES, list, request);

        zassert_equal(response.error_group.value_or(0), std::to_underlying(SmpGroupId::FILES));
        zassert_equal(response.error.value_or(0), std::to_underlying(FilesMgmtGroup::Error::NO_CARD),
            "Served without an SD service, to say there is no card");
    }

    auto card = std::make_shared<FakeFsService>();
    card->directories["scripts"] = {{.name = "oil.lua", .size = 120}};

    ManagementController controller(configuration_service, work_queue, nullptr, BUILD_NUMBER, std::nullopt, card);
    zassert_equal(controller.Initialize(), 0);

    SmpTestClient client;
    const auto response = client.Read(SmpGroupId::FILES, list, request);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("total"), 1);
    zassert_equal(response.entry_texts.size(), 1);
    zassert_str_equal(response.entry_texts[0].at("name").c_str(), "oil.lua");
    zassert_equal(response.entries[0].at("size"), 120);
}
