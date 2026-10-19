#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/logging_domain/control/configuration/logging_control_configuration_manager.h"
#include "domain/logging_domain/control/models/logging_control_configuration.h"

#include "logging_control_test_support.h"

using namespace logging_control_test;

using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::logging_domain::control::configuration::LoggingControlConfigurationManager;
using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::utilities::memory::Mrm;

namespace {

std::shared_ptr<FsService> FormattedFs() {
    DtFs::InitInternalFs();

    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    zassert_true(fs_service->Format(), "The internal file system must format.");

    return fs_service;
}

std::shared_ptr<LoggingControlConfigurationManager> MakeManager(const std::shared_ptr<FsService>& fs_service) {
    return std::make_shared<LoggingControlConfigurationManager>(
        std::make_unique<CborConfigurationService<CborLoggingControlConfig>>("logging_control_config", fs_service)
    );
}

LoggingControlConfiguration MakeConfiguration(const std::vector<LoggingControlBinding>& triggers) {
    LoggingControlConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    for(const auto& trigger : triggers)
        configuration.triggers.push_back(trigger);

    return configuration;
}

// One trigger of each selector kind, with values that need more than one byte.
std::vector<LoggingControlBinding> MixedTriggers() {
    LoggingControlBinding text = SensorTrigger("ignition");

    LoggingControlBinding number;
    number.event.channel = EventChannelId::Settings;
    number.event.event_type = 300;
    number.event.payload_key = 4;
    number.event.selector_key = 2;
    number.event.selector_value = -7;

    LoggingControlBinding flag;
    flag.event.channel = EventChannelId::Logging;
    flag.event.event_type = 0;
    flag.event.payload_key = 1;
    flag.event.selector_key = 0;
    flag.event.selector_value = true;

    LoggingControlBinding none;
    none.event.channel = EventChannelId::Navigation;
    none.event.event_type = 5;
    none.event.payload_key = 6;
    none.event.selector_key = 9;

    return { text, number, flag, none };
}

void AssertSame(const LoggingControlConfiguration& expected, const LoggingControlConfiguration& actual) {
    zassert_equal(actual.triggers.size(), expected.triggers.size());

    for(size_t i = 0; i < expected.triggers.size(); i++) {
        const auto& a = expected.triggers[i].event;
        const auto& b = actual.triggers[i].event;

        zassert_equal(b.channel, a.channel, "trigger %u channel", static_cast<unsigned>(i));
        zassert_equal(b.event_type, a.event_type);
        zassert_equal(b.payload_key, a.payload_key);
        zassert_equal(b.selector_key, a.selector_key);
        zassert_equal(
            b.selector_value.index(), a.selector_value.index(), "trigger %u selector kind", static_cast<unsigned>(i)
        );
        zassert_true(b.selector_value == a.selector_value, "trigger %u selector", static_cast<unsigned>(i));
    }
}

using Bytes = std::vector<uint8_t>;

// {Sensors, DataUpdated, Value, selector SensorId = "ab"}
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes K_VALID = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF5, 0x00, 0x62, 0x61, 0x62, 0x00, 0xFF, 0xFF, 0xFF };
// A trigger on channel 99, which names no channel, without a selector.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes UNKNOWN_CHANNEL = { 0x9F, 0x9F, 0x9F, 0x18, 0x63, 0x00, 0x01, 0xF4, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF };
// The same as the first with the selector a float64 (1.0), which cannot select.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes FLOAT_SELECTOR = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF5, 0x00, 0xFB, 0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF };

} // namespace

