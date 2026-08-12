#include <chrono>
#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/i_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_helpers.hpp"

using namespace std::chrono;
using namespace eerie_leap::subsys::time;

namespace {

// The RTC is not backed by hardware yet; RtcProvider offsets uptime by this fixed epoch.
constexpr auto kRtcEpoch = milliseconds(1761106217000);

// k_msleep only guarantees a lower bound, and QEMU targets built with icount
// sleep let the guest clock follow the (shared) host clock while idle, so the
// upper bounds below only have to catch a mis-scaled clock.
constexpr auto kSleepSlack = 100ms;

} // namespace

ZTEST_SUITE(boot_elapsed_time_provider, NULL, NULL, NULL, NULL, NULL);

ZTEST(boot_elapsed_time_provider, test_GetTime_returns_valid_elapsed_time) {
    std::shared_ptr<ITimeProvider> time_provider = std::make_shared<BootElapsedTimeProvider>();

    auto current_time1 = time_provider->GetTime();
    k_msleep(1);
    auto current_time2 = time_provider->GetTime();

    zassert_true(current_time2 - current_time1 >= 1ms);
    zassert_true(current_time2 - current_time1 <= kSleepSlack);
}

ZTEST(boot_elapsed_time_provider, test_GetTime_tracks_the_kernel_uptime) {
    BootElapsedTimeProvider time_provider;

    auto uptime_before = k_uptime_get();
    auto reported = TimeHelpers::ToMilliseconds(time_provider.GetTime()).count();
    auto uptime_after = k_uptime_get();

    zassert_between_inclusive(reported, uptime_before, uptime_after);
}

ZTEST(boot_elapsed_time_provider, test_GetTime_is_monotonic) {
    BootElapsedTimeProvider time_provider;

    auto previous = time_provider.GetTime();
    for (int i = 0; i < 16; ++i) {
        auto current = time_provider.GetTime();

        zassert_true(current >= previous, "time went backwards at iteration %d", i);

        previous = current;
    }
}

ZTEST(boot_elapsed_time_provider, test_GetTime_advances_by_the_slept_amount) {
    BootElapsedTimeProvider time_provider;

    auto before = time_provider.GetTime();
    k_msleep(20);
    auto after = time_provider.GetTime();

    zassert_true(after - before >= 20ms);
    zassert_true(after - before <= 20ms + kSleepSlack);
}

ZTEST(boot_elapsed_time_provider, test_independent_instances_agree) {
    BootElapsedTimeProvider first;
    BootElapsedTimeProvider second;

    zassert_true(abs(second.GetTime() - first.GetTime()) <= 10ms);
}

ZTEST_SUITE(rtc_provider, NULL, NULL, NULL, NULL, NULL);

ZTEST(rtc_provider, test_GetTime_is_at_or_after_the_rtc_epoch) {
    std::shared_ptr<ITimeProvider> time_provider = std::make_shared<RtcProvider>();

    zassert_true(TimeHelpers::ToMilliseconds(time_provider->GetTime()) >= kRtcEpoch);
}

ZTEST(rtc_provider, test_GetTime_offsets_the_boot_elapsed_time_by_the_epoch) {
    RtcProvider rtc_provider;
    BootElapsedTimeProvider boot_provider;

    auto offset = rtc_provider.GetTime() - boot_provider.GetTime();

    zassert_true(abs(offset - kRtcEpoch) <= 10ms);
}

ZTEST(rtc_provider, test_GetTime_is_monotonic) {
    RtcProvider rtc_provider;

    auto previous = rtc_provider.GetTime();
    for (int i = 0; i < 16; ++i) {
        auto current = rtc_provider.GetTime();

        zassert_true(current >= previous, "time went backwards at iteration %d", i);

        previous = current;
    }
}

ZTEST(rtc_provider, test_GetTime_advances_by_the_slept_amount) {
    RtcProvider rtc_provider;

    auto before = rtc_provider.GetTime();
    k_msleep(20);
    auto after = rtc_provider.GetTime();

    zassert_true(after - before >= 20ms);
    zassert_true(after - before <= 20ms + kSleepSlack);
}

ZTEST(rtc_provider, test_GetTime_is_ahead_of_the_boot_elapsed_time) {
    std::shared_ptr<ITimeProvider> rtc_provider = std::make_shared<RtcProvider>();
    std::shared_ptr<ITimeProvider> boot_provider = std::make_shared<BootElapsedTimeProvider>();

    zassert_true(rtc_provider->GetTime() > boot_provider->GetTime());
}
