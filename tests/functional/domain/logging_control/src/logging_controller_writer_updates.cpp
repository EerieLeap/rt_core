#include "logging_controller_test_support.h"

// A configuration update on a unit that writes the log (rt_daq, writer role), L9: a configuration of any
// type stored on the unit after LoggingController::Start() stops the writer for good (the wanted state is
// cleared as by RequestStop(ALL_UNITS), and neither the restart after a logging configuration nor the sensors
// restart starts it again) and broadcasts the LOGGING stop. What a unit stores while it starts, a
// rejected write and the unit's own settings are no updates. The real sensor pipeline runs, and the
// stores are those of an SMP write or a shell command: through a manager. The unit's CDMP bus is the
// loopback one, which hands the unit its own frames, so each stop it broadcasts is heard by its own
// handler (RequestStop(ALL_UNITS) again): a case waits for that (Peer::WaitForOwnAnswer()) before it requests
// anything, or runs without a COM service where the order of a request matters.
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING

    #include <memory>
    #include <optional>
    #include <vector>

    #include <zephyr/ztest.h>

    #include "domain/system_domain/configuration/system_configuration_manager.h"

using namespace logging_controller_test;

using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::logging_domain::services::LogWriterService;
using eerie_leap::domain::system_domain::configuration::SystemConfigurationManager;

namespace {

using Type = ConfigurationService::Type;

constexpr float K_ON = 1.0F;
constexpr float K_OFF = 0.0F;

bool IsStart(const std::optional<RecordedFrame>& frame) {
    return frame.has_value() && frame->data[3] == 0x01;
}

bool IsStop(const std::optional<RecordedFrame>& frame) {
    return frame.has_value() && frame->data[3] == 0x00;
}

bool IsStarted(const std::optional<StatusLog::Entry>& entry) {
    return entry.has_value() && entry->is_active;
}

bool IsStopped(const std::optional<StatusLog::Entry>& entry) {
    return entry.has_value() && !entry->is_active && entry->reason == LoggingStatusReason::Stopped;
}

bool IsQuiet(StatusLog& log) {
    return !log.WaitForEntry(NO_WAIT_MS).has_value();
}

// `switch` is the sensor the logging configuration logs, `spare` and `other` are bound to nothing.
SensorList Base() {
    return { Indicator("switch", "1"), Analog("spare"), Analog("other") };
}

// One job on the configuration queue that stores the sensors, then holds the queue until Release(): what
// the store queued (the supervisor's run, the sensors restart) is provably pending behind it.
class HeldSensorsStore {
private:
    struct Context {
        k_sem stored{};
        k_sem release{};
        bool result = false;

        Context() {
            k_sem_init(&stored, 0, 1);
            k_sem_init(&release, 0, K_SEM_MAX_LIMIT);
        }
    };

    std::shared_ptr<Context> context_ = std::make_shared<Context>();

public:
    HeldSensorsStore(std::shared_ptr<SensorsConfigurationManager> manager, SensorList sensors) {
        TestConfigQueue()->Run([context = context_, manager = std::move(manager), sensors = std::move(sensors)] {
            context->result = manager->Update(sensors);
            k_sem_give(&context->stored);
            // Gives up after 5 s, so a case that never releases cannot wedge the queue for good.
            k_sem_take(&context->release, K_SECONDS(5));
        });
    }

    HeldSensorsStore(const HeldSensorsStore&) = delete;
    HeldSensorsStore& operator=(const HeldSensorsStore&) = delete;
    HeldSensorsStore(HeldSensorsStore&&) = delete;
    HeldSensorsStore& operator=(HeldSensorsStore&&) = delete;

    ~HeldSensorsStore() {
        Release();
    }

    [[nodiscard]] bool WaitUntilStored(int timeout_ms = WAIT_MS) {
        return k_sem_take(&context_->stored, Ms(timeout_ms)) == 0 && context_->result;
    }

    void Release() {
        k_sem_give(&context_->release);
    }
};

// The unit: with or without a scripted peer on its CDMP bus, the real sensor pipeline, and a controller
// in the writer role.
struct Fixture {
    std::unique_ptr<Harness> harness;
    std::shared_ptr<FakeGpio> gpio = std::make_shared<FakeGpio>(4);
    // Subscribed before the controller's triggers, so it runs first in a dispatch.
    SensorsBlocker blocker{ SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated };
    SensorProbe switch_probe{ "switch" };
    StatusLog log;
    std::unique_ptr<ProcessingRig> rig;
    std::unique_ptr<Peer> peer;
    std::unique_ptr<LoggingController> controller;
    std::shared_ptr<LogWriterService> writer;
    std::shared_ptr<LoggingControlConfigurationManager> ui;
    std::shared_ptr<LoggingControlConfigurationManager> display;
    std::shared_ptr<CanbusConfigurationManager> canbus;
    std::shared_ptr<SystemConfigurationManager> system;

