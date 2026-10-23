#include "logging_controller_test_support.h"

// The controller in the remote role, as the gauge builds it: the logging domain is off. The
// role-independent behaviour (the trigger rebuild, the resolver, the teardown of both) is tested
// here, because this is the role without a log writer in the way.
#ifndef CONFIG_EERIE_LEAP_DOMAIN_LOGGING

    #include <memory>
    #include <optional>
    #include <vector>

    #include <zephyr/ztest.h>

    #include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
    #include "domain/logging_domain/control/remote_logging_control.h"
    #include "domain/logging_domain/event_bus/logging_event_type.h"
    #include "domain/logging_domain/event_bus/logging_payload_type.h"
    #include "domain/system_domain/models/product_feature.h"

using namespace logging_controller_test;

using eerie_leap::domain::logging_domain::control::RemoteLoggingControl;
using eerie_leap::domain::system_domain::models::HasFeature;
using eerie_leap::domain::system_domain::models::ProductFeature;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::string::StringHelpers;

namespace {

// The unit and a scripted peer on its bus, brought online, and the rig the controller is built on.
// Declared so that the controller goes first, then the rig, then the unit.
struct Fixture {
    Harness harness;
    ControllerRig rig;
    Peer peer;
    std::unique_ptr<LoggingController> controller;

    Fixture() :
        rig({ { "switch" }, { "a" }, { "b" }, { "ab" }, { "known" }, { "off", false } }, true),
        peer(harness) {

        harness.BringOnline();
        harness.Settle();
    }

    Parts MakeParts() const {
        Parts parts;
        parts.com = harness.com;

        return parts;
    }

    // Builds the controller of the case with the unit's COM service.
    LoggingController& Build(Parts parts) {
        controller = MakeController(rig, std::move(parts));

        return *controller;
    }

    // Builds and initializes it.
    void Start(const LoggingController::ConfigurationSetup& setup = nullptr) {
        zassert_equal(Build(MakeParts()).Initialize(setup), 0, "The controller initializes");
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

    // The controller before the rig and the unit it stands on.
    if(Current() != nullptr) {
        Current()->controller.reset();
        delete Current();
        Current() = nullptr;
    }

    ResetLoopbackDevice();
}

Fixture& F() {
    return *Current();
}

bool IsStart(const std::optional<RecordedFrame>& frame) {
    return frame.has_value() && frame->data[3] == 0x01;
}

bool IsStop(const std::optional<RecordedFrame>& frame) {
    return frame.has_value() && frame->data[3] == 0x00;
}

} // namespace

ZTEST_SUITE(logging_controller_remote, NULL, SetUp, Before, After, NULL);

ZTEST(logging_controller_remote, test_TheRemoteRoleBuildsItsPieces) {
    auto parts = F().MakeParts();
    const auto configuration_service = parts.configuration_service;

    zassert_equal(F().Build(std::move(parts)).Initialize(), 0);

    const auto control = F().controller->GetControl();
    zassert_not_null(dynamic_cast<RemoteLoggingControl*>(control.get()), "The remote role's control");
    zassert_not_null(F().controller->GetLoggingControlConfigurationManager().get());
    zassert_true(
        configuration_service->IsRegistered(ConfigurationService::Type::LoggingControl),
        "The triggers' configuration is registered"
    );
    zassert_false(
        configuration_service->IsRegistered(ConfigurationService::Type::Logging),
        "and a unit without a log writer has no logging configuration"
    );
    zassert_false(
        configuration_service->GetCborConfiguration(ConfigurationService::Type::LoggingControl).empty(),
        "The default configuration is stored"
    );

    // The sender is wired to the COM service.
    control->RequestStart(ALL_UNITS);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "A request goes out as the LOGGING broadcast");

    // The importer is wired into the control.
    zassert_false(control->IsActive());
    F().peer.ShareStatus({ 0x01 });
    zassert_true(
        WaitUntil([&control] {
            return control->IsActive();
        }),
        "The shared status reaches the control"
    );
}

    #ifdef CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS

