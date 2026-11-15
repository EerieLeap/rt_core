#include <zephyr/ztest.h>

#include "subsys/bluetooth/access_point/fast_link_policy.h"

using eerie_leap::subsys::bluetooth::access_point::FastLinkPolicy;
using Decision = FastLinkPolicy::Decision;
using IdleWork = FastLinkPolicy::IdleWork;
using LinkRequest = FastLinkPolicy::LinkRequest;

namespace {

constexpr Decision NOTHING{};
constexpr Decision FAST_AND_NO_IDLE{ .request = LinkRequest::FAST, .idle_work = IdleWork::CANCEL };
constexpr Decision FAST_AND_IDLE_WORK{ .request = LinkRequest::FAST, .idle_work = IdleWork::SCHEDULE };
constexpr Decision IDLE_WORK_ONLY{ .idle_work = IdleWork::SCHEDULE };
constexpr Decision NO_IDLE_WORK{ .idle_work = IdleWork::CANCEL };
constexpr Decision FALLBACK{ .request_fallback = true };

// 7.5 ms and 15 ms, 30 ms and 50 ms, in units of 1.25 ms.
constexpr uint16_t INTERVAL_7_5_MS = 6;
constexpr uint16_t INTERVAL_15_MS = 12;
constexpr uint16_t INTERVAL_18_75_MS = 15;
constexpr uint16_t INTERVAL_30_MS = 24;
constexpr uint16_t INTERVAL_50_MS = 40;

// A fast request that has been made, as AccessPointService makes it.
Decision HoldAndRequest(FastLinkPolicy& policy) {
    const Decision decision = policy.OnHold();
    if(decision.request == LinkRequest::FAST)
        policy.OnFastRequested();

    return decision;
}

} // namespace

ZTEST_SUITE(fast_link_policy, NULL, NULL, NULL, NULL, NULL);

ZTEST(fast_link_policy, test_first_hold_requests_fast_and_stops_the_idle_work) {
    FastLinkPolicy policy;

    zassert_equal(policy.OnHold(), FAST_AND_NO_IDLE);
    zassert_true(policy.IsFast());
    zassert_equal(policy.GetHoldCount(), 1);
}

ZTEST(fast_link_policy, test_holds_are_counted) {
    FastLinkPolicy policy;

    zassert_equal(policy.OnHold(), FAST_AND_NO_IDLE);
    zassert_equal(policy.OnHold(), NOTHING, "The second hold has nothing to request");
    zassert_equal(policy.GetHoldCount(), 2);

    zassert_equal(policy.OnRelease(), NOTHING, "One hold is still held");
    zassert_equal(policy.GetHoldCount(), 1);
    zassert_equal(policy.OnRelease(), IDLE_WORK_ONLY);
    zassert_equal(policy.GetHoldCount(), 0);
}

ZTEST(fast_link_policy, test_idle_work_starts_after_the_last_release) {
    FastLinkPolicy policy;
    (void)policy.OnHold();
    (void)policy.OnHold();
    (void)policy.OnRelease();

    zassert_equal(policy.OnRelease(), IDLE_WORK_ONLY);
    // The link stays fast until the work runs.
    zassert_true(policy.IsFast());

    zassert_equal(policy.OnIdleTimeout(), Decision{ .request = LinkRequest::IDLE });
    zassert_false(policy.IsFast());
}

ZTEST(fast_link_policy, test_a_release_without_a_hold_is_ignored) {
    FastLinkPolicy policy;

    zassert_equal(policy.OnRelease(), NOTHING);
    zassert_equal(policy.GetHoldCount(), 0);

    // Not a negative count that the next hold would have to cancel out.
    zassert_equal(policy.OnHold(), FAST_AND_NO_IDLE);
    zassert_equal(policy.OnRelease(), IDLE_WORK_ONLY);
}

ZTEST(fast_link_policy, test_the_idle_work_that_runs_during_a_hold_changes_nothing) {
    FastLinkPolicy policy;
    (void)policy.OnHold();

    // The work was already running when the hold cancelled it.
    zassert_equal(policy.OnIdleTimeout(), NOTHING);
    zassert_true(policy.IsFast());
}

ZTEST(fast_link_policy, test_smp_activity_requests_fast_and_schedules_the_idle_work) {
    FastLinkPolicy policy;

    zassert_equal(policy.OnSmpActivity(), FAST_AND_IDLE_WORK);
    zassert_equal(policy.OnSmpActivity(), IDLE_WORK_ONLY, "Already fast, only the idle delay restarts");
}

ZTEST(fast_link_policy, test_smp_activity_during_a_hold_does_not_schedule_the_idle_work) {
    FastLinkPolicy policy;
    (void)policy.OnHold();

    zassert_equal(policy.OnSmpActivity(), NOTHING);
    zassert_equal(policy.OnSmpActivity(), NOTHING);

    zassert_equal(policy.OnRelease(), IDLE_WORK_ONLY);
}

ZTEST(fast_link_policy, test_hold_after_smp_activity_keeps_the_request_and_stops_the_idle_work) {
    FastLinkPolicy policy;
    (void)policy.OnSmpActivity();

    // The fast request is already made, so only the idle work changes.
    zassert_equal(policy.OnHold(), NO_IDLE_WORK);
    zassert_equal(policy.OnIdleTimeout(), NOTHING);
}

