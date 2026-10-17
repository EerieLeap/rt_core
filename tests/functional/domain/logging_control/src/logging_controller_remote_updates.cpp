#include "logging_controller_test_support.h"

// A configuration update on a unit without a log writer (the gauge, remote role), L9: a configuration of
// any type stored on the unit after LoggingController::Start() drops the wanted start and sends the
// LOGGING stop, so nothing re-asserts a start that undoes it; the triggers take a baseline. What a unit
// stores while it starts, a rejected write and the unit's own settings are no updates. The real sensor
// pipeline runs; the unit's CDMP bus is the loopback one, which hands the unit its own frames, so each
// broadcast of the unit is answered by its own handler (Peer::WaitForOwnAnswer()) before a case goes on.
#ifndef CONFIG_EERIE_LEAP_DOMAIN_LOGGING

#include <memory>
#include <optional>
#include <vector>

#include <zephyr/ztest.h>

using namespace logging_controller_test;

using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;

namespace {

using Type = ConfigurationService::Type;

constexpr float kOn = 1.0F;
constexpr float kOff = 0.0F;

bool IsStart(const std::optional<RecordedFrame>& frame) {
    return frame.has_value() && frame->data[3] == 0x01;
}

bool IsStop(const std::optional<RecordedFrame>& frame) {
    return frame.has_value() && frame->data[3] == 0x00;
}

// The unit, a scripted peer, and the gauge's pieces: a GPIO with a few inputs, the real sensor
// pipeline, and a controller in the remote role.
struct Fixture {
    Harness harness;
    std::shared_ptr<FakeGpio> gpio = std::make_shared<FakeGpio>(4);
    // Subscribed before the controller's triggers, so it runs first in a dispatch.
    SensorsBlocker blocker {SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated};
    SensorProbe switch_probe {"switch"};
    std::unique_ptr<ProcessingRig> rig;
    Peer peer {harness};
    std::unique_ptr<LoggingController> controller;
    std::shared_ptr<LoggingControlConfigurationManager> system;
    std::shared_ptr<LoggingControlConfigurationManager> ui;
    std::shared_ptr<LoggingControlConfigurationManager> display;
    std::shared_ptr<CanbusConfigurationManager> canbus;

    Fixture() {
        harness.BringOnline();
        harness.Settle();
    }

    // The gauge's boot: the sensors, the controller (the logging hook stores @p triggers), processing,
    // then Start().
    void Boot(const SensorList& sensors, std::initializer_list<std::string_view> triggers = {}, bool has_seed = false) {
        rig = std::make_unique<ProcessingRig>(sensors, true, gpio);

        Parts parts;
        parts.com = harness.com;
        controller = MakeController(*rig, std::move(parts));
        zassert_equal(controller->Initialize(triggers.size() > 0 ? StoresTriggersOn(triggers) : nullptr), 0,
            "The controller initializes");

        rig->StartProcessing();
        if(has_seed)
            WaitForSeed();

        zassert_equal(controller->Start(), 0, "The controller starts");
    }

    // The GPIO reader of the processing's generation seeds `switch` with its level, 0, when it starts.
    // Returns once the triggers have seen it, so a later value is not overtaken by it.
    void WaitForSeed() {
        zassert_true(switch_probe.WaitForValue(kOff), "The seed was published");
        zassert_true(blocker.Fence(), "and the triggers saw it");
    }

    // The stand-ins and the CAN configuration of the types this suite has no domain for.
    void RegisterStandIns() {
        auto& service = *rig->configuration_service;
        system = MakeStandIn(service, Type::System, "stand_in_system", rig->fs_service);
        ui = MakeStandIn(service, Type::Ui, "stand_in_ui", rig->fs_service);
        display = MakeStandIn(service, Type::Display, "stand_in_display", rig->fs_service);

        canbus = std::make_shared<CanbusConfigurationManager>(
            std::make_unique<CborConfigurationService<CborCanbusConfig>>("stand_in_canbus", rig->fs_service), nullptr);
        service.RegisterCborConfigurationManager(Type::Canbus, canbus);
    }

    // The unit wants logging: the start goes out and a logging unit shares that it logs.
    void WantLogging() {
        controller->GetControl()->RequestStart(kAllUnits);
        const auto start = peer.WaitForBroadcast();
        zassert_true(IsStart(start), "The start goes out");
        zassert_true(peer.WaitForOwnAnswer(*start), "and the unit's own handler is done with it");
        peer.ShareStatus({0x01});
    }

    // Exactly one stop leaves the unit, and nothing else.
    void AssertOneStop() {
        const auto stop = peer.WaitForBroadcast();
        zassert_true(IsStop(stop), "The update sends the LOGGING stop");
        zassert_true(peer.WaitForOwnAnswer(*stop), "and the unit's own handler is done with it");
        AssertSilent("and no other command");
    }

