#include <algorithm>
#include <memory>
#include <utility>

#include <eerie_memory.hpp>
#include <zephyr/devicetree.h>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"

#include "subsys/device_tree/dt_adc.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/adc_domain/configuration/adc_default_configuration.h"
#include "domain/configuration_domain/services/configuration_service.h"

#include "controllers/adc_controller.h"

#include "calibration_test_support.h"

#ifdef CONFIG_EERIE_LEAP_SMP
    #include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

    #include "domain/adc_domain/smp/calibration_mgmt_group.h"

    #include "smp_test_client.h"
#endif

using eerie_leap::controllers::AdcController;
using eerie_leap::domain::adc_domain::configuration::AdcDefaultConfiguration;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::subsys::device_tree::DtAdc;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::utilities::memory::Mrm;

using calibration_test_support::MakeTimeService;
using calibration_test_support::RecordingService;

namespace {

// native_sim declares two emulated ADCs (four channels, then two). QEMU targets declare a third
// one whose channel states no resolution, which rt_core's ADC Initialize() rejects, so there the
// controller must fail to initialize; the smp scenario leaves it out (RT_CORE_TEST_NO_FAILING_ADC).
constexpr bool kDtHasFailingAdc = DT_NODE_EXISTS(DT_ALIAS(adc2));
constexpr size_t kDtChannelCount = 6;

struct Fixture {
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<WorkQueueThread> work_queue;
};

Fixture MakeFixture() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    DtAdc::Initialize();

    auto work_queue = std::make_shared<WorkQueueThread>("adc_test_config_wq", 6144, 5);
    zassert_true(work_queue->Initialize());

    return { .fs_service = std::move(fs_service), .work_queue = std::move(work_queue) };
}

void ExpectDefaultConfiguration(const AdcController& controller) {
    const auto configuration = controller.GetConfigurationManager()->Get();
    zassert_not_null(configuration.get());
    zassert_equal(configuration->samples, AdcDefaultConfiguration::SAMPLES);
    zassert_equal(configuration->channel_configurations->size(), kDtChannelCount);

    const auto default_calibrator = AdcDefaultConfiguration::CreateCalibrator(Mrm::GetDefaultPmr());
    const auto& default_table = *default_calibrator->GetCalibrationTable();

    for(size_t i = 0; i < kDtChannelCount; i++) {
        const auto& calibrator = configuration->channel_configurations->at(i)->calibrator;
        zassert_not_null(calibrator.get(), "channel %zu", i);
        zassert_equal(
            calibrator->GetInterpolationMethod(), default_calibrator->GetInterpolationMethod(), "channel %zu", i
        );

        const auto& table = *calibrator->GetCalibrationTable();
        zassert_equal(table.size(), default_table.size(), "channel %zu", i);
        for(size_t j = 0; j < table.size(); j++) {
            zassert_equal(table[j].voltage, default_table[j].voltage, "channel %zu point %zu", i, j);
            zassert_equal(table[j].value, default_table[j].value, "channel %zu point %zu", i, j);
        }
    }
}

} // namespace

ZTEST_SUITE(adc_controller, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_controller, test_Initialize_registers_a_default_configuration_sized_to_the_device_tree) {
    auto fixture = MakeFixture();
    auto configuration_service = std::make_shared<ConfigurationService>();

    AdcController controller(fixture.fs_service, fixture.work_queue, configuration_service, nullptr);
    zassert_is_null(controller.GetManager().get());

    if constexpr(kDtHasFailingAdc) {
        // main() treats this as fatal.
        zassert_equal(controller.Initialize(), -1);
        zassert_is_null(controller.GetManager().get());
        zassert_is_null(controller.GetConfigurationManager().get());
        zassert_false(configuration_service->IsRegistered(ConfigurationService::Type::Adc));
        return;
    }

    zassert_equal(controller.Initialize(), 0);

    zassert_not_null(controller.GetManager().get());
    zassert_equal(controller.GetManager()->GetChannelCount(), kDtChannelCount);
    zassert_not_null(controller.GetConfigurationManager().get());
    zassert_equal(controller.GetConfigurationManager()->GetAdcManager().get(), controller.GetManager().get());

    ExpectDefaultConfiguration(controller);

    // The configuration service exports what the controller's manager holds.
    zassert_true(configuration_service->IsRegistered(ConfigurationService::Type::Adc));

    const auto exported = configuration_service->GetCborConfiguration(ConfigurationService::Type::Adc);
    const auto expected = controller.GetConfigurationManager()->GetCborConfiguration();
    zassert_false(exported.empty());
    zassert_true(std::ranges::equal(exported, expected));
}

ZTEST(adc_controller, test_Initialize_without_a_configuration_service) {
    auto fixture = MakeFixture();

    AdcController controller(fixture.fs_service, fixture.work_queue, nullptr, nullptr);

    if constexpr(kDtHasFailingAdc) {
        zassert_equal(controller.Initialize(), -1);
        zassert_is_null(controller.GetManager().get());
        return;
    }

    zassert_equal(controller.Initialize(), 0);
    zassert_not_null(controller.GetManager().get());
    zassert_is_null(controller.GetCalibrationService().get(), "No calibration without a time service");

    ExpectDefaultConfiguration(controller);
}