    // @param has_com Whether the unit goes online with a scripted peer.
    // @param starts Whether Start() is called: a unit that is still starting stores without stopping.
    void Boot(
        const SensorList& sensors,
        std::initializer_list<std::string_view> triggers = {},
        bool has_com = true,
        bool starts = true,
        bool has_seed = false
    ) {

        if(has_com) {
            harness = std::make_unique<Harness>();
            harness->BringOnline();
            harness->Settle();
            peer = std::make_unique<Peer>(*harness);
        }

        // A Harness formats the file system itself.
        rig = std::make_unique<ProcessingRig>(sensors, has_com, gpio);

        Parts parts;
        parts.com = has_com ? harness->com : nullptr;
        controller = MakeController(*rig, std::move(parts));
        zassert_equal(
            controller->Initialize(triggers.size() > 0 ? StoresTriggersOn(triggers) : nullptr), 0,
            "The controller initializes"
        );
        writer = controller->GetLogWriterService();
        StoreLoggingConfiguration(*controller, { "switch" });

        rig->Register(*controller);
        rig->StartProcessing();
        if(has_seed)
            WaitForSeed();

        if(starts)
            zassert_equal(controller->Start(), 0, "The controller starts");
    }

    // The GPIO reader of the processing's generation seeds `switch` with its level, 0, when it starts.
    // Returns once the triggers have seen it, so a later value is not overtaken by it.
    void WaitForSeed() {
        zassert_true(switch_probe.WaitForValue(K_OFF), "The seed was published");
        zassert_true(blocker.Fence(), "and the triggers saw it");
    }

    // The stand-ins, the CAN configuration and the system configuration of the unit.
    void RegisterOthers() {
        auto& service = *rig->configuration_service;
        ui = MakeStandIn(service, Type::Ui, "stand_in_ui", rig->fs_service);
        display = MakeStandIn(service, Type::Display, "stand_in_display", rig->fs_service);

        canbus = std::make_shared<CanbusConfigurationManager>(
            std::make_unique<CborConfigurationService<CborCanbusConfig>>("stand_in_canbus", rig->fs_service), nullptr
        );
        service.RegisterCborConfigurationManager(Type::Canbus, canbus);

        system = std::make_shared<SystemConfigurationManager>(
            std::make_unique<CborConfigurationService<CborSystemConfig>>(
                "system_config", rig->fs_service, TestConfigQueue()
            )
        );
        service.RegisterCborConfigurationManager(Type::System, system);
    }

    // The unit logs: one session, one file.
    void StartLogging() {
        // This unit alone: a request for every unit would also broadcast the start.
        controller->GetControl()->RequestStart(controller->GetUnitUid());
        zassert_true(IsStarted(log.WaitForEntry()), "A request starts the writer");
        zassert_true(WaitUntil([this] {
            return writer->IsRunning();
        }));
        zassert_equal(CountLogFiles(*rig->fs_service), 1U);
    }

    // The unit's own stop leaves the bus once, and its own handler answers it.
    void AssertOneStopBroadcast() const {
        const auto stop = peer->WaitForBroadcast();
        zassert_true(IsStop(stop), "The update broadcasts the LOGGING stop");
        zassert_true(peer->WaitForOwnAnswer(*stop), "and the unit's own handler is done with it");
        zassert_false(peer->WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "and nothing else");
    }

    // The update stopped the writer for good: one Stopped, no new file, no restart.
    void AssertStoppedForGood() {
        zassert_true(Flush());
        zassert_true(Flush());
        zassert_true(IsStopped(log.WaitForEntry()), "The update stops the writer");
        zassert_true(IsQuiet(log), "once, and nothing starts it again");
        zassert_true(writer->IsStopped());
        zassert_equal(CountLogFiles(*rig->fs_service), 1U, "without a new file");
    }

    // A new request starts a new session.
    void AssertStartsAgain() {
        controller->GetControl()->RequestStart(controller->GetUnitUid());
        zassert_true(IsStarted(log.WaitForEntry()), "A new request starts a new session");
        zassert_true(WaitUntil([this] {
            return writer->IsRunning() && CountLogFiles(*rig->fs_service) == 2;
        }));
    }

