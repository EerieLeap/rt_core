#include <zephyr/ztest.h>

#include "dfu_test_support.h"

using namespace dfu_test;

struct dfu_group_fixture {
    Unit* unit = nullptr;
};

namespace {

constexpr uint8_t STATUS = 0;

void* SetUp() {
    static dfu_group_fixture fixture{};
    fixture.unit = new Unit();

    return &fixture;
}

void Before(void* data) {
    auto* fixture = static_cast<dfu_group_fixture*>(data);

    fixture->unit->Start();
    fixture->unit->status_source.status = DfuStatus{};
}

void TearDown(void* data) {
    delete static_cast<dfu_group_fixture*>(data)->unit;
}

} // namespace

ZTEST_SUITE(dfu_group, NULL, SetUp, Before, NULL, TearDown);

ZTEST_F(dfu_group, test_status_of_a_confirmed_image) {
    const Response response = fixture->unit->client.Read(SmpGroupId::DFU, STATUS);

    zassert_true(response.IsOk());
    zassert_true(response.Flag("confirmed"));
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::CONFIRMED));
    zassert_false(response.Has("confirm_in_ms"), "Only a confirming image has a time left");
    zassert_equal(response.Value("reset_cause"), 0);
}

ZTEST_F(dfu_group, test_status_while_verifying) {
    fixture->unit->status_source.status = {
        .confirmed = false,
        .state = DfuState::VERIFYING,
        .confirm_in_ms = std::nullopt,
        .reset_cause = 0x04,
    };

    const Response response = fixture->unit->client.Read(SmpGroupId::DFU, STATUS);

    zassert_true(response.IsOk());
    zassert_false(response.Flag("confirmed"));
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::VERIFYING));
    zassert_false(response.Has("confirm_in_ms"));
    zassert_equal(response.Value("reset_cause"), 0x04);
}

ZTEST_F(dfu_group, test_status_while_confirming_has_the_time_left) {
    fixture->unit->status_source.status = {
        .confirmed = false,
        .state = DfuState::CONFIRMING,
        .confirm_in_ms = 12345,
        .reset_cause = 0x10,
    };

    const Response response = fixture->unit->client.Read(SmpGroupId::DFU, STATUS);

    zassert_true(response.IsOk());
    zassert_false(response.Flag("confirmed"));
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::CONFIRMING));
    zassert_equal(response.Value("confirm_in_ms"), 12345);
    zassert_equal(response.Value("reset_cause"), 0x10);
}

ZTEST_F(dfu_group, test_status_of_a_failed_confirmation) {
    fixture->unit->status_source.status = {
        .confirmed = false,
        .state = DfuState::FAILED,
        .confirm_in_ms = std::nullopt,
        .reset_cause = 0,
    };

    const Response response = fixture->unit->client.Read(SmpGroupId::DFU, STATUS);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::FAILED));
}

ZTEST_F(dfu_group, test_status_is_read_only) {
    const Response response = fixture->unit->client.Write(SmpGroupId::DFU, STATUS, CborMap().Build());

    zassert_false(response.IsOk());
}

ZTEST_F(dfu_group, test_unregistered_group_is_not_served) {
    Unit& unit = *fixture->unit;
    unit.group.reset();

    const auto response = unit.client.Send(smp_test::SmpOperation::READ, SmpGroupId::DFU, STATUS, CborMap().Build());

    // MCUmgr answers a group it does not know with an error.
    zassert_true(response.has_value());
    zassert_false(response->IsOk());
}
