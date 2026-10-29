#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/event_bus/event.h"
#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/threading/work_queue_thread.h"

#include "isr_sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::threading::WorkQueueThread;

// Feeds a control sensor (CONTROL_INDICATOR or CONTROL_ANALOG) with the values its control publishes
// as InputRequested on sensor_events, processed on the work queue.
//
// The bus thread queues each value and submits one work item, which turns the queued values into
// readings in order. An indicator keeps every level, since a Toggle needs both a press and its
// release; one that finds the queue full is dropped and logged. An analog sensor keeps only its
// newest value, as GpioSensorReader keeps a pin's newest level. Seeds nothing: the start step gives
// an indicator its released input (ReadingPipeline::CommitStartValues()).
class ControlSensorReader : public IsrSensorReaderBase {
public:
    // The levels an indicator keeps waiting for the work queue, 8 presses with their releases. A
    // control publishes at a person's pace, so the queue fills only if processing itself stalls.
    static constexpr size_t QUEUE_CAPACITY = 16;

private:
    struct PendingWork {
        k_work work;
        ControlSensorReader* reader;
    };

    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<DispatchGuard> guard_;
    AnySubscription subscription_;

    bool is_indicator_ = false;
    bool is_unreadable_logged_ = false; // Inside the guard only.

    k_spinlock lock_{};
    std::array<float, QUEUE_CAPACITY> queue_{};
    size_t head_ = 0;
    size_t count_ = 0;
    atomic_t dropped_count_ = ATOMIC_INIT(0);

    PendingWork pending_work_{};
    k_work_sync work_sync_{};

    static void WorkHandler(k_work* work);

    void OnInput(const EventData& value);
    void QueueInput(float value);
    void ProcessPendingInputs() noexcept;

public:
    /**
     * @brief Subscribes to the input of the runtime's sensor.
     * @throws std::runtime_error for a sensor that is not a control sensor, and when the subscription
     *         fails, as it does while the sensors channel is not registered.
     */
    ControlSensorReader(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime,
        ProcessSensorCallback process_sensor_callback,
        std::shared_ptr<WorkQueueThread> work_queue_thread
    );

    // Detaches the guard, drops the subscription, then waits for the work item. Never from the
    // processing work queue.
    ~ControlSensorReader() override;

    ControlSensorReader(const ControlSensorReader&) = delete;
    ControlSensorReader& operator=(const ControlSensorReader&) = delete;
    ControlSensorReader(ControlSensorReader&&) = delete;
    ControlSensorReader& operator=(ControlSensorReader&&) = delete;

    /** @return The levels dropped for want of room in the queue since the reader was built. */
    [[nodiscard]] uint32_t GetDroppedCount() const {
        return static_cast<uint32_t>(atomic_get(&dropped_count_));
    }
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