// The gauge writes no logs, so a firmware update has none to end: no hook is registered, and an upload
// or a reset (here: the notifications, called directly) reaches nothing of the controller.
// A UI built after the last change of the status shows its default until the status changes again.
ZTEST(logging_controller_remote, test_RepublishStatusPublishesTheNetworksStatusAgain) {
    F().Start();
    StatusLog log;
    F().peer.ShareStatus({ 0x01 });
    zassert_true(
        WaitUntil([] {
            return F().controller->GetControl()->IsActive();
        }),
        "The shared status reaches the control"
    );

    // What the change itself published.
    while(log.WaitForEntry(NO_WAIT_MS).has_value()) {
    }

    F().controller->RepublishStatus();

    const auto entry = log.WaitForEntry();
    zassert_true(entry.has_value(), "The status is published again, although it did not change");
    zassert_true(entry->is_active);
}

// Not a unit's to fail: before there is a status to republish it does nothing.
ZTEST(logging_controller_remote, test_RepublishStatusBeforeInitializeDoesNothing) {
    F().Build(F().MakeParts());

    F().controller->RepublishStatus();
}

ZTEST(logging_controller_remote, test_TheRemoteRoleRegistersNoFirmwareUpdateHooks) {
    F().Start();
    const auto control = F().controller->GetControl();
    F().peer.ShareStatus({ 0x01 });
    zassert_true(
        WaitUntil([&control] {
            return control->IsActive();
        }),
        "The other unit logs"
    );

    zassert_false(F().controller->AreFirmwareUpdateHooksRegistered());

    const int64_t begin_ms = k_uptime_get();
    zassert_equal(NotifyUploadStarted(), MGMT_CB_OK);
        #ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
    zassert_equal(NotifyReset(), MGMT_CB_OK);
        #endif
    zassert_true(k_uptime_get() - begin_ms < 500, "and nothing waits");

    zassert_true(control->IsActive(), "The status of the logging unit is untouched");
    zassert_true(control->GetReason() == LoggingStatusReason::Stopped, "and so is its reason");
    zassert_false(F().peer.WaitForBroadcast(200).has_value(), "No stop is sent to the units that log");
}

    #endif // CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS

// A unit without a log writer never sends the LOGGING feature, so senders skip it.
ZTEST(logging_controller_remote, test_TheUnitNeverSendsTheLoggingFeature) {
    F().Start();
    F().controller->GetControl()->RequestStart(ALL_UNITS);
    zassert_true(IsStart(F().peer.WaitForBroadcast()));

    zassert_false(HasFeature(F().harness.com->GetFeatures(), ProductFeature::LOGGING));
}

ZTEST(logging_controller_remote, test_AWantedStartIsSentAgainWhenALoggingUnitAppears) {
    F().Start();

    // No unit shares a status.
    F().controller->GetControl()->RequestStart(ALL_UNITS);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "The start goes out");
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    // A logging unit appears, idle.
    F().peer.ShareStatus({ 0x00, 0x00 });
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "so the start that nobody received is sent again");
}

// The remote role wires the restart monitor: a logging unit that reboots within the heartbeat
// timeout, with its record in its first heartbeat, goes from logging straight to Stopped, which only
// the monitor sees.
ZTEST(logging_controller_remote, test_AQuickRestartOfTheLoggingUnitIsReasserted) {
    F().Start();

    F().controller->GetControl()->RequestStart(ALL_UNITS);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "The start goes out");

    F().peer.ShareStatus({ 0x01 });

    F().peer.Restart({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, true);
    F().peer.ShareStatus({ 0x00, 0x00 });
    const auto again = F().peer.WaitForCommandTo(PEER_ID);
    zassert_true(IsStart(again), "The restarted unit lost the start: it is sent again, to that unit alone");
    zassert_false(F().peer.WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");
}

ZTEST(logging_controller_remote, test_WithoutAComServiceInitializeFails) {
    Parts parts;
    const auto configuration_service = parts.configuration_service;

    auto& controller = F().Build(std::move(parts));

    zassert_equal(controller.Initialize(), -1, "The remote role is blind without the COM service");
    zassert_is_null(controller.GetControl().get());
    zassert_is_null(controller.GetLoggingControlConfigurationManager().get());
    zassert_false(configuration_service->IsRegistered(ConfigurationService::Type::LoggingControl));

    zassert_equal(controller.Initialize(), -1, "and stays so");

    // Destroyed in After(), as it was never initialized.
}

ZTEST(logging_controller_remote, test_AFailedInitializeLeavesNothingBehind) {
    // A queue that never ran: the control cannot create its task.
    auto parts = F().MakeParts();
    parts.queue = std::make_shared<WorkQueueThread>("never_ran", 4096, 5);
    const auto configuration_service = parts.configuration_service;

    auto& controller = F().Build(std::move(parts));

    zassert_equal(controller.Initialize(), -1);
    zassert_is_null(controller.GetControl().get());
    zassert_is_null(controller.GetLoggingControlConfigurationManager().get());
    zassert_false(
        configuration_service->IsRegistered(ConfigurationService::Type::LoggingControl),
        "A controller that did not initialize registers nothing"
    );

    F().controller.reset();
}

ZTEST(logging_controller_remote, test_AStoredConfigurationRebuildsTheTriggersAndReplays) {
    // The publisher takes the commit before any trigger exists, so only a replay can show it again.
    SensorProbe probe("ab");
    F().rig.Commit("ab", 1.0F);
    zassert_true(probe.WaitForValue(1.0F), "The publisher took the commit");

    auto parts = F().MakeParts();
    const auto configuration_service = parts.configuration_service;
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0);
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "No triggers, nothing to start");

    // The SMP path: a client stores the configuration, and the apply runs on the configuration queue.
    QueuedApply apply(configuration_service, ConfigurationService::Type::LoggingControl, TRIGGER_ON_AB);
    zassert_true(apply.WaitForSuccess(), "The configuration is stored");

    zassert_true(IsStart(F().peer.WaitForBroadcast()), "The rebuilt trigger sees a switch that is on");
    zassert_true(probe.WaitForValue(1.0F), "because the store replayed the value");
}

