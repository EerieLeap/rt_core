#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/logging_domain/control/logging_trigger_set.h"

#include "logging_control_test_support.h"

using namespace logging_control_test;

using eerie_leap::domain::logging_domain::control::LoggingTriggerSet;
using eerie_leap::subsys::event_bus::EventBinding;

namespace {

// The set under test and the control it drives.
struct Fixture {
    std::shared_ptr<FakeLoggingControl> control = std::make_shared<FakeLoggingControl>();
    std::unique_ptr<LoggingTriggerSet> set;

    void Build(
        const std::vector<LoggingTrigger>& triggers,
        const LoggingTriggerSet::SelectorResolver& resolver = nullptr,
        LoggingTriggerSet::FirstValue first_value = LoggingTriggerSet::FirstValue::OneStarts
    ) {

        set = std::make_unique<LoggingTriggerSet>(control, triggers, resolver, first_value);
    }

    void BuildOnSensor(std::string_view sensor_id) {
        Build({ SensorTrigger(sensor_id) });
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
    LogCounter::Get().Watch("logging_trigger_set_logger");
}

void After(void*) {
    // The set first: it detaches its handlers while the control is still there.
    if(Current() != nullptr) {
        Current()->set.reset();
        delete Current();
        Current() = nullptr;
    }

    // The bus delivers what the case published before the next one subscribes.
    k_msleep(20);
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(logging_trigger_set, NULL, SetUp, Before, After, NULL);

ZTEST(logging_trigger_set, test_ASwitchTurnedOnStartsAndTurnedOffStops) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);
    PublishSensorValue("switch", 0.0F);

    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "0, 1, 0 requests a start, then a stop");
    ZASSERT_LOGGED(0, "A working trigger logs no warning");
}

ZTEST(logging_trigger_set, test_ATriggerRequestsForItsTarget) {
    auto all = SensorTrigger("all");
    auto one = SensorTrigger("one");
    one.target_uid = 0x1234;
    auto other = SensorTrigger("other");
    other.target_uid = 0xABCD;
    F().Build({ all, one, other });

    PublishSensorValue("all", 1.0F);
    PublishSensorValue("one", 1.0F);
    PublishSensorValue("other", 1.0F);
    PublishSensorValue("one", 0.0F);

    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Start, Kind::Start, Kind::Stop }));
    zassert_true(
        F().control->Targets() == std::vector<uint32_t>({ ALL_UNITS, 0x1234, 0xABCD, 0x1234 }),
        "Each request names the target of its trigger"
    );
}

ZTEST(logging_trigger_set, test_AFirstValueOfOneStarts) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 1.0F);

    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "A switch that is already on starts logging");
}

ZTEST(logging_trigger_set, test_AFirstValueOfZeroDoesNothing) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 0.0F);
    zassert_equal(F().control->Requests().size(), 0, "A switch that is off asserts nothing");

    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "It acts on its first change");
}

ZTEST(logging_trigger_set, test_ARepeatedValueRequestsNothingMore) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 1.0F);
    PublishSensorValue("switch", 1.0F);
    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 0.0F);

    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Stop }));
}

ZTEST(logging_trigger_set, test_BoolsAndWholeNumbersAreRead) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", true);
    PublishSensorValue("switch", false);
    PublishSensorValue("switch", 1);
    PublishSensorValue("switch", uint32_t{ 0 });
    PublishSensorValue("switch", 1.0F);

    zassert_true(
        F().control->Kinds() == Kinds({ Kind::Start, Kind::Stop, Kind::Start, Kind::Stop, Kind::Start }),
        "bool, int, uint32_t and float all read as a switch position"
    );
}

ZTEST(logging_trigger_set, test_ANonBooleanValueIsIgnored) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 1.0F);
    PublishSensorValue("switch", 0.5F);
    PublishSensorValue("switch", 2.0F);
    PublishSensorValue("switch", -1);
    PublishSensorValue("switch", std::string("1"));
    PublishSensorValue("switch", std::numeric_limits<float>::quiet_NaN());
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "A value that is not 0 or 1 is no change");

    PublishSensorValue("switch", 0.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "The last state was kept");
}