    void AssertSilent(const char* message) {
        zassert_true(Flush());
        zassert_false(peer.WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "%s", message);
    }

    // The logging unit restarts quickly, announcing the record it held, and shares Stopped: the case
    // that re-asserts a wanted start (test_AQuickRestartOfTheLoggingUnitIsReasserted).
    void RestartLoggingUnit() {
        peer.Restart({{LOGGING_STATUS_CODE, 1, {0x00, 0x00}}}, true);
        peer.ShareStatus({0x00, 0x00});
    }

    // The unit's own COM service restarts and forgets the network.
    void RestartComService() {
        zassert_true(harness.com->Stop());
        harness.BringOnline();
        harness.Settle();
    }

    // Per-type check of rule 2: the store sends the stop once, and nothing re-asserts the start after.
    template<typename TStore>
    void AssertTheStoreSendsTheStop(TStore store) {
        Boot({Analog("spare")});
        RegisterStandIns();
        WantLogging();

        store();

        AssertOneStop();

        RestartLoggingUnit();
        AssertSilent("A start the update dropped is not sent again when the logging unit restarts");
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

    // The members go in reverse order of their declaration: the controller before the processing and
    // the unit it stands on.
    delete Current();
    Current() = nullptr;

    ResetLoopbackDevice();
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(remote_updates, NULL, SetUp, Before, After, NULL);

// ======== Rule 2: every type of configuration ========

ZTEST(remote_updates, test_AStoredSensorsConfigurationSendsTheStop) {
    F().AssertTheStoreSendsTheStop([] {
        F().rig->Apply({Analog("spare"), Analog("added")});
    });
}

ZTEST(remote_updates, test_AStoredLoggingControlConfigurationSendsTheStop) {
    F().AssertTheStoreSendsTheStop([] {
        zassert_true(F().controller->GetLoggingControlConfigurationManager()->Update(TriggersOn({"a"})));
    });
}

ZTEST(remote_updates, test_AStoredCanbusConfigurationSendsTheStop) {
    F().AssertTheStoreSendsTheStop([] {
        zassert_true(F().canbus->Update(canbus_com_loopback_test::ComConfiguration()));
    });
}

ZTEST(remote_updates, test_AStoredSystemConfigurationSendsTheStop) {
    F().AssertTheStoreSendsTheStop([] { StoreStandIn(F().system); });
}

ZTEST(remote_updates, test_AStoredUiConfigurationSendsTheStop) {
    F().AssertTheStoreSendsTheStop([] { StoreStandIn(F().ui); });
}

ZTEST(remote_updates, test_AStoredDisplayConfigurationSendsTheStop) {
    F().AssertTheStoreSendsTheStop([] { StoreStandIn(F().display); });
}

ZTEST(remote_updates, test_AStoredAdcConfigurationSendsTheStop) {
    F().AssertTheStoreSendsTheStop([] {
        const auto adc = F().rig->adc_configuration_manager;
        zassert_true(adc->Update(*adc->Get()), "The same bytes stored again count");
    });
}

// An SMP write, on the configuration queue, through the service.
ZTEST(remote_updates, test_AnSmpWriteSendsTheStop) {
    F().Boot({Analog("spare")});
    F().WantLogging();

    const auto stored = F().rig->configuration_service->GetCborConfiguration(Type::Sensors);
    zassert_false(stored.empty());
    QueuedApply apply(F().rig->configuration_service, Type::Sensors, std::vector<uint8_t>(stored.begin(), stored.end()));
    zassert_true(apply.WaitForSuccess(), "The write is applied");

    F().AssertOneStop();
}

// ======== What is no update ========

ZTEST(remote_updates, test_ARejectedWriteSendsNothing) {
    F().Boot({Analog("spare")});
    F().WantLogging();

    QueuedApply apply(F().rig->configuration_service, Type::Sensors, std::vector<uint8_t> {0xFF, 0x01, 0x02});
    zassert_false(apply.WaitForSuccess(), "Malformed CBOR is rejected");
    F().AssertSilent("A rejected write is not an update");

    F().RestartLoggingUnit();
    zassert_true(IsStart(F().peer.WaitForCommand()), "and the wanted start is still re-asserted");
}

ZTEST(remote_updates, test_TheUnitsOwnSettingSendsNothing) {
    F().Boot({Analog("spare")});
    F().RegisterStandIns();
    F().WantLogging();

    zassert_true(F().display->UpdateOwnSetting(TriggersOn({})), "The brightness the gauge's own UI persists");
    F().AssertSilent("A unit's own setting is no configuration update");

    F().RestartLoggingUnit();
    zassert_true(IsStart(F().peer.WaitForCommand()), "and the wanted start stands");
}

// The gauge's order: the sensors hook adds the logging button, the logging hook stores its trigger, the
// UI test configuration is stored, then Initialize(), processing, Start(). None of it stops anything,
// and a switch that is on still starts (L3).
ZTEST(remote_updates, test_TheBootsStoresSendNothing) {
    F().rig = std::make_unique<ProcessingRig>(
        SensorList {Analog("spare"), Indicator("switch_on", "1")}, true, F().gpio,
        [](std::shared_ptr<SensorsConfigurationManager> manager) {
            auto sensors = *manager->Get();
            sensors.push_back(Button());
            zassert_true(manager->Update(sensors), "The hook stores the button");
        });
    F().RegisterStandIns();
    StoreStandIn(F().ui);

    Parts parts;
    parts.com = F().harness.com;
    F().controller = MakeController(*F().rig, std::move(parts));
    zassert_equal(F().controller->Initialize(StoresTriggersOn({"switch_on"})), 0);
    StoreStandIn(F().ui);

    F().rig->StartProcessing();
    zassert_equal(F().controller->Start(), 0);

    const auto start = F().peer.WaitForBroadcast();
    zassert_true(IsStart(start), "The first command is the start of a switch that is on: no boot store stopped anything");
    zassert_true(F().peer.WaitForOwnAnswer(*start));
    F().AssertSilent("and no stop followed");
}

// ======== Rule 3: the LOGGING command from another unit ========

ZTEST(remote_updates, test_AStopFromAnotherUnitDropsTheWantedStart) {
    F().Boot({Analog("spare")});
    F().WantLogging();

    const auto result = F().peer.Broadcast({0x00});
    zassert_true(result.has_value(), "The unit answers the broadcast");
    zassert_equal(*result, CdmpResultCode::SUCCESS);
    F().AssertSilent("and sends nothing: the stop went to every unit");

    F().RestartLoggingUnit();
    F().AssertSilent("The wanted start is gone, so the restart of the logging unit re-asserts nothing");
}

ZTEST(remote_updates, test_AStartFromAnotherUnitIsNotServed) {
    F().Boot({Analog("spare")});
    F().WantLogging();

    const auto result = F().peer.Broadcast({0x01});
    zassert_true(result.has_value());
    zassert_equal(*result, CdmpResultCode::FAILURE, "This unit has no writer");
    F().AssertSilent("and sends nothing");

    F().RestartLoggingUnit();
    zassert_true(IsStart(F().peer.WaitForCommand()), "The wanted start is as it was");
}

// ======== The re-assertions, across the COM restart a CAN configuration causes ========

// A CAN configuration stored on this unit restarts its COM service: the network is forgotten, the status
// goes through Unavailable to Stopped, which would send a wanted start.
ZTEST(remote_updates, test_NoReassertionAfterAnUpdateAcrossTheUnitsOwnComRestart) {
    F().Boot({Analog("spare")});
    F().RegisterStandIns();
    F().WantLogging();

    zassert_true(F().canbus->Update(canbus_com_loopback_test::ComConfiguration()));
    F().AssertOneStop();

    F().RestartComService();
    F().peer.ShareStatus({0x00, 0x00});
    F().AssertSilent("The logging unit coming back Stopped is not a reason to start");
}

// A CAN configuration stored on the logging unit: its stop reaches this unit before the logging unit
// claims its ID again, and the record it announces afterwards finds nothing wanted.
ZTEST(remote_updates, test_NoReassertionAfterTheLoggingUnitsOwnUpdateAndComRestart) {
    F().Boot({Analog("spare")});
    F().WantLogging();

    zassert_equal(F().peer.Broadcast({0x00}).value_or(CdmpResultCode::FAILURE), CdmpResultCode::SUCCESS);

    F().RestartLoggingUnit();
    F().AssertSilent("The restarted logging unit finds no wanted start");
}

// Rule 5: a restart with no update in between is re-asserted, with Start() called.
ZTEST(remote_updates, test_ARestartWithoutAnUpdateIsStillReasserted) {
    F().Boot({Analog("spare")});
    F().WantLogging();

    F().RestartLoggingUnit();

    const auto start = F().peer.WaitForCommand();
    zassert_true(IsStart(start), "The logging unit lost the start: it is sent again");
    zassert_equal(start->data[0], PEER_ID, "to that unit alone");
    F().AssertSilent("once");
}

// ======== Rule 4: the triggers after an update ========

ZTEST(remote_updates, test_AfterAnUpdateASwitchThatIsOnStartsNothing) {
    F().Boot({Input("switch", 1), Analog("spare")}, {"switch"}, true);
    F().RegisterStandIns();

    PublishSensorValue("switch", kOn);
    const auto start = F().peer.WaitForBroadcast();
    zassert_true(IsStart(start), "A switch turned on starts logging");
    zassert_true(F().peer.WaitForOwnAnswer(*start));

    StoreStandIn(F().ui);
    F().AssertOneStop();

    PublishSensorValue("switch", kOn);
    F().AssertSilent("The switch is still on, and an update does not start it again");

    PublishSensorValue("switch", kOff);
    const auto stop = F().peer.WaitForBroadcast();
    zassert_true(IsStop(stop), "Turned off, it stops (a stop that changes nothing)");
    zassert_true(F().peer.WaitForOwnAnswer(*stop));

    PublishSensorValue("switch", kOn);
    zassert_true(IsStart(F().peer.WaitForBroadcast()), "and turned on again, it starts");
}

// The triggers stored again: the update stops logging, the rebuilt trigger reads its switch, which is on,
// as its baseline, and only a change after that acts.
ZTEST(remote_updates, test_ATriggerRebuiltByAnUpdateTakesItsSwitchAsABaseline) {
    F().Boot({Input("switch", 1), Analog("spare")}, {"switch"}, true);

    F().rig->Commit("switch", kOn);
    zassert_true(F().switch_probe.WaitForValue(kOn), "The publisher took the commit");
    zassert_true(F().blocker.Fence());
    const auto start = F().peer.WaitForBroadcast();
    zassert_true(IsStart(start), "The switch turned on");
    zassert_true(F().peer.WaitForOwnAnswer(*start));

    F().switch_probe.Clear();
    zassert_true(F().controller->GetLoggingControlConfigurationManager()->Update(TriggersOn({"switch"})));
    zassert_true(Flush(), "The rebuild ran");
    F().AssertOneStop();
    zassert_true(F().switch_probe.WaitForValue(kOn), "and its replay delivered the switch, still on");
    zassert_true(F().blocker.Fence());
    F().AssertSilent("The rebuilt trigger starts nothing: the replayed value is its baseline");

    PublishSensorValue("switch", kOff);
    const auto stop = F().peer.WaitForBroadcast();
    zassert_true(IsStop(stop), "It acts on its next change");
    zassert_true(F().peer.WaitForOwnAnswer(*stop));
    PublishSensorValue("switch", kOn);
    zassert_true(IsStart(F().peer.WaitForBroadcast()));
}

ZTEST(remote_updates, test_ATriggerOnAVanishedSensorForgetsIt) {
    F().Boot({Indicator("one", "1"), Analog("spare")}, {"one"});

    const auto start = F().peer.WaitForBroadcast();
    zassert_true(IsStart(start), "The indicator is on at boot");
    zassert_true(F().peer.WaitForOwnAnswer(*start));

    F().rig->Apply({Analog("spare")});
    F().AssertOneStop();

    PublishSensorValue("one", kOff);
    PublishSensorValue("one", kOn);
    F().AssertSilent("The sensor is gone: its trigger forgot it, and what the old generation still delivers asks nothing");

    SensorProbe probe("one");
    F().rig->Apply({Indicator("one", "1"), Analog("spare")});
    F().AssertOneStop();
    zassert_true(probe.WaitForValue(kOn), "The sensor is back, on");
    zassert_true(F().blocker.Fence());
    F().AssertSilent("and a 1 after a sensors update is a baseline: it starts nothing");
}

ZTEST(remote_updates, test_ATriggerOnASensorAddedByAnUpdateTakesABaseline) {
    F().Boot({Analog("spare")}, {"late"});
    F().AssertSilent("The trigger waits for its sensor");

    SensorProbe probe("late");
    F().rig->Apply({Indicator("late", "1"), Analog("spare")});
    F().AssertOneStop();
    zassert_true(probe.WaitForValue(kOn), "The sensor the update brought is on");
    zassert_true(F().blocker.Fence());
    F().AssertSilent("and a baseline starts nothing");
}

ZTEST(remote_updates, test_ASensorsUpdateWhoseRestartFailsStillStops) {
    F().Boot({Analog("spare")});
    F().WantLogging();

    F().gpio->FailRegistrations();
    F().rig->Apply({Analog("spare"), Input("added", 2)});
    zassert_true(F().rig->IsProcessingStopped(), "The restart failed");

    F().AssertOneStop();
}

// ======== The controller's end ========

ZTEST(remote_updates, test_TheUpdateStopGoesWithTheController) {
    F().Boot({Analog("spare")});
    F().RegisterStandIns();
    F().WantLogging();

    F().controller.reset();

    StoreStandIn(F().ui);
    F().rig->Apply({Analog("spare"), Analog("added")});
    F().AssertSilent("A store after the controller went reports to nobody");
}

#endif // !CONFIG_EERIE_LEAP_DOMAIN_LOGGING
