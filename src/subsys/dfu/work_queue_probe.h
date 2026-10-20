#pragma once

#include <atomic>

#include <zephyr/kernel.h>

#include "i_probe.h"

namespace eerie_leap::subsys::dfu {

/**
 * @brief IProbe that submits a k_work to a work queue; the handler sets the ran flag.
 *
 * A queue that is stuck on one item, or whose thread died, never runs the handler, so Check() stays false.
 * The destructor waits for a handler that is running, so the queue must still be alive then.
 */
class WorkQueueProbe final : public IProbe {
private:
    // Standard layout, so that the handler can find the flag from the k_work with CONTAINER_OF.
    struct Item {
        k_work work{};
        std::atomic<bool> ran{ false };
    };

    Item item_{};
    k_work_q* work_q_;
    const char* name_;

    static void Handler(k_work* work);

public:
    /**
     * @param name Names the probe in the log; must outlive the probe (a literal).
     * @param work_q The queue to probe; initialized, and started before the first Arm() is expected to run.
     */
    WorkQueueProbe(const char* name, k_work_q* work_q);
    ~WorkQueueProbe() override;

    WorkQueueProbe(const WorkQueueProbe&) = delete;
    WorkQueueProbe& operator=(const WorkQueueProbe&) = delete;
    WorkQueueProbe(WorkQueueProbe&&) = delete;
    WorkQueueProbe& operator=(WorkQueueProbe&&) = delete;

    void Arm() override;
    [[nodiscard]] bool Check() const override;
    [[nodiscard]] const char* GetName() const override;
};

} // namespace eerie_leap::subsys::dfu