// A sensor that sends such values would flood the log, so each trigger says so once.
ZTEST(logging_trigger_set, test_AnUnreadableValueIsLoggedOncePerTrigger) {
    F().Build({ SensorTrigger("a"), SensorTrigger("b") });
    ZASSERT_LOGGED(0, "Nothing is logged before a value arrives");

    PublishSensorValue("a", 0.5F);
    PublishSensorValue("a", 2.0F);
    PublishSensorValue("a", -1);
    PublishSensorValue("a", std::string("1"));
    PublishSensorValue("a", std::numeric_limits<float>::quiet_NaN());
    ZASSERT_LOGGED(1, "Five unreadable values of one trigger are one warning");

    PublishSensorValue("a", 1.0F);
    PublishSensorValue("a", 3.0F);
    ZASSERT_LOGGED(1, "Nor does a readable value in between bring it back");

    PublishSensorValue("b", 0.5F);
    ZASSERT_LOGGED(2, "Another trigger says it once for itself");
}

ZTEST(logging_trigger_set, test_AnUnknownStateStaysUnknownAcrossAnUnreadableValue) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 2.0F);
    zassert_equal(F().control->Requests().size(), 0);

    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "The first readable value is the first value");
}

ZTEST(logging_trigger_set, test_AnUnknownChannelDoesNothing) {
    LoggingTrigger no_channel = SensorTrigger("switch");
    no_channel.binding.channel = EventChannelId::None;
    // Navigation is a valid id that this binary never registers.
    LoggingTrigger unregistered = SensorTrigger("switch");
    unregistered.binding.channel = EventChannelId::Navigation;

    F().Build({ no_channel, unregistered, SensorTrigger("switch") });

    zassert_equal(F().set->GetSubscribedCount(), 1, "Only the trigger on a registered channel subscribes");
    ZASSERT_LOGGED(2, "Each trigger that does nothing is logged once, when the set is built");

    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "The sensor trigger works");
}

ZTEST(logging_trigger_set, test_AnUnknownSensorDoesNothing) {
    F().BuildOnSensor("missing");

    PublishSensorValue("switch", 1.0F);

    zassert_equal(F().control->Requests().size(), 0, "A trigger on another sensor never fires");
}

ZTEST(logging_trigger_set, test_TheResolverIsAskedAboutEveryTrigger) {
    std::vector<std::string> asked;
    F().Build({ SensorTrigger("known"), SensorTrigger("missing") }, [&asked](const EventBinding& binding) {
        asked.emplace_back(std::get<std::pmr::string>(binding.selector_value).c_str());

        return std::get<std::pmr::string>(binding.selector_value) == "known";
    });

    zassert_equal(asked.size(), 2);
    zassert_equal(asked[0], "known");
    zassert_equal(asked[1], "missing");
    zassert_equal(F().set->GetSubscribedCount(), 2, "A trigger the resolver does not know stays subscribed");
    ZASSERT_LOGGED(1, "The trigger that selects nothing is logged once, when the set is built");

    PublishSensorValue("missing", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "It works once the sensor exists");
}

ZTEST(logging_trigger_set, test_EachTriggerActsOnItsOwnSensor) {
    F().Build({ SensorTrigger("a"), SensorTrigger("b") });

    PublishSensorValue("a", 1.0F);
    PublishSensorValue("b", 1.0F);
    PublishSensorValue("a", 0.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Start, Kind::Stop }));

    PublishSensorValue("b", 0.0F);
    zassert_true(
        F().control->Kinds() == Kinds({ Kind::Start, Kind::Start, Kind::Stop, Kind::Stop }),
        "b's state was its own: it was on"
    );
}

ZTEST(logging_trigger_set, test_ARebuiltSetSeesTheSwitchAsNew) {
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }));

    F().set.reset();
    F().BuildOnSensor("switch");

    // The replay a rebuild asks the publisher for.
    PublishSensorValue("switch", 1.0F);

    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Start }), "A new set starts from unknown");
}

ZTEST(logging_trigger_set, test_ANullControlIsRefused) {
    bool is_refused = false;
    try {
        const std::vector<LoggingTrigger> triggers = { SensorTrigger("switch") };
        LoggingTriggerSet set(nullptr, triggers);
    } catch(const std::invalid_argument&) {
        is_refused = true;
    }

    zassert_true(is_refused, "A set without a control would crash on the first value");
}

