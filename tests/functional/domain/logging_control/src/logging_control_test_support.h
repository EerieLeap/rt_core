#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/ztest.h>

#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/event_bus/event_channel_registry.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/threading/work_queue_thread.h"
#include "utilities/string/string_helpers.h"

#include "subsys/cdmp/models/cdmp_device_info.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"

#include "domain/logging_domain/control/i_logging_command_sender.h"
#include "domain/logging_domain/control/i_logging_control.h"
#include "domain/logging_domain/control/i_logging_restart_monitor.h"
#include "domain/logging_domain/control/models/logging_trigger.h"
#include "domain/logging_domain/event_bus/logging_event_type.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/logging_domain/event_bus/logging_payload_type.h"
#include "domain/logging_domain/event_bus/logging_status_reason.h"
#include "domain/sensor_domain/event_bus/sensor_event_type.h"
#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"

// The fixtures of the logging control suites. Everything has external linkage on purpose (a named
// namespace, inline functions), so every file of the binary shares one bus and one configuration
// queue: a channel stays with the first bus that registers it, and the registry holds one channel
// per id.
//
// Nothing here asserts on a thread other than the test's: fakes, blockers and jobs record into
// shared contexts, every wait has a timeout, and the contexts are owned by shared_ptr so a case
// that an assertion aborted leaves nothing a handler could reach.
namespace logging_control_test {

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::domain::logging_domain::control::ILoggingCommandSender;
using eerie_leap::domain::logging_domain::control::ILoggingControl;
using eerie_leap::domain::logging_domain::control::ILoggingRestartMonitor;
using eerie_leap::domain::logging_domain::control::models::LoggingTrigger;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;

// Every unit that can log, as a request names it.
inline constexpr uint32_t kAllUnits = ILoggingControl::kAllUnits;
inline constexpr uint8_t kBroadcast = ILoggingCommandSender::kBroadcast;

// Well past the time an event takes through the bus or a job through a queue.
inline constexpr int k_wait_ms = 2000;
inline constexpr int k_no_wait_ms = 100;
// What a fence event carries as its source, so a blocker recognises it.
inline constexpr uint32_t k_fence_source_id = 0xFE7CE;

inline k_timeout_t Ms(int milliseconds) {
    return K_MSEC(milliseconds);
}

// ======== The bus ========

// Carries sensor_events and logging_events in this binary; the channels drop every publish until a
// bus registers them. The registry resolves Sensors and Logging onto them, as the applications'
// InitializeEventChannels() does.
class LoggingControlTestBus : public EventBus {
public:
    // The first error of the registrations; 0 when the fixture is the one the registry resolves to.
    int registration = 0;

    LoggingControlTestBus() : EventBus("logging_control_bus", 4096) {
        Keep(RegisterChannel(SensorEventsChannel::GetInstance()));
        Keep(RegisterChannel(LoggingEventsChannel::GetInstance()));

        auto& registry = EventChannelRegistry::GetInstance();
        Keep(registry.Register(EventChannelId::Sensors, SensorEventsChannel::GetInstance()));
        Keep(registry.Register(EventChannelId::Logging, LoggingEventsChannel::GetInstance()));
    }

