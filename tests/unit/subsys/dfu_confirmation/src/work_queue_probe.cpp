#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/dfu/work_queue_probe.h"

using eerie_leap::subsys::dfu::WorkQueueProbe;

namespace {

constexpr size_t QUEUE_STACK_SIZE = 2048;
constexpr int QUEUE_PRIORITY = K_PRIO_PREEMPT(8);
constexpr int WAIT_MS = 1000;

K_THREAD_STACK_DEFINE(queue_stack, QUEUE_STACK_SIZE);
k_work_q queue;

/** A work item that keeps the queue's thread busy until the test releases it. */
struct Blocker {
    k_work work;
    k_sem entered;
    k_sem release;
};

Blocker blocker;

void BlockerHandler(k_work*) {
    k_sem_give(&blocker.entered);
    k_sem_take(&blocker.release, K_FOREVER);
}

bool WaitUntilRan(const WorkQueueProbe& probe) {
    for(int i = 0; i < WAIT_MS; ++i) {
        if(probe.Check())
            return true;

        k_msleep(1);
    }

    return false;
}

void* SetupQueue() {
    k_work_queue_init(&queue);
    k_work_queue_start(&queue, queue_stack, K_THREAD_STACK_SIZEOF(queue_stack), QUEUE_PRIORITY, nullptr);

    return nullptr;
}

void TeardownQueue(void*) {
    // Every test has released its blocker, so the queue drains and stops.
    zassert_true(k_work_queue_drain(&queue, true) >= 0);
    zassert_equal(k_work_queue_stop(&queue, K_MSEC(WAIT_MS)), 0);
}

void ResetBlocker(void*) {
    k_work_init(&blocker.work, BlockerHandler);
    k_sem_init(&blocker.entered, 0, 1);
    k_sem_init(&blocker.release, 0, 1);
}

} // namespace

ZTEST_SUITE(work_queue_probe, NULL, SetupQueue, ResetBlocker, NULL, TeardownQueue);

ZTEST(work_queue_probe, test_probe_runs_on_its_work_queue) {
    WorkQueueProbe probe("test_queue", &queue);

    zassert_false(probe.Check(), "A probe that was never armed has not run");
    zassert_equal(strcmp(probe.GetName(), "test_queue"), 0);

    probe.Arm();
    zassert_true(WaitUntilRan(probe));

    // Each Arm() starts over.
    probe.Arm();
    zassert_true(WaitUntilRan(probe));
}

ZTEST(work_queue_probe, test_probe_does_not_run_while_the_queue_is_stuck) {
    WorkQueueProbe probe("test_queue", &queue);

    k_work_submit_to_queue(&queue, &blocker.work);
    zassert_equal(k_sem_take(&blocker.entered, K_MSEC(WAIT_MS)), 0, "The queue never got to the blocker");

    probe.Arm();
    k_msleep(50);
    zassert_false(probe.Check(), "A blocked queue cannot run the probe");

    // Arming again while the first request is still queued does not queue a second one.
    probe.Arm();
    k_msleep(10);
    zassert_false(probe.Check());

    k_sem_give(&blocker.release);
    zassert_true(WaitUntilRan(probe), "The probe runs once the queue is free again");
}

ZTEST(work_queue_probe, test_destroying_a_queued_probe_cancels_its_work) {
    k_work_submit_to_queue(&queue, &blocker.work);
    zassert_equal(k_sem_take(&blocker.entered, K_MSEC(WAIT_MS)), 0);

    {
        WorkQueueProbe probe("test_queue", &queue);
        probe.Arm();
    }

    // The queue must not touch the destroyed probe.
    k_sem_give(&blocker.release);
    zassert_true(k_work_queue_drain(&queue, false) >= 0);
}
