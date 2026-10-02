#include <memory>
#include <optional>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_thread.h"
#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/configuration_domain/smp/config_mgmt_group.h"

#include "smp_groups_test_support.h"

using namespace smp_groups_test;

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::configuration_domain::smp::ConfigMgmtGroup;

using Command = ConfigMgmtGroup::Command;
using Error = ConfigMgmtGroup::Error;

namespace {

constexpr uint32_t CANBUS = std::to_underlying(ConfigurationService::Type::Canbus);
constexpr uint32_t UI = std::to_underlying(ConfigurationService::Type::Ui);
constexpr uint32_t ADC = std::to_underlying(ConfigurationService::Type::Adc);
constexpr uint32_t UI_MAX_SIZE = 2000;
constexpr size_t WRITE_CHUNK_SIZE = 900;

uint32_t Crc(const Bytes& data) {
    return crc32_ieee(data.data(), data.size());
}

uint8_t Id(Command command) {
    return std::to_underlying(command);
}

void AssertError(const Response& response, Error error) {
    zassert_equal(response.error_group.value_or(0), std::to_underlying(SmpGroupId::CONFIG));
    zassert_equal(response.error.value_or(0), std::to_underlying(error),
        "Expected error %u, got %u", std::to_underlying(error), response.error.value_or(0));
}

class Harness {
public:
    std::shared_ptr<WorkQueueThread> work_queue;
    std::shared_ptr<ConfigurationService> configuration_service = std::make_shared<ConfigurationService>();
    std::shared_ptr<FakeConfigurationManager> canbus = std::make_shared<FakeConfigurationManager>();
    std::shared_ptr<FakeConfigurationManager> ui = std::make_shared<FakeConfigurationManager>();
    std::unique_ptr<ConfigMgmtGroup> group;
    SmpTestClient client;

    Harness() {
        work_queue = std::make_shared<WorkQueueThread>("smp_groups_config_wq", 4096, 5);
        zassert_true(work_queue->Initialize());

        configuration_service->RegisterCborConfigurationManager(ConfigurationService::Type::Canbus, canbus);
        configuration_service->RegisterCborConfigurationManager(ConfigurationService::Type::Ui, ui, UI_MAX_SIZE);

        group = std::make_unique<ConfigMgmtGroup>(configuration_service, work_queue);
        zassert_true(group->Initialize());
        group->Register();
    }

    ~Harness() {
        group.reset();
        work_queue.reset();
    }

    Response Crc(uint32_t type) {
        return client.Read(SmpGroupId::CONFIG, Id(Command::CRC), CborMap().Put("type", type).Build());
    }

    Response ReadChunk(uint32_t type, uint32_t offset) {
        return client.Read(SmpGroupId::CONFIG, Id(Command::READ), CborMap().Put("type", type).Put("off", offset).Build());
    }

    Response WriteFirst(uint32_t type, std::span<const uint8_t> chunk, uint32_t length, uint32_t crc,
        std::optional<uint32_t> token = std::nullopt) {

        CborMap body;
        body.Put("type", type).Put("off", 0).Put("len", length).Put("crc", crc).Put("data", chunk);
        if(token.has_value())
            body.Put("tok", *token);

        return client.Write(SmpGroupId::CONFIG, Id(Command::WRITE), body.Build());
    }

    Response WriteNext(uint32_t type, uint32_t offset, uint32_t token, std::span<const uint8_t> chunk) {
        return client.Write(SmpGroupId::CONFIG, Id(Command::WRITE),
            CborMap().Put("type", type).Put("off", offset).Put("tok", token).Put("data", chunk).Build());
    }

    // Returns the final response.
    Response WriteAll(uint32_t type, const Bytes& data, uint32_t crc) {
        const size_t first = std::min(data.size(), WRITE_CHUNK_SIZE);
        auto response = WriteFirst(type, std::span(data).first(first), data.size(), crc);
        if(!response.IsOk())
            return response;

        const uint32_t token = response.Value("tok");
        for(size_t offset = first; offset < data.size(); offset += WRITE_CHUNK_SIZE) {
            const size_t size = std::min(WRITE_CHUNK_SIZE, data.size() - offset);
            response = WriteNext(type, offset, token, std::span(data).subspan(offset, size));
            if(!response.IsOk())
                return response;
        }

        return response;
    }

    Bytes ReadAll(uint32_t type, uint32_t* crc = nullptr) {
        auto response = ReadChunk(type, 0);
        zassert_true(response.IsOk());

        const uint32_t length = response.Value("len");
        if(crc != nullptr)
            *crc = response.Value("crc");

        Bytes data = response.bytes["data"];
        while(data.size() < length) {
            response = ReadChunk(type, data.size());
            zassert_true(response.IsOk());
            zassert_equal(response.Value("off"), data.size());

            const auto& chunk = response.bytes["data"];
            zassert_false(chunk.empty());
            data.insert(data.end(), chunk.begin(), chunk.end());
        }

        return data;
    }
};

} // namespace