ZTEST_SUITE(logging_control_configuration_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(logging_control_configuration_manager, test_TheDefaultHasNoTriggers) {
    const auto manager = MakeManager(FormattedFs());

    const auto configuration = manager->Get();
    zassert_not_null(configuration.get(), "A unit that never stored triggers has the default");
    zassert_true(configuration->triggers.empty(), "which is none");

    const auto info = manager->GetCborConfigurationInfo();
    zassert_true(info.size > 0, "and is stored");
    zassert_true(info.is_applied);
}

ZTEST(logging_control_configuration_manager, test_StoredTriggersSurviveARestart) {
    const auto fs_service = FormattedFs();
    const auto expected = MakeConfiguration(MixedTriggers());

    {
        const auto manager = MakeManager(fs_service);
        zassert_true(manager->Update(expected));
        AssertSame(expected, *manager->Get());
    }

    // A second manager on the same file system loads what the first stored.
    const auto restarted = MakeManager(fs_service);
    zassert_true(restarted->GetCborConfigurationInfo().is_applied);
    AssertSame(expected, *restarted->Get());
}

ZTEST(logging_control_configuration_manager, test_AnInvalidConfigurationIsRefused) {
    const auto manager = MakeManager(FormattedFs());
    const auto good = MakeConfiguration({ SensorTrigger("a") });
    zassert_true(manager->Update(good));
    const auto stored = manager->GetCborConfigurationInfo();

    LoggingControlBinding no_channel = SensorTrigger("a");
    no_channel.event.channel = EventChannelId::None;
    zassert_false(manager->Update(MakeConfiguration({ no_channel })), "A trigger without a channel is refused");
    AssertSame(good, *manager->Get());
    zassert_equal(manager->GetCborConfigurationInfo().crc, stored.crc, "and nothing is stored");

    std::array<char, 128> reason{};
    zassert_false(
        manager->ApplyCborConfiguration(UNKNOWN_CHANNEL, reason), "Stored CBOR that names no channel is refused"
    );
    zassert_not_null(std::strstr(reason.data(), "unknown event channel"), "with the reason: %s", reason.data());
    AssertSame(good, *manager->Get());

    reason = {};
    zassert_false(
        manager->ApplyCborConfiguration(FLOAT_SELECTOR, reason), "A selector that cannot select does not decode"
    );
    zassert_not_null(std::strstr(reason.data(), "Malformed CBOR"), "with the reason: %s", reason.data());
    AssertSame(good, *manager->Get());
    zassert_equal(manager->GetCborConfigurationInfo().crc, stored.crc);

    zassert_true(manager->ApplyCborConfiguration(K_VALID), "Valid CBOR applies");
    zassert_equal(manager->Get()->triggers.size(), 1);
    const auto& binding = manager->Get()->triggers[0].event;
    zassert_equal(binding.channel, EventChannelId::Sensors);
    zassert_true(std::get<std::pmr::string>(binding.selector_value) == "ab");
}

ZTEST(logging_control_configuration_manager, test_TheUpdatedHandlerRunsAfterEachStore) {
    const auto fs_service = FormattedFs();
    const auto calls = std::make_shared<std::atomic<int>>(0);
    const auto thread = std::make_shared<std::atomic<k_tid_t>>(nullptr);

    const auto manager = MakeManager(fs_service);
    zassert_equal(calls->load(), 0, "Loading, or creating the default, calls nobody");

    manager->RegisterConfigurationUpdatedHandler([calls, thread] {
        (*calls)++;
        *thread = k_current_get();
    });
    zassert_equal(calls->load(), 0, "Registering calls nothing");

    zassert_true(manager->Update(MakeConfiguration({ SensorTrigger("a") })));
    zassert_equal(calls->load(), 1);
    zassert_equal(thread->load(), k_current_get(), "on the storing thread");

    zassert_false(manager->Update(MakeConfiguration({ LoggingControlBinding{} })), "A refused store calls nobody");
    zassert_equal(calls->load(), 1);

    // An apply on the configuration work queue, as SMP does it.
    const auto done = std::make_shared<k_sem>();
    k_sem_init(done.get(), 0, 1);
    const auto is_applied = std::make_shared<std::atomic<bool>>(false);
    TestConfigQueue()->Run([manager, done, is_applied] {
        is_applied->store(manager->ApplyCborConfiguration(K_VALID));
        k_sem_give(done.get());
    });
    zassert_equal(k_sem_take(done.get(), K_SECONDS(5)), 0);
    zassert_true(is_applied->load());
    zassert_equal(calls->load(), 2);
    zassert_equal(thread->load(), QueueThread(*TestConfigQueue()), "An apply calls it on the thread that stored it");

    manager->RegisterConfigurationUpdatedHandler(nullptr);
    zassert_true(manager->Update(MakeConfiguration({ SensorTrigger("b") })));
    zassert_equal(calls->load(), 2, "A cleared handler is not called");
}

// The controller clears the handler when it goes, while a store on the configuration work queue may be
// calling it: the clear returns at once, the call in progress finishes on its own reference to the
// handler, and the next store calls nothing.
ZTEST(logging_control_configuration_manager, test_AHandlerClearedDuringItsCallFinishesThatCall) {
    const auto manager = MakeManager(FormattedFs());

    struct Signals {
        k_sem entered{};
        k_sem release{};
        k_sem stored{};
        std::atomic<int> calls = 0;
        std::atomic<bool> is_stored = false;
    };
    const auto signals = std::make_shared<Signals>();
    k_sem_init(&signals->entered, 0, 1);
    k_sem_init(&signals->release, 0, 1);
    k_sem_init(&signals->stored, 0, 1);

    // Owned by the handler alone, so it lives exactly as long as the handler does.
    auto sentinel = std::make_shared<int>(0);
    const std::weak_ptr<int> weak_sentinel = sentinel;
    manager->RegisterConfigurationUpdatedHandler([signals, sentinel = std::move(sentinel)] {
        signals->calls++;
        k_sem_give(&signals->entered);
        // Gives up after 5 s, so a case that never releases cannot wedge the queue for good.
        k_sem_take(&signals->release, K_SECONDS(5));
    });
    zassert_false(weak_sentinel.expired(), "the registered handler holds the sentinel");

    TestConfigQueue()->Run([signals, manager] {
        signals->is_stored = manager->Update(MakeConfiguration({ SensorTrigger("a") }));
        k_sem_give(&signals->stored);
    });
    zassert_equal(k_sem_take(&signals->entered, K_SECONDS(2)), 0, "the store must call the handler");

    const int64_t start_ms = k_uptime_get();
    manager->RegisterConfigurationUpdatedHandler(nullptr);
    const int64_t elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(
        elapsed_ms < NO_WAIT_MS, "clearing the handler waited %lld ms for its call", static_cast<long long>(elapsed_ms)
    );
    zassert_false(weak_sentinel.expired(), "the call in progress must keep its handler alive");

    k_sem_give(&signals->release);
    zassert_equal(k_sem_take(&signals->stored, K_SECONDS(2)), 0, "the store must finish");
    zassert_true(FlushQueue(*TestConfigQueue()));
    zassert_true(signals->is_stored.load(), "the configuration must have been stored");
    zassert_true(weak_sentinel.expired(), "the handler must go once its call returned");

    zassert_true(manager->Update(MakeConfiguration({ SensorTrigger("b") })));
    zassert_equal(signals->calls.load(), 1, "a cleared handler must not be called again");
}

// Rebuilding the triggers allocates and can throw. The configuration is stored by then, so the
// store reports success, and the next store calls the handler again.
ZTEST(logging_control_configuration_manager, test_AHandlerThatThrowsDoesNotFailTheStore) {
    const auto manager = MakeManager(FormattedFs());

    const auto calls = std::make_shared<std::atomic<int>>(0);
    manager->RegisterConfigurationUpdatedHandler([calls] {
        (*calls)++;

        throw std::runtime_error("the trigger set could not be rebuilt");
    });

    zassert_true(manager->Update(MakeConfiguration({ SensorTrigger("a") })), "The store happened");
    zassert_equal(calls->load(), 1);
    zassert_equal(manager->Get()->triggers.size(), 1, "and is in use");

    zassert_true(manager->ApplyCborConfiguration(K_VALID), "An apply reports the same");
    zassert_equal(calls->load(), 2);

    // Not a std::exception.
    manager->RegisterConfigurationUpdatedHandler([calls] {
        (*calls)++;

        throw 7; // NOLINT(bugprone-std-exception-baseclass): the case is about a handler that throws something that is not a std::exception
    });
    zassert_true(manager->Update(MakeConfiguration({ SensorTrigger("b") })));
    zassert_equal(calls->load(), 3);

    manager->RegisterConfigurationUpdatedHandler(nullptr);
}
