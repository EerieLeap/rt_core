#include <string>

#include <zephyr/ztest.h>

#include "subsys/threading/thread.h"

using eerie_leap::subsys::threading::IThread;
using eerie_leap::subsys::threading::Thread;

namespace {

constexpr int STACK_SIZE = 2048;
constexpr int OVERSIZED_STACK_SIZE = 4 * 1024 * 1024;
constexpr int PRIORITY = 5;
constexpr int SYNC_TIMEOUT_MS = 500;
constexpr int IDLE_TIMEOUT_MS = 50;

// Blocks inside the entry point so the test can observe a live thread.
class BlockingWorker : public IThread {
private:
    k_sem entered_;
    k_sem resume_;
    atomic_t entries_;
    atomic_t exits_;

public:
    BlockingWorker() : entries_(ATOMIC_INIT(0)), exits_(ATOMIC_INIT(0)) {
        k_sem_init(&entered_, 0, K_SEM_MAX_LIMIT);
        k_sem_init(&resume_, 0, K_SEM_MAX_LIMIT);
    }

    void ThreadEntry() override {
        atomic_inc(&entries_);
        k_sem_give(&entered_);
        k_sem_take(&resume_, K_FOREVER);
        atomic_inc(&exits_);
    }

    bool WaitForEntry(int timeout_ms) { return k_sem_take(&entered_, K_MSEC(timeout_ms)) == 0; }
    void Resume() { k_sem_give(&resume_); }

    [[nodiscard]] int Entries() const { return static_cast<int>(atomic_get(&entries_)); }
    [[nodiscard]] int Exits() const { return static_cast<int>(atomic_get(&exits_)); }
};

// Terminates on its own so Join() has something to wait for.
class SleepingWorker : public IThread {
private:
    int sleep_ms_;
    atomic_t entries_;
    atomic_t finished_;

public:
    explicit SleepingWorker(int sleep_ms)
        : sleep_ms_(sleep_ms), entries_(ATOMIC_INIT(0)), finished_(ATOMIC_INIT(0)) {}

    void ThreadEntry() override {
        atomic_inc(&entries_);
        k_sleep(K_MSEC(sleep_ms_));
        atomic_inc(&finished_);
    }

    [[nodiscard]] int Entries() const { return static_cast<int>(atomic_get(&entries_)); }
    [[nodiscard]] int Finished() const { return static_cast<int>(atomic_get(&finished_)); }
};

} // namespace

ZTEST_SUITE(thread, NULL, NULL, NULL, NULL, NULL);

ZTEST(thread, test_Start_is_ignored_before_Initialize) {
    BlockingWorker worker;
    Thread thread("uninitialized", &worker, STACK_SIZE, PRIORITY);

    zassert_false(thread.Start());

    zassert_is_null(thread.GetStack());
    zassert_is_null(thread.GetThread());
    zassert_false(thread.IsRunning());
    zassert_false(worker.WaitForEntry(IDLE_TIMEOUT_MS));
    zassert_equal(worker.Entries(), 0);
}

ZTEST(thread, test_Start_is_ignored_when_the_stack_cannot_be_allocated) {
    BlockingWorker worker;
    Thread thread("oversized", &worker, OVERSIZED_STACK_SIZE, PRIORITY);

    zassert_false(thread.Initialize());
    zassert_false(thread.Start());

    zassert_is_null(thread.GetStack());
    zassert_is_null(thread.GetThread());
    zassert_false(thread.IsRunning());
    zassert_false(worker.WaitForEntry(IDLE_TIMEOUT_MS));
}

ZTEST(thread, test_Initialize_allocates_the_stack_without_starting) {
    BlockingWorker worker;
    Thread thread("initialized", &worker, STACK_SIZE, PRIORITY);

    zassert_true(thread.Initialize());

    zassert_not_null(thread.GetStack());
    zassert_is_null(thread.GetThread());
    zassert_false(thread.IsRunning());
    zassert_false(worker.WaitForEntry(IDLE_TIMEOUT_MS));
}

ZTEST(thread, test_Start_runs_the_entry_point) {
    BlockingWorker worker;
    Thread thread("runner", &worker, STACK_SIZE, PRIORITY);

    thread.Initialize();

    zassert_true(thread.Start());
    zassert_true(worker.WaitForEntry(SYNC_TIMEOUT_MS));
    zassert_true(thread.IsRunning());
    zassert_not_null(thread.GetThread());

    worker.Resume();
    thread.Join();

    zassert_equal(worker.Entries(), 1);
    zassert_equal(worker.Exits(), 1);
    zassert_false(thread.IsRunning());
}

ZTEST(thread, test_Start_applies_the_thread_name) {
    BlockingWorker worker;
    Thread thread("named_worker", &worker, STACK_SIZE, PRIORITY);

    thread.Initialize();
    thread.Start();

    zassert_true(worker.WaitForEntry(SYNC_TIMEOUT_MS));
    zassert_equal(std::string(k_thread_name_get(thread.GetThread())), std::string("named_worker"));

    worker.Resume();
    thread.Join();
}