struct smp_config_group_fixture {
    Harness* harness;
};

static void* ConfigGroupSetup() {
    static smp_config_group_fixture fixture{};

    return &fixture;
}

static void ConfigGroupBefore(void* data) {
    static_cast<smp_config_group_fixture*>(data)->harness = new Harness();
}

static void ConfigGroupAfter(void* data) {
    auto* fixture = static_cast<smp_config_group_fixture*>(data);
    delete fixture->harness;
    fixture->harness = nullptr;
}

ZTEST_SUITE(smp_config_group, NULL, ConfigGroupSetup, ConfigGroupBefore, ConfigGroupAfter, NULL);

ZTEST_F(smp_config_group, test_list_reports_the_registered_types_and_caps) {
    auto response = fixture->harness->client.Read(SmpGroupId::CONFIG, Id(Command::LIST));

    zassert_true(response.IsOk());
    zassert_equal(response.entries.size(), 2);
    zassert_equal(response.entries[0]["type"], CANBUS);
    zassert_equal(response.entries[0]["max"], ConfigurationService::DEFAULT_MAX_CBOR_SIZE);
    zassert_equal(response.entries[1]["type"], UI);
    zassert_equal(response.entries[1]["max"], UI_MAX_SIZE);
}

ZTEST_F(smp_config_group, test_crc_describes_the_current_configuration) {
    Harness& harness = *fixture->harness;
    harness.canbus->stored = Pattern(1500, 7);

    auto response = harness.Crc(CANBUS);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("type"), CANBUS);
    zassert_equal(response.Value("len"), 1500);
    zassert_equal(response.Value("crc"), Crc(harness.canbus->stored));
}

ZTEST_F(smp_config_group, test_unknown_type_is_rejected) {
    Harness& harness = *fixture->harness;

    AssertError(harness.Crc(ADC), Error::UNKNOWN_TYPE);
    AssertError(harness.ReadChunk(ADC, 0), Error::UNKNOWN_TYPE);
    AssertError(harness.WriteFirst(ADC, Pattern(10, 1), 10, 0), Error::UNKNOWN_TYPE);
}

ZTEST_F(smp_config_group, test_chunked_read_returns_the_configuration) {
    Harness& harness = *fixture->harness;
    harness.canbus->stored = Pattern(3000, 3);

    uint32_t crc = 0;
    auto data = harness.ReadAll(CANBUS, &crc);

    zassert_true(data == harness.canbus->stored);
    zassert_equal(crc, Crc(harness.canbus->stored));
}

ZTEST_F(smp_config_group, test_read_serves_the_snapshot_taken_at_offset_zero) {
    Harness& harness = *fixture->harness;
    const Bytes original = Pattern(2500, 5);
    harness.canbus->stored = original;

    auto first = harness.ReadChunk(CANBUS, 0);
    zassert_true(first.IsOk());

    harness.canbus->stored = Pattern(2500, 99);

    Bytes data = first.bytes["data"];
    while(data.size() < first.Value("len")) {
        auto response = harness.ReadChunk(CANBUS, data.size());
        zassert_true(response.IsOk());
        data.insert(data.end(), response.bytes["data"].begin(), response.bytes["data"].end());
    }

    zassert_true(data == original, "A change during the read does not mix two configurations");
    zassert_equal(first.Value("crc"), Crc(original));
}

ZTEST_F(smp_config_group, test_read_without_a_snapshot_is_rejected) {
    Harness& harness = *fixture->harness;
    harness.canbus->stored = Pattern(2000, 1);

    AssertError(harness.ReadChunk(CANBUS, 100), Error::BAD_OFFSET);

    zassert_true(harness.ReadChunk(CANBUS, 0).IsOk());
    AssertError(harness.ReadChunk(UI, 100), Error::BAD_OFFSET);
}

ZTEST_F(smp_config_group, test_chunked_write_applies_the_configuration) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(2500, 11);

    auto first = harness.WriteFirst(CANBUS, std::span(data).first(WRITE_CHUNK_SIZE), data.size(), Crc(data));
    zassert_true(first.IsOk());
    zassert_equal(first.Value("off"), WRITE_CHUNK_SIZE);
    zassert_not_equal(first.Value("tok"), 0);

    const uint32_t token = first.Value("tok");
    auto second = harness.WriteNext(CANBUS, WRITE_CHUNK_SIZE, token, std::span(data).subspan(WRITE_CHUNK_SIZE, WRITE_CHUNK_SIZE));
    zassert_true(second.IsOk());
    zassert_equal(harness.canbus->apply_count, 0, "Nothing is applied before the last chunk");

    auto last = harness.WriteNext(CANBUS, 2 * WRITE_CHUNK_SIZE, token, std::span(data).subspan(2 * WRITE_CHUNK_SIZE));
    zassert_true(last.IsOk());
    zassert_equal(last.Value("off"), data.size());
    zassert_equal(harness.canbus->apply_count, 1);
    zassert_true(harness.canbus->stored == data);

    zassert_true(harness.ReadAll(CANBUS) == data, "The written configuration reads back");
}

