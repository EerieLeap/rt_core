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

// What OPEN asks for, checked against the ports and the stored configuration. Synthetic, TAP on a channel
// the configuration runs, or LISTEN_ONLY and ACTIVE on a port it doesn't.
std::optional<CanMonitorError> CanMonitorService::PlanOwnBus(const CanMonitorOpenRequest& request, OpenPlan& plan) {
    plan.own_type = request.type.value_or(CanbusType::CLASSICAL_CAN);
    plan.own_bitrate = request.bitrate.value_or(0);

    if(plan.own_type == CanbusType::CANFD) {
        can_mode_t capabilities = 0;
        if(can_get_capabilities(plan.controller, &capabilities) != 0 || (capabilities & CAN_MODE_FD) == 0)
            return CanMonitorError::BAD_MODE;

        // An FD port takes its data bitrate, or uses the nominal one.
        plan.own_data_bitrate = request.data_bitrate.value_or(0);
    }

    if(!Canbus::IsBitrateSupported(plan.own_type, plan.own_bitrate)
        || (plan.own_type == CanbusType::CANFD && !Canbus::IsBitrateSupported(plan.own_type, plan.own_data_bitrate)))
        return CanMonitorError::BAD_BITRATE;

    if(plan.own_type == CanbusType::CANFD && plan.own_data_bitrate == 0)
        plan.own_data_bitrate = plan.own_bitrate;

    return std::nullopt;
}

std::optional<CanMonitorError> CanMonitorService::PlanPort(const CanMonitorOpenRequest& request, OpenPlan& plan) const {
    plan.controller = dt_canbus_provider_ ? dt_canbus_provider_(plan.port) : nullptr;
    if(plan.controller == nullptr)
        return CanMonitorError::NOT_FOUND;

    if(static_cast<uint32_t>(plan.mode) >= CAN_MONITOR_MODE_COUNT)
        return CanMonitorError::BAD_MODE;

    plan.proxy = canbus_service_->GetCanbus(plan.port);
    plan.bus = plan.proxy ? plan.proxy->Get() : nullptr;

    // TAP is for a channel the configuration runs, the other modes for a port it doesn't.
    if((plan.mode == CanMonitorMode::TAP) != (plan.bus != nullptr))
        return CanMonitorError::BAD_MODE;

    plan.is_own_bus = plan.mode != CanMonitorMode::TAP;

    return plan.is_own_bus ? PlanOwnBus(request, plan) : std::nullopt;
}

std::expected<CanMonitorService::OpenPlan, CanMonitorError>
CanMonitorService::PlanOpen(const CanMonitorOpenRequest& request) const {
    OpenPlan plan;
    plan.is_synthetic = request.synthetic.has_value();

#ifndef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
    if(plan.is_synthetic)
        return std::unexpected(CanMonitorError::BAD_MODE);
#endif

    if(request.filters.size() > CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS
        || (plan.is_synthetic && !request.synthetic->IsValid()))
        return std::unexpected(CanMonitorError::UNKNOWN);

    plan.port = plan.is_synthetic ? CAN_MONITOR_SYNTHETIC_PORT : request.ch;
    plan.mode = plan.is_synthetic ? CanMonitorMode::TAP : request.mode;

    if(!plan.is_synthetic) {
        if(const auto error = PlanPort(request, plan); error.has_value())
            return std::unexpected(*error);
    }

    return plan;
}

// A client that lost the answer asks again: it gets its session, until a request has used the token.
// Otherwise the link, the port and the slots decide. Nothing when the session may be opened.
std::optional<std::expected<CanMonitorOpenAnswer, CanMonitorError>>
CanMonitorService::Admit(const OpenPlan& plan, const RequestKey& key) const {
    if(const OpenSession* existing = FindConst(plan.port); existing != nullptr) {
        if(!existing->is_token_used && existing->key == key)
            return existing->answer;

        return std::unexpected(CanMonitorError::BUSY);
    }

    if(link_.notifier == nullptr || link_.notifier->GetMaxNotificationSize() == 0)
        return std::unexpected(CanMonitorError::NOT_LISTENING);

    if(sessions_.size() >= CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_SESSIONS)
        return std::unexpected(CanMonitorError::BUSY);

    return std::nullopt;
}

CanMonitorOpenAnswer CanMonitorService::MakeAnswer(const OpenPlan& plan) const {
    CanMonitorOpenAnswer answer;
    answer.tok = NewToken();
    answer.ch = plan.port;
    answer.mode = plan.mode;
    answer.lease_ms = CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS;

    if(plan.is_own_bus) {
        answer.type = plan.own_type;
        answer.bitrate = plan.own_bitrate;
        answer.data_bitrate = plan.own_data_bitrate;
    } else if(plan.bus != nullptr) {
        answer.type = plan.bus->GetType();
        ReadBitrates(*plan.bus, answer.bitrate, answer.data_bitrate);
    }

    return answer;
}