// The paused service may be registered before or after Initialize(); a job pauses it either way.
ZTEST(adc_controller, test_Initialize_creates_the_calibration_service) {
    auto fixture = MakeFixture();

    for(const bool is_registered_first : { true, false }) {
        auto paused_service = std::make_shared<RecordingService>();

        AdcController controller(fixture.fs_service, fixture.work_queue, nullptr, MakeTimeService());
        zassert_is_null(controller.GetCalibrationService().get());

        if(is_registered_first)
            controller.RegisterPausedService(paused_service);

        if constexpr(kDtHasFailingAdc) {
            zassert_equal(controller.Initialize(), -1);
            zassert_is_null(controller.GetCalibrationService().get());
            return;
        }

        zassert_equal(controller.Initialize(), 0);

        if(!is_registered_first)
            controller.RegisterPausedService(paused_service);

        const auto calibration_service = controller.GetCalibrationService();
        zassert_not_null(calibration_service.get());

        zassert_false(
            calibration_service->Start(static_cast<int>(kDtChannelCount)), "A channel the unit does not have"
        );
        zassert_true(
            calibration_service->Start(static_cast<int>(kDtChannelCount) - 1), "The last channel, on the second ADC"
        );
        zassert_true(paused_service->IsPaused(), "registered first: %d", is_registered_first);
        zassert_true(calibration_service->Stop());
        zassert_true(paused_service->IsRunning());
        zassert_equal(paused_service->pauses, 1);
        zassert_equal(paused_service->resumes, 1);
    }
}

#ifdef CONFIG_EERIE_LEAP_SMP
namespace {

using eerie_leap::domain::adc_domain::smp::CalibrationMgmtGroup;
using smp_test::CborMap;
using smp_test::Response;
using smp_test::SmpGroupId;
using smp_test::SmpTestClient;

using Command = CalibrationMgmtGroup::Command;
using Error = CalibrationMgmtGroup::Error;

Response Start(SmpTestClient& client, uint32_t channel) {
    return client.Write(
        SmpGroupId::CALIBRATION, std::to_underlying(Command::START), CborMap().Put("channel", channel).Build()
    );
}

Response Stop(SmpTestClient& client) {
    return client.Write(SmpGroupId::CALIBRATION, std::to_underlying(Command::STOP), CborMap().Build());
}

Response Read(SmpTestClient& client) {
    return client.Read(SmpGroupId::CALIBRATION, std::to_underlying(Command::READ));
}

void AssertError(const Response& response, Error error) {
    zassert_equal(response.error_group.value_or(0), std::to_underlying(SmpGroupId::CALIBRATION));
    zassert_equal(
        response.error.value_or(0), std::to_underlying(error), "Expected error %u, got %u", std::to_underlying(error),
        response.error.value_or(0)
    );
}

void AssertNotServed(SmpTestClient& client, const char* message) {
    const auto response = Read(client);
    zassert_false(response.IsOk(), "%s", message);
    zassert_equal(response.Value("rc"), MGMT_ERR_ENOTSUP, "%s", message);
}

} // namespace

// The controller serves calibration over SMP once the service to pause is known, with the unit's
// channel count, and takes the group with it: a second controller registers it again.
ZTEST(adc_controller, test_the_calibration_group_lives_with_the_controller) {
    // The smp scenario leaves the failing ADC out on every board (RT_CORE_TEST_NO_FAILING_ADC); a
    // build that keeps it fails here, so the case cannot go missing as a skip.
    static_assert(!kDtHasFailingAdc, "the smp scenario must build without the ADC that fails to initialize");

    auto fixture = MakeFixture();
    SmpTestClient client;

    for(int round = 0; round < 2; round++) {
        auto paused_service = std::make_shared<RecordingService>();

        {
            AdcController controller(fixture.fs_service, fixture.work_queue, nullptr, MakeTimeService());
            zassert_equal(controller.Initialize(), 0);
            AssertNotServed(client, "Served before the service to pause is known");

            controller.RegisterPausedService(paused_service);
            AssertError(Read(client), Error::NOT_RUNNING);

            AssertError(Start(client, static_cast<uint32_t>(kDtChannelCount)), Error::BAD_CHANNEL);
            zassert_true(Start(client, static_cast<uint32_t>(kDtChannelCount) - 1).IsOk(), "round %d", round);
            zassert_true(controller.GetCalibrationService()->IsRunning());
            zassert_true(paused_service->IsPaused());

            const auto running = Read(client);
            zassert_true(running.IsOk());
            zassert_equal(running.Value("channel"), kDtChannelCount - 1);
            zassert_true(running.Flag("running"));

            zassert_true(Stop(client).IsOk());
            zassert_true(paused_service->IsRunning());
            zassert_equal(paused_service->resumes, 1);
        }

        AssertNotServed(client, "The destroyed controller left its group registered");
    }

    // Registered first, here as a unit with nothing to pause: served from Initialize() on.
    AdcController controller(fixture.fs_service, fixture.work_queue, nullptr, MakeTimeService());
    controller.RegisterPausedService(nullptr);
    zassert_equal(controller.Initialize(), 0);
    AssertError(Read(client), Error::NOT_RUNNING);
    zassert_true(Start(client, 0).IsOk());
    zassert_true(Stop(client).IsOk());
}
#endif