ZTEST(logging_controller_remote, test_TheSetupHookConfiguresTheFirstTriggers) {
    const uint32_t generation = SensorSubscriptionGeneration();
    F().Start(StoresTriggersOn({ "a" }));

    PublishSensorValue("a", 1.0F);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "The hook's trigger starts logging");

    // The hook stored before the handler existed: no rebuild was queued, so the one subscription
    // since the start is the boot set's.
    zassert_true(Flush());
    zassert_equal(SensorSubscriptionGeneration(), generation + 1, "The hook's store rebuilt nothing");
}

ZTEST(logging_controller_remote, test_AStoredConfigurationReplacesTheTriggers) {
    F().Start(StoresTriggersOn({ "a" }));

    const auto manager = F().controller->GetLoggingControlConfigurationManager();
    zassert_true(manager->Update(TriggersOn({ "b" })), "The new triggers are stored");
    zassert_true(Flush(), "The rebuild ran");

    PublishSensorValue("a", 1.0F);
    zassert_true(Flush());
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "The old trigger is gone");

    PublishSensorValue("b", 1.0F);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "and the new one acts");
}

// L6: the store returns at once and the rebuild runs on the queue, once for stores that come before
// it runs.
ZTEST(logging_controller_remote, test_StoresCoalesceIntoOneRebuildOnTheQueue) {
    F().Start();
    const auto manager = F().controller->GetLoggingControlConfigurationManager();
    const uint32_t generation = SensorSubscriptionGeneration();

    HeldStore store(manager, { { "a" }, { "b" } });
    zassert_true(store.WaitUntilStored(), "Both stores are done");
    zassert_true(store.AllStored());
    zassert_equal(
        SensorSubscriptionGeneration(), generation, "The rebuild is pending behind the stores, not run by them"
    );

    store.Release();
    zassert_true(Flush(), "The rebuild ran");

    // From no triggers to {"b"}: one subscription. A rebuild per store would have added and
    // dropped {"a"} on the way.
    zassert_equal(SensorSubscriptionGeneration(), generation + 1, "One rebuild for both stores");

    PublishSensorValue("a", 1.0F);
    zassert_true(Flush());
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "The first store never acted");

    PublishSensorValue("b", 1.0F);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "The last one did");
}

