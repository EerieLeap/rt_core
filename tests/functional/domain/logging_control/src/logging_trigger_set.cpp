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

    void Build(const std::vector<LoggingTrigger>& triggers, LoggingTriggerSet::SelectorResolver resolver = nullptr) {
        set = std::make_unique<LoggingTriggerSet>(control, triggers, std::move(resolver));
    }

    void BuildOnSensor(std::string_view sensor_id) {
        Build({SensorTrigger(sensor_id)});
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

    PublishSensorValue("switch", 0.0f);
    PublishSensorValue("switch", 1.0f);
    PublishSensorValue("switch", 0.0f);

    zassert_true(F().control->Kinds() == Kinds({Kind::Start, Kind::Stop}), "0, 1, 0 requests a start, then a stop");
    zassert_logged(0, "A working trigger logs no warning");
}

ZTEST(logging_trigger_set, test_AFirstValueOfOneStarts) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 1.0f);

    zassert_true(F().control->Kinds() == Kinds({Kind::Start}), "A switch that is already on starts logging");
}

ZTEST(logging_trigger_set, test_AFirstValueOfZeroDoesNothing) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 0.0f);
    PublishSensorValue("switch", 0.0f);
    zassert_equal(F().control->Requests().size(), 0, "A switch that is off asserts nothing");

    PublishSensorValue("switch", 1.0f);
    zassert_true(F().control->Kinds() == Kinds({Kind::Start}), "It acts on its first change");
}

ZTEST(logging_trigger_set, test_ARepeatedValueRequestsNothingMore) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 1.0f);
    PublishSensorValue("switch", 1.0f);
    PublishSensorValue("switch", 0.0f);
    PublishSensorValue("switch", 0.0f);

    zassert_true(F().control->Kinds() == Kinds({Kind::Start, Kind::Stop}));
}

ZTEST(logging_trigger_set, test_BoolsAndWholeNumbersAreRead) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", true);
    PublishSensorValue("switch", false);
    PublishSensorValue("switch", 1);
    PublishSensorValue("switch", uint32_t{0});
    PublishSensorValue("switch", 1.0f);

    zassert_true(F().control->Kinds() == Kinds({Kind::Start, Kind::Stop, Kind::Start, Kind::Stop, Kind::Start}),
        "bool, int, uint32_t and float all read as a switch position");
}

ZTEST(logging_trigger_set, test_ANonBooleanValueIsIgnored) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 1.0f);
    PublishSensorValue("switch", 0.5f);
    PublishSensorValue("switch", 2.0f);
    PublishSensorValue("switch", -1);
    PublishSensorValue("switch", std::string("1"));
    PublishSensorValue("switch", std::numeric_limits<float>::quiet_NaN());
    zassert_true(F().control->Kinds() == Kinds({Kind::Start}), "A value that is not 0 or 1 is no change");

    PublishSensorValue("switch", 0.0f);
    zassert_true(F().control->Kinds() == Kinds({Kind::Start, Kind::Stop}), "The last state was kept");
}

// A sensor that sends such values would flood the log, so each trigger says so once.
ZTEST(logging_trigger_set, test_AnUnreadableValueIsLoggedOncePerTrigger) {
    F().Build({SensorTrigger("a"), SensorTrigger("b")});
    zassert_logged(0, "Nothing is logged before a value arrives");

    PublishSensorValue("a", 0.5f);
    PublishSensorValue("a", 2.0f);
    PublishSensorValue("a", -1);
    PublishSensorValue("a", std::string("1"));
    PublishSensorValue("a", std::numeric_limits<float>::quiet_NaN());
    zassert_logged(1, "Five unreadable values of one trigger are one warning");

    PublishSensorValue("a", 1.0f);
    PublishSensorValue("a", 3.0f);
    zassert_logged(1, "Nor does a readable value in between bring it back");

    PublishSensorValue("b", 0.5f);
    zassert_logged(2, "Another trigger says it once for itself");
}

ZTEST(logging_trigger_set, test_AnUnknownStateStaysUnknownAcrossAnUnreadableValue) {
    F().BuildOnSensor("switch");

    PublishSensorValue("switch", 2.0f);
    zassert_equal(F().control->Requests().size(), 0);

    PublishSensorValue("switch", 1.0f);
    zassert_true(F().control->Kinds() == Kinds({Kind::Start}), "The first readable value is the first value");
}