    static LoggingControlTestBus& Get() {
        static LoggingControlTestBus bus;

        return bus;
    }

private:
    void Keep(int result) {
        if(registration == 0)
            registration = result;
    }
};

// Every suite's setup.
inline void* SetUpChannels() {
    zassert_ok(LoggingControlTestBus::Get().registration, "The test bus could not register the channels.");

    return nullptr;
}

// ======== Queues ========

// The configuration work queue of the binary: RemoteLoggingControl sends from it, and the manager
// stores from it. 16 KiB, because exceptions unwind on it (qemu_riscv64 needs the room).
inline std::shared_ptr<WorkQueueThread> TestConfigQueue() {
    static std::shared_ptr<WorkQueueThread> queue = [] {
        auto created = std::make_shared<WorkQueueThread>("logging_control_config_queue", 16384, 5);
        zassert_true(created->Initialize(), "The configuration queue did not start.");

        return created;
    }();

    return queue;
}

inline k_tid_t QueueThread(WorkQueueThread& queue) {
    return k_work_queue_thread_get(queue.GetWorkQueue());
}

inline std::string CurrentThreadName() {
    const char* name = k_thread_name_get(k_current_get());

    return name == nullptr ? std::string() : std::string(name);
}

// Returns once everything queued before the call has run.
[[nodiscard]] inline bool FlushQueue(WorkQueueThread& queue) {
    auto done = std::make_shared<k_sem>();
    k_sem_init(done.get(), 0, 1);

    queue.Run([done] { k_sem_give(done.get()); });

    return k_sem_take(done.get(), K_SECONDS(5)) == 0;
}

// Holds a queue in one job until Open(), so whatever is queued meanwhile runs after it, in order.
// Each Close() has its own context, so an Open() never leaves a token for the next one.
class QueueGate {
private:
    struct Context {
        k_sem entered;
        k_sem open;

        Context() {
            k_sem_init(&entered, 0, 1);
            k_sem_init(&open, 0, 1);
        }
    };

    std::shared_ptr<Context> context_;

public:
    // Returns once the queue runs the gate's job, so it is provably held. The job gives up after
    // 5 s, so a case that never opens it cannot wedge the queue for good.
    [[nodiscard]] bool Close(WorkQueueThread& queue) {
        Open();

        auto context = std::make_shared<Context>();
        queue.Run([context] {
            k_sem_give(&context->entered);
            k_sem_take(&context->open, K_SECONDS(5));
        });
        context_ = context;

        return k_sem_take(&context->entered, K_SECONDS(5)) == 0;
    }

    // Does nothing while no gate is closed.
    void Open() {
        if(context_ == nullptr)
            return;

        k_sem_give(&context_->open);
        context_.reset();
    }
};

inline QueueGate& Gate() {
    static QueueGate gate;

    return gate;
}

// ======== Fakes ========

// A logging control that records the requests it gets. Optionally the request holds its caller until
// Release() ("a call in progress"), or also queues a job on a work queue ("work that starts
// logging"), which a closed gate keeps from running.
class FakeLoggingControl final : public ILoggingControl {
public:
    enum class Kind {
        Start,
        Stop
    };

    struct Request {
        Kind kind;
        k_tid_t thread;
        std::string thread_name;
        // What the request named; kAllUnits for a received command.
        uint32_t target_uid = kAllUnits;

        bool operator==(const Request& other) const {
            return kind == other.kind;
        }
    };

private:
    struct Context {
        k_mutex lock;
        k_sem requested;
        k_sem held;
        k_sem release;
        k_sem job_ran;
        std::vector<Request> requests;
        std::atomic<bool> is_holding = false;
        std::shared_ptr<WorkQueueThread> job_queue;

        Context() {
            k_mutex_init(&lock);
            k_sem_init(&requested, 0, K_SEM_MAX_LIMIT);
            k_sem_init(&held, 0, K_SEM_MAX_LIMIT);
            k_sem_init(&release, 0, K_SEM_MAX_LIMIT);
            k_sem_init(&job_ran, 0, K_SEM_MAX_LIMIT);
        }
    };

    std::shared_ptr<Context> context_ = std::make_shared<Context>();

    void Record(Kind kind, uint32_t target_uid = kAllUnits) {
        {
            ScopedMutex guard(context_->lock);
            context_->requests.push_back({kind, k_current_get(), CurrentThreadName(), target_uid});
        }
        k_sem_give(&context_->requested);

        if(context_->job_queue != nullptr)
            context_->job_queue->Run([context = context_] { k_sem_give(&context->job_ran); });

        if(context_->is_holding) {
            k_sem_give(&context_->held);
            // Gives up after 5 s, so a case that never releases cannot wedge the thread for good.
            k_sem_take(&context_->release, K_SECONDS(5));
        }
    }

public:
    void RequestStart(uint32_t target_uid) override { Record(Kind::Start, target_uid); }
    void RequestStop(uint32_t target_uid) override { Record(Kind::Stop, target_uid); }
    // As the supervisor takes it: a request, accepted.
    bool HandleLoggingCommand(bool is_start) override {
        Record(is_start ? Kind::Start : Kind::Stop);

        return true;
    }
    [[nodiscard]] bool IsActive() const override { return false; }