ZTEST(logging_trigger_set, test_DestroyingTheSetWhileADispatchIsInFlightRequestsNothing) {
    // Made before the set, so its handler runs first in a dispatch and holds the bus thread there.
    SensorsBlocker blocker(SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated);

    F().BuildOnSensor("switch");

    blocker.Arm();
    PublishSensorValue("switch", 1.0F, true);
    zassert_true(blocker.WaitUntilBlocking(), "The bus thread is inside the dispatch");

    // The bus has the set's handler in its snapshot of this dispatch.
    F().set.reset();

    blocker.Release();
    zassert_true(blocker.Fence(), "The dispatch finished");
    zassert_equal(F().control->Requests().size(), 0, "A detached handler requests nothing");

    // Nothing is subscribed any more.
    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);
    zassert_equal(F().control->Requests().size(), 0);
}

// ======== A configuration update (OnConfigurationUpdated) ========

namespace {

const LoggingTriggerSet::SelectorResolver KNOWN = [](const EventBinding&) {
    return true;
};
const LoggingTriggerSet::SelectorResolver K_UNKNOWN = [](const EventBinding&) {
    return false;
};

// Polls @p predicate every 2 ms until it holds or k_wait_ms passed.
template<typename TPredicate>
[[nodiscard]] bool Eventually(TPredicate predicate) {
    const int64_t deadline = k_uptime_get() + WAIT_MS;
    while(!predicate()) {
        if(k_uptime_get() >= deadline)
            return false;

        k_msleep(2);
    }

    return true;
}

} // namespace

// The old generation keeps publishing until processing stops, so what a withdrawn trigger is
// published afterwards must request nothing, whatever it is: a 1 would start, a 0 after it would
// stop. A value equal to the one it saw last would pass for no change anyway, so the checks publish
// both.
ZTEST(logging_trigger_set, test_AWithdrawnTriggerForgetsItsSensorAndRequestsNothing) {
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }));

    F().set->OnConfigurationUpdated(K_UNKNOWN);

    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);
    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "The old generation's values request nothing");
    zassert_equal(F().set->GetSubscribedCount(), 1, "and the trigger stays subscribed");
}

ZTEST(logging_trigger_set, test_ARestoredTriggerReadsABaseline) {
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 1.0F);
    F().set->OnConfigurationUpdated(K_UNKNOWN);
    F().set->OnConfigurationUpdated(KNOWN);

    PublishSensorValue("switch", 0.0F);
    zassert_true(
        F().control->Kinds() == Kinds({ Kind::Start }), "A first value of 0 is a baseline and asserts nothing"
    );

    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Start }), "and the 1 after it is a change");
}

ZTEST(logging_trigger_set, test_ARestoredTriggerThatIsOnStartsNothing) {
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 1.0F);
    F().set->OnConfigurationUpdated(K_UNKNOWN);
    F().set->OnConfigurationUpdated(KNOWN);

    PublishSensorValue("switch", 1.0F);

    zassert_true(
        F().control->Kinds() == Kinds({ Kind::Start }),
        "The sensor back with a 1 is a baseline: the update stopped logging, and it starts nothing"
    );

    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);
    zassert_true(
        F().control->Kinds() == Kinds({ Kind::Start, Kind::Stop, Kind::Start }),
        "and the switch acts on its next change"
    );
}

// The hazard the withdrawal ends: a sensor that returns under its id starts its switch at 0, which a
// trigger that kept its state would read as 1 to 0 and stop whatever runs then.
ZTEST(logging_trigger_set, test_AReturningSensorNoLongerStopsWhatRuns) {
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 1.0F);
    F().set->OnConfigurationUpdated(K_UNKNOWN);
    F().set->OnConfigurationUpdated(KNOWN);

    PublishSensorValue("switch", 0.0F);

    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "A first 0 stops nothing");
}

ZTEST(logging_trigger_set, test_AnUpdateThatFindsTheSensorKeepsTheState) {
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 1.0F);

    F().set->OnConfigurationUpdated(KNOWN);
    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "The 1 is still no change");

    PublishSensorValue("switch", 0.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "and the 0 is");
}

ZTEST(logging_trigger_set, test_OnlyTheTriggersWhoseSensorIsGoneAreWithdrawn) {
    F().Build({ SensorTrigger("a"), SensorTrigger("b") });
    PublishSensorValue("a", 1.0F);
    PublishSensorValue("b", 1.0F);

    // The resolver is asked about every trigger, with its own binding.
    std::vector<std::string> asked;
    F().set->OnConfigurationUpdated([&asked](const EventBinding& binding) {
        // NOLINTNEXTLINE(readability-redundant-string-cstr): a pmr string does not convert to std::string without it
        const std::string id = std::get<std::pmr::string>(binding.selector_value).c_str();
        asked.push_back(id);

        return id == "a";
    });
    zassert_equal(asked.size(), 2U);

    PublishSensorValue("b", 0.0F);
    PublishSensorValue("b", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Start }), "b is withdrawn");

    PublishSensorValue("a", 0.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Start, Kind::Stop }), "and a kept its state");
}

