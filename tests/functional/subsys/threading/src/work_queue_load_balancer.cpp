#include <memory>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_load_balancer.h"

using eerie_leap::subsys::threading::WorkQueueLoadBalancer;
using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr int STACK_SIZE = 2048;
constexpr int PRIORITY = 6;

// AddThread() reads GetWorkQueue(), so only initialized queues can join the balancer.
std::shared_ptr<WorkQueueThread> MakeQueueThread(const char* name) {
    auto queue_thread = std::make_shared<WorkQueueThread>(name, STACK_SIZE, PRIORITY);
    queue_thread->Initialize();

    return queue_thread;
}

class BalancedQueues {
private:
    std::vector<std::shared_ptr<WorkQueueThread>> queue_threads_;

public:
    WorkQueueLoadBalancer balancer;

    explicit BalancedQueues(int count) {
        static const char* names[] = {"wq_lb_0", "wq_lb_1", "wq_lb_2", "wq_lb_3"};

        for(int i = 0; i < count; ++i) {
            queue_threads_.push_back(MakeQueueThread(names[i]));
            balancer.AddThread(queue_threads_.back());
        }
    }

    ~BalancedQueues() {
        for(auto& queue_thread : queue_threads_)
            queue_thread->Stop();
    }

    [[nodiscard]] const std::shared_ptr<WorkQueueThread>& At(int index) const {
        return queue_threads_[index];
    }
};

} // namespace

ZTEST_SUITE(work_queue_load_balancer, NULL, NULL, NULL, NULL, NULL);

ZTEST(work_queue_load_balancer, test_GetLeastLoadedQueue_always_returns_the_only_queue) {
    BalancedQueues queues(1);

    for(int i = 0; i < 4; ++i)
        zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(0));
}

ZTEST(work_queue_load_balancer, test_GetLeastLoadedQueue_spreads_reservations_over_idle_queues) {
    BalancedQueues queues(3);

    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(0));
    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(1));
    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(2));
    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(0));
}

ZTEST(work_queue_load_balancer, test_OnWorkComplete_releases_the_reserved_capacity) {
    BalancedQueues queues(2);

    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(0));

    queues.balancer.OnWorkComplete(*queues.At(0), 0);

    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(0));
}

ZTEST(work_queue_load_balancer, test_GetLeastLoadedQueue_avoids_the_queue_with_recorded_load) {
    BalancedQueues queues(2);

    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(0));
    queues.balancer.OnWorkComplete(*queues.At(0), 250);

    // The first queue carries 250 ms of load, so the idle one wins even though it is second.
    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(1));
    queues.balancer.OnWorkComplete(*queues.At(1), 0);

    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(1));
}

ZTEST(work_queue_load_balancer, test_pending_reservations_outweigh_recorded_load) {
    BalancedQueues queues(2);

    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(0));
    queues.balancer.OnWorkComplete(*queues.At(0), 5);

    // Queue 0 scores 5 against queue 1's single outstanding reservation, worth 10.
    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(1));
    zassert_equal(queues.balancer.GetLeastLoadedQueue(), queues.At(0));
}