ZTEST(fast_link_policy, test_a_new_connection_gets_the_idle_work) {
    FastLinkPolicy policy;

    zassert_equal(policy.OnConnected(), IDLE_WORK_ONLY);
    zassert_false(policy.IsFast());
    zassert_equal(policy.OnIdleTimeout(), Decision{ .request = LinkRequest::IDLE });
}

ZTEST(fast_link_policy, test_a_new_connection_while_held_is_fast_at_once) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);
    zassert_equal(policy.OnDisconnected(), NO_IDLE_WORK);
    zassert_false(policy.IsFast());
    zassert_equal(policy.GetHoldCount(), 1, "Holds outlive the connection");

    zassert_equal(policy.OnConnected(), FAST_AND_NO_IDLE);
    zassert_true(policy.IsFast());
    zassert_equal(policy.OnIdleTimeout(), NOTHING);
}

ZTEST(fast_link_policy, test_release_after_a_disconnect_starts_the_idle_work_for_the_next_connection) {
    FastLinkPolicy policy;
    (void)policy.OnHold();
    (void)policy.OnDisconnected();
    zassert_equal(policy.OnRelease(), IDLE_WORK_ONLY);

    zassert_equal(policy.OnConnected(), IDLE_WORK_ONLY);
    zassert_false(policy.IsFast());
}

ZTEST(fast_link_policy, test_fallback_is_asked_when_the_interval_stays_above_15_ms) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);
    zassert_true(policy.IsFallbackArmed());

    zassert_equal(policy.OnParametersUpdated(INTERVAL_18_75_MS), FALLBACK);
}

ZTEST(fast_link_policy, test_fallback_is_asked_for_a_central_that_keeps_its_long_interval) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);

    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), FALLBACK);
}

ZTEST(fast_link_policy, test_fallback_is_not_asked_up_to_15_ms) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);
    zassert_equal(policy.OnParametersUpdated(INTERVAL_7_5_MS), NOTHING);

    policy = FastLinkPolicy{};
    (void)HoldAndRequest(policy);
    zassert_equal(policy.OnParametersUpdated(INTERVAL_15_MS), NOTHING, "15 ms is the limit itself");
}

ZTEST(fast_link_policy, test_fallback_is_asked_once) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);

    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), FALLBACK);
    // The answer to the fallback request itself, or any later change.
    zassert_equal(policy.OnParametersUpdated(INTERVAL_30_MS), NOTHING);
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), NOTHING);
    zassert_false(policy.IsFallbackArmed());
}

ZTEST(fast_link_policy, test_an_answer_within_15_ms_uses_up_the_fallback) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);

    zassert_equal(policy.OnParametersUpdated(INTERVAL_15_MS), NOTHING);
    // A change the central makes later is not the answer to the request.
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), NOTHING);
}

ZTEST(fast_link_policy, test_nothing_is_asked_before_a_fast_request_was_made) {
    FastLinkPolicy policy;

    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), NOTHING);

    // The decision is not the request: the update that precedes it is not its answer.
    (void)policy.OnHold();
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), NOTHING);
}

ZTEST(fast_link_policy, test_a_new_fast_request_arms_the_fallback_again) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), FALLBACK);
    (void)policy.OnRelease();
    zassert_equal(policy.OnIdleTimeout(), Decision{ .request = LinkRequest::IDLE });

    (void)HoldAndRequest(policy);
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), FALLBACK);
}

ZTEST(fast_link_policy, test_smp_activity_after_idle_arms_the_fallback_again) {
    FastLinkPolicy policy;
    (void)policy.OnSmpActivity();
    policy.OnFastRequested();
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), FALLBACK);
    (void)policy.OnIdleTimeout();

    zassert_equal(policy.OnSmpActivity(), FAST_AND_IDLE_WORK);
    policy.OnFastRequested();
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), FALLBACK);
}

ZTEST(fast_link_policy, test_a_reconnect_while_held_arms_the_fallback_again) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);
    (void)policy.OnDisconnected();
    zassert_false(policy.IsFallbackArmed());

    zassert_equal(policy.OnConnected(), FAST_AND_NO_IDLE);
    policy.OnFastRequested();
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), FALLBACK);
}

ZTEST(fast_link_policy, test_the_idle_request_is_not_followed_by_a_fallback) {
    FastLinkPolicy policy;
    (void)policy.OnSmpActivity();
    policy.OnFastRequested();
    (void)policy.OnIdleTimeout();

    // The update the idle request brings: 30-50 ms.
    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), NOTHING);
}

ZTEST(fast_link_policy, test_a_refused_fast_request_leaves_nothing_to_answer) {
    FastLinkPolicy policy;
    (void)policy.OnHold();
    policy.OnFastRequested();
    policy.OnFastRequestFailed();

    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), NOTHING);
}

ZTEST(fast_link_policy, test_a_disconnect_ends_the_pending_fallback) {
    FastLinkPolicy policy;
    (void)HoldAndRequest(policy);
    (void)policy.OnDisconnected();

    zassert_equal(policy.OnParametersUpdated(INTERVAL_50_MS), NOTHING);
}