    // Each request blocks its caller until Release().
    void HoldRequests() { context_->is_holding = true; }
    [[nodiscard]] bool WaitUntilHeld(int timeout_ms = k_wait_ms) {
        return k_sem_take(&context_->held, Ms(timeout_ms)) == 0;
    }
    void Release() {
        context_->is_holding = false;
        k_sem_give(&context_->release);
    }

    // Each request also queues a job on @p queue that gives the semaphore WaitForJob() takes.
    void RunJobOn(std::shared_ptr<WorkQueueThread> queue) { context_->job_queue = std::move(queue); }
    [[nodiscard]] bool WaitForJob(int timeout_ms = k_wait_ms) {
        return k_sem_take(&context_->job_ran, Ms(timeout_ms)) == 0;
    }

    std::vector<Request> Requests() const {
        ScopedMutex guard(context_->lock);

        return context_->requests;
    }

    // The targets of the requests, in order.
    std::vector<uint32_t> Targets() const {
        std::vector<uint32_t> targets;
        for(const auto& request : Requests())
            targets.push_back(request.target_uid);

        return targets;
    }

    std::vector<Kind> Kinds() const {
        std::vector<Kind> kinds;
        for(const auto& request : Requests())
            kinds.push_back(request.kind);

        return kinds;
    }

    // Whether @p count requests arrived in time.
    [[nodiscard]] bool WaitForRequests(size_t count, int timeout_ms = k_wait_ms) const {
        const int64_t deadline = k_uptime_get() + timeout_ms;
        while(Requests().size() < count) {
            if(k_uptime_get() >= deadline)
                return false;

            k_msleep(2);
        }

        return true;
    }
};

using Kind = FakeLoggingControl::Kind;
using Kinds = std::vector<Kind>;

// A command sender that records what it is asked to send, and on which thread. Optionally a send
// holds its caller until Release() ("a send in progress").
class FakeCommandSender final : public ILoggingCommandSender {
public:
    struct Send {
        bool is_start;
        k_tid_t thread;
        uint8_t device_id;
    };

private:
    struct Context {
        k_mutex lock;
        k_sem held;
        k_sem release;
        std::vector<Send> sends;
        std::atomic<bool> is_holding = false;

        Context() {
            k_mutex_init(&lock);
            k_sem_init(&held, 0, K_SEM_MAX_LIMIT);
            k_sem_init(&release, 0, K_SEM_MAX_LIMIT);
        }
    };

    std::shared_ptr<Context> context_ = std::make_shared<Context>();

public:
    void SendLoggingCommand(bool is_start, uint8_t device_id) override {
        {
            ScopedMutex guard(context_->lock);
            context_->sends.push_back({is_start, k_current_get(), device_id});
        }

        if(context_->is_holding) {
            k_sem_give(&context_->held);
            // Gives up after 5 s, so a case that never releases cannot wedge the configuration
            // queue, which every case of the binary shares, for good.
            k_sem_take(&context_->release, K_SECONDS(5));
        }
    }

    // Each send blocks its caller, after it is recorded, until Release().
    void HoldSends() { context_->is_holding = true; }
    [[nodiscard]] bool WaitUntilHeld(int timeout_ms = k_wait_ms) {
        return k_sem_take(&context_->held, Ms(timeout_ms)) == 0;
    }
    void Release() {
        context_->is_holding = false;
        k_sem_give(&context_->release);
    }