ZTEST(logging_controller_remote, test_TriggersAreCheckedAgainstTheSensors) {
    // "off" is disabled, "missing" does not exist; the integer selector names "known" by its hash, and
    // a trigger off the Sensors channel is not checked.
    LoggingControlConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    configuration.triggers.push_back(SensorTrigger("known"));
    configuration.triggers.push_back(SensorTrigger("missing"));
    configuration.triggers.push_back(SensorTrigger("off"));

    auto by_hash = SensorTrigger("known");
    by_hash.event.selector_value = static_cast<int>(StringHelpers::GetHash("known"));
    configuration.triggers.push_back(std::move(by_hash));

    LoggingControlBinding status;
    status.event.channel = EventChannelId::Logging;
    status.event.event_type = static_cast<uint32_t>(LoggingEventType::StatusUpdated);
    status.event.payload_key = static_cast<uint32_t>(LoggingPayloadType::IsActive);
    configuration.triggers.push_back(std::move(status));

    LogCounter::Get().Watch("logging_trigger_set_logger");
    F().Start([&configuration](const std::shared_ptr<LoggingControlConfigurationManager>& manager) {
        zassert_true(manager->Update(configuration), "The triggers are stored");
    });
    ZASSERT_LOGGED(2, "A trigger on a missing sensor and one on a disabled sensor are logged, the rest are not");

    // Without a sensors manager nothing can be checked.
    F().controller.reset();
    LogCounter::Get().Watch("logging_trigger_set_logger");
    auto parts = F().MakeParts();
    parts.has_sensors_manager = false;
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0);
    zassert_equal(
        F().controller->GetLoggingControlConfigurationManager()->Get()->triggers.size(), 5U,
        "The first controller's five triggers load again"
    );
    ZASSERT_LOGGED(0, "Triggers are not checked without the sensors");
}

ZTEST(logging_controller_remote, test_DestroyingDuringATriggerDispatchRequestsNothing) {
    // Subscribed first, so it runs before the trigger in a dispatch.
    SensorsBlocker blocker(SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated);

    F().Start(StoresTriggersOn({ "switch" }));

    blocker.Arm();
    PublishSensorValue("switch", 1.0F, true);
    zassert_true(blocker.WaitUntilBlocking(), "The bus thread is inside the dispatch, before the trigger");

    F().controller.reset();

    blocker.Release();
    zassert_true(blocker.Fence(), "The dispatch ended");
    zassert_true(Flush());
    zassert_false(
        F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(),
        "The trigger the dispatch had already taken requests nothing from a destroyed controller"
    );
}

ZTEST(logging_controller_remote, test_DestroyingWithARebuildPendingBuildsNothing) {
    F().Start();
    const auto manager = F().controller->GetLoggingControlConfigurationManager();
    const uint32_t generation = SensorSubscriptionGeneration();

    // The store queues the rebuild behind itself.
    HeldStore store(manager, { { "switch" } });
    zassert_true(store.WaitUntilStored());
    zassert_equal(SensorSubscriptionGeneration(), generation, "The rebuild is pending");

    F().controller.reset();
    store.Release();
    zassert_true(Flush());

    zassert_equal(SensorSubscriptionGeneration(), generation, "The pending rebuild never ran");
    PublishSensorValue("switch", 1.0F);
    zassert_true(Flush());
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "and nothing acts on the stored trigger");

    // The test's manager outlives the controller: a store reaches nothing.
    zassert_true(manager->Update(TriggersOn({ "switch" })), "A store after the destruction still works");
    zassert_true(Flush());
    zassert_equal(SensorSubscriptionGeneration(), generation, "and rebuilds nothing");
    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value());
}

// The boot replay: a controller built after the sensors started still sees a switch that is on.
ZTEST(logging_controller_remote, test_ASwitchThatIsAlreadyOnStartsAtBoot) {
    SensorProbe probe("switch");
    F().rig.Commit("switch", 1.0F);
    zassert_true(probe.WaitForValue(1.0F), "The publisher took the commit");

    // Without triggers nothing is replayed, and nothing starts.
    F().Start();
    zassert_true(Flush());
    zassert_false(probe.WaitForValue(1.0F, NO_FRAME_TIMEOUT_MS), "A controller without triggers replays nothing");
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value());
    F().controller.reset();

    F().Start(StoresTriggersOn({ "switch" }));
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "A switch that is already on starts logging");
    zassert_true(probe.WaitForValue(1.0F), "because the boot replayed it");
}

