#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/ztest.h>

#include "domain/logging_domain/control/i_logging_control.h"
#include "domain/logging_domain/models/log_session_info.h"
#include "domain/logging_domain/smp/logging_mgmt_group.h"

#include "smp_groups_test_support.h"

using namespace smp_groups_test;

using eerie_leap::domain::logging_domain::control::ILoggingControl;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;
using eerie_leap::domain::logging_domain::models::LogSessionInfo;
using eerie_leap::domain::logging_domain::models::LogSessionProvider;
using eerie_leap::domain::logging_domain::smp::LoggingMgmtGroup;

using Command = LoggingMgmtGroup::Command;

namespace {

constexpr uint64_t SINCE_MS = 1'790'000'000'123ULL;
constexpr uint64_t FILE_BYTES = 5'000'000'000ULL;

uint8_t Id(Command command) {
    return std::to_underlying(command);
}

struct Request {
    bool is_start;
    uint32_t uid;

    bool operator==(const Request& other) const {
        return is_start == other.is_start && uid == other.uid;
    }
};

class FakeControl final : public ILoggingControl {
public:
    std::vector<Request> requests;
    bool is_active = false;
    LoggingStatusReason reason = LoggingStatusReason::Stopped;

    void RequestStart(uint32_t target_uid) override {
        requests.push_back({ true, target_uid });
    }
    void RequestStop(uint32_t target_uid) override {
        requests.push_back({ false, target_uid });
    }
    bool HandleLoggingCommand(bool /*is_start*/) override {
        return true;
    }
    [[nodiscard]] bool IsActive() const override {
        return is_active;
    }
    [[nodiscard]] LoggingStatusReason GetReason() const override {
        return reason;
    }
};

LogSessionInfo MakeSession(const char* file_name) {
    LogSessionInfo info;
    info.since_ms = static_cast<int64_t>(SINCE_MS);
    info.file_bytes = FILE_BYTES;
    std::strncpy(info.file_name.data(), file_name, info.file_name.size() - 1);

    return info;
}

class Harness {
public:
    std::shared_ptr<FakeControl> control = std::make_shared<FakeControl>();
    std::optional<LogSessionInfo> session;
    std::unique_ptr<LoggingMgmtGroup> group;
    SmpTestClient client;

    explicit Harness(bool has_provider) {
        LogSessionProvider provider;
        if(has_provider) {
            provider = [this] {
                return session;
            };
        }

        group = std::make_unique<LoggingMgmtGroup>(control, std::move(provider));
        group->Register();
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    ~Harness() {
        group.reset();
    }

    Response Status() {
        return client.Read(SmpGroupId::LOGGING, Id(Command::STATUS));
    }
    Response Start(const Bytes& body) {
        return client.Write(SmpGroupId::LOGGING, Id(Command::START), body);
    }
    Response Stop(const Bytes& body) {
        return client.Write(SmpGroupId::LOGGING, Id(Command::STOP), body);
    }
};

} // namespace

ZTEST_SUITE(smp_logging_group, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_logging_group, test_status_without_provider_has_active_and_reason_only) {
    Harness harness(false);
    harness.control->reason = LoggingStatusReason::Unavailable;

    const auto idle = harness.Status();
    zassert_true(idle.IsOk());
    zassert_false(idle.Flag("active"));
    zassert_equal(idle.Value("reason"), std::to_underlying(LoggingStatusReason::Unavailable));
    zassert_false(idle.Has("since_ms"));
    zassert_false(idle.Has("file"));
    zassert_false(idle.Has("bytes"));

    // Active carries no reason, whatever the control last kept.
    harness.control->is_active = true;
    const auto active = harness.Status();
    zassert_true(active.Flag("active"));
    zassert_equal(active.Value("reason"), 0);
}

ZTEST(smp_logging_group, test_status_with_session_and_file_has_every_field) {
    Harness harness(true);
    harness.control->is_active = true;
    harness.session = MakeSession("log_2026.elog");

    const auto response = harness.Status();
    zassert_true(response.IsOk());
    zassert_true(response.Flag("active"));
    zassert_equal(response.Value("reason"), 0);
    zassert_equal(response.WideValue("since_ms"), SINCE_MS);
    zassert_equal(response.texts.at("file"), "log_2026.elog");
    zassert_equal(response.WideValue("bytes"), FILE_BYTES);
}

ZTEST(smp_logging_group, test_status_with_session_and_no_file_has_since_only) {
    Harness harness(true);
    harness.control->is_active = true;
    harness.session = MakeSession("");

    const auto response = harness.Status();
    zassert_true(response.IsOk());
    zassert_equal(response.WideValue("since_ms"), SINCE_MS);
    zassert_false(response.Has("file"));
    zassert_false(response.Has("bytes"));
}

ZTEST(smp_logging_group, test_status_with_provider_and_no_session_has_active_and_reason_only) {
    Harness harness(true);
    harness.control->reason = LoggingStatusReason::NoSpace;

    const auto response = harness.Status();
    zassert_true(response.IsOk());
    zassert_false(response.Flag("active"));
    zassert_equal(response.Value("reason"), std::to_underlying(LoggingStatusReason::NoSpace));
    zassert_false(response.Has("since_ms"));
}

ZTEST(smp_logging_group, test_start_and_stop_take_the_uid_and_default_to_all_units) {
    Harness harness(false);

    zassert_true(harness.Start(CborMap().Build()).IsOk());
    zassert_true(harness.Start(CborMap().Put("uid", 0).Build()).IsOk());
    zassert_true(harness.Start(CborMap().Put("uid", 0xFFFFFFFFU).Build()).IsOk());
    zassert_true(harness.Stop(CborMap().Build()).IsOk());
    zassert_true(harness.Stop(CborMap().Put("uid", 0).Build()).IsOk());
    zassert_true(harness.Stop(CborMap().Put("uid", 77).Build()).IsOk());

    const std::vector<Request> expected = {
        { true, ILoggingControl::ALL_UNITS },
        { true, ILoggingControl::ALL_UNITS },
        { true, 0xFFFFFFFFU },
        { false, ILoggingControl::ALL_UNITS },
        { false, ILoggingControl::ALL_UNITS },
        { false, 77 },
    };
    zassert_true(harness.control->requests == expected, "The requests reach the control as sent");
}

ZTEST(smp_logging_group, test_a_uid_of_the_wrong_type_is_a_bad_request) {
    Harness harness(false);

    zassert_false(harness.Start(CborMap().PutText("uid", "7").Build()).IsOk());
    zassert_false(harness.Stop(CborMap().PutText("uid", "7").Build()).IsOk());
    zassert_true(harness.control->requests.empty(), "A bad request starts and stops nothing");
}
