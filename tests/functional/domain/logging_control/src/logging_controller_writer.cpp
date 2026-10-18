#include "logging_controller_test_support.h"

// The controller in the writer role, as rt_daq builds it: the logging domain is on, and the log
// volume is the test's internal file system. The remote role's file is absent from this scenario,
// and this one from the base scenario.
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING

    #include <atomic>
    #include <memory>
    #include <optional>
    #include <vector>

    #include <zephyr/ztest.h>

    #ifdef CONFIG_EERIE_LEAP_ASSETS
        #include <string>

        #include "subsys/assets/asset_kind.h"
        #include "subsys/assets/asset_provider.h"
        #include "subsys/assets/asset_store.h"
        #include "subsys/assets/assets_manager.h"
    #endif

    #include "configuration/cbor/cbor_logging_config/cbor_logging_config.h"
    #include "domain/logging_domain/configuration/logging_configuration_manager.h"
    #include "domain/logging_domain/services/log_writer_supervisor.h"
    #include "domain/system_domain/models/product_feature.h"

using namespace logging_controller_test;

using eerie_leap::domain::logging_domain::configuration::LoggingConfigurationManager;
using eerie_leap::domain::logging_domain::control::WriterLoggingControl;
using eerie_leap::domain::system_domain::models::HasFeature;
using eerie_leap::domain::system_domain::models::ProductFeature;
using eerie_leap::subsys::threading::IService;
using eerie_leap::utilities::memory::Mrm;

namespace {

// Built per case: with the unit on the CDMP bus, or alone.
struct Fixture {
    std::unique_ptr<Harness> harness;
    std::unique_ptr<ControllerRig> rig;
    std::unique_ptr<Peer> peer;
    std::unique_ptr<LoggingController> controller;

    // @param has_com Whether the unit goes online with a scripted peer.
    void Open(bool has_com) {
        if(has_com)
            harness = std::make_unique<Harness>();

        // A Harness formats the file system itself.
        rig = std::make_unique<ControllerRig>(std::initializer_list<SensorSpec>{ { "switch" }, { "ab" } }, has_com);

        if(has_com) {
            harness->BringOnline();
            harness->Settle();
            peer = std::make_unique<Peer>(*harness);
        }
    }

    Parts MakeParts() const {
        Parts parts;
        parts.com = harness != nullptr ? harness->com : nullptr;

        return parts;
    }

    LoggingController& Build(Parts parts) {
        controller = MakeController(*rig, std::move(parts));

        return *controller;
    }

    // Builds and initializes it.
    void Start(const LoggingController::ConfigurationSetup& setup = nullptr) {
        zassert_equal(Build(MakeParts()).Initialize(setup), 0, "The controller initializes");
    }

