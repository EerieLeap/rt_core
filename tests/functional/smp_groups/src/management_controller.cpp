#include <memory>
#include <utility>

#include <zephyr/ztest.h>

#ifdef CONFIG_EERIE_LEAP_SMP_ASSETS
    #include <array>
    #include <string_view>

    #include "subsys/assets/asset_kind.h"
    #include "subsys/assets/asset_store.h"
    #include "subsys/assets/assets_manager.h"
    #include "subsys/assets/smp/assets_mgmt_group.h"
#endif
#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/system_domain/models/system_configuration.h"
#include "subsys/threading/work_queue_thread.h"

#include "controllers/management_controller.h"

#include "smp_groups_test_support.h"
#ifdef CONFIG_EERIE_LEAP_SMP_ASSETS
    #include "assets_test_support.h"
#endif

using namespace smp_groups_test;

using eerie_leap::controllers::ManagementController;
#ifdef CONFIG_EERIE_LEAP_SMP_ASSETS
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::assets::smp::AssetsMgmtGroup;
#endif
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::system_domain::models::SensorCapabilities;
using eerie_leap::domain::system_domain::models::SystemConfiguration;
using eerie_leap::domain::system_domain::smp::FilesMgmtGroup;
using eerie_leap::subsys::threading::WorkQueueThread;

ZTEST_SUITE(management_controller, NULL, NULL, NULL, NULL, NULL);

ZTEST(management_controller, test_serves_the_config_and_device_groups) {
    const auto work_queue = std::make_shared<WorkQueueThread>("mgmt_test_config_wq", 4096, 5);
    zassert_true(work_queue->Initialize());

    const auto configuration_service = std::make_shared<ConfigurationService>();
    configuration_service->RegisterCborConfigurationManager(
        ConfigurationService::Type::Ui, std::make_shared<FakeConfigurationManager>()
    );

    ManagementController controller(
        configuration_service, work_queue, nullptr,
        SensorCapabilities{ .adc_channels = 0, .gpio_channels = 1, .sensors_max = 64 }
    );
    zassert_equal(controller.Initialize(), 0);

    SmpTestClient client;

    const auto device = client.Read(SmpGroupId::DEVICE, 0);
    zassert_true(device.IsOk());
    zassert_equal(device.Value("build"), SystemConfiguration::SW_BUILD);
    zassert_equal(device.texts.at("board"), CONFIG_BOARD_TARGET);
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
    const auto work_queue = std::make_shared<WorkQueueThread>("mgmt_test_files_wq", 4096, 5);
    zassert_true(work_queue->Initialize());

    const auto configuration_service = std::make_shared<ConfigurationService>();
    const uint8_t list = std::to_underlying(FilesMgmtGroup::Command::LIST);
    const auto request = CborMap().PutText("dir", "scripts").Build();

    {
        ManagementController controller(configuration_service, work_queue, nullptr);
        zassert_equal(controller.Initialize(), 0);

        SmpTestClient client;
        const auto response = client.Read(SmpGroupId::FILES, list, request);

        zassert_equal(response.error_group.value_or(0), std::to_underlying(SmpGroupId::FILES));
        zassert_equal(
            response.error.value_or(0), std::to_underlying(FilesMgmtGroup::Error::NO_CARD),
            "Served without an SD service, to say there is no card"
        );
    }

    const auto card = std::make_shared<FakeFsService>();
    card->directories["scripts"] = { { .name = "oil.lua", .size = 120 } };

    ManagementController controller(configuration_service, work_queue, nullptr, std::nullopt, card);
    zassert_equal(controller.Initialize(), 0);

    SmpTestClient client;
    const auto response = client.Read(SmpGroupId::FILES, list, request);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("total"), 1);
    zassert_equal(response.entry_texts.size(), 1);
    zassert_str_equal(response.entry_texts[0].at("name").c_str(), "oil.lua");
    zassert_equal(response.entries[0].at("size"), 120);
}

