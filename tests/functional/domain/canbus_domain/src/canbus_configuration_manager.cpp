#include <array>
#include <memory>

#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"

using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;

namespace {

std::shared_ptr<CanbusConfigurationManager> CreateManager() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    return std::make_shared<CanbusConfigurationManager>(
        std::make_unique<CborConfigurationService<CborCanbusConfig>>("canbus_config", fs_service), nullptr);
}

} // namespace

ZTEST_SUITE(canbus_configuration_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(canbus_configuration_manager, test_CanbusConfigurationManager_ApplyCborConfiguration_reports_why_it_rejects) {
    auto manager = CreateManager();

    int notifications = 0;
    manager->RegisterConfigurationUpdatedHandler([&notifications] { notifications++; });

    CborSerializer<CborCanbusConfig> serializer;
    const auto exported = manager->GetCborConfiguration();
    auto cbor_config = serializer.Deserialize(exported);
    zassert_not_null(cbor_config.get());
    // The default configuration has no channels.
    cbor_config->com_config.bus_channel = 0;

    std::array<char, 96> reason{};
    zassert_false(manager->ApplyCborConfiguration(serializer.Serialize(*cbor_config), reason));
    zassert_str_equal(reason.data(), "Invalid CAN Bus COM configuration. COM bus channel is not configured");
    zassert_equal(notifications, 0);

    zassert_true(manager->ApplyCborConfiguration(exported));
    zassert_equal(notifications, 1);
}