    // Everything the failed Initialize() promises: every getter is null and nothing is registered.
    static void AssertNothingBuilt(
        const LoggingController& controller,
        const std::shared_ptr<ConfigurationService>& configuration_service
    ) {

        zassert_is_null(controller.GetControl().get());
        zassert_is_null(controller.GetLoggingControlConfigurationManager().get());
        zassert_is_null(controller.GetConfigurationManager().get());
        zassert_is_null(controller.GetLogWriterService().get());
        zassert_is_null(controller.GetDependentService().get());
        zassert_false(configuration_service->IsRegistered(ConfigurationService::Type::Logging));
        zassert_false(configuration_service->IsRegistered(ConfigurationService::Type::LoggingControl));
    }
};

Fixture*& Current() {
    static Fixture* fixture = nullptr;

    return fixture;
}

void* SetUp() {
    return SetUpChannels();
}

void Before(void*) {
    Current() = new Fixture();
}

void After(void*) {
    Gate().Open();

    // The controller before what it stands on.
    if(Current() != nullptr) {
        Current()->controller.reset();
        Current()->peer.reset();
        Current()->rig.reset();
        Current()->harness.reset();
        delete Current();
        Current() = nullptr;
    }

    ResetLoopbackDevice();
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(logging_controller_writer, NULL, SetUp, Before, After, NULL);

ZTEST(logging_controller_writer, test_TheWriterRoleBuildsItsPieces) {
    F().Open(false);
    StatusLog log;

    auto parts = F().MakeParts();
    const auto configuration_service = parts.configuration_service;
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0);
    auto& controller = *F().controller;

    const auto control = controller.GetControl();
    zassert_not_null(
        dynamic_cast<WriterLoggingControl*>(control.get()), "The writer role's control: the writer plus the network"
    );
    zassert_not_null(controller.GetLoggingControlConfigurationManager().get());
    zassert_not_null(controller.GetConfigurationManager().get());
    zassert_not_null(controller.GetDependentService().get());
    const auto writer = controller.GetLogWriterService();
    zassert_not_null(writer.get());
    zassert_true(writer->IsStopped(), "The writer is built and stopped");
    zassert_true(configuration_service->IsRegistered(ConfigurationService::Type::Logging));
    zassert_true(configuration_service->IsRegistered(ConfigurationService::Type::LoggingControl));

    StoreLoggingConfiguration(controller, { "switch" });

    control->RequestStart(ALL_UNITS);
    auto entry = log.WaitForEntry();
    zassert_true(entry.has_value() && entry->is_active, "A request starts the writer");
    zassert_true(WaitUntil([&writer] {
        return writer->IsRunning();
    }));
    zassert_true(
        WaitUntil([&control] {
            return control->IsActive();
        }),
        "and the control follows the status"
    );

    control->RequestStop(ALL_UNITS);
    entry = log.WaitForEntry();
    zassert_true(entry.has_value() && !entry->is_active, "and stops it");
    zassert_equal(entry->reason, LoggingStatusReason::Stopped);
    zassert_true(WaitUntil([&writer] {
        return writer->IsStopped();
    }));
}

// The stored file name is part of the format: rt_daq stored its logging configuration under it before
// the controller moved to rt_core, and a rename would drop that configuration without a word.
ZTEST(logging_controller_writer, test_TheStoredLoggingConfigurationOfRtDaqStillLoads) {
    F().Open(false);

    {
        // A literal, not the controller's constant: the name is what is pinned.
        LoggingConfigurationManager stored(
            std::make_unique<CborConfigurationService<CborLoggingConfig>>(
                "logging_config", F().rig->fs_service, TestConfigQueue()
            )
        );

        LoggingConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
        configuration.logging_interval_ms = 1234;
        configuration.max_log_size_mb = 0;
        configuration.sensor_configurations.emplace(
            StringHelpers::GetHash("switch"), SensorLoggingConfiguration{ .is_enabled = true }
        );
        zassert_true(stored.Update(configuration), "The configuration is stored");
    }

    F().Start();

    zassert_equal(
        F().controller->GetConfigurationManager()->Get()->logging_interval_ms, 1234U,
        "The controller loaded the configuration stored under its file name"
    );
}

ZTEST(logging_controller_writer, test_ACdmpStartIsAcceptedAndShared) {
    F().Open(true);
    F().Start();
    StoreLoggingConfiguration(*F().controller, { "switch" });
    const auto writer = F().controller->GetLogWriterService();

    zassert_true(F().peer->WaitForRecord({ 0x00, 0x00 }), "The unit shares that it does not log");

    const auto started = F().peer->Ask({ 0x01 });
    zassert_true(started.has_value(), "The start is answered");
    zassert_equal(*started, CdmpResultCode::SUCCESS);
    zassert_true(
        WaitUntil([&writer] {
            return writer->IsRunning();
        }),
        "and the writer starts"
    );
    zassert_true(F().peer->WaitForRecord({ 0x01, 0x00 }), "The unit shares that it logs");

    const auto stopped = F().peer->Ask({ 0x00 });
    zassert_true(stopped.has_value(), "The stop is answered");
    zassert_equal(*stopped, CdmpResultCode::SUCCESS);
    zassert_true(F().peer->WaitForRecord({ 0x00, 0x00 }), "and the unit shares that it stopped");
    zassert_true(WaitUntil([&writer] {
        return writer->IsStopped();
    }));

    // The log names this unit and the tool the controller was given.
    const auto content = ReadTheLogFile(*F().rig->fs_service);
    zassert_false(content.empty(), "The session wrote a file");
    zassert_true(
        ContainsText(content, Property("device_id", std::to_string(OWN_ID))),
        "The log header carries the CDMP device ID of this unit"
    );
    zassert_true(
        ContainsText(content, std::string("<tool_id>") + CONTROLLER_TOOL_ID + "</tool_id>"), "and the tool ID"
    );
}

// The LOGGING feature this unit sends on CDMP follows the log volume: senders skip a unit that cannot
// log. It is set before the first heartbeat, polled once a second, and cleared with the controller.
ZTEST(logging_controller_writer, test_TheLoggingFeatureFollowsTheLogVolume) {
    F().Open(true);
    const auto volume = std::make_shared<SwitchableVolume>(F().rig->fs_service);
    const auto com = F().harness->com;
    zassert_false(HasFeature(com->GetFeatures(), ProductFeature::LOGGING), "Cleared until a writer exists");

    auto parts = F().MakeParts();
    parts.log_volume = volume;
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0);