    std::vector<Send> Sends() const {
        ScopedMutex guard(context_->lock);

        return context_->sends;
    }

    std::vector<bool> Starts() const {
        std::vector<bool> starts;
        for(const auto& send : Sends())
            starts.push_back(send.is_start);

        return starts;
    }

    // The IDs the sends went to, in order (kBroadcast for a broadcast).
    std::vector<uint8_t> Devices() const {
        std::vector<uint8_t> devices;
        for(const auto& send : Sends())
            devices.push_back(send.device_id);

        return devices;
    }

    // How many sends were broadcasts, and how many went to @p device_id.
    size_t Broadcasts() const {
        size_t count = 0;
        for(const auto& send : Sends())
            count += send.device_id == kBroadcast ? 1 : 0;

        return count;
    }

    size_t SentTo(uint8_t device_id) const {
        size_t count = 0;
        for(const auto& send : Sends())
            count += send.device_id == device_id ? 1 : 0;

        return count;
    }

    [[nodiscard]] bool WaitForSends(size_t count, int timeout_ms = k_wait_ms) const {
        const int64_t deadline = k_uptime_get() + timeout_ms;
        while(Sends().size() < count) {
            if(k_uptime_get() >= deadline)
                return false;

            k_msleep(2);
        }

        return true;
    }
};

// The CDMP device table as a case sets it: this unit and the other devices, safe to change while the
// control reads it. Add() puts an online logger in the table.
class FakeNetworkInfo final : public eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo {
public:
    using CdmpDeviceInfo = eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
    using CdmpDeviceType = eerie_leap::subsys::cdmp::models::CdmpDeviceType;
    using CdmpDeviceStatus = eerie_leap::subsys::cdmp::models::CdmpDeviceStatus;

    static constexpr uint32_t k_own_uid = 0xA0A0A0A0;

private:
    struct Context {
        mutable k_mutex lock;
        CdmpDeviceInfo own {.device_id = 1, .uid = k_own_uid, .device_type = CdmpDeviceType::DISPLAY, .status = CdmpDeviceStatus::ONLINE};
        std::vector<CdmpDeviceInfo> devices;

        Context() {
            k_mutex_init(&lock);
        }
    };

    std::shared_ptr<Context> context_ = std::make_shared<Context>();

public:
    CdmpDeviceInfo GetDeviceInfo() const override {
        ScopedMutex guard(context_->lock);

        return context_->own;
    }

    size_t GetNetworkDevices(std::span<CdmpDeviceInfo> devices) const override {
        ScopedMutex guard(context_->lock);

        const size_t count = std::min(devices.size(), context_->devices.size());
        for(size_t i = 0; i < count; i++)
            devices[i] = context_->devices[i];

        return count;
    }

    size_t GetNetworkDeviceCount() const override {
        ScopedMutex guard(context_->lock);

        return context_->devices.size();
    }

    // Puts a device in the table, or replaces the one with that ID.
    void Add(
        uint8_t device_id,
        uint32_t uid,
        CdmpDeviceType type = CdmpDeviceType::LOGGER,
        CdmpDeviceStatus status = CdmpDeviceStatus::ONLINE) {

        Remove(device_id);

        ScopedMutex guard(context_->lock);
        context_->devices.push_back({.device_id = device_id, .uid = uid, .device_type = type, .status = status});
    }

    void Remove(uint8_t device_id) {
        ScopedMutex guard(context_->lock);

        std::erase_if(context_->devices, [device_id](const CdmpDeviceInfo& device) { return device.device_id == device_id; });
    }
};

// A restart monitor that reports when a case says so. Report() runs the handler on the caller's
// thread, as the CDMP work queue does for the real one. CopyHandler() is "a report already under
// way": it holds the handler as the monitor's report would, so a case can call it after the control
// is gone. FailNextSet() makes the next non-null SetRestartHandler() throw std::bad_alloc, after it
// ran the hook of OnSet(), which stands for whatever happens on other threads meanwhile.
class FakeRestartMonitor final : public ILoggingRestartMonitor {
private:
    struct Context {
        k_mutex lock;
        std::shared_ptr<const RestartHandler> handler;
        std::function<void()> on_set;
        std::atomic<bool> is_failing = false;

