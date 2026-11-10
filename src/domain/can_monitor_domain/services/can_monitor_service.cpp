#include "can_monitor_service.h"

#include <algorithm>
#include <new>
#include <utility>

#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "subsys/canbus/canbus_time.h"
#include "subsys/threading/scoped_mutex.h"
#include "utilities/memory/memory_resource_manager.h"

namespace eerie_leap::domain::can_monitor_domain::services {

LOG_MODULE_REGISTER(can_monitor_logger, CONFIG_EERIE_LEAP_CAN_MONITOR_LOG_LEVEL);

using eerie_leap::subsys::canbus::CanbusConfig;
using eerie_leap::subsys::canbus::NowUs;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;

using models::CAN_MONITOR_MODE_COUNT;
using models::CAN_MONITOR_SYNTHETIC_PORT;

CanMonitorService::CanMonitorService(
    std::shared_ptr<CanbusService> canbus_service,
    DtCanbusProvider dt_canbus_provider,
    CanMonitorLink link
) :
    canbus_service_(std::move(canbus_service)),
    dt_canbus_provider_(std::move(dt_canbus_provider)),
    link_(link),
    sender_(std::move(link)) {

    k_mutex_init(&lock_);

    // Opening a session then can't fail on the list.
    sessions_.reserve(CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_SESSIONS);
}

CanMonitorService::~CanMonitorService() {
    {
        const ScopedMutex guard(lock_);
        EndAll(CanEndReason::CONFIGURATION_STORED);
    }

    // The sender goes with the members; sessions still draining are freed with it.
}

bool CanMonitorService::DoInitialize() {
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
    if(!is_synthetic) {
        if(!dt_canbus_provider_ || dt_canbus_provider_(port) == nullptr)
            return std::unexpected(CanMonitorError::NOT_FOUND);

        // LISTEN_ONLY and ACTIVE build a bus of their own on a port the configuration doesn't use: Phase 3.
        if(mode != CanMonitorMode::TAP)
            return std::unexpected(CanMonitorError::BAD_MODE);

        proxy = canbus_service_->GetCanbus(port);
        bus = proxy ? proxy->Get() : nullptr;
        if(bus == nullptr)
            return std::unexpected(CanMonitorError::BAD_MODE);
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
    if(bus != nullptr) {
        const CanbusConfig config = bus->GetConfig();
        answer.type = config.type;
        answer.bitrate = bus->IsBitrateDetected() ? config.bitrate : 0;
        answer.data_bitrate = config.type == CanbusType::CANFD ? config.data_bitrate : 0;
    }

    auto session = CanMonitorSession::Create(
        answer.tok, port, mode, answer.type, answer.bitrate, answer.data_bitrate, NowUs(), sender_.GetWakeSignal(),
        Mrm::GetExtPmr()
    );
    if(!session)
        return std::unexpected(CanMonitorError::NO_MEMORY);

    session->SetFilters(request.filters);

    OpenSession open;
    open.key = key;
    open.answer = answer;
    open.session = session.get();
    open.proxy = std::move(proxy);
    open.bus = bus;

    try {
        sender_.Attach(std::move(session));
    } catch(const std::bad_alloc&) {
        return std::unexpected(CanMonitorError::NO_MEMORY);
    }

    // The sender serves the session from here; a failure below hands it back.
    try {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
        if(is_synthetic) {
            open.source = std::make_unique<CanSyntheticSource>(*open.session, *request.synthetic);
            if(!open.source->Start()) {
                sender_.Discard(open.session);
                return std::unexpected(CanMonitorError::NO_MEMORY);
            }
        }
#endif

        if(!is_synthetic) {
            open.session->BindBus(bus);
            if(!bus->SetTap(open.session)) {
                LOG_ERR("CAN monitor: the bus of port %u refused the tap.", port);
                sender_.Discard(open.session);
                return std::unexpected(CanMonitorError::UNKNOWN);
            }
        }
    } catch(const std::bad_alloc&) {
        sender_.Discard(open.session);
        return std::unexpected(CanMonitorError::NO_MEMORY);
    }

    if(link_.hold_fast)
        link_.hold_fast();

    sessions_.push_back(std::move(open));

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

// The producer stops first (the tap removed, the source joined), so the sender may free the ring once
// it has drained it.
void CanMonitorService::EndSession(OpenSession& open, CanEndReason reason) {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
    if(open.source)
        open.source->Stop();
#endif

    // A reconfiguration may have put another bus behind the proxy; its tap is not ours to clear.
    if(open.bus != nullptr && open.proxy->Get() == open.bus)
        open.bus->SetTap(nullptr);

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

void CanMonitorService::OnNotificationSent() {
    sender_.OnNotificationSent();
}

void CanMonitorService::OnDisconnected() {
    {
        const ScopedMutex guard(lock_);
        EndAll(CanEndReason::DISCONNECTED);
    }

    sender_.OnDisconnected();
}

size_t CanMonitorService::GetSessionCount() const {
    const ScopedMutex guard(lock_);

    return sessions_.size();
}

} // namespace eerie_leap::domain::can_monitor_domain::services
