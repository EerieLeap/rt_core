#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/logging_domain/control/configuration_update_logging_stop.h"

#include "logging_controller_test_support.h"

using namespace logging_controller_test;

using eerie_leap::domain::logging_domain::control::ConfigurationUpdateLoggingStop;

// The update rule of L9 on its own: a real configuration service and manager, fakes for the control and
// the sender. The rule runs in every scenario: it belongs to both roles.
namespace {

using Type = ConfigurationService::Type;

// What the update handler saw: the types, and how many requests the control had when it ran.
struct HandlerLog {
    std::vector<Type> types;
    std::vector<size_t> requests_seen;
};

struct Fixture {
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<ConfigurationService> service = std::make_shared<ConfigurationService>();
    std::shared_ptr<LoggingControlConfigurationManager> manager;
    std::shared_ptr<FakeLoggingControl> control = std::make_shared<FakeLoggingControl>();
    std::shared_ptr<FakeCommandSender> sender = std::make_shared<FakeCommandSender>();
    std::shared_ptr<HandlerLog> handler_log = std::make_shared<HandlerLog>();
    std::unique_ptr<ConfigurationUpdateLoggingStop> stop;

    Fixture() {
        DtFs::InitInternalFs();
        fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        zassert_true(fs_service->Format(), "The internal file system must format.");

        manager = std::make_shared<LoggingControlConfigurationManager>(
            std::make_unique<CborConfigurationService<CborLoggingControlConfig>>(
                "logging_control_config", fs_service, TestConfigQueue()));
        service->RegisterCborConfigurationManager(Type::LoggingControl, manager);
    }

    // Initialized, not started.
    void Build(bool has_control = true, bool has_sender = true, bool has_handler = false) {
        ConfigurationUpdateLoggingStop::UpdateHandler on_update;
        if(has_handler) {
            on_update = [log = handler_log, control = control](Type type) {
                log->types.push_back(type);
                log->requests_seen.push_back(control->Requests().size());
            };
        }

        stop = std::make_unique<ConfigurationUpdateLoggingStop>(
            service,
            TestConfigQueue(),
            has_control ? std::static_pointer_cast<eerie_leap::domain::logging_domain::control::ILoggingControl>(control) : nullptr,
            has_sender ? std::static_pointer_cast<eerie_leap::domain::logging_domain::control::ILoggingCommandSender>(sender) : nullptr,
            std::move(on_update));
        zassert_true(stop->Initialize(), "The update stop did not initialize.");
    }

    // Initialized and started.
    void Start(bool has_control = true, bool has_sender = true, bool has_handler = false) {
        Build(has_control, has_sender, has_handler);
        zassert_true(stop->Start(), "The update stop did not start.");
    }

    // A configuration update, stored on the calling thread.
    void Store(std::initializer_list<std::string_view> sensor_ids = {"a"}) {
        zassert_true(manager->Update(TriggersOn(sensor_ids)), "The configuration was not stored.");
    }