        Context() {
            k_mutex_init(&lock);
        }
    };

    std::shared_ptr<Context> context_ = std::make_shared<Context>();

public:
    void SetRestartHandler(RestartHandler handler) override {
        if(handler && context_->on_set)
            context_->on_set();

        if(handler && context_->is_failing.exchange(false))
            throw std::bad_alloc();

        auto slot = handler ? std::make_shared<const RestartHandler>(std::move(handler)) : nullptr;

        ScopedMutex guard(context_->lock);
        context_->handler = std::move(slot);
    }

    void FailNextSet() { context_->is_failing = true; }

    // Runs before every non-null SetRestartHandler(), on its caller's thread. Null removes it.
    void OnSet(std::function<void()> hook) { context_->on_set = std::move(hook); }

    [[nodiscard]] bool HasHandler() const {
        ScopedMutex guard(context_->lock);

        return context_->handler != nullptr;
    }

    [[nodiscard]] std::shared_ptr<const RestartHandler> CopyHandler() const {
        ScopedMutex guard(context_->lock);

        return context_->handler;
    }

    // A logging unit restarted and is idle: runs the handler, if there is one.
    void Report(uint8_t device_id) const {
        const auto handler = CopyHandler();
        if(handler != nullptr)
            (*handler)(device_id);
    }
};

// ======== The bus thread ========

// A subscriber made before the code under test, so it runs first in a dispatch. Arm() makes its next
// event hold the bus thread until Release(), so a case can act while a dispatch is in flight.
// Fence() returns once every event published before it has been dispatched. The blocker owns its
// state through a shared context, so one left behind by an aborted case leaves nothing a handler
// could reach; Release() frees the bus thread.
template<typename TChannel>
class DispatchBlocker {
private:
    struct Context {
        k_sem entered;
        k_sem release;
        k_sem fence;
        std::atomic<bool> is_armed = false;

        Context() {
            k_sem_init(&entered, 0, 1);
            k_sem_init(&release, 0, K_SEM_MAX_LIMIT);
            k_sem_init(&fence, 0, K_SEM_MAX_LIMIT);
        }
    };

    TChannel& channel_;
    typename TChannel::EventTypeEnum type_;
    std::shared_ptr<Context> context_ = std::make_shared<Context>();
    AnySubscription subscription_;

public:
    DispatchBlocker(TChannel& channel, typename TChannel::EventTypeEnum type) : channel_(channel), type_(type) {
        subscription_ = CreateScopedSubscription(
            channel_,
            type_,
            [context = context_](const typename TChannel::EventMessage& event) {
                if(event.source_id == k_fence_source_id) {
                    k_sem_give(&context->fence);
                    return;
                }

                if(!context->is_armed.exchange(false))
                    return;

                k_sem_give(&context->entered);
                // Gives up after 5 s, so a case that never releases cannot wedge the bus for good.
                k_sem_take(&context->release, K_SECONDS(5));
            });
        zassert_not_null(subscription_.get(), "The blocker's subscription was refused.");
    }

    ~DispatchBlocker() {
        Release();
    }

    DispatchBlocker(const DispatchBlocker&) = delete;
    DispatchBlocker& operator=(const DispatchBlocker&) = delete;

    void Arm() { context_->is_armed = true; }

    [[nodiscard]] bool WaitUntilBlocking(int timeout_ms = k_wait_ms) {
        return k_sem_take(&context_->entered, Ms(timeout_ms)) == 0;
    }

    void Release() {
        context_->is_armed = false;
        k_sem_give(&context_->release);
    }

