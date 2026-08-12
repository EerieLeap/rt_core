#include <chrono>
#include <memory>

#include <zephyr/ztest.h>

#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/i_time_provider.h"
#include "subsys/time/i_time_service.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_helpers.hpp"
#include "subsys/time/time_service.h"

using namespace std::chrono;
using namespace eerie_leap::subsys::time;

namespace {

class StubTimeProvider : public ITimeProvider {
public:
    explicit StubTimeProvider(milliseconds value, milliseconds step = 0ms) : value_(value), step_(step) { }

    std::chrono::system_clock::time_point GetTime() override {
        auto current = value_;
        value_ += step_;
        ++calls_;

        return TimeHelpers::FromMilliseconds(current);
    }

    int calls() const { return calls_; }

private:
    milliseconds value_;
    milliseconds step_;
    int calls_ = 0;
};

struct Fixture {
    std::shared_ptr<StubTimeProvider> rtc_provider;
    std::shared_ptr<StubTimeProvider> boot_provider;
    std::shared_ptr<TimeService> service;
};

Fixture MakeFixture(milliseconds rtc_value = 1761106217000ms, milliseconds boot_value = 1234ms, milliseconds step = 0ms) {
    auto rtc_provider = std::make_shared<StubTimeProvider>(rtc_value, step);
    auto boot_provider = std::make_shared<StubTimeProvider>(boot_value, step);

    return {rtc_provider, boot_provider, std::make_shared<TimeService>(rtc_provider, boot_provider)};
}

} // namespace

ZTEST_SUITE(time_service, NULL, NULL, NULL, NULL, NULL);

ZTEST(time_service, test_GetCurrentTime_returns_the_rtc_provider_time) {
    auto fixture = MakeFixture();

    zassert_equal(TimeHelpers::ToMilliseconds(fixture.service->GetCurrentTime()).count(), 1761106217000);
}

ZTEST(time_service, test_GetTimeSinceBoot_returns_the_boot_provider_time) {
    auto fixture = MakeFixture();

    zassert_equal(TimeHelpers::ToMilliseconds(fixture.service->GetTimeSinceBoot()).count(), 1234);
}

ZTEST(time_service, test_GetCurrentTime_only_queries_the_rtc_provider) {
    auto fixture = MakeFixture();

    fixture.service->GetCurrentTime();

    zassert_equal(fixture.rtc_provider->calls(), 1);
    zassert_equal(fixture.boot_provider->calls(), 0);
}

ZTEST(time_service, test_GetTimeSinceBoot_only_queries_the_boot_provider) {
    auto fixture = MakeFixture();

    fixture.service->GetTimeSinceBoot();

    zassert_equal(fixture.boot_provider->calls(), 1);
    zassert_equal(fixture.rtc_provider->calls(), 0);
}

ZTEST(time_service, test_Initialize_does_not_query_the_providers) {
    auto fixture = MakeFixture();

    fixture.service->Initialize();

    zassert_equal(fixture.rtc_provider->calls(), 0);
    zassert_equal(fixture.boot_provider->calls(), 0);
}

ZTEST(time_service, test_values_are_not_cached_between_calls) {
    auto fixture = MakeFixture(1000ms, 10ms, 5ms);

    zassert_equal(TimeHelpers::ToMilliseconds(fixture.service->GetCurrentTime()).count(), 1000);
    zassert_equal(TimeHelpers::ToMilliseconds(fixture.service->GetCurrentTime()).count(), 1005);
    zassert_equal(TimeHelpers::ToMilliseconds(fixture.service->GetTimeSinceBoot()).count(), 10);
    zassert_equal(TimeHelpers::ToMilliseconds(fixture.service->GetTimeSinceBoot()).count(), 15);
    zassert_equal(fixture.rtc_provider->calls(), 2);
    zassert_equal(fixture.boot_provider->calls(), 2);
}

ZTEST(time_service, test_the_same_provider_can_back_both_clocks) {
    auto provider = std::make_shared<StubTimeProvider>(500ms);
    auto service = std::make_shared<TimeService>(provider, provider);

    zassert_equal(TimeHelpers::ToMilliseconds(service->GetCurrentTime()).count(), 500);
    zassert_equal(TimeHelpers::ToMilliseconds(service->GetTimeSinceBoot()).count(), 500);
    zassert_equal(provider->calls(), 2);
}

ZTEST(time_service, test_service_keeps_the_providers_alive) {
    std::shared_ptr<ITimeService> service;

    {
        auto provider = std::make_shared<StubTimeProvider>(777ms);
        service = std::make_shared<TimeService>(provider, provider);
    }

    zassert_equal(TimeHelpers::ToMilliseconds(service->GetCurrentTime()).count(), 777);
}

ZTEST(time_service, test_service_is_usable_through_the_interface) {
    std::shared_ptr<ITimeService> service =
        std::make_shared<TimeService>(std::make_shared<RtcProvider>(), std::make_shared<BootElapsedTimeProvider>());

    zassert_true(service->GetCurrentTime() > service->GetTimeSinceBoot());
    zassert_true(TimeHelpers::ToMilliseconds(service->GetTimeSinceBoot()) >= 0ms);
}