    zassert_true(HasFeature(com->GetFeatures(), ProductFeature::LOGGING), "Set as soon as the controller is built");
    zassert_true(HasFeature(com->GetFeatures(), ProductFeature::CANBUS), "The other features stay");

    volume->SetAvailable(false);
    zassert_true(
        WaitUntil(
            [&com] {
                return !HasFeature(com->GetFeatures(), ProductFeature::LOGGING);
            },
            2500
        ),
        "The volume going away clears the feature within a poll or two"
    );
    zassert_true(HasFeature(com->GetFeatures(), ProductFeature::CANBUS));

    volume->SetAvailable(true);
    zassert_true(
        WaitUntil(
            [&com] {
                return HasFeature(com->GetFeatures(), ProductFeature::LOGGING);
            },
            2500
        ),
        "and the volume coming back sets it again"
    );

    F().controller.reset();
    zassert_false(HasFeature(com->GetFeatures(), ProductFeature::LOGGING), "Tearing down the writer clears it");
}

ZTEST(logging_controller_writer, test_WithoutAnAvailableLogVolumeTheFeatureStartsCleared) {
    F().Open(true);
    const auto volume = std::make_shared<SwitchableVolume>(F().rig->fs_service);
    volume->SetAvailable(false);
    const auto com = F().harness->com;

    auto parts = F().MakeParts();
    parts.log_volume = volume;
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0);

    zassert_false(HasFeature(com->GetFeatures(), ProductFeature::LOGGING));
}

ZTEST(logging_controller_writer, test_WithoutAComServiceTriggersStillLog) {
    F().Open(false);
    StatusLog log;
    F().Start(StoresTriggersOn({ "switch" }));
    StoreLoggingConfiguration(*F().controller, { "switch" });

    PublishSensorValue("switch", 1.0F);
    auto entry = log.WaitForEntry();
    zassert_true(entry.has_value() && entry->is_active, "A trigger starts the writer");

    PublishSensorValue("switch", 0.0F);
    entry = log.WaitForEntry();
    zassert_true(entry.has_value() && !entry->is_active, "and stops it");
    zassert_equal(entry->reason, LoggingStatusReason::Stopped);
}

ZTEST(logging_controller_writer, test_AStoredConfigurationRebuildsTheTriggersAndReplays) {
    F().Open(false);
    SensorProbe probe("ab");
    F().rig->Commit("ab", 1.0F);
    zassert_true(probe.WaitForValue(1.0F), "The publisher took the commit");

    StatusLog log;
    auto parts = F().MakeParts();
    const auto configuration_service = parts.configuration_service;
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0);
    StoreLoggingConfiguration(*F().controller, { "ab" });

    QueuedApply apply(configuration_service, ConfigurationService::Type::LoggingControl, TRIGGER_ON_AB);
    zassert_true(apply.WaitForSuccess(), "The configuration is stored");

    const auto entry = log.WaitForEntry();
    zassert_true(
        entry.has_value() && entry->is_active, "A switch that is on starts the writer once its trigger exists"
    );
}

