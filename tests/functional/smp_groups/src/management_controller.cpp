#include <memory>
#include <utility>

#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_thread.h"
#include "domain/configuration_domain/services/configuration_service.h"

#include "controllers/management_controller.h"

#include "smp_groups_test_support.h"

using namespace smp_groups_test;

using eerie_leap::controllers::ManagementController;
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

    ManagementController controller(configuration_service, work_queue, nullptr, BUILD_NUMBER);
    zassert_equal(controller.Initialize(), 0);

    SmpTestClient client;

    const auto device = client.Read(SmpGroupId::DEVICE, 0);
    zassert_true(device.IsOk());
    zassert_equal(device.Value("build"), BUILD_NUMBER);
    zassert_false(device.Has("cdmp_id"), "No CDMP identity without network info");

    const auto types = client.Read(SmpGroupId::CONFIG, 0);
    zassert_true(types.IsOk());
    zassert_equal(types.entries.size(), 1);
    zassert_equal(types.entries[0].at("type"), std::to_underlying(ConfigurationService::Type::Ui));
}
