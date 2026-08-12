#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_thread.h"

using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr int STACK_SIZE = 4096;
constexpr int PRIORITY = 5;
constexpr int SYNC_TIMEOUT_MS = 1000;
constexpr int IDLE_TIMEOUT_MS = 100;

struct TaskProbe {
    k_sem finished;
    atomic_t runs;
    int reschedules;

    explicit TaskProbe(int reschedules = 0) : runs(ATOMIC_INIT(0)), reschedules(reschedules) {
        k_sem_init(&finished, 0, K_SEM_MAX_LIMIT);
    }

    [[nodiscard]] int Runs() const { return static_cast<int>(atomic_get(&runs)); }
    bool WaitForRun(int timeout_ms) { return k_sem_take(&finished, K_MSEC(timeout_ms)) == 0; }
};

WorkQueueTaskResult ProbeHandler(TaskProbe* probe) {
    atomic_val_t previous_runs = atomic_inc(&probe->runs);
    k_sem_give(&probe->finished);

    return {
        .reschedule = previous_runs < probe->reschedules,
        .delay = K_MSEC(5)
    };
}

std::unique_ptr<WorkQueueThread> MakeQueueThread(const char* name) {
    auto queue_thread = std::make_unique<WorkQueueThread>(name, STACK_SIZE, PRIORITY);
    queue_thread->Initialize();

    return queue_thread;
}

template<typename TAction>
bool ThrowsRuntimeError(TAction&& action) {
    try {
        action();
    } catch(const std::runtime_error&) {
        return true;
    } catch(...) {
        return false;
    }

    return false;
}

} // namespace

ZTEST_SUITE(work_queue_thread, NULL, NULL, NULL, NULL, NULL);

ZTEST(work_queue_thread, test_uninitialized_queue_rejects_every_operation) {
    // The destructor stops work_q_ unconditionally even though only Initialize() sets it up,
    // so the storage is zeroed to keep this case defined.
    alignas(WorkQueueThread) std::array<std::byte, sizeof(WorkQueueThread)> storage {};
    auto* queue_thread = new (storage.data()) WorkQueueThread("wq_uninitialized", STACK_SIZE, PRIORITY);

    zassert_true(ThrowsRuntimeError([&] { (void)queue_thread->GetWorkQueue(); }));
    zassert_true(ThrowsRuntimeError([&] { queue_thread->Run([] {}); }));
    zassert_true(ThrowsRuntimeError([&] { queue_thread->Stop(); }));
    zassert_true(ThrowsRuntimeError([&] {
        (void)queue_thread->CreateTask(ProbeHandler, std::make_unique<TaskProbe>()); }));

    std::destroy_at(queue_thread);
}

ZTEST(work_queue_thread, test_Initialize_starts_a_named_queue_thread) {
    auto queue_thread = MakeQueueThread("wq_named");

    auto* queue = queue_thread->GetWorkQueue();

    zassert_not_null(queue);
    zassert_not_null(queue->thread_id);
    zassert_not_null(queue_thread->GetStack());
    zassert_equal(std::string(k_thread_name_get(queue->thread_id)), std::string("wq_named"));
    zassert_equal(k_thread_priority_get(queue->thread_id), PRIORITY);

    queue_thread->Stop();
}

ZTEST(work_queue_thread, test_CreateTask_runs_the_handler_with_owned_user_data) {
    auto queue_thread = MakeQueueThread("wq_owned");

    auto probe = std::make_unique<TaskProbe>();
    TaskProbe* probe_ptr = probe.get();

    auto task = queue_thread->CreateTask(ProbeHandler, std::move(probe));

    zassert_equal(task.GetUserdata(), probe_ptr);

    task.Schedule();

    zassert_true(probe_ptr->WaitForRun(SYNC_TIMEOUT_MS));
    zassert_equal(probe_ptr->Runs(), 1);

    queue_thread->Stop();

    zassert_false(task.IsScheduled());
}

ZTEST(work_queue_thread, test_CreateTask_runs_the_handler_with_borrowed_user_data) {
    auto queue_thread = MakeQueueThread("wq_borrowed");

    TaskProbe probe;
    auto task = queue_thread->CreateTask(ProbeHandler, &probe);

    zassert_equal(task.GetUserdata(), &probe);

    task.Schedule();

    zassert_true(probe.WaitForRun(SYNC_TIMEOUT_MS));
    zassert_equal(probe.Runs(), 1);

    queue_thread->Stop();
}

ZTEST(work_queue_thread, test_Schedule_defers_the_handler_by_the_requested_delay) {
    auto queue_thread = MakeQueueThread("wq_delayed");

    TaskProbe probe;
    auto task = queue_thread->CreateTask(ProbeHandler, &probe);

    int64_t started_at = k_uptime_get();
    task.Schedule(K_MSEC(150));

    zassert_true(probe.WaitForRun(SYNC_TIMEOUT_MS));
    zassert_true(k_uptime_get() - started_at >= 140, "handler ran before the requested delay");

    queue_thread->Stop();
}