ZTEST(logging_trigger_set, test_AnUnknownChannelDoesNothing) {
    LoggingTrigger no_channel = SensorTrigger("switch");
    no_channel.binding.channel = EventChannelId::None;
    // Navigation is a valid id that this binary never registers.
    LoggingTrigger unregistered = SensorTrigger("switch");
    unregistered.binding.channel = EventChannelId::Navigation;

    F().Build({no_channel, unregistered, SensorTrigger("switch")});

    zassert_equal(F().set->GetSubscribedCount(), 1, "Only the trigger on a registered channel subscribes");
    zassert_logged(2, "Each trigger that does nothing is logged once, when the set is built");

    PublishSensorValue("switch", 1.0f);
    zassert_true(F().control->Kinds() == Kinds({Kind::Start}), "The sensor trigger works");
}

ZTEST(logging_trigger_set, test_AnUnknownSensorDoesNothing) {
    F().BuildOnSensor("missing");

    PublishSensorValue("switch", 1.0f);

    zassert_equal(F().control->Requests().size(), 0, "A trigger on another sensor never fires");
}

ZTEST(logging_trigger_set, test_TheResolverIsAskedAboutEveryTrigger) {
    std::vector<std::string> asked;
    F().Build({SensorTrigger("known"), SensorTrigger("missing")}, [&asked](const EventBinding& binding) {
        asked.push_back(std::get<std::pmr::string>(binding.selector_value).c_str());

        return std::get<std::pmr::string>(binding.selector_value) == "known";
    });

    zassert_equal(asked.size(), 2);
    zassert_equal(asked[0], "known");
    zassert_equal(asked[1], "missing");
    zassert_equal(F().set->GetSubscribedCount(), 2, "A trigger the resolver does not know stays subscribed");
    zassert_logged(1, "The trigger that selects nothing is logged once, when the set is built");

    PublishSensorValue("missing", 1.0f);
    zassert_true(F().control->Kinds() == Kinds({Kind::Start}), "It works once the sensor exists");
}

ZTEST(logging_trigger_set, test_EachTriggerActsOnItsOwnSensor) {
    F().Build({SensorTrigger("a"), SensorTrigger("b")});

    PublishSensorValue("a", 1.0f);
    PublishSensorValue("b", 1.0f);
    PublishSensorValue("a", 0.0f);
    zassert_true(F().control->Kinds() == Kinds({Kind::Start, Kind::Start, Kind::Stop}));

    PublishSensorValue("b", 0.0f);
    zassert_true(F().control->Kinds() == Kinds({Kind::Start, Kind::Start, Kind::Stop, Kind::Stop}),
        "b's state was its own: it was on");
}

ZTEST(logging_trigger_set, test_ARebuiltSetSeesTheSwitchAsNew) {
    F().BuildOnSensor("switch");
    PublishSensorValue("switch", 1.0f);
    zassert_true(F().control->Kinds() == Kinds({Kind::Start}));

    F().set.reset();
    F().BuildOnSensor("switch");

    // The replay a rebuild asks the publisher for.
    PublishSensorValue("switch", 1.0f);

    zassert_true(F().control->Kinds() == Kinds({Kind::Start, Kind::Start}), "A new set starts from unknown");
}

ZTEST(logging_trigger_set, test_ANullControlIsRefused) {
    bool is_refused = false;
    try {
        const std::vector<LoggingTrigger> triggers = {SensorTrigger("switch")};
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
    PublishSensorValue("switch", 1.0f, true);
    zassert_true(blocker.WaitUntilBlocking(), "The bus thread is inside the dispatch");

    // The bus has the set's handler in its snapshot of this dispatch.
    F().set.reset();

    blocker.Release();
    zassert_true(blocker.Fence(), "The dispatch finished");
    zassert_equal(F().control->Requests().size(), 0, "A detached handler requests nothing");

    // Nothing is subscribed any more.
    PublishSensorValue("switch", 0.0f);
    PublishSensorValue("switch", 1.0f);
    zassert_equal(F().control->Requests().size(), 0);
}