ZTEST(logging_controller_writer, test_AFailedInitializeLeavesEveryGetterNull) {
    F().Open(false);

    // What the writer role cannot do without: each is refused before anything is built.
    const auto refused = [](Parts parts, const char* what) {
        const auto configuration_service = parts.configuration_service;
        auto& controller = F().Build(std::move(parts));
        zassert_equal(controller.Initialize(), -1, "The writer needs %s", what);
        Fixture::AssertNothingBuilt(controller, configuration_service);
        F().controller.reset();
    };

    auto parts = F().MakeParts();
    parts.has_log_volume = false;
    refused(std::move(parts), "a log volume");

    parts = F().MakeParts();
    parts.has_time_service = false;
    refused(std::move(parts), "a time service");

    parts = F().MakeParts();
    parts.has_frame = false;
    refused(std::move(parts), "the readings frame");

    parts = F().MakeParts();
    parts.has_sensors_manager = false;
    refused(std::move(parts), "the sensors");

    // A queue that never ran: the managers and the writer are built, then the supervisor refuses.
    parts = F().MakeParts();
    parts.queue = std::make_shared<WorkQueueThread>("never_ran", 4096, 5);
    const auto second_service = parts.configuration_service;
    auto& without_queue = F().Build(std::move(parts));
    zassert_equal(without_queue.Initialize(), -1, "The supervisor needs a running queue");
    Fixture::AssertNothingBuilt(without_queue, second_service);

    F().controller.reset();
}

ZTEST(logging_controller_writer, test_DestroyingWhileLoggingStopsTheWriter) {
    F().Open(false);
    StatusLog log;
    F().Start(StoresTriggersOn({ "switch" }));
    StoreLoggingConfiguration(*F().controller, { "switch" });
    const auto writer = F().controller->GetLogWriterService();
    const auto dependent = F().controller->GetDependentService();

    PublishSensorValue("switch", 1.0F);
    auto entry = log.WaitForEntry();
    zassert_true(entry.has_value() && entry->is_active);
    zassert_true(WaitUntil([&writer] {
        return writer->IsRunning();
    }));

    LogCounter::Get().Watch("logging_controller_logger");
    F().controller.reset();
    ZASSERT_LOGGED(0, "The supervisor's queued stop stopped the writer; the direct stop was not needed");

    zassert_true(writer->IsStopped(), "The writer is never dropped while it runs");
    entry = log.WaitForEntry();
    zassert_true(entry.has_value() && !entry->is_active, "and its stop is published");
    zassert_equal(entry->reason, LoggingStatusReason::Stopped);

    // A sensors restart still holds the dependent: it must not bring the writer back.
    const auto result = std::make_shared<std::atomic<int>>(-1);
    TestConfigQueue()->Run([dependent, result] {
        result->store(dependent->Start() ? 1 : 0);
    });
    zassert_true(Flush());
    zassert_equal(result->load(), 1, "The restart's start is a no-op");
    zassert_true(writer->IsStopped(), "because the stop went through the supervisor and cleared the wanted state");
}

