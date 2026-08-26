#include <utility>

#include <zephyr/sys/printk.h>

#include "subsys/threading/scoped_mutex.h"
#include "utilities/memory/memory_resource_manager.h"

#include "event_bus.h"

namespace eerie_leap::subsys::event_bus {

using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
EventBus<EventTypeEnum, PayloadTypeEnum>::EventBus(
    std::string bus_name,
    int k_stack_size,
    DispatchGuardFn dispatch_guard_before,
    DispatchGuardFn dispatch_guard_after)
        : bus_name_(
            std::move(bus_name)),
            dispatch_guard_before_(dispatch_guard_before),
            dispatch_guard_after_(dispatch_guard_after) {

    subscribers_ = std::make_shared<SubscriberMap<EventTypeEnum, PayloadTypeEnum>>();
    k_sem_init(&processing_semaphore_, 1, 1);
    k_mutex_init(&subscribers_mutex_);

    work_queue_thread_ = std::make_unique<WorkQueueThread>(
        bus_name_,
        k_stack_size,
        10,
        true,
        Mrm::GetExtPmr());

    Initialize();
}

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
void EventBus<EventTypeEnum, PayloadTypeEnum>::Initialize() {
    work_queue_thread_->Initialize();

    auto event_task = std::make_unique<EventBusTaskType>();
    event_task->processing_semaphore = &processing_semaphore_;
    event_task->subscribers = subscribers_;
    event_task->subscribers_mutex = &subscribers_mutex_;
    event_task->dispatch_guard_before = dispatch_guard_before_;
    event_task->dispatch_guard_after = dispatch_guard_after_;
    work_queue_task_ = work_queue_thread_->CreateTask(ProcessEventWork, std::move(event_task));
}

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
template<EventFilter<EventTypeEnum, PayloadTypeEnum> FilterType>
std::expected<SubscriptionHandle<EventTypeEnum>, std::string>
EventBus<EventTypeEnum, PayloadTypeEnum>::Subscribe(EventTypeEnum type, FilterType filter, EventHandler<EventTypeEnum, PayloadTypeEnum> handler) {
    ScopedMutex guard(subscribers_mutex_);

    try {
        size_t id = next_id_++;
        auto subscription = std::make_shared<Subscription<EventTypeEnum, PayloadTypeEnum>>(id, type, filter, std::move(handler));

        (*subscribers_)[type].push_back(std::move(subscription));

        return SubscriptionHandle<EventTypeEnum>{id, type};
    } catch (const std::exception& e) {
        return std::unexpected("Subscription failed: " + std::string(e.what()));
    }
}

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
bool EventBus<EventTypeEnum, PayloadTypeEnum>::Unsubscribe(SubscriptionHandle<EventTypeEnum>& handle) {
    if(!handle.IsValid())
        return false;

    ScopedMutex guard(subscribers_mutex_);

    auto it = subscribers_->find(handle.GetEventType());
    if(it == subscribers_->end())
        return false;

    auto& subs = it->second;
    auto sub_it = std::find_if(subs.begin(), subs.end(),
        [&handle](const auto& sub) {
            return sub->id == handle.GetId();
    });

    if(sub_it != subs.end()) {
        subs.erase(sub_it);
        handle.Invalidate();

        return true;
    }

    return false;
}

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
void EventBus<EventTypeEnum, PayloadTypeEnum>::Publish(const Event<EventTypeEnum, PayloadTypeEnum>& event) {
    ProcessEvent(subscribers_, &subscribers_mutex_, event, dispatch_guard_before_, dispatch_guard_after_);
}

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
void EventBus<EventTypeEnum, PayloadTypeEnum>::PublishAsync(const Event<EventTypeEnum, PayloadTypeEnum>& event) {
    if(!work_queue_task_)
        return;

    auto* task = work_queue_task_.value().GetUserdata();

    {
        ScopedMutex guard(task->queue_mutex);

        // Shed the oldest rather than the newest
        while(task->event_queue.size() >= EventBusTaskType::k_max_queued_events) {
            task->event_queue.pop();
            ++task->dropped_events;
        }

        task->event_queue.push(event);
    }

    work_queue_task_.value().Reschedule();
}

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
void EventBus<EventTypeEnum, PayloadTypeEnum>::ProcessEvent(
    std::shared_ptr<SubscriberMap<EventTypeEnum, PayloadTypeEnum>>& subscribers,
    k_mutex* subscribers_mutex,
    const Event<EventTypeEnum, PayloadTypeEnum>& event,
    DispatchGuardFn dispatch_guard_before,
    DispatchGuardFn dispatch_guard_after) {

    // Snapshot under the lock: a handler may subscribe or unsubscribe, which would
    // otherwise mutate the very vector being iterated.
    std::vector<SubscriptionPtr<EventTypeEnum, PayloadTypeEnum>> matched;

    {
        ScopedMutex guard(*subscribers_mutex);

        if(auto it = subscribers->find(event.type); it != subscribers->end()) {
            matched.reserve(it->second.size());

            for(const auto& subscription : it->second) {
                if(subscription->filter(event))
                    matched.push_back(subscription);
            }
        }
    }

    // The guards run for every publication, matching subscribers or not.
    if(dispatch_guard_before)
        dispatch_guard_before();

    struct GuardRelease {
        DispatchGuardFn fn;
        ~GuardRelease() { if(fn) fn(); }
    } release_guard{dispatch_guard_after};

    for(const auto& subscription : matched) {
        try {
            subscription->handler(event);
        } catch (const std::exception& e) {
            printk("[event_bus] subscriber handler threw for event type %u: %s\n",
                static_cast<unsigned>(event.type), e.what());
        } catch (...) {
            printk("[event_bus] subscriber handler threw a non-standard exception for event type %u\n",
                static_cast<unsigned>(event.type));
        }
    }
}

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
threading::WorkQueueTaskResult EventBus<EventTypeEnum, PayloadTypeEnum>::ProcessEventWork(EventBusTaskType* task) {
    if(k_sem_take(task->processing_semaphore, K_NO_WAIT) != 0)
        return {
            .reschedule = false
        };

    // A throw from a dispatch would otherwise keep the semaphore forever and wedge
    // the bus for the rest of the run.
    struct SemaphoreRelease {
        k_sem* sem;
        ~SemaphoreRelease() { k_sem_give(sem); }
    } release{task->processing_semaphore};

    while(true) {
        std::optional<Event<EventTypeEnum, PayloadTypeEnum>> event;
        size_t dropped = 0;

        {
            ScopedMutex guard(task->queue_mutex);

            if(!task->event_queue.empty()) {
                event = std::move(task->event_queue.front());
                task->event_queue.pop();
            }

            dropped = std::exchange(task->dropped_events, 0);
        }

        if(dropped != 0)
            printk("[event_bus] dropped %u queued events, subscribers cannot keep up\n",
                static_cast<unsigned>(dropped));

        if(!event)
            break;

        ProcessEvent(task->subscribers, task->subscribers_mutex, event.value(), task->dispatch_guard_before, task->dispatch_guard_after);
    }

    return {
        .reschedule = false
    };
}

} // namespace eerie_leap::subsys::event_bus