ZTEST(logging_trigger_set, test_ANullResolverOrAnEmptySetWithdrawsNothing) {
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 1.0F);

    F().set->OnConfigurationUpdated(nullptr);
    PublishSensorValue("switch", 0.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "A null resolver changes nothing");

    F().set.reset();
    F().Build({});
    F().set->OnConfigurationUpdated(K_UNKNOWN);
    zassert_equal(F().set->GetSubscribedCount(), 0U, "A set without triggers has nothing to withdraw");
}

ZTEST(logging_trigger_set, test_AResolverThatThrowsWithdrawsNothing) {
    F().Build({ SensorTrigger("a"), SensorTrigger("b") });
    PublishSensorValue("a", 1.0F);
    PublishSensorValue("b", 1.0F);

    bool is_thrown = false;
    try {
        F().set->OnConfigurationUpdated([](const EventBinding& binding) -> bool {
            if(std::get<std::pmr::string>(binding.selector_value) == "b")
                throw std::runtime_error("The sensors could not be read.");

            return false;
        });
    } catch(const std::runtime_error&) {
        is_thrown = true;
    }
    zassert_true(is_thrown, "What the resolver throws reaches the caller");

    PublishSensorValue("a", 0.0F);
    PublishSensorValue("b", 0.0F);
    zassert_true(
        F().control->Kinds() == Kinds({ Kind::Start, Kind::Start, Kind::Stop, Kind::Stop }),
        "Neither trigger was withdrawn: the answers are applied together, after all were asked"
    );
}

ZTEST(logging_trigger_set, test_ABaselineIsSetEvenWhenTheResolverThrows) {
    F().Build({ SensorTrigger("a") });

    bool is_thrown = false;
    try {
        F().set->OnConfigurationUpdated([](const EventBinding&) -> bool {
            throw std::runtime_error("No sensors.");
        });
    } catch(const std::runtime_error&) {
        is_thrown = true;
    }
    zassert_true(is_thrown);

    PublishSensorValue("a", 1.0F);
    zassert_equal(F().control->Requests().size(), 0U, "The update was made: the first value is a baseline");

    PublishSensorValue("a", 0.0F);
    PublishSensorValue("a", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Stop, Kind::Start }), "and the trigger works");
}

// A trigger that selected nothing when the set was built is withdrawn by the first update that still
// finds nothing; one that finds it restores it.
ZTEST(logging_trigger_set, test_ATriggerThatSelectedNothingIsWithdrawnByTheFirstUpdate) {
    F().Build({ SensorTrigger("missing") }, K_UNKNOWN);
    ZASSERT_LOGGED(1, "Logged when the set is built");

    F().set->OnConfigurationUpdated(K_UNKNOWN);
    ZASSERT_LOGGED(2, "and again when the update still finds nothing");

    PublishSensorValue("missing", 1.0F);
    zassert_equal(F().control->Requests().size(), 0U, "It waits for its sensor");

    F().set->OnConfigurationUpdated(KNOWN);
    PublishSensorValue("missing", 1.0F);
    zassert_equal(
        F().control->Requests().size(), 0U, "A sensor an update brings starts nothing: its first value is a baseline"
    );

    PublishSensorValue("missing", 0.0F);
    PublishSensorValue("missing", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Stop, Kind::Start }), "and it acts from its next change");
}

ZTEST(logging_trigger_set, test_AWithdrawalIsLoggedOnceAndARestoreIsNoWarning) {
    F().BuildOnSensor("switch");
    ZASSERT_LOGGED(0, "Nothing is logged for a working trigger");

    F().set->OnConfigurationUpdated(K_UNKNOWN);
    ZASSERT_LOGGED(1, "A withdrawal is logged");

    F().set->OnConfigurationUpdated(K_UNKNOWN);
    F().set->OnConfigurationUpdated(K_UNKNOWN);
    ZASSERT_LOGGED(1, "once, however many applies find the sensor missing");

    F().set->OnConfigurationUpdated(KNOWN);
    F().set->OnConfigurationUpdated(KNOWN);
    ZASSERT_LOGGED(1, "and the sensor back is no warning");

    F().set->OnConfigurationUpdated(K_UNKNOWN);
    ZASSERT_LOGGED(2, "A second loss is a second withdrawal");
}

