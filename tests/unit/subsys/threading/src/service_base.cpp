#include <stdexcept>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/threading/service_base.h"

using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;

namespace {

class ThrowingService : public ServiceBase<> {
private:
    bool DoStart() override {
        start_calls++;
        if(throw_on_start)
            throw std::runtime_error("start failed");

        return true;
    }

    bool DoStop() override {
        stop_calls++;
        if(throw_on_stop)
            throw std::runtime_error("stop failed");

        return true;
    }

public:
    bool throw_on_start = false;
    bool throw_on_stop = false;
    int start_calls = 0;
    int stop_calls = 0;
};

// Pausable; DoPause() reports that it entered and waits to be released, so another thread's
// transition can be attempted while it runs.
class BlockingPauseService : public ServiceBase<> {
private:
    bool DoStart() override {
        start_calls++;
        return true;
    }

    bool DoStop() override {
        stop_calls++;
        return true;
    }

    bool DoPause() override {
        pause_calls++;
        k_sem_give(&pause_entered);
        k_sem_take(&pause_release, K_FOREVER);
        return true;
    }

    bool DoResume() override {
        resume_calls++;
        return true;
    }

public:
    k_sem pause_entered;
    k_sem pause_release;
    int start_calls = 0;
    int stop_calls = 0;
    int pause_calls = 0;
    int resume_calls = 0;

    BlockingPauseService() {
        k_sem_init(&pause_entered, 0, 1);
        k_sem_init(&pause_release, 0, 1);
    }

    [[nodiscard]] bool IsPausable() const noexcept override {
        return true;
    }
};

constexpr int kTransitionThreadPriority = 5;

K_THREAD_STACK_DEFINE(pausing_stack, 4096);
K_THREAD_STACK_DEFINE(stopping_stack, 4096);

struct Transition {
    BlockingPauseService* service;
    bool result;
};

void PauseFromAnotherThread(void* context, void*, void*) {
    auto* transition = static_cast<Transition*>(context);
    transition->result = transition->service->Pause();
}

void StopFromAnotherThread(void* context, void*, void*) {
    auto* transition = static_cast<Transition*>(context);
    transition->result = transition->service->Stop();
}

} // namespace

ZTEST_SUITE(service_base, NULL, NULL, NULL, NULL, NULL);

// A calibration Pause() on one thread and a configuration restart's Stop() on another: the Stop()
// waits for the pause to complete instead of tearing the service down under it, and wins last.
ZTEST(service_base, test_a_transition_waits_for_one_in_progress_on_another_thread) {
    BlockingPauseService service;
    zassert_true(service.Start());

    Transition pause{ .service = &service, .result = false };
    k_thread pausing;
    k_thread_create(
        &pausing, pausing_stack, K_THREAD_STACK_SIZEOF(pausing_stack), PauseFromAnotherThread, &pause, nullptr, nullptr,
        kTransitionThreadPriority, 0, K_NO_WAIT
    );
    zassert_ok(k_sem_take(&service.pause_entered, K_SECONDS(1)));

    Transition stop{ .service = &service, .result = false };
    k_thread stopping;
    k_thread_create(
        &stopping, stopping_stack, K_THREAD_STACK_SIZEOF(stopping_stack), StopFromAnotherThread, &stop, nullptr,
        nullptr, kTransitionThreadPriority, 0, K_NO_WAIT
    );
    k_msleep(50);

    // Taken before the release and asserted after the joins, so a failure leaves no thread behind.
    const int stop_calls_during_pause = service.stop_calls;
    const ServiceState state_during_pause = service.GetState();

    k_sem_give(&service.pause_release);
    zassert_ok(k_thread_join(&pausing, K_SECONDS(1)));
    zassert_ok(k_thread_join(&stopping, K_SECONDS(1)));

    zassert_equal(stop_calls_during_pause, 0, "Stop() ran while the pause was in progress");
    zassert_equal(state_during_pause, ServiceState::RUNNING);

    zassert_true(pause.result);
    zassert_true(stop.result);
    zassert_equal(service.pause_calls, 1);
    zassert_equal(service.stop_calls, 1);
    zassert_equal(service.GetState(), ServiceState::STOPPED, "The later transition is the one that holds");

    // A resume of what the restart has taken back is refused rather than acted on.
    zassert_false(service.Resume());
    zassert_equal(service.resume_calls, 0);
}

ZTEST(service_base, test_Start_restores_state_when_DoStart_throws) {
    ThrowingService service;
    service.throw_on_start = true;

    bool is_thrown = false;
    try {
        service.Start();
    } catch(const std::runtime_error&) {
        is_thrown = true;
    }

    zassert_true(is_thrown);
    zassert_equal(service.GetState(), ServiceState::STOPPED);

    service.throw_on_start = false;
    zassert_true(service.Start(), "A later Start() is not refused");
    zassert_equal(service.GetState(), ServiceState::RUNNING);
    zassert_equal(service.start_calls, 2);
}

ZTEST(service_base, test_Stop_restores_state_when_DoStop_throws) {
    ThrowingService service;
    zassert_true(service.Start());

    service.throw_on_stop = true;

    bool is_thrown = false;
    try {
        service.Stop();
    } catch(const std::runtime_error&) {
        is_thrown = true;
    }

    zassert_true(is_thrown);
    zassert_equal(service.GetState(), ServiceState::RUNNING);

    service.throw_on_stop = false;
    zassert_true(service.Stop(), "A later Stop() is not refused");
    zassert_equal(service.GetState(), ServiceState::STOPPED);
    zassert_equal(service.stop_calls, 2);
}