ZTEST_F(smp_config_group, test_crc_mismatch_is_not_applied) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(1200, 2);

    AssertError(harness.WriteAll(CANBUS, data, Crc(data) + 1), Error::CRC_MISMATCH);
    zassert_equal(harness.canbus->apply_count, 0);
}

ZTEST_F(smp_config_group, test_rejected_configuration_reports_apply_failed) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(1200, 4);
    harness.canbus->reject = true;

    auto response = harness.WriteAll(CANBUS, data, Crc(data));
    AssertError(response, Error::APPLY_FAILED);
    zassert_false(response.Has("msg"), "Without a reason there is no msg");
    zassert_equal(harness.canbus->apply_count, 1);

    harness.canbus->reject = false;
    zassert_true(harness.WriteAll(CANBUS, data, Crc(data)).IsOk(), "A failed write closes its session");
}

ZTEST_F(smp_config_group, test_apply_failed_carries_the_managers_reason) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(1200, 4);
    harness.canbus->reject = true;
    harness.canbus->rejection_reason = "Invalid CAN Bus COM configuration. COM bus channel is not configured";

    auto response = harness.WriteAll(CANBUS, data, Crc(data));

    AssertError(response, Error::APPLY_FAILED);
    zassert_equal(response.texts["msg"], harness.canbus->rejection_reason);

    harness.canbus->rejection_reason.clear();
    zassert_false(harness.WriteAll(CANBUS, data, Crc(data)).Has("msg"), "A reason does not outlive its write");
}

ZTEST_F(smp_config_group, test_a_long_reason_is_truncated_between_characters) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(1200, 4);
    harness.canbus->reject = true;

    harness.canbus->rejection_reason = std::string(200, 'x');
    auto response = harness.WriteAll(CANBUS, data, Crc(data));
    AssertError(response, Error::APPLY_FAILED);
    zassert_equal(response.texts["msg"], std::string(ConfigMgmtGroup::MAX_REASON_LENGTH, 'x'));

    // A two-byte UTF-8 character that would straddle the limit is left out whole.
    const std::string prefix(ConfigMgmtGroup::MAX_REASON_LENGTH - 1, 'x');
    harness.canbus->rejection_reason = prefix + "\xC3\xA9";
    response = harness.WriteAll(CANBUS, data, Crc(data));
    zassert_equal(response.texts["msg"], prefix);
}

ZTEST_F(smp_config_group, test_second_writer_is_busy_until_the_owner_restarts) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(2000, 8);

    auto owner = harness.WriteFirst(CANBUS, std::span(data).first(WRITE_CHUNK_SIZE), data.size(), Crc(data));
    zassert_true(owner.IsOk());
    const uint32_t token = owner.Value("tok");

    AssertError(harness.WriteFirst(UI, Pattern(10, 1), 10, 0), Error::BUSY);

    auto restarted = harness.WriteFirst(CANBUS, std::span(data).first(WRITE_CHUNK_SIZE), data.size(), Crc(data), token);
    zassert_true(restarted.IsOk(), "The owner may restart with its token");
    zassert_not_equal(restarted.Value("tok"), token, "A restart opens a new session");
}

ZTEST_F(smp_config_group, test_a_repeated_first_request_gets_the_sessions_token) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(2000, 13);
    const auto first_chunk = std::span(data).first(WRITE_CHUNK_SIZE);

    auto first = harness.WriteFirst(CANBUS, first_chunk, data.size(), Crc(data));
    zassert_true(first.IsOk());

    // The answer got lost, so the client sends the same request again.
    auto repeated = harness.WriteFirst(CANBUS, first_chunk, data.size(), Crc(data));
    zassert_true(repeated.IsOk(), "A repeat of the first request is not BUSY");
    zassert_equal(repeated.Value("off"), WRITE_CHUNK_SIZE);
    zassert_equal(repeated.Value("tok"), first.Value("tok"));

    const uint32_t token = repeated.Value("tok");
    zassert_true(harness.WriteNext(CANBUS, WRITE_CHUNK_SIZE, token,
        std::span(data).subspan(WRITE_CHUNK_SIZE, WRITE_CHUNK_SIZE)).IsOk());
    zassert_true(harness.WriteNext(CANBUS, 2 * WRITE_CHUNK_SIZE, token,
        std::span(data).subspan(2 * WRITE_CHUNK_SIZE)).IsOk());
    zassert_equal(harness.canbus->apply_count, 1);
    zassert_true(harness.canbus->stored == data);
}