    // The channel delivers in order, so everything published before the marker was dispatched when
    // the blocker sees it.
    [[nodiscard]] bool Fence() {
        channel_.PublishAsync({.source_id = k_fence_source_id, .type = type_});

        return k_sem_take(&context_->fence, Ms(k_wait_ms)) == 0;
    }
};

using SensorsBlocker = DispatchBlocker<SensorEventsChannel>;
using LoggingBlocker = DispatchBlocker<LoggingEventsChannel>;

// ======== Events ========

// DataUpdated as the sensor events publisher sends it: the sensor's id hashed, and its value.
// Synchronous by default, so the handlers have run when it returns; asynchronous goes through the
// bus thread.
inline void PublishSensorValue(std::string_view sensor_id, EventData value, bool is_async = false) {
    SensorEventsChannel::EventMessage event {
        .source_id = 1,
        .type = SensorEventType::DataUpdated
    };
    event.payload.insert_or_assign(SensorPayloadType::SensorId, EventData(StringHelpers::GetHash(sensor_id)));
    event.payload.insert_or_assign(SensorPayloadType::Value, std::move(value));

    if(is_async)
        SensorEventsChannel::GetInstance().PublishAsync(event);
    else
        SensorEventsChannel::GetInstance().Publish(event);
}

// StatusUpdated as the NetworkStateImporter publishes it: the reason only while inactive.
inline void PublishStatus(
    bool is_active,
    std::optional<LoggingStatusReason> reason = std::nullopt,
    bool is_async = false) {

    LoggingEventsChannel::EventMessage event {
        .source_id = 2,
        .type = LoggingEventType::StatusUpdated
    };
    event.payload.insert_or_assign(LoggingPayloadType::IsActive, EventData(is_active));
    if(reason.has_value())
        event.payload.insert_or_assign(LoggingPayloadType::Reason, EventData(static_cast<uint32_t>(*reason)));

    if(is_async)
        LoggingEventsChannel::GetInstance().PublishAsync(event);
    else
        LoggingEventsChannel::GetInstance().Publish(event);
}

// ======== Triggers ========

// A trigger on the Value of one sensor.
inline LoggingTrigger SensorTrigger(std::string_view sensor_id) {
    LoggingTrigger trigger;
    trigger.binding.channel = EventChannelId::Sensors;
    trigger.binding.event_type = static_cast<uint32_t>(SensorEventType::DataUpdated);
    trigger.binding.payload_key = static_cast<uint32_t>(SensorPayloadType::Value);
    trigger.binding.selector_key = static_cast<uint32_t>(SensorPayloadType::SensorId);
    trigger.binding.selector_value = std::pmr::string(sensor_id);

    return trigger;
}

// ======== The log ========

// Whether the log has backends to count with. The minimal log mode that ztest selects off native_sim
// prints to printk and has none, so there the counts are not asserted and the cases check only what
// the code does.
inline constexpr bool k_can_count_logs = !IS_ENABLED(CONFIG_LOG_MODE_MINIMAL);

// Counts the warnings and errors of one logger, so a case can assert "logged once". The counting
// backend is defined once for the binary (in logging_control_test_support.cpp); a case names the
// logger with Watch(), publishes, and reads Count().
class LogCounter {
public:
    static LogCounter& Get();

    // Counts messages of the logger @p source_name (its module name) at WRN or worse from now on.
    void Watch(const char* source_name);
    // Flushes the deferred log first, so every message of the case has been processed.
    size_t Count();

    // The backend's entry point. Any context: native_sim logs in immediate mode, which calls the
    // backend in the context of the logging caller, an ISR included, so no mutex.
    void Add(const char* source_name);

private:
    k_spinlock lock_{};
    const char* source_name_ = nullptr;
    size_t count_ = 0;

    LogCounter() = default;
};

// Asserts the count of the watched logger where the log can be counted.
#define zassert_logged(expected, message) \
    do { \
        if(logging_control_test::k_can_count_logs) \
            zassert_equal(logging_control_test::LogCounter::Get().Count(), (expected), message); \
    } while(0)

} // namespace logging_control_test