#ifdef CONFIG_EERIE_LEAP_SMP_ASSETS

ZTEST(management_controller, test_serves_the_assets_group) {
    // Unwinding an exception on qemu_riscv64 needs more stack than the usual work queue has.
    const auto work_queue = std::make_shared<WorkQueueThread>("mgmt_test_assets_wq", 16384, 5);
    zassert_true(work_queue->Initialize());

    const auto volume = assets_group_test::GetSimFsService();
    volume->DeleteRecursive();

    // A provider keeps the span of its extensions, so they are in static storage.
    static constexpr std::array<std::string_view, 1> EXTENSIONS = { ".png" };
    const auto registry = std::make_shared<AssetsManager>();
    zassert_true(registry->Register(
        {
            .kind = AssetKind::IMAGES,
            .name = "images",
            .store = std::make_shared<AssetStore>(volume, "images"),
            .extensions = EXTENSIONS,
            .access = eerie_leap::subsys::assets::ToMask(eerie_leap::subsys::assets::AssetAccess::READ),
            .max_size = 0,
        }
    ));

    const Bytes data = Pattern(40, 7);
    zassert_true(volume->WriteFile("images/logo.png", data.data(), data.size()));

    const auto configuration_service = std::make_shared<ConfigurationService>();

    {
        ManagementController controller(configuration_service, work_queue, nullptr, std::nullopt, nullptr, registry);
        zassert_equal(controller.Initialize(), 0);

        SmpTestClient client;

        const auto kinds = client.Read(SmpGroupId::ASSETS, std::to_underlying(AssetsMgmtGroup::Command::KINDS));
        zassert_true(kinds.IsOk());
        zassert_equal(kinds.entries.size(), 1);
        zassert_equal(kinds.entries[0].at("kind"), std::to_underlying(AssetKind::IMAGES));
        zassert_str_equal(kinds.entry_texts[0].at("name").c_str(), "images");
        zassert_true(kinds.entry_flags[0].at("avail"));

        const auto listing = client.Read(
            SmpGroupId::ASSETS, std::to_underlying(AssetsMgmtGroup::Command::LIST),
            CborMap().Put("kind", static_cast<uint32_t>(AssetKind::IMAGES)).Build()
        );
        zassert_true(listing.IsOk());
        zassert_equal(listing.entries.size(), 1);
        zassert_str_equal(listing.entry_texts[0].at("name").c_str(), "logo.png");
        zassert_equal(listing.entries[0].at("size"), data.size());

        const auto chunk = client.Read(
            SmpGroupId::ASSETS, std::to_underlying(AssetsMgmtGroup::Command::READ),
            CborMap()
                .Put("kind", static_cast<uint32_t>(AssetKind::IMAGES))
                .PutText("name", "logo.png")
                .Put("off", 0)
                .Build()
        );
        zassert_true(chunk.IsOk());
        zassert_true(chunk.bytes.at("data") == data);
    }

    volume->DeleteRecursive();
}

ZTEST(management_controller, test_serves_an_empty_assets_group_without_a_registry) {
    const auto work_queue = std::make_shared<WorkQueueThread>("mgmt_test_no_assets_wq", 16384, 5);
    zassert_true(work_queue->Initialize());

    const auto configuration_service = std::make_shared<ConfigurationService>();

    ManagementController controller(configuration_service, work_queue, nullptr);
    zassert_equal(controller.Initialize(), 0);

    SmpTestClient client;

    const auto kinds = client.Read(SmpGroupId::ASSETS, std::to_underlying(AssetsMgmtGroup::Command::KINDS));

    zassert_true(kinds.IsOk(), "The group is served, to say the unit has no kinds");
    zassert_true(kinds.entries.empty());
    zassert_equal(kinds.list_sizes.at("kinds"), 0);
}

#endif // CONFIG_EERIE_LEAP_SMP_ASSETS