    bool Flush() const { return FlushQueue(*TestConfigQueue()); }
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
        // A request or a send a failed case left holding would keep the update stop's destructor waiting.
        Current()->control->Release();
        Current()->sender->Release();
        Current()->stop.reset();
        delete Current();
        Current() = nullptr;
    }
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(configuration_update_logging_stop, NULL, SetUp, Before, After, NULL);

ZTEST(configuration_update_logging_stop, test_NothingIsStoppedBeforeStart) {
    F().Build();
    zassert_false(F().stop->IsStarted());

    F().Store();
    zassert_true(F().Flush());

    zassert_equal(F().control->Requests().size(), 0U, "The unit's boot stores stop nothing");
    zassert_equal(F().sender->Sends().size(), 0U);

    zassert_true(F().stop->Start());
    zassert_true(F().stop->IsStarted());
    F().Store();
    zassert_equal(F().control->Requests().size(), 1U, "From Start() on they do");
}

ZTEST(configuration_update_logging_stop, test_AStoreAfterStartStopsOnTheStoringThreadAndBroadcastsFromTheQueue) {
    F().Start();

    F().Store();

    const auto requests = F().control->Requests();
    zassert_equal(requests.size(), 1U, "The stop is requested before the store returns");
    zassert_equal(requests[0].kind, Kind::Stop);
    zassert_equal(requests[0].thread, k_current_get(), "on the thread that stored the configuration");

    zassert_true(F().sender->WaitForSends(1), "and broadcast");
    const auto sends = F().sender->Sends();
    zassert_false(sends[0].is_start, "a stop");
    zassert_equal(sends[0].thread, QueueThread(*TestConfigQueue()), "from the configuration work queue");

    zassert_true(F().Flush());
    zassert_equal(F().sender->Sends().size(), 1U, "once");
}

ZTEST(configuration_update_logging_stop, test_AnSmpApplyStops) {
    F().Start();

    QueuedApply apply(F().service, Type::LoggingControl, k_trigger_on_ab);
    zassert_true(apply.WaitForSuccess(), "The write is applied through the service");

    const auto requests = F().control->Requests();
    zassert_equal(requests.size(), 1U);
    zassert_equal(requests[0].kind, Kind::Stop);
    zassert_equal(requests[0].thread, QueueThread(*TestConfigQueue()), "on the thread that applied it");
    zassert_true(F().sender->WaitForSends(1));
}

ZTEST(configuration_update_logging_stop, test_ARejectedApplyStopsNothing) {
    F().Start();

    const std::vector<uint8_t> malformed = {0xFF, 0x01, 0x02};
    zassert_false(F().service->ApplyCborConfiguration(Type::LoggingControl, malformed));
    zassert_true(F().Flush());

    zassert_equal(F().control->Requests().size(), 0U, "A rejected write is not an update");
    zassert_equal(F().sender->Sends().size(), 0U);
}

ZTEST(configuration_update_logging_stop, test_UpdateOwnSettingStopsNothing) {
    F().Start();

    zassert_true(F().manager->UpdateOwnSetting(TriggersOn({"a"})));
    zassert_true(F().Flush());

    zassert_equal(F().control->Requests().size(), 0U, "A unit's own setting is not an update");
    zassert_equal(F().sender->Sends().size(), 0U);
}

// One job stores twice; the broadcast task, queued by the first report, runs only after it.
ZTEST(configuration_update_logging_stop, test_StoresBeforeTheBroadcastRunsSendOneStop) {
    F().Start();

    HeldStore held(F().manager, {{"a"}, {"b"}});
    zassert_true(held.WaitUntilStored());
    zassert_true(held.AllStored());

    zassert_equal(F().control->Requests().size(), 2U, "Each store requests the stop");
    zassert_equal(F().sender->Sends().size(), 0U, "and the broadcast waits behind the job");

    held.Release();
    zassert_true(F().sender->WaitForSends(1));
    zassert_true(F().Flush());
    zassert_equal(F().sender->Sends().size(), 1U, "Several stores before it runs send one stop");
}

ZTEST(configuration_update_logging_stop, test_TheUpdateHandlerRunsBeforeTheStop) {
    F().Start(true, true, true);

    F().Store();

    zassert_equal(F().handler_log->types.size(), 1U);
    zassert_equal(F().handler_log->types[0], Type::LoggingControl, "It is told the type");
    zassert_equal(F().handler_log->requests_seen[0], 0U, "and runs before the stop is requested");
    zassert_equal(F().control->Requests().size(), 1U);
}

ZTEST(configuration_update_logging_stop, test_AnUpdateHandlerThatThrowsStillStops) {
    F().Build(true, true, false);
    // Replaces the stop with one whose handler throws.
    F().stop = std::make_unique<ConfigurationUpdateLoggingStop>(
        F().service, TestConfigQueue(), F().control, F().sender,
        [](Type) { throw std::runtime_error("The handler failed."); });
    zassert_true(F().stop->Initialize());
    zassert_true(F().stop->Start());

    F().Store();

    zassert_equal(F().control->Requests().size(), 1U, "The stop follows a handler that failed");
    zassert_true(F().sender->WaitForSends(1));
    zassert_equal(F().manager->Get()->triggers.size(), 1U, "and the store is no failure");
}

ZTEST(configuration_update_logging_stop, test_WithoutAControlOnlyTheStopIsBroadcast) {
    F().Start(false, true);

    F().Store();

    zassert_true(F().sender->WaitForSends(1), "A unit without a logging controller still tells the others");
    zassert_false(F().sender->Sends()[0].is_start);
    zassert_equal(F().control->Requests().size(), 0U);
}

ZTEST(configuration_update_logging_stop, test_WithoutASenderNothingIsBroadcast) {
    F().Start(true, false);

    F().Store();
    zassert_true(F().Flush());

    zassert_equal(F().control->Requests().size(), 1U, "The control stops (a remote control sends the stop itself)");
    zassert_equal(F().sender->Sends().size(), 0U);
}

ZTEST(configuration_update_logging_stop, test_DestructionClearsTheHandlerAndWaitsForAReport) {
    F().Start();
    F().control->HoldRequests();

    // The store runs on the queue, inside the report, which the control holds in RequestStop().
    TestConfigQueue()->Run([manager = F().manager] { (void)manager->Update(TriggersOn({"a"})); });
    zassert_true(F().control->WaitUntilHeld(), "The report is inside the control");

    struct Destruction {
        k_sem done;
        std::unique_ptr<ConfigurationUpdateLoggingStop> stop;
    };
    auto destruction = std::make_shared<Destruction>();
    k_sem_init(&destruction->done, 0, 1);
    destruction->stop = std::move(F().stop);

    WorkQueueThread helper("update_stop_destroyer", 4096, 5);
    zassert_true(helper.Initialize());
    helper.Run([destruction] {
        destruction->stop.reset();
        k_sem_give(&destruction->done);
    });

    zassert_not_equal(k_sem_take(&destruction->done, Ms(k_no_wait_ms)), 0,
        "The destructor must wait for the report in progress");

    F().control->Release();
    zassert_equal(k_sem_take(&destruction->done, Ms(k_wait_ms)), 0, "and returns once it is done");

    zassert_true(F().Flush());
    zassert_equal(F().sender->Sends().size(), 0U, "A broadcast still pending is dropped");

    F().Store();
    zassert_true(F().Flush());
    zassert_equal(F().control->Requests().size(), 1U, "and a later store reports nothing");
    zassert_equal(F().sender->Sends().size(), 0U);
}

ZTEST(configuration_update_logging_stop, test_DestroyingWithABroadcastPendingSendsNothing) {
    F().Start();

    HeldStore held(F().manager, {{"a"}});
    zassert_true(held.WaitUntilStored());
    zassert_equal(F().control->Requests().size(), 1U);

    F().stop.reset();

    held.Release();
    zassert_true(F().Flush());
    zassert_equal(F().sender->Sends().size(), 0U, "The pending broadcast was cancelled with the update stop");
}

ZTEST(configuration_update_logging_stop, test_ANullServiceOrQueueIsRefused) {
    bool is_service_refused = false;
    try {
        ConfigurationUpdateLoggingStop stop(nullptr, TestConfigQueue(), F().control, F().sender);
    } catch(const std::invalid_argument&) {
        is_service_refused = true;
    }
    zassert_true(is_service_refused, "It needs the configuration service");

    bool is_queue_refused = false;
    try {
        ConfigurationUpdateLoggingStop stop(F().service, nullptr, F().control, F().sender);
    } catch(const std::invalid_argument&) {
        is_queue_refused = true;
    }
    zassert_true(is_queue_refused, "and the configuration work queue");
}

ZTEST(configuration_update_logging_stop, test_StartBeforeInitializeFailsAndStartIsOnce) {
    F().stop = std::make_unique<ConfigurationUpdateLoggingStop>(F().service, TestConfigQueue(), F().control, F().sender);

    zassert_false(F().stop->Start(), "Start() needs Initialize()");
    zassert_false(F().stop->IsStarted());

    zassert_true(F().stop->Initialize());
    zassert_true(F().stop->Initialize(), "Initialize() is once, and repeats its result");
    zassert_true(F().stop->Start());
    zassert_false(F().stop->Start(), "and so is Start()");
    zassert_true(F().stop->IsStarted());

    F().Store();
    zassert_equal(F().control->Requests().size(), 1U, "A second Start() changed nothing");
}

ZTEST(configuration_update_logging_stop, test_AQueueThatDoesNotRunFailsToInitialize) {
    auto never_ran = std::make_shared<WorkQueueThread>("never_ran_update_stop", 4096, 5);
    ConfigurationUpdateLoggingStop stop(F().service, never_ran, F().control, F().sender);

    zassert_false(stop.Initialize(), "The broadcast needs a queue that runs");
    zassert_false(stop.Start());
}