// The update asks the resolver, which reads the sensors under their manager's lock, outside the
// set's guard, so a delivery on the bus thread never waits for it; then it takes the guard, which waits
// for the delivery in progress. A value that was delivered before the withdrawal is seen; every later
// one is not.
ZTEST(logging_trigger_set, test_AnUpdateWaitsForADeliveryInProgress) {
    F().BuildOnSensor("switch");
    F().control->HoldRequests();

    PublishSensorValue("switch", 1.0F, true);
    zassert_true(F().control->WaitUntilHeld(), "The bus thread is inside the trigger's handler, holding the guard");

    struct Context {
        k_sem done{};
        std::atomic<bool> was_asked = false;

        Context() {
            k_sem_init(&done, 0, 1);
        }
    };
    auto context = std::make_shared<Context>();

    TestConfigQueue()->Run([context, &set = F().set] {
        set->OnConfigurationUpdated([context](const EventBinding&) {
            context->was_asked = true;

            return false;
        });
        k_sem_give(&context->done);
    });

    zassert_true(
        Eventually([&context] {
            return context->was_asked.load();
        }),
        "The resolver does not wait for the delivery"
    );
    zassert_not_equal(k_sem_take(&context->done, Ms(NO_WAIT_MS)), 0, "but the withdrawal does");

    F().control->Release();
    zassert_equal(k_sem_take(&context->done, Ms(WAIT_MS)), 0, "and goes through once the delivery is done");
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "The value it delivered was seen");

    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "and the withdrawal holds from then on");
}

// ======== After an update every first value is a baseline (L9) ========

ZTEST(logging_trigger_set, test_AfterAnUpdateAFirstValueIsABaseline) {
    F().BuildOnSensor("switch");
    F().set->OnConfigurationUpdated();

    PublishSensorValue("switch", 1.0F);
    zassert_equal(
        F().control->Requests().size(), 0U, "A switch that is on asserts nothing: the update stopped logging"
    );

    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Stop, Kind::Start }), "It acts on its next change");
}

ZTEST(logging_trigger_set, test_AfterAnUpdateAFirstValueOfZeroIsABaselineToo) {
    F().BuildOnSensor("switch");
    F().set->OnConfigurationUpdated();

    PublishSensorValue("switch", 0.0F);
    PublishSensorValue("switch", 1.0F);

    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "0 to 1 starts");
}

ZTEST(logging_trigger_set, test_AfterAnUpdateAKnownValueIsKept) {
    F().Build({ SensorTrigger("a"), SensorTrigger("b") });
    PublishSensorValue("a", 0.0F);
    PublishSensorValue("b", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "b starts at boot");

    F().set->OnConfigurationUpdated();

    PublishSensorValue("a", 1.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Start }), "a saw its 0 before: 0 to 1 starts");

    PublishSensorValue("b", 1.0F);
    zassert_true(
        F().control->Kinds() == Kinds({ Kind::Start, Kind::Start }), "b saw its 1 before: 1 again is no change"
    );

    PublishSensorValue("b", 0.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Start, Kind::Start, Kind::Stop }), "and its 0 is");
}

ZTEST(logging_trigger_set, test_ASetBuiltWithBaselinesStartsNothingOnAFirstOne) {
    F().Build({ SensorTrigger("switch") }, nullptr, LoggingTriggerSet::FirstValue::IsBaseline);

    PublishSensorValue("switch", 1.0F);
    zassert_equal(F().control->Requests().size(), 0U, "A rebuilt set after an update: the replayed 1 is a baseline");

    PublishSensorValue("switch", 0.0F);
    zassert_true(F().control->Kinds() == Kinds({ Kind::Stop }), "and it acts on the next change");
}

ZTEST(logging_trigger_set, test_AnUpdateWithoutTriggersOrRepeatedChangesNothing) {
    F().Build({});
    F().set->OnConfigurationUpdated();
    zassert_equal(F().set->GetSubscribedCount(), 0U);

    F().set.reset();
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 0.0F);
    F().set->OnConfigurationUpdated();
    F().set->OnConfigurationUpdated();
    PublishSensorValue("switch", 1.0F);

    zassert_true(F().control->Kinds() == Kinds({ Kind::Start }), "Updates in a row change no known value");
}
