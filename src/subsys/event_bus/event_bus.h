#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <zephyr/kernel.h>

#include "subsys/threading/scoped_mutex.h"
#include "subsys/threading/work_queue_thread.h"
#include "utilities/memory/memory_resource_manager.h"

#include "event_bus_task.h"
#include "i_event_bus.h"
#include "i_event_channel.h"

namespace eerie_leap::subsys::event_bus {

namespace threading = eerie_leap::subsys::threading;

using threading::ScopedMutex;
using threading::WorkQueueThread;
using eerie_leap::utilities::memory::Mrm;

// Transport shared by one or more channels: owns the worker thread that drains their
// queues. The bus never sees the events it carries, so channels whose event and
// payload enums are unrelated can share one thread.
class EventBus : public IEventBus {
private:
    static constexpr int k_default_priority = 10;

    std::string bus_name_;

    EventChannelSlots channels_{};
    std::atomic<size_t> channel_count_{0};
    k_mutex channels_mutex_;

    std::unique_ptr<WorkQueueThread> work_queue_thread_;
    std::optional<threading::WorkQueueTask<EventBusTask>> work_queue_task_;

    k_sem processing_semaphore_;

    void Initialize();

    static threading::WorkQueueTaskResult ProcessEventWork(EventBusTask* task);

protected:
    EventBus(std::string bus_name, int k_stack_size, int priority = k_default_priority);

public:
    ~EventBus() override;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Idempotent; -EEXIST when the channel already belongs to another bus.
    int RegisterChannel(IEventChannel& channel);

    void Wake() override;
};

inline EventBus::EventBus(std::string bus_name, int k_stack_size, int priority)
    : bus_name_(std::move(bus_name)) {

    k_sem_init(&processing_semaphore_, 1, 1);
    k_mutex_init(&channels_mutex_);

    work_queue_thread_ = std::make_unique<WorkQueueThread>(
        bus_name_,
        k_stack_size,
        priority,
        true,
        Mrm::GetExtPmr());

    Initialize();
}

inline EventBus::~EventBus() {
    // Channels outlive a stack-allocated bus in the tests; leaving them pointing at
    // it would dangle on the next publish.
    size_t count = channel_count_.load(std::memory_order_acquire);
    for(size_t i = 0; i < count; ++i)
        channels_[i]->OnRegistered(nullptr);
}

inline void EventBus::Initialize() {
    work_queue_thread_->Initialize();

    auto event_task = std::make_unique<EventBusTask>();
    event_task->processing_semaphore = &processing_semaphore_;
    event_task->channels = &channels_;
    event_task->channel_count = &channel_count_;

    work_queue_task_ = work_queue_thread_->CreateTask(ProcessEventWork, std::move(event_task));
}

inline int EventBus::RegisterChannel(IEventChannel& channel) {
    ScopedMutex guard(channels_mutex_);

    const auto* bound = channel.GetBus();
    if(bound == this)
        return 0;

    if(bound != nullptr)
        return -EEXIST;

    size_t count = channel_count_.load(std::memory_order_relaxed);
    if(count == k_max_bus_channels)
        return -ENOSPC;

    channels_[count] = &channel;
    channel.OnRegistered(this);

    // Publish the slot only once it is filled; the drain loop reads it without a lock.
    channel_count_.store(count + 1, std::memory_order_release);

    return 0;
}

inline void EventBus::Wake() {
    if(work_queue_task_)
        work_queue_task_.value().Reschedule();
}

inline threading::WorkQueueTaskResult EventBus::ProcessEventWork(EventBusTask* task) {
    if(k_sem_take(task->processing_semaphore, K_NO_WAIT) != 0) {
        return {
            .reschedule = false
        };
    }

    // A throw from a dispatch would otherwise keep the semaphore forever and wedge
    // the bus for the rest of the run.
    struct SemaphoreRelease {
        k_sem* sem;
        ~SemaphoreRelease() { k_sem_give(sem); }
    } release{task->processing_semaphore};

    size_t count = task->channel_count->load(std::memory_order_acquire);

    // Round-robin one event at a time, so a busy channel cannot starve its peers.
    bool dispatched = true;
    while(dispatched) {
        dispatched = false;

        for(size_t i = 0; i < count; ++i) {
            if((*task->channels)[i]->DrainOne())
                dispatched = true;
        }
    }

    return {
        .reschedule = false
    };
}

} // namespace eerie_leap::subsys::event_bus