// The destructor waits for the configuration work queue to run the stop. On that queue it cannot,
// so it stops the writer itself instead of waiting for itself forever.
ZTEST(logging_controller_writer, test_DestroyingOnTheConfigurationQueueStopsTheWriterWithoutWaiting) {
    F().Open(false);
    StatusLog log;
    F().Start(StoresTriggersOn({ "switch" }));
    StoreLoggingConfiguration(*F().controller, { "switch" });
    const auto writer = F().controller->GetLogWriterService();

    PublishSensorValue("switch", 1.0F);
    auto entry = log.WaitForEntry();
    zassert_true(entry.has_value() && entry->is_active);
    zassert_true(WaitUntil([&writer] {
        return writer->IsRunning();
    }));

    // The job destroys it, so the holder is the only owner and the destructor runs inside the job.
    struct Context {
        std::unique_ptr<LoggingController> controller;
        k_sem done{};
    };
    const auto context = std::make_shared<Context>();
    k_sem_init(&context->done, 0, 1);
    context->controller = std::move(F().controller);

    LogCounter::Get().Watch("logging_controller_logger");
    TestConfigQueue()->Run([context] {
        context->controller.reset();
        k_sem_give(&context->done);
    });
    zassert_equal(k_sem_take(&context->done, Ms(WAIT_MS)), 0, "The destructor returned on the queue");
    ZASSERT_LOGGED(2, "On the queue the destructor says it cannot wait, then stops the writer directly");

    zassert_true(writer->IsStopped(), "and left the writer stopped");
    entry = log.WaitForEntry();
    zassert_true(entry.has_value() && !entry->is_active, "with its stop published");
    zassert_true(Flush(), "The queue is free");
}

ZTEST(logging_controller_writer, test_DestroyingWhileLoggingWithdrawsTheCommand) {
    F().Open(true);
    F().Start();
    StoreLoggingConfiguration(*F().controller, { "switch" });
    const auto writer = F().controller->GetLogWriterService();

    const auto started = F().peer->Ask({ 0x01 });
    zassert_true(started.has_value() && *started == CdmpResultCode::SUCCESS);
    zassert_true(WaitUntil([&writer] {
        return writer->IsRunning();
    }));

    LogCounter::Get().Watch("logging_controller_logger");
    F().controller.reset();
    ZASSERT_LOGGED(0, "The supervisor's queued stop stopped the writer; the direct stop was not needed");

    zassert_true(writer->IsStopped(), "The writer is stopped with the controller");

    // The command goes with the handler, and nothing is left to start the writer again.
    const auto again = F().peer->Ask({ 0x01 });
    zassert_true(again.has_value(), "The unit still answers");
    zassert_equal(*again, CdmpResultCode::UNSUPPORTED_COMMAND, "but no one serves LOGGING any more");
    zassert_true(Flush());
    zassert_true(writer->IsStopped());
}

// The log writer's logger factory keeps the device-ID provider, and a dependent that SensorsController
// holds keeps the writer, so the provider outlives the controller. It must not keep the COM service,
// which the unit's CAN reconfiguration destroys.
ZTEST(logging_controller_writer, test_TheDeviceIdProviderDoesNotKeepTheComServiceAlive) {
    F().Open(true);
    F().Start();
    const auto dependent = F().controller->GetDependentService();
    zassert_not_null(dependent.get());
    const std::weak_ptr<CanbusComService> weak_com = F().harness->com;

    F().controller.reset();
    F().peer.reset();
    F().harness.reset();

    zassert_true(weak_com.expired(), "Nothing of the controller holds the COM service");
}

    #ifdef CONFIG_EERIE_LEAP_ASSETS

// ======== The log files as an asset kind ========

namespace {

using eerie_leap::subsys::assets::AssetAccess;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetProvider;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::assets::ToMask;

// A log name the writer could have given a file, which no file of a case has.
constexpr const char* OTHER_LOG_NAME = "log_data_20000101T000000Z_0000.mf4";

// The name of the one log file on the volume; empty when there is none, or more than one.
std::string TheLogFileName(FsService& fs_service) {
    if(!fs_service.Exists(LogFileNaming::LOGS_DIR))
        return {};

    const auto names = fs_service.ListFiles(LogFileNaming::LOGS_DIR);

    return names.size() == 1 ? names.front() : std::string();
}

// Builds and initializes the controller with a registry, with a trigger on "switch" stored first and a
// logging configuration that logs it.
std::shared_ptr<AssetsManager> StartWithAssets() {
    auto assets = std::make_shared<AssetsManager>();

    auto parts = F().MakeParts();
    parts.assets_manager = assets;
    zassert_equal(
        F().Build(std::move(parts)).Initialize(StoresTriggersOn({ "switch" })), 0, "The controller initializes"
    );
    StoreLoggingConfiguration(*F().controller, { "switch" });

    return assets;
}

} // namespace