// The session and its place in the sender, which serves it from here on; whatever fails after this hands it back.
std::expected<CanMonitorService::OpenSession, CanMonitorError> CanMonitorService::CreateSession(
    const CanMonitorOpenRequest& request,
    const OpenPlan& plan,
    const CanMonitorOpenAnswer& answer,
    const RequestKey& key
) {
    CanMonitorSessionParameters parameters;
    parameters.token = answer.tok;
    parameters.port = plan.port;
    parameters.mode = plan.mode;
    parameters.type = answer.type;
    parameters.bitrate = answer.bitrate;
    parameters.data_bitrate = answer.data_bitrate;
    parameters.start_time_us = NowUs();
    parameters.is_delta = request.delta;
    if(!plan.is_synthetic)
        parameters.counters = MakeCounters(plan.controller);

    auto session = CanMonitorSession::Create(parameters, sender_.GetWakeSignal(), Mrm::GetExtPmr());
    if(!session)
        return std::unexpected(CanMonitorError::NO_MEMORY);

    session->SetFilters(request.filters);

    OpenSession open;
    open.key = key;
    open.answer = answer;
    open.session = session.get();
    open.proxy = plan.proxy;
    open.controller = plan.controller;
    open.bus = plan.bus;
    open.lease_deadline_ms = k_uptime_get() + CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS;

    try {
        sender_.Attach(std::move(session));
    } catch(const std::bad_alloc&) {
        return std::unexpected(CanMonitorError::NO_MEMORY);
    }

    return open;
}

void CanMonitorService::AttachTransmitter([[maybe_unused]] OpenSession& open, [[maybe_unused]] Canbus& bus) {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX
    CanMonitorSession* session = open.session;

    open.transmitter = std::make_unique<transmit::CanSessionTransmitter>(
        bus, open.answer.ch, work_queue_->GetWorkQueue(), [this, session](CanTxEvent event, uint64_t count) {
            sender_.QueueTxEvent(session, event, count);
        }
    );
    session->SetStateListener(open.transmitter.get());
#endif
}

std::optional<CanMonitorError>
CanMonitorService::StartSynthetic([[maybe_unused]] OpenSession& open, const CanMonitorOpenRequest& request) {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
    if(!request.synthetic.has_value())
        return CanMonitorError::UNKNOWN;

    open.source = std::make_unique<CanSyntheticSource>(*open.session, *request.synthetic);
    if(!open.source->Start()) {
        sender_.Discard(open.session);
        return CanMonitorError::NO_MEMORY;
    }
#else
    (void)request;
#endif

    return std::nullopt;
}

std::optional<CanMonitorError> CanMonitorService::StartTap(OpenSession& open) {
    AttachTransmitter(open, *open.bus);

    open.session->BindBus(open.bus);
    if(!open.bus->SetTap(open.session)) {
        LOG_ERR("CAN monitor: the bus of port %u refused the tap.", open.answer.ch);
        sender_.Discard(open.session);
        return CanMonitorError::UNKNOWN;
    }

    return std::nullopt;
}

// Initialize() first: it configures the controller and clears any tap. The tap goes on before Start(), so
// the first frame, or the detected bitrate, finds it.
std::optional<CanMonitorError> CanMonitorService::StartOwnBus(OpenSession& open, const OpenPlan& plan) {
    const can_mode_t modes = plan.mode == CanMonitorMode::LISTEN_ONLY ? CAN_MODE_LISTENONLY : 0;
    auto own_bus = std::make_unique<Canbus>(
        CanbusConfig(plan.controller, plan.own_type, plan.own_bitrate, plan.own_data_bitrate, modes)
    );

    if(!own_bus->Initialize()) {
        sender_.Discard(open.session);
        return plan.own_bitrate != 0 ? CanMonitorError::BAD_BITRATE : CanMonitorError::UNKNOWN;
    }

    // Past this the controller is started, which a Stop() of a bus that never started does not undo.
    const auto abandon = [&] {
        own_bus->SetTap(nullptr);
        can_stop(plan.controller);
        sender_.Discard(open.session);
    };

    try {
        // A listen-only session never transmits, so it gets no transmitter.
        if(plan.mode == CanMonitorMode::ACTIVE)
            AttachTransmitter(open, *own_bus);

        open.session->BindBus(own_bus.get());

        if(!own_bus->SetTap(open.session) || !own_bus->Start()) {
            LOG_ERR("CAN monitor: the bus of port %u would not start.", open.answer.ch);
            abandon();
            return CanMonitorError::UNKNOWN;
        }
    } catch(const std::bad_alloc&) {
        abandon();
        return CanMonitorError::NO_MEMORY;
    }

    open.bus = own_bus.get();
    open.own_bus = std::move(own_bus);

    return std::nullopt;
}