ZTEST_F(smp_config_group, test_only_an_exact_repeat_of_the_first_request_gets_the_token) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(2000, 14);
    const auto first_chunk = std::span(data).first(WRITE_CHUNK_SIZE);

    auto first = harness.WriteFirst(CANBUS, first_chunk, data.size(), Crc(data));
    zassert_true(first.IsOk());

    Bytes other_chunk(first_chunk.begin(), first_chunk.end());
    other_chunk.back() ^= 0xFF;
    AssertError(harness.WriteFirst(CANBUS, other_chunk, data.size(), Crc(data)), Error::BUSY);
    AssertError(harness.WriteFirst(CANBUS, first_chunk.first(100), data.size(), Crc(data)), Error::BUSY);
    AssertError(harness.WriteFirst(CANBUS, first_chunk, data.size() + 1, Crc(data)), Error::BUSY);
    AssertError(harness.WriteFirst(CANBUS, first_chunk, data.size(), Crc(data) + 1), Error::BUSY);
    AssertError(harness.WriteFirst(UI, first_chunk, data.size(), Crc(data)), Error::BUSY);

    const uint32_t token = first.Value("tok");
    zassert_true(harness.WriteNext(CANBUS, WRITE_CHUNK_SIZE, token,
        std::span(data).subspan(WRITE_CHUNK_SIZE, WRITE_CHUNK_SIZE)).IsOk(), "The session is untouched");
    AssertError(harness.WriteFirst(CANBUS, first_chunk, data.size(), Crc(data)), Error::BUSY);
}

ZTEST_F(smp_config_group, test_wrong_token_is_rejected) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(2000, 6);

    auto first = harness.WriteFirst(CANBUS, std::span(data).first(WRITE_CHUNK_SIZE), data.size(), Crc(data));
    const uint32_t token = first.Value("tok");

    AssertError(harness.WriteNext(CANBUS, WRITE_CHUNK_SIZE, token + 1, std::span(data).subspan(WRITE_CHUNK_SIZE, 100)),
        Error::BAD_TOKEN);
    AssertError(harness.WriteNext(UI, WRITE_CHUNK_SIZE, token, std::span(data).subspan(WRITE_CHUNK_SIZE, 100)),
        Error::BAD_TOKEN);
}

ZTEST_F(smp_config_group, test_wrong_offset_reports_where_to_resume) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(1500, 9);

    auto first = harness.WriteFirst(CANBUS, std::span(data).first(WRITE_CHUNK_SIZE), data.size(), Crc(data));
    const uint32_t token = first.Value("tok");

    auto wrong = harness.WriteNext(CANBUS, 100, token, std::span(data).subspan(100, 10));
    AssertError(wrong, Error::BAD_OFFSET);
    zassert_equal(wrong.Value("off"), WRITE_CHUNK_SIZE);

    auto resumed = harness.WriteNext(CANBUS, WRITE_CHUNK_SIZE, token, std::span(data).subspan(WRITE_CHUNK_SIZE));
    zassert_true(resumed.IsOk(), "The session survives a wrong offset");
    zassert_true(harness.canbus->stored == data);
}

ZTEST_F(smp_config_group, test_oversize_transfers_are_rejected) {
    Harness& harness = *fixture->harness;

    AssertError(harness.WriteFirst(UI, Pattern(10, 1), UI_MAX_SIZE + 1, 0), Error::TOO_LARGE);
    AssertError(harness.WriteFirst(CANBUS, Pattern(20, 1), 10, 0), Error::TOO_LARGE);
    zassert_equal(harness.canbus->apply_count, 0);
}

ZTEST_F(smp_config_group, test_idle_session_times_out) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(2000, 12);

    auto stale = harness.WriteFirst(CANBUS, std::span(data).first(WRITE_CHUNK_SIZE), data.size(), Crc(data));
    const uint32_t token = stale.Value("tok");

    k_msleep(CONFIG_EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS + 100);

    AssertError(harness.WriteNext(CANBUS, WRITE_CHUNK_SIZE, token, std::span(data).subspan(WRITE_CHUNK_SIZE, 100)),
        Error::BAD_TOKEN);
    zassert_true(harness.WriteAll(UI, Pattern(100, 3), Crc(Pattern(100, 3))).IsOk(), "Another client may write now");
}

ZTEST_F(smp_config_group, test_unregistered_group_is_not_supported) {
    Harness& harness = *fixture->harness;
    harness.group->Unregister();

    auto response = harness.client.Read(SmpGroupId::CONFIG, Id(Command::LIST));

    zassert_equal(response.Value("rc"), MGMT_ERR_ENOTSUP);
}