ZTEST(logging_controller_writer, test_TheLogsAreRegisteredOnceTheControllerIsInitialized) {
    F().Open(false);
    const auto assets = std::make_shared<AssetsManager>();
    const auto volume = std::make_shared<SwitchableVolume>(F().rig->fs_service);

    auto parts = F().MakeParts();
    parts.assets_manager = assets;
    parts.log_volume = volume;
    auto& controller = F().Build(std::move(parts));
    zassert_true(assets->GetProviders().empty(), "Nothing is registered before Initialize()");
    zassert_equal(controller.Initialize(), 0);

    const auto provider = assets->Find(AssetKind::LOGS);
    zassert_not_null(provider.get(), "The logs are registered once the controller is initialized");
    zassert_equal(provider->kind, AssetKind::LOGS);
    zassert_true(provider->name == "logs", "The kind's name");
    zassert_equal(provider->extensions.size(), 1);
    zassert_true(provider->extensions[0] == ".mf4", "An MDF 4 file");
    zassert_equal(
        provider->access, ToMask(AssetAccess::READ) | ToMask(AssetAccess::DELETE), "Read and delete, no upload"
    );
    zassert_equal(provider->max_size, 0U, "A log is not uploaded");
    zassert_not_null(provider->store.get());
    zassert_true(provider->store->GetDirectory() == LogFileNaming::LOGS_DIR, "The directory the writer logs to");
    zassert_true(static_cast<bool>(provider->is_in_use), "The file being written is not read or deleted");

    zassert_true(
        F().rig->fs_service->Exists(LogFileNaming::LOGS_DIR), "The store creates its directory on a mounted volume"
    );
    zassert_equal(assets->GetProviders().size(), 1, "Only the logs are the controller's");

    // The store is on the log volume the controller was given, not on its internal file system.
    zassert_true(provider->store->IsAvailable());
    volume->SetAvailable(false);
    zassert_false(provider->store->IsAvailable(), "The store follows the log volume");
    zassert_true(F().rig->fs_service->IsAvailable(), "and not the internal file system");
    volume->SetAvailable(true);
    zassert_true(provider->store->IsAvailable());
}

ZTEST(logging_controller_writer, test_TheLogBeingWrittenIsInUseAndNoOtherIs) {
    F().Open(false);
    StatusLog log;
    const auto assets = StartWithAssets();
    const auto writer = F().controller->GetLogWriterService();
    const auto provider = assets->Find(AssetKind::LOGS);
    zassert_not_null(provider.get());

    zassert_false(provider->is_in_use(AssetAccess::READ, OTHER_LOG_NAME), "No file is open before the first start");

    PublishSensorValue("switch", 1.0F);
    auto entry = log.WaitForEntry();
    zassert_true(entry.has_value() && entry->is_active, "A trigger starts the writer");
    zassert_true(WaitUntil([&writer] {
        return writer->IsRunning();
    }));

    const std::string name = TheLogFileName(*F().rig->fs_service);
    zassert_false(name.empty(), "The writer has one file open");

    zassert_true(provider->is_in_use(AssetAccess::READ, name), "The file being written is not read");
    zassert_true(provider->is_in_use(AssetAccess::DELETE, name), "and not deleted");
    zassert_false(provider->is_in_use(AssetAccess::WRITE, name), "Nothing writes a log but the writer");
    zassert_false(provider->is_in_use(AssetAccess::READ, OTHER_LOG_NAME), "Another log is free to read");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, OTHER_LOG_NAME), "and to delete");

    PublishSensorValue("switch", 0.0F);
    entry = log.WaitForEntry();
    zassert_true(entry.has_value() && !entry->is_active, "A trigger stops the writer");
    zassert_true(WaitUntil([&writer] {
        return writer->IsStopped();
    }));

    zassert_false(provider->is_in_use(AssetAccess::READ, name), "Once the writer stopped, nothing is in use");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, name));
}

