#include <memory>

#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_task.h"

using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;

namespace {

struct Payload {
    int value = 0;
    int* destructions = nullptr;

    ~Payload() {
        if(destructions != nullptr)
            ++(*destructions);
    }
};

// These cases never submit anything, so the kernel objects only have to be valid storage.
k_work_q idle_queue {};
k_work_sync idle_sync {};

void NoopWorkHandler(k_work*) {}

WorkQueueTask<Payload> MakeTask(const WorkQueueTask<Payload>::Handler& handler) {
    return WorkQueueTask<Payload>(&idle_queue, &idle_sync, NoopWorkHandler, handler);
}

} // namespace

ZTEST_SUITE(work_queue_task, NULL, NULL, NULL, NULL, NULL);

ZTEST(work_queue_task, test_WorkQueueTaskResult_defaults_to_no_reschedule) {
    WorkQueueTaskResult result;
    k_timeout_t no_wait = K_NO_WAIT;

    zassert_false(result.reschedule);
    zassert_equal(result.delay.ticks, no_wait.ticks);
}

ZTEST(work_queue_task, test_Execute_forwards_owned_user_data_to_the_handler) {
    Payload* observed = nullptr;
    auto task = MakeTask([&observed](Payload* payload) {
        observed = payload;
        return WorkQueueTaskResult {};
    });

    auto payload = std::make_unique<Payload>();
    payload->value = 42;
    Payload* expected = payload.get();
    task.SetUserData(std::move(payload));

    task.Execute();

    zassert_equal(observed, expected);
    zassert_equal(observed->value, 42);
    zassert_equal(task.GetUserdata(), expected);
}

ZTEST(work_queue_task, test_Execute_forwards_borrowed_user_data_to_the_handler) {
    Payload payload;
    payload.value = 7;

    Payload* observed = nullptr;
    auto task = MakeTask([&observed](Payload* argument) {
        observed = argument;
        return WorkQueueTaskResult {};
    });

    task.SetUserData(&payload);
    task.Execute();

    zassert_equal(observed, &payload);
    zassert_equal(task.GetUserdata(), &payload);
}

ZTEST(work_queue_task, test_Execute_returns_the_handler_result) {
    auto task = MakeTask([](Payload*) {
        return WorkQueueTaskResult {
            .reschedule = true,
            .delay = K_MSEC(25)
        };
    });

    task.SetUserData(std::make_unique<Payload>());

    auto result = task.Execute();
    k_timeout_t expected_delay = K_MSEC(25);

    zassert_true(result.reschedule);
    zassert_equal(result.delay.ticks, expected_delay.ticks);
}

ZTEST(work_queue_task, test_Execute_can_be_repeated) {
    int calls = 0;
    auto task = MakeTask([&calls](Payload*) {
        ++calls;
        return WorkQueueTaskResult {};
    });

    task.SetUserData(std::make_unique<Payload>());

    task.Execute();
    task.Execute();
    task.Execute();

    zassert_equal(calls, 3);
}

ZTEST(work_queue_task, test_SetUserData_releases_the_previously_owned_payload) {
    int destructions = 0;
    auto task = MakeTask([](Payload*) { return WorkQueueTaskResult {}; });

    auto first = std::make_unique<Payload>();
    first->destructions = &destructions;
    task.SetUserData(std::move(first));

    auto second = std::make_unique<Payload>();
    Payload* expected = second.get();
    task.SetUserData(std::move(second));

    zassert_equal(destructions, 1);
    zassert_equal(task.GetUserdata(), expected);
}

ZTEST(work_queue_task, test_owned_user_data_is_released_with_the_task) {
    int destructions = 0;

    {
        auto task = MakeTask([](Payload*) { return WorkQueueTaskResult {}; });

        auto payload = std::make_unique<Payload>();
        payload->destructions = &destructions;
        task.SetUserData(std::move(payload));

        zassert_equal(destructions, 0);
    }

    zassert_equal(destructions, 1);
}

ZTEST(work_queue_task, test_borrowed_user_data_outlives_the_task) {
    int destructions = 0;
    Payload payload;
    payload.destructions = &destructions;

    {
        auto task = MakeTask([](Payload*) { return WorkQueueTaskResult {}; });
        task.SetUserData(&payload);
    }

    zassert_equal(destructions, 0);
}

ZTEST(work_queue_task, test_IsScheduled_is_false_before_the_task_is_submitted) {
    auto task = MakeTask([](Payload*) { return WorkQueueTaskResult {}; });
    task.SetUserData(std::make_unique<Payload>());

    zassert_false(task.IsScheduled());
}

ZTEST(work_queue_task, test_Cancel_reports_nothing_to_cancel_for_an_idle_task) {
    auto task = MakeTask([](Payload*) { return WorkQueueTaskResult {}; });
    task.SetUserData(std::make_unique<Payload>());

    zassert_false(task.Cancel());
}