    // The per-type check of rule 2. With a COM service the unit hears its own broadcast on the loopback
    // bus, whose handler requests the stop a second time, so a case with one cannot tell that the update
    // stopped the writer itself: the same check runs without a COM service, where nothing else can.
    template<typename TStore>
    void AssertTheStoreStopsLogging(bool has_com, TStore store) {
        Boot(Base(), {}, has_com);
        RegisterOthers();
        StartLogging();

        store();

        AssertStoppedForGood();
        if(has_com)
            AssertOneStopBroadcast();
        AssertStartsAgain();
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

    if(Current() != nullptr) {
        // The controller before the processing and the unit it stands on.
        Current()->controller.reset();
        Current()->system.reset();
        Current()->canbus.reset();
        Current()->display.reset();
        Current()->ui.reset();
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

ZTEST_SUITE(writer_updates, NULL, SetUp, Before, After, NULL);

// ======== Rule 2: every type of configuration ========

// Each store is what an SMP write or a shell command does, through a manager.

// The sensors restart is the dependent's path: an addition used to finalize the file and open the next.
static void StoreSensors() {
    F().rig->Apply({ Indicator("switch", "1"), Analog("spare"), Analog("other"), Analog("added") });
}

static void StoreAdc() {
    const auto adc = F().rig->adc_configuration_manager;
    zassert_true(adc->Update(*adc->Get()), "The same bytes stored again count");
}

// The logging configuration used to restart a running writer into a new file.
static void StoreLogging() {
    LoggingConfiguration configuration(std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr());
    configuration.logging_interval_ms = 200;
    configuration.max_log_size_mb = 0;
    configuration.sensor_configurations.emplace(
        StringHelpers::GetHash("switch"), SensorLoggingConfiguration{ .is_enabled = true }
    );

    zassert_true(F().controller->GetConfigurationManager()->Update(configuration));
}

static void StoreLoggingControl() {
    zassert_true(F().controller->GetLoggingControlConfigurationManager()->Update(TriggersOn({ "switch" })));
}

static void StoreCanbus() {
    zassert_true(F().canbus->Update(canbus_com_loopback_test::ComConfiguration()));
}

static void StoreSystem() {
    zassert_true(F().system->Update(*F().system->Get()));
}

static void StoreUi() {
    StoreStandIn(F().ui);
}

static void StoreDisplay() {
    StoreStandIn(F().display);
}

// rt_daq's `logging reapply-sensors`: the manager applies its own stored bytes.
static void ReapplySensors() {
    const auto stored = F().rig->sensors_manager->GetCborConfiguration();
    zassert_false(stored.empty());
    zassert_true(F().rig->sensors_manager->ApplyCborConfiguration(stored));
}

// The write of a client, through the service, on the configuration queue.
static void WriteSensorsOverSmp() {
    const auto stored = F().rig->configuration_service->GetCborConfiguration(Type::Sensors);
    QueuedApply apply(
        F().rig->configuration_service, Type::Sensors, std::vector<uint8_t>(stored.begin(), stored.end())
    );
    zassert_true(apply.WaitForSuccess(), "The write is applied");
}

    // With a COM service the update also broadcasts the stop; without one the writer's own stop is all there is.
    #define WRITER_STOPS_FOR_GOOD(name, store) \
        ZTEST(writer_updates, test_A##name##StopsLoggingForGood) { \
            F().AssertTheStoreStopsLogging(true, store); \
        } \
        ZTEST(writer_updates, test_A##name##StopsLoggingForGoodWithoutAComService) { \
            F().AssertTheStoreStopsLogging(false, store); \
        }

WRITER_STOPS_FOR_GOOD(StoredSensorsConfiguration, StoreSensors)
WRITER_STOPS_FOR_GOOD(StoredAdcConfiguration, StoreAdc)
WRITER_STOPS_FOR_GOOD(StoredLoggingConfiguration, StoreLogging)
WRITER_STOPS_FOR_GOOD(StoredLoggingControlConfiguration, StoreLoggingControl)
WRITER_STOPS_FOR_GOOD(StoredCanbusConfiguration, StoreCanbus)
WRITER_STOPS_FOR_GOOD(StoredSystemConfiguration, StoreSystem)
WRITER_STOPS_FOR_GOOD(StoredUiConfiguration, StoreUi)
WRITER_STOPS_FOR_GOOD(StoredDisplayConfiguration, StoreDisplay)
WRITER_STOPS_FOR_GOOD(ShellReapplyOfTheStoredSensors, ReapplySensors)
WRITER_STOPS_FOR_GOOD(SmpWriteOfTheSensors, WriteSensorsOverSmp)

    #undef WRITER_STOPS_FOR_GOOD

// ======== Order ========

// A start requested after the update's stop is the later request, and wins: the file of the old
// generation is finalized by the sensors restart and logging goes on in a new one. Without a COM
// service: the loopback bus would hand the unit its own stop after the start.
ZTEST(writer_updates, test_AStartRequestedAfterTheUpdatesStopWins) {
    F().Boot(Base(), {}, false);
    F().StartLogging();

    auto sensors = Base();
    sensors.push_back(Analog("added"));
    HeldSensorsStore held(F().rig->sensors_manager, sensors);
    zassert_true(held.WaitUntilStored(), "The sensors are stored, and the supervisor's run waits behind the job");

    F().controller->GetControl()->RequestStart(ALL_UNITS);
    held.Release();

    zassert_true(IsStopped(F().log.WaitForEntry()), "The restart finalizes the file");
    zassert_true(IsStarted(F().log.WaitForEntry()), "and logging goes on in a new one: the later request wins");
    zassert_true(WaitUntil([] {
        return F().writer->IsRunning() && CountLogFiles(*F().rig->fs_service) == 2;
    }));
    zassert_true(IsQuiet(F().log));
}

// ======== What is no update ========

ZTEST(writer_updates, test_ARejectedWriteStopsNothing) {
    F().Boot(Base());
    F().StartLogging();

    QueuedApply apply(F().rig->configuration_service, Type::Sensors, std::vector<uint8_t>{ 0xFF, 0x01, 0x02 });
    zassert_false(apply.WaitForSuccess(), "Malformed CBOR is rejected");
    zassert_true(Flush());

    zassert_true(IsQuiet(F().log), "A rejected write is not an update");
    zassert_true(F().writer->IsRunning());
    zassert_false(F().peer->WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "and sends nothing");
}

ZTEST(writer_updates, test_TheUnitsOwnSettingStopsNothing) {
    F().Boot(Base());
    F().RegisterOthers();
    F().StartLogging();

    zassert_true(F().display->UpdateOwnSetting(TriggersOn({})));
    zassert_true(Flush());

    zassert_true(IsQuiet(F().log), "A unit's own setting is no configuration update");
    zassert_true(F().writer->IsRunning());
    zassert_false(F().peer->WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value());
}

// Before Start() the unit is still starting: a sensors store restarts the writer into a new file, as it
// always did, and nothing is broadcast.
//
// The switch is off from the sensors store on, as the gauge's logging button is at every boot. A trigger
// stored before Start() reads its first value as at boot, and a first value of 1 starts logging, which is
// broadcast to the other units. The new generation samples its sensors 100 ms after it starts, as long as
// the wait for a broadcast below, so with the switch on that start falls on either side of the wait
// depending on the speed of the target (it did on the 64 bit QEMU ones, not on the others). A switch that
// is off asserts nothing, so a broadcast here is an update's stop. The start of a switch that is on at boot
// is test_ASwitchThatIsOnAtBootStillStarts.
ZTEST(writer_updates, test_ABootTimeStoreStopsNothing) {
    F().Boot(Base(), {}, true, false);
    F().RegisterOthers();
    F().StartLogging();

    F().rig->Apply({ Indicator("switch", "0"), Analog("spare"), Analog("other"), Analog("added") });
    zassert_true(IsStopped(F().log.WaitForEntry()), "The file is finalized");
    zassert_true(IsStarted(F().log.WaitForEntry()), "and logging goes on in the next");
    zassert_true(WaitUntil([] {
        return F().writer->IsRunning() && CountLogFiles(*F().rig->fs_service) == 2;
    }));

    StoreStandIn(F().ui);
    zassert_true(F().controller->GetLoggingControlConfigurationManager()->Update(TriggersOn({ "switch" })));
    zassert_true(Flush());
    zassert_false(F().peer->WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "Nothing is broadcast");

    // From Start() on it is an update.
    zassert_equal(F().controller->Start(), 0);
    StoreStandIn(F().ui);
    F().AssertOneStopBroadcast();
}

// ======== Rule 3: the LOGGING command from another unit ========

ZTEST(writer_updates, test_AStopFromAnotherUnitStopsTheWriterAndIsNotEchoed) {
    F().Boot(Base());
    F().StartLogging();

    const auto result = F().peer->Broadcast({ 0x00 });
    zassert_true(result.has_value(), "The unit answers the broadcast");
    zassert_equal(*result, CdmpResultCode::SUCCESS);

    zassert_true(IsStopped(F().log.WaitForEntry()), "The writer stops");
    zassert_true(F().writer->IsStopped());
    zassert_false(
        F().peer->WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(),
        "and the stop is not sent on: the other unit sent it to every unit"
    );
}

ZTEST(writer_updates, test_WithoutAComServiceAnUpdateStopsAndBroadcastsNothing) {
    F().Boot(Base(), {}, false);
    F().RegisterOthers();
    F().StartLogging();

    StoreStandIn(F().ui);

    F().AssertStoppedForGood();
    F().AssertStartsAgain();
}

// ======== Rule 4: the triggers ========

// L3 with Start() called: a switch that is on at boot starts logging.
ZTEST(writer_updates, test_ASwitchThatIsOnAtBootStillStarts) {
    F().Boot(Base(), { "switch" }, false);

    zassert_true(IsStarted(F().log.WaitForEntry()), "A switch that is already on starts logging");
    zassert_true(WaitUntil([] {
        return F().writer->IsRunning();
    }));
    zassert_true(IsQuiet(F().log));
}

ZTEST(writer_updates, test_AfterAnUpdateASwitchThatIsOnStartsNothing) {
    F().Boot({ Input("switch", 1), Analog("spare"), Analog("other") }, { "switch" }, true, true, true);
    F().RegisterOthers();

    PublishSensorValue("switch", K_ON);
    zassert_true(IsStarted(F().log.WaitForEntry()), "A switch turned on starts logging");
    zassert_true(WaitUntil([] {
        return F().writer->IsRunning();
    }));
    zassert_true(IsStart(F().peer->WaitForBroadcast()), "and the other units: the trigger names every unit");

    StoreStandIn(F().ui);
    F().AssertStoppedForGood();
    F().AssertOneStopBroadcast();

    PublishSensorValue("switch", K_ON);
    zassert_true(Flush());
    zassert_true(IsQuiet(F().log), "The switch is still on, and an update does not start it again");

    PublishSensorValue("switch", K_OFF);
    PublishSensorValue("switch", K_ON);
    zassert_true(IsStarted(F().log.WaitForEntry()), "A change after the update starts it");
    zassert_true(WaitUntil([] {
        return F().writer->IsRunning() && CountLogFiles(*F().rig->fs_service) == 2;
    }));
}

// The triggers stored again: the update stops logging, the rebuilt trigger reads its switch, which is on,
// as its baseline.
ZTEST(writer_updates, test_ATriggerRebuiltByAnUpdateTakesItsSwitchAsABaseline) {
    F().Boot({ Input("switch", 1), Analog("spare"), Analog("other") }, { "switch" }, true, true, true);

    F().rig->Commit("switch", K_ON);
    zassert_true(F().switch_probe.WaitForValue(K_ON), "The publisher took the commit");
    zassert_true(F().blocker.Fence());
    zassert_true(IsStarted(F().log.WaitForEntry()), "The switch turned on");
    zassert_true(WaitUntil([] {
        return F().writer->IsRunning();
    }));
    zassert_true(IsStart(F().peer->WaitForBroadcast()), "and the other units follow");

    F().switch_probe.Clear();
    zassert_true(F().controller->GetLoggingControlConfigurationManager()->Update(TriggersOn({ "switch" })));
    zassert_true(Flush(), "The rebuild ran");
    F().AssertStoppedForGood();
    F().AssertOneStopBroadcast();
    zassert_true(F().switch_probe.WaitForValue(K_ON), "and its replay delivered the switch, still on");
    zassert_true(F().blocker.Fence());
    zassert_true(IsQuiet(F().log), "The replayed value is the rebuilt trigger's baseline: it starts nothing");

    PublishSensorValue("switch", K_OFF);
    PublishSensorValue("switch", K_ON);
    zassert_true(IsStarted(F().log.WaitForEntry()), "It acts on its next change");
}

// ======== The controller ========

ZTEST(writer_updates, test_TheWriterRoleStartsItsUpdateStop) {
    F().Boot(Base(), {}, true, false);
    F().RegisterOthers();
    F().StartLogging();

    zassert_equal(F().controller->Start(), 0);
    LogCounter::Get().Watch("logging_controller_logger");
    zassert_equal(F().controller->Start(), 0, "A second call repeats the first one's result");
    ZASSERT_LOGGED(1, "and logs an error");

    StoreStandIn(F().ui);
    F().AssertStoppedForGood();
}

#endif // CONFIG_EERIE_LEAP_DOMAIN_LOGGING