ZTEST(logging_controller_writer, test_DestroyingTheControllerUnregistersTheLogs) {
    F().Open(false);
    StatusLog log;
    const auto assets = StartWithAssets();
    const auto writer = F().controller->GetLogWriterService();
    const auto provider = assets->Find(AssetKind::LOGS);
    zassert_not_null(provider.get());

    PublishSensorValue("switch", 1.0F);
    const auto entry = log.WaitForEntry();
    zassert_true(entry.has_value() && entry->is_active);
    zassert_true(WaitUntil([&writer] {
        return writer->IsRunning();
    }));
    const std::string name = TheLogFileName(*F().rig->fs_service);
    zassert_false(name.empty());
    zassert_true(provider->is_in_use(AssetAccess::DELETE, name));

    F().controller.reset();

    zassert_is_null(assets->Find(AssetKind::LOGS).get(), "The logs are no longer served");
    zassert_true(writer->IsStopped(), "The writer was stopped with the controller");
    zassert_false(
        provider->is_in_use(AssetAccess::DELETE, name),
        "A provider found before refuses nothing once the writer closed the file"
    );
}

// The controller keeps nothing of the registry alive: the registry owns the provider.
ZTEST(logging_controller_writer, test_TheProviderDoesNotKeepTheRegistryAlive) {
    F().Open(false);
    auto assets = StartWithAssets();
    const auto provider = assets->Find(AssetKind::LOGS);
    zassert_not_null(provider.get());

    F().controller.reset();
    const std::weak_ptr<AssetsManager> weak_assets = assets;
    assets.reset();

    zassert_true(weak_assets.expired(), "Nothing the controller registered holds the registry");
    zassert_false(
        provider->is_in_use(AssetAccess::READ, OTHER_LOG_NAME), "A provider outliving its registry refuses nothing"
    );
}

ZTEST(logging_controller_writer, test_AControllerThatFailedToInitializeRegistersNothing) {
    F().Open(false);
    const auto assets = std::make_shared<AssetsManager>();

    auto parts = F().MakeParts();
    parts.assets_manager = assets;
    parts.has_time_service = false;
    zassert_equal(F().Build(std::move(parts)).Initialize(), -1, "The writer needs a time service");
    zassert_true(assets->GetProviders().empty(), "A refused controller registers nothing");
    F().controller.reset();

    // A queue that never ran: everything is built, then the supervisor refuses.
    parts = F().MakeParts();
    parts.assets_manager = assets;
    parts.queue = std::make_shared<WorkQueueThread>("never_ran_assets", 4096, 5);
    zassert_equal(F().Build(std::move(parts)).Initialize(), -1, "The supervisor needs a running queue");
    zassert_true(assets->GetProviders().empty(), "and neither does one that failed after building the writer");
    F().controller.reset();

    zassert_true(assets->GetProviders().empty());
}

// A kind registered already stays with its owner: the controller serves nothing, and its teardown
// must not withdraw the other's.
ZTEST(logging_controller_writer, test_ALogsKindRegisteredAlreadyStaysWithItsOwner) {
    F().Open(false);
    const auto assets = std::make_shared<AssetsManager>();

    AssetProvider other;
    other.kind = AssetKind::LOGS;
    other.name = "logs";
    other.store = std::make_shared<AssetStore>(F().rig->fs_service, "other_logs");
    zassert_true(assets->Register(std::move(other)));
    const auto registered = assets->Find(AssetKind::LOGS);

    auto parts = F().MakeParts();
    parts.assets_manager = assets;
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0, "A kind registered already is no failure");
    zassert_equal(assets->Find(AssetKind::LOGS).get(), registered.get(), "The first registration stays");

    F().controller.reset();

    zassert_equal(
        assets->Find(AssetKind::LOGS).get(), registered.get(), "and the controller's end withdraws nothing of it"
    );
}

    #endif // CONFIG_EERIE_LEAP_ASSETS

#endif // CONFIG_EERIE_LEAP_DOMAIN_LOGGING
