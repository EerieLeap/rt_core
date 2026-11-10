#include "can_monitor_service.h"

#include <algorithm>
#include <new>
#include <utility>

#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "domain/can_monitor_domain/utilities/can_record.h"
#include "subsys/canbus/canbus_time.h"
#include "subsys/threading/scoped_mutex.h"
#include "utilities/memory/memory_resource_manager.h"

namespace eerie_leap::domain::can_monitor_domain::services {

LOG_MODULE_REGISTER(can_monitor_logger, CONFIG_EERIE_LEAP_CAN_MONITOR_LOG_LEVEL);

using eerie_leap::subsys::canbus::CanbusConfig;
using eerie_leap::subsys::canbus::NowUs;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::utilities::memory::Mrm;

using models::CAN_MONITOR_MODE_COUNT;
using models::CAN_MONITOR_SYNTHETIC_PORT;
using models::CAN_MONITOR_VERSION;
using models::CanMonitorPortStatus;
using utilities::CanBusState;
using utilities::ToCanBusState;

namespace {

/// An own bus is looked at this often for having stopped by itself.
constexpr int64_t OWN_BUS_CHECK_MS = 500;

/// The controller's state and counters as a state record has them; stopped when it can't be read.
CanBusState ReadState(const device* controller, can_bus_err_cnt& counters) {
    counters = {};

    can_state state = CAN_STATE_STOPPED;
    if(controller == nullptr || can_get_state(controller, &state, &counters) != 0) {
        counters = {};
        return CanBusState::STOPPED;
    }

    return ToCanBusState(state);
}

/// The bitrate a bus runs at, 0 until it has one.
void ReadBitrates(const Canbus& bus, uint32_t& bitrate, uint32_t& data_bitrate) {
    const CanbusConfig config = bus.GetConfig();
    bitrate = bus.IsBitrateDetected() ? config.bitrate : 0;
    data_bitrate = config.type == CanbusType::CANFD ? config.data_bitrate : 0;
}

} // namespace

CanMonitorService::CanMonitorService(
    std::shared_ptr<CanbusService> canbus_service,
    DtCanbusProvider dt_canbus_provider,
    CanMonitorLink link,
    CanErrorCountersFactory counters_factory
) :
    canbus_service_(std::move(canbus_service)),
    dt_canbus_provider_(std::move(dt_canbus_provider)),
    link_(link),
    counters_factory_(std::move(counters_factory)),
    sender_(std::move(link)) {

    k_mutex_init(&lock_);

    // Opening a session then can't fail on the list.
    sessions_.reserve(CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_SESSIONS);
}

CanMonitorService::~CanMonitorService() {
    // Nothing of the queue's may run once the sessions go.
    if(lease_task_)
        lease_task_->Cancel();
    if(disconnect_task_)
        disconnect_task_->Cancel();

    {
        const ScopedMutex guard(lock_);
        EndAll(CanEndReason::CONFIGURATION_STORED);
    }

    if(work_queue_)
        work_queue_->Stop();

    // The sender goes with the members; sessions still draining are freed with it.
}

bool CanMonitorService::DoInitialize() {
    if(!work_queue_) {
        work_queue_ = std::make_unique<WorkQueueThread>(
            "can_monitor_wq", CONFIG_EERIE_LEAP_CAN_MONITOR_WORK_QUEUE_STACK_SIZE,
            CONFIG_EERIE_LEAP_CAN_MONITOR_WORK_QUEUE_PRIORITY
        );

        if(!work_queue_->Initialize()) {
            work_queue_.reset();
            return false;
        }

        lease_task_ = work_queue_->CreateTask(
            [](CanMonitorService* service) {
                service->EndExpired();
                return WorkQueueTaskResult{};
            },
            this
        );
        disconnect_task_ = work_queue_->CreateTask(
            [](CanMonitorService* service) {
                service->EndOnDisconnect();
                return WorkQueueTaskResult{};
            },
            this
        );
    }

    return sender_.Start();
}

bool CanMonitorService::DoStart() {
    return true;
}

// A new CAN configuration is about to replace the buses the sessions tap.
bool CanMonitorService::DoStop() {
    const ScopedMutex guard(lock_);
    EndAll(CanEndReason::CONFIGURATION_STORED);

    return true;
}

CanMonitorService::OpenSession* CanMonitorService::Find(uint8_t port) {
    const auto it = std::ranges::find(sessions_, port, [](const OpenSession& open) {
        return open.answer.ch;
    });

    return it != sessions_.end() ? &*it : nullptr;
}

const CanMonitorService::OpenSession* CanMonitorService::FindConst(uint8_t port) const {
    const auto it = std::ranges::find(sessions_, port, [](const OpenSession& open) {
        return open.answer.ch;
    });

    return it != sessions_.end() ? &*it : nullptr;
}

CanMonitorService::OpenSession* CanMonitorService::Find(uint32_t token) {
    const auto it = std::ranges::find(sessions_, token, [](const OpenSession& open) {
        return open.answer.tok;
    });

    return it != sessions_.end() ? &*it : nullptr;
}

uint32_t CanMonitorService::NewToken() const {
    for(;;) {
        const uint32_t token = sys_rand32_get();
        if(token != 0 && std::ranges::none_of(sessions_, [token](const OpenSession& open) {
               return open.answer.tok == token;
           }))
            return token;
    }
}

std::shared_ptr<ICanErrorCounters> CanMonitorService::MakeCounters(const device* controller) const {
    if(counters_factory_)
        return counters_factory_(controller);

    return std::make_shared<DeviceCanErrorCounters>(controller);
}

std::expected<CanMonitorService::OpenSession*, CanMonitorError> CanMonitorService::Touch(uint32_t token) {
    if(token == 0)
        return std::unexpected(CanMonitorError::BAD_TOKEN);

    OpenSession* open = Find(token);
    if(open == nullptr)
        return std::unexpected(CanMonitorError::NO_SESSION);

    open->lease_deadline_ms = k_uptime_get() + CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS;
    open->is_token_used = true;

    return open;
}

std::expected<CanMonitorOpenAnswer, CanMonitorError> CanMonitorService::Open(const CanMonitorOpenRequest& request) {
    const ScopedMutex guard(lock_);

    if(!IsRunning() || !sender_.IsRunning())
        return std::unexpected(CanMonitorError::BUSY);

    const bool is_synthetic = request.synthetic.has_value();

#ifndef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
    if(is_synthetic)
        return std::unexpected(CanMonitorError::BAD_MODE);
#endif

    if(request.filters.size() > CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS
        || (is_synthetic && !request.synthetic->IsValid()))
        return std::unexpected(CanMonitorError::UNKNOWN);

    const uint8_t port = is_synthetic ? CAN_MONITOR_SYNTHETIC_PORT : request.ch;
    const CanMonitorMode mode = is_synthetic ? CanMonitorMode::TAP : request.mode;

    std::shared_ptr<CanbusProxy> proxy;
    Canbus* bus = nullptr;
    const device* controller = nullptr;

    // What a session on a bus of its own runs at.
    CanbusType own_type = CanbusType::CLASSICAL_CAN;
    uint32_t own_bitrate = 0;
    uint32_t own_data_bitrate = 0;
    const bool is_own_bus = !is_synthetic && mode != CanMonitorMode::TAP;

    if(!is_synthetic) {
        controller = dt_canbus_provider_ ? dt_canbus_provider_(port) : nullptr;
        if(controller == nullptr)
            return std::unexpected(CanMonitorError::NOT_FOUND);

        if(static_cast<uint32_t>(mode) >= CAN_MONITOR_MODE_COUNT)
            return std::unexpected(CanMonitorError::BAD_MODE);

        proxy = canbus_service_->GetCanbus(port);
        bus = proxy ? proxy->Get() : nullptr;

        // TAP is for a channel the configuration runs, the other modes for a port it doesn't.
        if((mode == CanMonitorMode::TAP) != (bus != nullptr))
            return std::unexpected(CanMonitorError::BAD_MODE);

        if(is_own_bus) {
            own_type = request.type.value_or(CanbusType::CLASSICAL_CAN);
            own_bitrate = request.bitrate.value_or(0);

            can_mode_t capabilities = 0;
            if(own_type == CanbusType::CANFD
                && (can_get_capabilities(controller, &capabilities) != 0 || (capabilities & CAN_MODE_FD) == 0))
                return std::unexpected(CanMonitorError::BAD_MODE);

            // An FD port takes its data bitrate, or uses the nominal one.
            if(own_type == CanbusType::CANFD)
                own_data_bitrate = request.data_bitrate.value_or(0);

            if(!Canbus::IsBitrateSupported(own_type, own_bitrate)
                || (own_type == CanbusType::CANFD && !Canbus::IsBitrateSupported(own_type, own_data_bitrate)))
                return std::unexpected(CanMonitorError::BAD_BITRATE);

            if(own_type == CanbusType::CANFD && own_data_bitrate == 0)
                own_data_bitrate = own_bitrate;
        }
    }

    const RequestKey key{ .port = port,
        .mode = mode,
        .type = request.type,
        .bitrate = request.bitrate,
        .data_bitrate = request.data_bitrate,
        .synthetic = request.synthetic };

    // A client that lost the answer asks again: it gets its session, until a request has used the token.
    if(const OpenSession* existing = Find(port); existing != nullptr) {
        if(!existing->is_token_used && existing->key == key)
            return existing->answer;

        return std::unexpected(CanMonitorError::BUSY);
    }

    if(link_.notifier == nullptr || link_.notifier->GetMaxNotificationSize() == 0)
        return std::unexpected(CanMonitorError::NOT_LISTENING);

    if(sessions_.size() >= CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_SESSIONS)
        return std::unexpected(CanMonitorError::BUSY);

    CanMonitorOpenAnswer answer;
    answer.tok = NewToken();
    answer.ch = port;
    answer.mode = mode;
    answer.lease_ms = CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS;
    if(is_own_bus) {
        answer.type = own_type;
        answer.bitrate = own_bitrate;
        answer.data_bitrate = own_data_bitrate;
    } else if(bus != nullptr) {
        answer.type = bus->GetType();
        ReadBitrates(*bus, answer.bitrate, answer.data_bitrate);
    }

    CanMonitorSessionParameters parameters;
    parameters.token = answer.tok;
    parameters.port = port;
    parameters.mode = mode;
    parameters.type = answer.type;
    parameters.bitrate = answer.bitrate;
    parameters.data_bitrate = answer.data_bitrate;
    parameters.start_time_us = NowUs();
    parameters.is_delta = request.delta;
    if(!is_synthetic)
        parameters.counters = MakeCounters(controller);

    auto session = CanMonitorSession::Create(parameters, sender_.GetWakeSignal(), Mrm::GetExtPmr());
    if(!session)
        return std::unexpected(CanMonitorError::NO_MEMORY);

    session->SetFilters(request.filters);

    OpenSession open;
    open.key = key;
    open.answer = answer;
    open.session = session.get();
    open.proxy = std::move(proxy);
    open.controller = controller;
    open.bus = bus;

    try {
        sender_.Attach(std::move(session));
    } catch(const std::bad_alloc&) {
        return std::unexpected(CanMonitorError::NO_MEMORY);
    }

    // The sender serves the session from here; a failure below hands it back.
    std::unique_ptr<Canbus> own_bus;
    try {
        if(is_synthetic) {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
            open.source = std::make_unique<CanSyntheticSource>(*open.session, *request.synthetic);
            if(!open.source->Start()) {
                sender_.Discard(open.session);
                return std::unexpected(CanMonitorError::NO_MEMORY);
            }
#endif
        } else if(is_own_bus) {
            const can_mode_t modes = mode == CanMonitorMode::LISTEN_ONLY ? CAN_MODE_LISTENONLY : 0;
            own_bus = std::make_unique<Canbus>(
                CanbusConfig(controller, own_type, own_bitrate, own_data_bitrate, modes)
            );

            // Initialize() first: it configures the controller and clears any tap. The tap goes on before
            // Start(), so the first frame, or the detected bitrate, finds it.
            if(!own_bus->Initialize()) {
                sender_.Discard(open.session);
                return std::unexpected(own_bitrate != 0 ? CanMonitorError::BAD_BITRATE : CanMonitorError::UNKNOWN);
            }

            open.session->BindBus(own_bus.get());
            if(!own_bus->SetTap(open.session) || !own_bus->Start()) {
                LOG_ERR("CAN monitor: the bus of port %u would not start.", port);
                own_bus->SetTap(nullptr);
                // Initialize() started the controller, which no Stop() of a bus that never started undoes.
                can_stop(controller);
                sender_.Discard(open.session);
                return std::unexpected(CanMonitorError::UNKNOWN);
            }

            open.bus = own_bus.get();
            open.own_bus = std::move(own_bus);
        } else {
            open.session->BindBus(bus);
            if(!bus->SetTap(open.session)) {
                LOG_ERR("CAN monitor: the bus of port %u refused the tap.", port);
                sender_.Discard(open.session);
                return std::unexpected(CanMonitorError::UNKNOWN);
            }
        }
    } catch(const std::bad_alloc&) {
        if(own_bus) {
            own_bus->SetTap(nullptr);
            can_stop(controller);
        }
        sender_.Discard(open.session);

        return std::unexpected(CanMonitorError::NO_MEMORY);
    }

    open.lease_deadline_ms = k_uptime_get() + CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS;

    if(link_.hold_fast)
        link_.hold_fast();

    sessions_.push_back(std::move(open));
    ScheduleHousekeeping();

    LOG_INF("CAN monitor: session %08x opened on port %u.", answer.tok, port);

    return answer;
}

std::expected<void, CanMonitorError> CanMonitorService::Close(uint32_t token) {
    const ScopedMutex guard(lock_);

    if(token == 0)
        return std::unexpected(CanMonitorError::BAD_TOKEN);

    OpenSession* open = Find(token);
    if(open == nullptr)
        return std::unexpected(CanMonitorError::NO_SESSION);

    EndSession(*open, CanEndReason::CLOSED);
    sessions_.erase(sessions_.begin() + (open - sessions_.data()));

    return {};
}

std::expected<void, CanMonitorError>
CanMonitorService::SetFilters(uint32_t token, std::span<const CanUnitFilter> filters) {
    const ScopedMutex guard(lock_);

    const auto open = Touch(token);
    if(!open)
        return std::unexpected(open.error());

    if(!(*open)->session->SetFilters(filters))
        return std::unexpected(CanMonitorError::UNKNOWN);

    return {};
}

std::expected<CanMonitorStats, CanMonitorError> CanMonitorService::GetStats(uint32_t token) {
    const ScopedMutex guard(lock_);

    const auto open = Touch(token);
    if(!open)
        return std::unexpected(open.error());

    const auto totals = (*open)->session->Totals();

    CanMonitorStats stats;
    stats.frames_received = totals.frames_received;
    stats.frames_sent = totals.frames_sent;
    stats.busy_us = totals.busy_us;
    stats.errors = totals.errors;
    stats.lost = totals.lost;
    stats.filtered = totals.filtered;

    // A synthetic session has no controller: error active, no errors.
    if((*open)->controller != nullptr) {
        can_bus_err_cnt counters{};
        stats.state = static_cast<uint8_t>(ReadState((*open)->controller, counters));
        stats.tec = counters.tx_err_cnt;
        stats.rec = counters.rx_err_cnt;
    }

    return stats;
}

CanMonitorStatus CanMonitorService::GetStatus() const {
    const ScopedMutex guard(lock_);

    CanMonitorStatus status;
    status.filters_max = CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS;
    status.ring_bytes = CONFIG_EERIE_LEAP_CAN_MONITOR_RING_SIZE;
    if(link_.notifier != nullptr)
        status.notify_max = static_cast<uint32_t>(
            std::min(link_.notifier->GetMaxNotificationSize(), CanMonitorSender::MAX_NOTIFICATION_SIZE)
        );
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX
    status.has_tx = true;
#endif
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
    status.has_synthetic = true;
#endif
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    status.has_delta = true;
#endif

    for(uint8_t channel = 0; channel < MAX_PORTS; ++channel) {
        const device* controller = dt_canbus_provider_ ? dt_canbus_provider_(channel) : nullptr;
        if(controller == nullptr)
            continue;

        CanMonitorPortStatus port;
        port.ch = channel;

        can_mode_t capabilities = 0;
        port.is_fd_capable = can_get_capabilities(controller, &capabilities) == 0 && (capabilities & CAN_MODE_FD) != 0;

        can_bus_err_cnt counters{};
        port.state = static_cast<uint8_t>(ReadState(controller, counters));

        const auto proxy = canbus_service_->GetCanbus(channel);
        const Canbus* bus = proxy ? proxy->Get() : nullptr;
        port.is_configured = bus != nullptr;

        const OpenSession* open = FindConst(channel);
        port.has_session = open != nullptr;

        // The channel's bitrates, or those of the session's own bus on a port the configuration leaves alone.
        if(bus == nullptr && open != nullptr && open->own_bus)
            bus = open->own_bus.get();
        if(bus != nullptr)
            ReadBitrates(*bus, port.bitrate, port.data_bitrate);

        status.ports.push_back(port);
    }

    return status;
}

Canbus* CanMonitorService::GetOwnBus(uint32_t token) {
    const ScopedMutex guard(lock_);

    OpenSession* open = Find(token);

    return open != nullptr ? open->own_bus.get() : nullptr;
}

// The producer stops first (the tap removed, the source joined, the own bus stopped), so the sender may
// free the ring once it has drained it.
void CanMonitorService::EndSession(OpenSession& open, CanEndReason reason) {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
    if(open.source)
        open.source->Stop();
#endif

    if(open.own_bus) {
        // Stop() waits for the bus's thread, which may be detecting a bitrate; the bus is freed with the entry.
        open.own_bus->SetTap(nullptr);
        if(!open.own_bus->Stop())
            LOG_ERR("CAN monitor: the bus of port %u would not stop.", open.answer.ch);
    } else if(open.bus != nullptr && open.proxy->Get() == open.bus) {
        // A reconfiguration may have put another bus behind the proxy; its tap is not ours to clear.
        open.bus->SetTap(nullptr);
    }

    if(link_.release_fast)
        link_.release_fast();

    sender_.End(open.session, reason);

    LOG_INF("CAN monitor: session %08x ended (%u).", open.answer.tok, static_cast<unsigned>(reason));
}

void CanMonitorService::EndAll(CanEndReason reason) {
    for(OpenSession& open : sessions_)
        EndSession(open, reason);

    sessions_.clear();
}

// The lease task wakes at the earliest deadline, and while a session owns a bus, to look at it.
void CanMonitorService::ScheduleHousekeeping() {
    if(!lease_task_ || sessions_.empty())
        return;

    const int64_t now = k_uptime_get();
    int64_t wake_ms = sessions_.front().lease_deadline_ms;
    for(const OpenSession& open : sessions_) {
        wake_ms = std::min(wake_ms, open.lease_deadline_ms);
        if(open.own_bus)
            wake_ms = std::min(wake_ms, now + OWN_BUS_CHECK_MS);
    }

    lease_task_->Reschedule(K_MSEC(std::max<int64_t>(wake_ms - now, 1)));
}

void CanMonitorService::EndExpired() {
    const ScopedMutex guard(lock_);

    const int64_t now = k_uptime_get();
    for(auto it = sessions_.begin(); it != sessions_.end();) {
        // A bus of its own that is no longer running has failed under the session.
        const bool is_failed = it->own_bus && it->own_bus->GetState() == ServiceState::STOPPED;

        if(now >= it->lease_deadline_ms || is_failed) {
            EndSession(*it, is_failed ? CanEndReason::ERROR : CanEndReason::LEASE);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }

    ScheduleHousekeeping();
}

void CanMonitorService::EndOnDisconnect() {
    const ScopedMutex guard(lock_);
    EndAll(CanEndReason::DISCONNECTED);
}

void CanMonitorService::OnNotificationSent() {
    sender_.OnNotificationSent();
}

void CanMonitorService::OnDisconnected() {
    // What is in flight never reports; forgetting it can't wait for the sessions to end.
    sender_.OnDisconnected();

    if(disconnect_task_)
        disconnect_task_->Schedule();
}

size_t CanMonitorService::GetSessionCount() const {
    const ScopedMutex guard(lock_);

    return sessions_.size();
}

} // namespace eerie_leap::domain::can_monitor_domain::services