ZTEST(logging_controller_remote, test_ControllersComeAndGoWhileTheUnitRuns) {
    SensorProbe probe("switch");

    for(int round = 0; round < 3; round++) {
        F().Start(StoresTriggersOn({ "switch" }));

        // The unit hears its own broadcasts on the loopback bus: each is answered before the case goes on.
        F().rig.Commit("switch", 1.0F);
        const auto start = F().peer.WaitForBroadcast();
        zassert_true(IsStart(start), "Round %d starts", round);
        zassert_true(F().peer.WaitForOwnAnswer(*start));
        F().rig.Commit("switch", 0.0F);
        const auto stop = F().peer.WaitForBroadcast();
        zassert_true(IsStop(stop), "Round %d stops", round);
        zassert_true(F().peer.WaitForOwnAnswer(*stop));

        // A store that rebuilds the triggers and replays, with the publisher still ticking, and the
        // controller destroyed at once: the job holds the manager, not the controller.
        HeldStore store(F().controller->GetLoggingControlConfigurationManager(), { { "switch" } });
        store.Release();
        F().controller.reset();

        zassert_true(Flush(), "Round %d: the queue is idle", round);
        F().harness.bus.Clear();
    }

    // Nothing of the three controllers is left to act. What is delivered now is a replay of the last
    // value, 0, so the 1 that is waited for is this commit.
    probe.Clear();
    F().rig.Commit("switch", 1.0F);
    zassert_true(probe.WaitForValue(1.0F), "The publisher took the commit");
    F().rig.Commit("switch", 0.0F);
    zassert_true(probe.WaitForValue(0.0F));
    zassert_true(Flush());
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "No trigger survived its controller");
}

// The stored file name is part of the format: the controller loads what a manager stored under it.
ZTEST(logging_controller_remote, test_StoredTriggersLoadWithoutTheHook) {
    {
        // A literal, not the controller's constant: the name is what is pinned.
        LoggingControlConfigurationManager stored(
            std::make_unique<CborConfigurationService<CborLoggingControlConfig>>(
                "logging_control_config", F().rig.fs_service, TestConfigQueue()
            )
        );
        zassert_true(stored.Update(TriggersOn({ "switch" })), "The triggers are stored");
    }

    F().Start();

    PublishSensorValue("switch", 1.0F);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "The controller loaded the triggers stored under its file name");
}

// A unit whose internal file system cannot be read or written has no stored configuration at all.
ZTEST(logging_controller_remote, test_WithoutAWritableFileSystemThereAreNoTriggers) {
    auto parts = F().MakeParts();
    parts.fs_service = std::make_shared<UnwritableFsService>();
    const auto configuration_service = parts.configuration_service;
    const uint32_t generation = SensorSubscriptionGeneration();

    LogCounter::Get().Watch("logging_controller_logger");
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0, "The unit still controls logging");
    ZASSERT_LOGGED(1, "The missing configuration is logged once");

    const auto manager = F().controller->GetLoggingControlConfigurationManager();
    zassert_not_null(manager.get());
    zassert_is_null(manager->Get().get(), "Nothing could be stored or loaded");
    zassert_true(configuration_service->IsRegistered(ConfigurationService::Type::LoggingControl));
    zassert_equal(SensorSubscriptionGeneration(), generation, "An empty set subscribes nothing");

    F().controller->GetControl()->RequestStart(ALL_UNITS);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "Requests still go out");

    zassert_false(manager->Update(TriggersOn({ "switch" })), "A store fails");
    zassert_true(Flush());
    zassert_equal(SensorSubscriptionGeneration(), generation, "and rebuilds nothing");
    PublishSensorValue("switch", 1.0F);
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value());

    // Destroyed in After().
}

// Both are optional: no publisher means no replay, no configuration service means nothing to
// register. A second Initialize() repeats the first one's result and builds nothing.
ZTEST(logging_controller_remote, test_WithoutAPublisherOrAConfigurationServiceTheTriggersStillAct) {
    auto parts = F().MakeParts();
    parts.configuration_service = nullptr;
    parts.has_publisher = false;
    const uint32_t generation = SensorSubscriptionGeneration();

    auto& controller = F().Build(std::move(parts));
    zassert_equal(controller.Initialize(StoresTriggersOn({ "switch" })), 0);
    zassert_true(Flush());
    zassert_equal(SensorSubscriptionGeneration(), generation + 1, "The boot set subscribed");

    PublishSensorValue("switch", 1.0F);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "The trigger acts");

    // A store rebuilds the set, and there is no publisher to replay.
    const auto manager = controller.GetLoggingControlConfigurationManager();
    zassert_true(manager->Update(TriggersOn({ "b" })), "The new triggers are stored");
    zassert_true(Flush(), "The rebuild ran");
    PublishSensorValue("b", 1.0F);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "The rebuilt trigger acts");

    const uint32_t rebuilt = SensorSubscriptionGeneration();
    zassert_equal(controller.Initialize(), 0, "A second call repeats the first one's result");
    zassert_true(Flush());
    zassert_equal(SensorSubscriptionGeneration(), rebuilt, "and builds nothing");
}