ZTEST(work_queue_thread, test_Reschedule_replaces_a_pending_delay) {
    auto queue_thread = MakeQueueThread("wq_rescheduled");

    TaskProbe probe;
    auto task = queue_thread->CreateTask(ProbeHandler, &probe);

    task.Schedule(K_SECONDS(30));
    task.Reschedule(K_MSEC(20));

    zassert_true(probe.WaitForRun(SYNC_TIMEOUT_MS));
    zassert_equal(probe.Runs(), 1);

    queue_thread->Stop();
}

ZTEST(work_queue_thread, test_Cancel_stops_a_delayed_handler_from_running) {
    auto queue_thread = MakeQueueThread("wq_cancelled");

    TaskProbe probe;
    auto task = queue_thread->CreateTask(ProbeHandler, &probe);

    task.Schedule(K_MSEC(300));
    task.Cancel();

    zassert_false(probe.WaitForRun(400));
    zassert_equal(probe.Runs(), 0);

    queue_thread->Stop();
}

ZTEST(work_queue_thread, test_a_handler_asking_to_reschedule_is_run_again) {
    constexpr int RESCHEDULES = 2;

    auto queue_thread = MakeQueueThread("wq_repeating");

    TaskProbe probe(RESCHEDULES);
    auto task = queue_thread->CreateTask(ProbeHandler, &probe);

    task.Schedule();

    for(int i = 0; i < RESCHEDULES + 1; ++i)
        zassert_true(probe.WaitForRun(SYNC_TIMEOUT_MS), "run %d never happened", i);

    zassert_false(probe.WaitForRun(IDLE_TIMEOUT_MS));
    zassert_equal(probe.Runs(), RESCHEDULES + 1);

    queue_thread->Stop();
}

ZTEST(work_queue_thread, test_tasks_of_one_queue_run_on_the_queue_thread) {
    auto queue_thread = MakeQueueThread("wq_affinity");

    k_tid_t observed = nullptr;
    k_sem done;
    k_sem_init(&done, 0, K_SEM_MAX_LIMIT);

    queue_thread->Run([&] {
        observed = k_current_get();
        k_sem_give(&done);
    });

    zassert_equal(k_sem_take(&done, K_MSEC(SYNC_TIMEOUT_MS)), 0);
    zassert_equal(observed, queue_thread->GetWorkQueue()->thread_id);
    zassert_true(observed != k_current_get());

    queue_thread->Stop();
}

ZTEST(work_queue_thread, test_Run_executes_every_submitted_handler) {
    constexpr int RUN_COUNT = 8;

    auto queue_thread = MakeQueueThread("wq_runner");

    atomic_t completed = ATOMIC_INIT(0);
    k_sem done;
    k_sem_init(&done, 0, K_SEM_MAX_LIMIT);

    for(int i = 0; i < RUN_COUNT; ++i) {
        queue_thread->Run([&] {
            atomic_inc(&completed);
            k_sem_give(&done);
        });
    }

    for(int i = 0; i < RUN_COUNT; ++i)
        zassert_equal(k_sem_take(&done, K_MSEC(SYNC_TIMEOUT_MS)), 0, "handler %d never ran", i);

    zassert_equal(atomic_get(&completed), RUN_COUNT);

    queue_thread->Stop();
}

ZTEST(work_queue_thread, test_Stop_drains_the_work_that_is_still_pending) {
    constexpr int RUN_COUNT = 5;

    auto queue_thread = MakeQueueThread("wq_drained");

    atomic_t completed = ATOMIC_INIT(0);

    for(int i = 0; i < RUN_COUNT; ++i)
        queue_thread->Run([&] { atomic_inc(&completed); });

    queue_thread->Stop();

    zassert_equal(atomic_get(&completed), RUN_COUNT);
}

ZTEST(work_queue_thread, test_queues_are_independent_of_each_other) {
    auto first_queue = MakeQueueThread("wq_first");
    auto second_queue = MakeQueueThread("wq_second");

    zassert_true(first_queue->GetWorkQueue() != second_queue->GetWorkQueue());
    zassert_true(first_queue->GetWorkQueue()->thread_id != second_queue->GetWorkQueue()->thread_id);

    TaskProbe first_probe;
    TaskProbe second_probe;

    auto first_task = first_queue->CreateTask(ProbeHandler, &first_probe);
    auto second_task = second_queue->CreateTask(ProbeHandler, &second_probe);

    first_task.Schedule();

    zassert_true(first_probe.WaitForRun(SYNC_TIMEOUT_MS));
    zassert_equal(second_probe.Runs(), 0);

    second_task.Schedule();

    zassert_true(second_probe.WaitForRun(SYNC_TIMEOUT_MS));
    zassert_equal(first_probe.Runs(), 1);

    first_queue->Stop();
    second_queue->Stop();
}