ZTEST(thread, test_Start_creates_a_preemptive_thread_by_default) {
    BlockingWorker worker;
    Thread thread("preemptive", &worker, STACK_SIZE, PRIORITY);

    thread.Initialize();
    thread.Start();

    zassert_true(worker.WaitForEntry(SYNC_TIMEOUT_MS));
    zassert_equal(k_thread_priority_get(thread.GetThread()), PRIORITY);

    worker.Resume();
    thread.Join();
}

ZTEST(thread, test_Start_creates_a_cooperative_thread_when_requested) {
    BlockingWorker worker;
    Thread thread("cooperative", &worker, STACK_SIZE, PRIORITY, true);

    thread.Initialize();
    thread.Start();

    zassert_true(worker.WaitForEntry(SYNC_TIMEOUT_MS));
    zassert_equal(k_thread_priority_get(thread.GetThread()), -PRIORITY);

    worker.Resume();
    thread.Join();
}

ZTEST(thread, test_Start_is_ignored_while_the_thread_is_running) {
    BlockingWorker worker;
    Thread thread("single_start", &worker, STACK_SIZE, PRIORITY);

    thread.Initialize();
    thread.Start();
    zassert_true(worker.WaitForEntry(SYNC_TIMEOUT_MS));

    zassert_false(thread.Start());

    zassert_false(worker.WaitForEntry(IDLE_TIMEOUT_MS));
    zassert_equal(worker.Entries(), 1);

    worker.Resume();
    thread.Join();
}

ZTEST(thread, test_Start_can_be_repeated_after_Join) {
    BlockingWorker worker;
    Thread thread("restarted", &worker, STACK_SIZE, PRIORITY);

    thread.Initialize();

    for(int i = 1; i <= 2; ++i) {
        thread.Start();

        zassert_true(worker.WaitForEntry(SYNC_TIMEOUT_MS));
        zassert_true(thread.IsRunning());

        worker.Resume();
        thread.Join();

        zassert_equal(worker.Entries(), i);
        zassert_equal(worker.Exits(), i);
    }
}

ZTEST(thread, test_Join_waits_for_the_entry_point_to_return) {
    SleepingWorker worker(100);
    Thread thread("joinable", &worker, STACK_SIZE, PRIORITY);

    thread.Initialize();
    thread.Start();

    zassert_equal(worker.Finished(), 0);

    thread.Join();

    zassert_equal(worker.Finished(), 1);
    zassert_false(thread.IsRunning());
}

ZTEST(thread, test_IsRunning_clears_itself_when_the_entry_point_returns) {
    SleepingWorker worker(50);
    Thread thread("self_terminating", &worker, STACK_SIZE, PRIORITY);

    thread.Initialize();
    thread.Start();

    zassert_true(thread.IsRunning());

    k_sleep(K_MSEC(200));

    zassert_equal(worker.Finished(), 1);
    zassert_false(thread.IsRunning());

    thread.Join();
}

ZTEST(thread, test_Start_can_be_repeated_after_the_entry_point_returned) {
    SleepingWorker worker(20);
    Thread thread("self_restarting", &worker, STACK_SIZE, PRIORITY);

    thread.Initialize();
    thread.Start();

    k_sleep(K_MSEC(150));
    zassert_equal(worker.Finished(), 1);

    thread.Start();

    k_sleep(K_MSEC(150));

    zassert_equal(worker.Entries(), 2);
    zassert_equal(worker.Finished(), 2);

    thread.Join();
}

ZTEST(thread, test_destructor_waits_for_the_entry_point_to_return) {
    SleepingWorker worker(150);

    {
        Thread thread("destroyed_while_running", &worker, STACK_SIZE, PRIORITY);
        thread.Initialize();
        thread.Start();

        zassert_equal(worker.Finished(), 0);
    }

    zassert_equal(worker.Finished(), 1);
}

ZTEST(thread, test_Join_is_ignored_when_the_thread_never_started) {
    BlockingWorker worker;
    Thread thread("never_started", &worker, STACK_SIZE, PRIORITY);

    thread.Join();
    thread.Initialize();
    thread.Join();

    zassert_false(thread.IsRunning());
    zassert_equal(worker.Entries(), 0);
}

ZTEST(thread, test_threads_run_independently_of_each_other) {
    constexpr int THREAD_COUNT = 3;

    BlockingWorker workers[THREAD_COUNT];
    Thread threads[] = {
        Thread("worker_0", &workers[0], STACK_SIZE, PRIORITY),
        Thread("worker_1", &workers[1], STACK_SIZE, PRIORITY),
        Thread("worker_2", &workers[2], STACK_SIZE, PRIORITY)
    };

    for(auto& thread : threads) {
        thread.Initialize();
        thread.Start();
    }

    for(auto& worker : workers)
        zassert_true(worker.WaitForEntry(SYNC_TIMEOUT_MS));

    for(auto& thread : threads)
        zassert_true(thread.IsRunning());

    zassert_true(threads[0].GetThread() != threads[1].GetThread());
    zassert_true(threads[1].GetThread() != threads[2].GetThread());

    for(auto& worker : workers)
        worker.Resume();

    for(auto& thread : threads)
        thread.Join();

    for(auto& worker : workers)
        zassert_equal(worker.Exits(), 1);
}