// ConfigurationService keeps the first manager of a type and cannot unregister it.
ZTEST(logging_controller_remote, test_ASecondControllerOnTheSameConfigurationServiceFails) {
    auto parts = F().MakeParts();
    const auto configuration_service = parts.configuration_service;
    zassert_equal(F().Build(std::move(parts)).Initialize(), 0);
    F().controller.reset();
    zassert_true(configuration_service->IsRegistered(ConfigurationService::Type::LoggingControl));

    auto again = F().MakeParts();
    again.configuration_service = configuration_service;

    LogCounter::Get().Watch("logging_controller_logger");
    auto& second = F().Build(std::move(again));
    zassert_equal(second.Initialize(), -1, "The service would keep the first controller's manager");
    ZASSERT_LOGGED(1, "and says why");
    zassert_is_null(second.GetControl().get());
    zassert_is_null(second.GetLoggingControlConfigurationManager().get());

    // Destroyed in After().
}

// ======== Start() and the LOGGING command (L9) ========

// The gauge answers the command too: a stop is accepted (and drops its wanted start), a start is not
// served. The controller wires the handler in the remote role.
ZTEST(logging_controller_remote, test_TheRemoteRoleServesTheLoggingCommand) {
    F().Start();

    const auto stopped = F().peer.Ask({ 0x00 });
    zassert_true(stopped.has_value(), "A stop is answered");
    zassert_equal(*stopped, CdmpResultCode::SUCCESS);

    const auto started = F().peer.Ask({ 0x01 });
    zassert_true(started.has_value(), "and so is a start");
    zassert_equal(*started, CdmpResultCode::FAILURE, "which this unit cannot serve");

    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "Nothing is sent in answer");
}

ZTEST(logging_controller_remote, test_TheHandlerGoesWithTheController) {
    F().Start();
    F().controller.reset();

    const auto result = F().peer.Ask({ 0x00 });
    zassert_true(result.has_value());
    zassert_equal(
        *result, CdmpResultCode::UNSUPPORTED_COMMAND, "The command is not served once the controller is gone"
    );
}

ZTEST(logging_controller_remote, test_StartBeforeInitializeOrAfterAFailedOneFails) {
    auto& before = F().Build(F().MakeParts());
    zassert_equal(before.Start(), -1, "Start() needs Initialize()");
    zassert_equal(before.Initialize(), 0, "which still works afterwards");
    zassert_equal(before.Start(), 0);
    F().controller.reset();

    // The remote role is blind without the COM service.
    Parts parts;
    auto& failed = F().Build(std::move(parts));
    zassert_equal(failed.Initialize(), -1);
    zassert_equal(failed.Start(), -1, "and a failed Initialize() has no Start()");
}

ZTEST(logging_controller_remote, test_StartTwiceReturnsTheFirstResult) {
    F().Start();

    zassert_equal(F().controller->Start(), 0);
    LogCounter::Get().Watch("logging_controller_logger");
    zassert_equal(F().controller->Start(), 0, "A second call repeats the first one's result");
    ZASSERT_LOGGED(1, "and logs an error");
}

ZTEST(logging_controller_remote, test_WithoutAConfigurationServiceStartSucceedsAndNothingStops) {
    auto parts = F().MakeParts();
    parts.configuration_service = nullptr;

    auto& controller = F().Build(std::move(parts));
    zassert_equal(controller.Initialize(), 0);
    zassert_equal(controller.Start(), 0, "There is nothing to report the stores, and that is no failure");

    controller.GetControl()->RequestStart(ALL_UNITS);
    zassert_true(IsStart(F().peer.WaitForBroadcast()));
    zassert_true(controller.GetLoggingControlConfigurationManager()->Update(TriggersOn({ "a" })));
    zassert_true(Flush());
    zassert_false(F().peer.WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "A store stops no logging");
}

#endif // !CONFIG_EERIE_LEAP_DOMAIN_LOGGING