std::optional<CanMonitorError>
CanMonitorService::StartProducer(OpenSession& open, const OpenPlan& plan, const CanMonitorOpenRequest& request) {
    try {
        if(plan.is_synthetic)
            return StartSynthetic(open, request);

        return plan.is_own_bus ? StartOwnBus(open, plan) : StartTap(open);
    } catch(const std::bad_alloc&) {
        sender_.Discard(open.session);

        return CanMonitorError::NO_MEMORY;
    }
}

std::expected<CanMonitorOpenAnswer, CanMonitorError> CanMonitorService::Open(const CanMonitorOpenRequest& request) {
    const ScopedMutex guard(lock_);

    if(!IsRunning() || !sender_.IsRunning())
        return std::unexpected(CanMonitorError::BUSY);

    const auto plan = PlanOpen(request);
    if(!plan)
        return std::unexpected(plan.error());

    const RequestKey key{ .port = plan->port,
        .mode = plan->mode,
        .type = request.type,
        .bitrate = request.bitrate,
        .data_bitrate = request.data_bitrate,
        .synthetic = request.synthetic };

    if(const auto refusal = Admit(*plan, key); refusal.has_value())
        return *refusal;

    const CanMonitorOpenAnswer answer = MakeAnswer(*plan);

    auto open = CreateSession(request, *plan, answer, key);
    if(!open)
        return std::unexpected(open.error());

    if(const auto error = StartProducer(*open, *plan, request); error.has_value())
        return std::unexpected(*error);

    if(link_.hold_fast)
        link_.hold_fast();

    sessions_.push_back(std::move(*open));
    ScheduleHousekeeping();

    LOG_INF("CAN monitor: session %08x opened on port %u.", answer.tok, plan->port);

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

    const OpenSession* open = Find(token);

    return open != nullptr ? open->own_bus.get() : nullptr;
}

// The producer stops first (the source joined, the tap removed), then what the session transmits, then the
// bus it owns, so the sender may free the ring once it has drained it. The transmitter's last tx event is
// queued before the end record.
void CanMonitorService::EndSession(OpenSession& open, CanEndReason reason) {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
    if(open.source)
        open.source->Stop();
#endif

    if(open.own_bus) {
        open.own_bus->SetTap(nullptr);
    } else if(open.bus != nullptr && open.proxy->Get() == open.bus) {
        // A reconfiguration may have put another bus behind the proxy; its tap is not ours to clear.
        open.bus->SetTap(nullptr);
    }

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX
    if(open.transmitter)
        open.transmitter->Shutdown();
#endif

    // Stop() waits for the bus's thread, which may be detecting a bitrate; the bus is freed with the entry.
    if(open.own_bus && !open.own_bus->Stop())
        LOG_ERR("CAN monitor: the bus of port %u would not stop.", open.answer.ch);

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

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX

std::expected<transmit::CanSessionTransmitter*, CanMonitorError> CanMonitorService::Transmitter(uint32_t token) {
    const auto open = Touch(token);
    if(!open)
        return std::unexpected(open.error());

    // A session that listens only, or has no bus, cannot transmit.
    if(!(*open)->transmitter)
        return std::unexpected(CanMonitorError::BAD_MODE);

    return (*open)->transmitter.get();
}

std::expected<CanMonitorArmState, CanMonitorError> CanMonitorService::Arm(uint32_t token, bool on) {
    const ScopedMutex guard(lock_);

    const auto transmitter = Transmitter(token);
    if(!transmitter)
        return std::unexpected(transmitter.error());

    return (*transmitter)->Arm(on, canbus_service_->GetComConfiguration());
}

std::expected<CanMonitorSendResult, CanMonitorError>
CanMonitorService::Send(uint32_t token, std::span<const CanMonitorTxFrame> frames) {
    const ScopedMutex guard(lock_);

    const auto transmitter = Transmitter(token);
    if(!transmitter)
        return std::unexpected(transmitter.error());

    return (*transmitter)->Send(frames);
}

std::expected<void, CanMonitorError>
CanMonitorService::SetPeriodic(uint32_t token, std::span<const CanMonitorPeriodic> list) {
    const ScopedMutex guard(lock_);

    const auto transmitter = Transmitter(token);
    if(!transmitter)
        return std::unexpected(transmitter.error());

    return (*transmitter)->SetPeriodic(list);
}

std::expected<uint32_t, CanMonitorError>
CanMonitorService::Replay(uint32_t token, const CanMonitorReplayRequest& request) {
    const ScopedMutex guard(lock_);

    const auto transmitter = Transmitter(token);
    if(!transmitter)
        return std::unexpected(transmitter.error());

    return (*transmitter)->Replay(request);
}

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_TX

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
