#include <atomic>
#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/i_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "controllers/canbus_controller.h"

using eerie_leap::controllers::CanbusController;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr int RECONFIGURE_DELAY_MS = CONFIG_EERIE_LEAP_CONTROLLER_CANBUS_RECONFIGURE_DELAY_MS;

// A service on the CAN bus, which a reconfiguration stops and starts again.
class FakeDependentService : public IService {
public:
    std::atomic<int> stops = 0;
    std::atomic<int> starts = 0;

    bool Initialize() override { return true; }
    bool Start() override { starts++; return true; }
    bool Stop() override { stops++; return true; }
    [[nodiscard]] ServiceState GetState() const noexcept override { return ServiceState::RUNNING; }
};

} // namespace

ZTEST_SUITE(canbus_controller, NULL, NULL, NULL, NULL, NULL);

ZTEST(canbus_controller, test_a_configuration_update_takes_effect_after_the_delay) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    auto work_queue = std::make_shared<WorkQueueThread>("canbus_test_config_wq", 6144, 5);
    zassert_true(work_queue->Initialize());

    CanbusController controller(fs_service, work_queue, nullptr);
    zassert_equal(controller.Initialize(), 0);

    auto service = std::make_shared<FakeDependentService>();
    controller.RegisterDependentService(service);

    const auto manager = controller.GetConfigurationManager();
    const auto configuration = manager->Get();

    zassert_true(manager->Update(*configuration));
    zassert_equal(service->stops.load(), 0, "The update reconfigured before returning");

    k_msleep(RECONFIGURE_DELAY_MS / 2);
    zassert_true(manager->Update(*configuration));

    k_msleep(RECONFIGURE_DELAY_MS * 3 / 4);
    zassert_equal(service->stops.load(), 0, "The second update did not restart the delay");

    k_msleep(RECONFIGURE_DELAY_MS);
    zassert_equal(service->stops.load(), 1, "The updates were not put into effect once");
    zassert_equal(service->starts.load(), 1);
}
