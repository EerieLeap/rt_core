#include <utility>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/ztest.h>

#include "smp_groups_test_support.h"

using namespace smp_groups_test;

namespace {

// The CAN monitor's group (74), whose domain this suite does not build.
constexpr SmpGroupId CAN_MONITOR = SmpGroupId::CAN_MONITOR;

} // namespace

ZTEST_SUITE(smp_can_monitor_group, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_can_monitor_group, test_a_build_without_the_group_answers_not_supported) {
    SmpTestClient client;

    // STATUS and OPEN, a read and a write: as firmware without the group answers, which the app reads as
    // "unsupported".
    const auto status = client.Read(CAN_MONITOR, 0);
    zassert_equal(status.Value("rc"), MGMT_ERR_ENOTSUP);

    const auto open = client.Write(CAN_MONITOR, 1, CborMap().Put("ch", 0).Put("mode", 0).Build());
    zassert_equal(open.Value("rc"), MGMT_ERR_ENOTSUP);
}
