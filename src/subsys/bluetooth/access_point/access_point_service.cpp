#include <algorithm>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <utility>

#include <zephyr/logging/log.h>

#include "subsys/bluetooth/ble.h"

#include "access_point_service.h"

LOG_MODULE_REGISTER(access_point_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::bluetooth::access_point {

#define BT_UUID_ACCESS_POINT_SERVICE BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_SERVICE_VAL)
#define BT_UUID_ACCESS_POINT_INFO BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_ENCODE(1))
#define BT_UUID_ACCESS_POINT_SMP BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_ENCODE(2))
#define BT_UUID_ACCESS_POINT_LIVE BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_ENCODE(3))
#define BT_UUID_ACCESS_POINT_BUS BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_ENCODE(4))

namespace {

// 7.5-15 ms while SMP traffic flows or a hold is held, 30-50 ms otherwise, all with a 4 s supervision timeout.
constexpr bt_le_conn_param FAST_PARAMETERS = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400);
constexpr bt_le_conn_param IDLE_PARAMETERS = BT_LE_CONN_PARAM_INIT(24, 40, 0, 400);
// For a central that left the fast request above 15 ms (R-7 of the CAN monitor plan): 15-30 ms.
constexpr bt_le_conn_param FALLBACK_PARAMETERS = BT_LE_CONN_PARAM_INIT(12, 24, 0, 400);
static_assert(FALLBACK_PARAMETERS.interval_min == FastLinkPolicy::FALLBACK_THRESHOLD_UNITS);

// The service, then declaration, value and CCC of `smp`, of `live`, then declaration and value of
// `info`, then declaration, value and CCC of `bus`. New characteristics go last: the order fixes the
// handles a bonded central has cached.
constexpr size_t SMP_VALUE_INDEX = 2;
constexpr size_t LIVE_VALUE_INDEX = 5;
constexpr size_t BUS_VALUE_INDEX = 10;

// Unlocks a k_mutex when it leaves scope.
class MutexGuard {
private:
    k_mutex& mutex_;

public:
    explicit MutexGuard(k_mutex& mutex) :
        mutex_(mutex) {
        k_mutex_lock(&mutex_, K_FOREVER);
    }
    ~MutexGuard() {
        k_mutex_unlock(&mutex_);
    }

    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
};

} // namespace

std::atomic<AccessPointService*> AccessPointService::instance = nullptr;

const bt_gatt_attr AccessPointService::ATTRIBUTES[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_ACCESS_POINT_SERVICE),

    // Requests change settings, so writing needs an encrypted link.
    BT_GATT_CHARACTERISTIC(
        BT_UUID_ACCESS_POINT_SMP,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_WRITE_ENCRYPT,
        nullptr,
        &SmpWriteCallback,
        nullptr
    ),
    // Centrals subscribe while connecting, possibly before pairing is done, so subscribing stays open.
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_ACCESS_POINT_LIVE,
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        nullptr,
        nullptr,
        nullptr
    ),
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // Readable without pairing, so a client can check the protocol version first.
    BT_GATT_CHARACTERISTIC(
        BT_UUID_ACCESS_POINT_INFO,
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        &InfoReadCallback,
        nullptr,
        nullptr
    ),

    // Appended after `info` so that no existing handle moves; a bonded central learns of it through
    // Service Changed. Subscribing is open like `live`'s.
    BT_GATT_CHARACTERISTIC(BT_UUID_ACCESS_POINT_BUS, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};

const STRUCT_SECTION_ITERABLE(bt_gatt_service_static, AccessPointService::gatt_service_) = BT_GATT_SERVICE(ATTRIBUTES);

AccessPointService::AccessPointService() :
    smp_notifier_(std::make_shared<Notifier>(&ATTRIBUTES[SMP_VALUE_INDEX], SmpSentCallback)),
    live_notifier_(std::make_shared<Notifier>(&ATTRIBUTES[LIVE_VALUE_INDEX], LiveSentCallback)),
    bus_notifier_(std::make_shared<Notifier>(&ATTRIBUTES[BUS_VALUE_INDEX], BusSentCallback)) {

    AccessPointService* expected = nullptr;
    if(!instance.compare_exchange_strong(expected, this))
        throw std::logic_error("Only one AccessPointService may exist");

    k_mutex_init(&bus_sent_mutex_);
    k_work_init(&fast_work_, FastWorkHandler);
    k_work_init(&fallback_work_, FallbackWorkHandler);
    k_work_init_delayable(&idle_work_, IdleWorkHandler);
}

AccessPointService::~AccessPointService() {
    if(connected_handler_id_ >= 0)
        Ble::UnregisterConnectedHandler(connected_handler_id_);
    if(disconnected_handler_id_ >= 0)
        Ble::UnregisterDisconnectedHandler(disconnected_handler_id_);
    if(parameters_updated_handler_id_ >= 0)
        Ble::UnregisterParametersUpdatedHandler(parameters_updated_handler_id_);

    instance.store(nullptr);

    k_work_sync sync{};
    k_work_cancel_sync(&fast_work_, &sync);
    k_work_cancel_sync(&fallback_work_, &sync);
    k_work_cancel_delayable_sync(&idle_work_, &sync);
}

void AccessPointService::Initialize(Callbacks callbacks, std::span<const uint8_t> info) {
    {
        const MutexGuard guard(bus_sent_mutex_);
        callbacks_ = std::move(callbacks);
    }
    info_size_ = std::min(info.size(), info_.size());
    std::copy_n(info.begin(), info_size_, info_.begin());

    // A new connection gets the long interval unless SMP traffic starts first.
    // A hold that outlives a connection makes the next one fast at once.
    connected_handler_id_ = Ble::RegisterConnectedHandler([this](bt_conn* /*conn*/) {
        const k_spinlock_key_t key = k_spin_lock(&policy_lock_);
        ApplyLocked(policy_.OnConnected());
        k_spin_unlock(&policy_lock_, key);
    });
    disconnected_handler_id_ = Ble::RegisterDisconnectedHandler([this](bt_conn* /*conn*/) {
        const k_spinlock_key_t key = k_spin_lock(&policy_lock_);
        ApplyLocked(policy_.OnDisconnected());
        k_spin_unlock(&policy_lock_, key);
    });
    parameters_updated_handler_id_ = Ble::RegisterParametersUpdatedHandler(
        [this](bt_conn* /*conn*/, uint16_t interval, uint16_t /*latency*/, uint16_t /*timeout*/) {
            OnParametersUpdated(interval);
        }
    );
}

AccessPointService::Notifier::Notifier(const bt_gatt_attr* attribute, bt_gatt_complete_func_t on_sent) :
    attribute_(attribute),
    on_sent_(on_sent) {}

size_t AccessPointService::Notifier::GetMaxNotificationSize() const {
    bt_conn* conn = Ble::AcquireActiveConn();
    if(conn == nullptr)
        return 0;

    const uint16_t mtu = bt_gatt_get_mtu(conn);
    const bool is_subscribed = bt_gatt_is_subscribed(conn, attribute_, BT_GATT_CCC_NOTIFY);
    bt_conn_unref(conn);

    // 3 bytes of ATT header.
    return is_subscribed && mtu > 3 ? mtu - 3 : 0;
}

int AccessPointService::Notifier::Notify(std::span<const uint8_t> data) {
    bt_conn* conn = Ble::AcquireActiveConn();
    if(conn == nullptr)
        return -ENOTCONN;

    bt_gatt_notify_params params{};
    params.attr = attribute_;
    params.data = data.data();
    params.len = static_cast<uint16_t>(data.size());
    params.func = on_sent_;

    const int result = bt_gatt_notify_cb(conn, &params);
    bt_conn_unref(conn);

    return result;
}

ssize_t AccessPointService::SmpWriteCallback(
    bt_conn* /*conn*/,
    const bt_gatt_attr* /*attr*/,
    const void* buf,
    uint16_t len,
    uint16_t offset,
    uint8_t /*flags*/
) {

    if(offset != 0)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);

    AccessPointService* service = instance.load();
    if(service != nullptr && service->callbacks_.on_smp_write) {
        service->callbacks_.on_smp_write({ static_cast<const uint8_t*>(buf), len });
        service->OnSmpActivity();
    }

    return len;
}

ssize_t AccessPointService::InfoReadCallback(
    bt_conn* conn,
    const bt_gatt_attr* attr,
    void* buf,
    uint16_t len,
    uint16_t offset
) {

    AccessPointService* service = instance.load();
    if(service == nullptr)
        return bt_gatt_attr_read(conn, attr, buf, len, offset, nullptr, 0);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, service->info_.data(), service->info_size_);
}

void AccessPointService::SetBusSentHandler(std::function<void()> handler) {
    // Waits for a running call: BusSentCallback() holds the mutex while the handler runs. The old
    // handler is destroyed here, after the swap.
    const MutexGuard guard(bus_sent_mutex_);
    callbacks_.on_bus_sent = std::move(handler);
}

void AccessPointService::SmpSentCallback(bt_conn* /*conn*/, void* /*user_data*/) {
    const AccessPointService* service = instance.load();
    if(service != nullptr && service->callbacks_.on_smp_sent)
        service->callbacks_.on_smp_sent();
}

void AccessPointService::LiveSentCallback(bt_conn* /*conn*/, void* /*user_data*/) {
    const AccessPointService* service = instance.load();
    if(service != nullptr && service->callbacks_.on_live_sent)
        service->callbacks_.on_live_sent();
}

void AccessPointService::BusSentCallback(bt_conn* /*conn*/, void* /*user_data*/) {
    AccessPointService* service = instance.load();
    if(service == nullptr)
        return;

    // Recursive, so the handler may replace itself.
    const MutexGuard guard(service->bus_sent_mutex_);
    if(!service->callbacks_.on_bus_sent)
        return;

    try {
        service->callbacks_.on_bus_sent();
    } catch(const std::exception& e) {
        LOG_ERR("The bus sent handler threw: %s", e.what());
    } catch(...) {
        LOG_ERR("The bus sent handler threw an unknown exception");
    }
}

void AccessPointService::HoldFast() {
    const k_spinlock_key_t key = k_spin_lock(&policy_lock_);
    ApplyLocked(policy_.OnHold());
    k_spin_unlock(&policy_lock_, key);
}

void AccessPointService::ReleaseFast() {
    const k_spinlock_key_t key = k_spin_lock(&policy_lock_);
    const bool is_held = policy_.GetHoldCount() > 0;
    ApplyLocked(policy_.OnRelease());
    k_spin_unlock(&policy_lock_, key);

    if(!is_held)
        LOG_WRN("ReleaseFast() without a hold");
}

void AccessPointService::OnSmpActivity() {
    const k_spinlock_key_t key = k_spin_lock(&policy_lock_);
    ApplyLocked(policy_.OnSmpActivity());
    k_spin_unlock(&policy_lock_, key);
}

void AccessPointService::OnParametersUpdated(uint16_t interval) {
    const k_spinlock_key_t key = k_spin_lock(&policy_lock_);
    const FastLinkPolicy::Decision decision = policy_.OnParametersUpdated(interval);
    ApplyLocked(decision);
    k_spin_unlock(&policy_lock_, key);

    if(decision.request_fallback)
        LOG_INF("The fast request left an interval of %u ms", static_cast<unsigned>(interval * 5 / 4));
}

void AccessPointService::ApplyLocked(const FastLinkPolicy::Decision& decision) {
    switch(decision.idle_work) {
        case FastLinkPolicy::IdleWork::SCHEDULE:
            k_work_reschedule(&idle_work_, K_MSEC(IDLE_TIMEOUT_MS));
            break;
        case FastLinkPolicy::IdleWork::CANCEL:
            k_work_cancel_delayable(&idle_work_);
            break;
        case FastLinkPolicy::IdleWork::KEEP:
            break;
    }

    // The idle request is made by the idle work itself.
    if(decision.request == FastLinkPolicy::LinkRequest::FAST)
        k_work_submit(&fast_work_);
    if(decision.request_fallback)
        k_work_submit(&fallback_work_);
}

void AccessPointService::FastWorkHandler(k_work* /*work*/) {
    AccessPointService* service = instance.load();
    if(service != nullptr)
        service->RequestFast();
}

void AccessPointService::FallbackWorkHandler(k_work* /*work*/) {
    if(instance.load() != nullptr)
        RequestFallback();
}

void AccessPointService::IdleWorkHandler(k_work* /*work*/) {
    AccessPointService* service = instance.load();
    if(service != nullptr)
        service->RequestIdle();
}

void AccessPointService::RequestFast() {
    bt_conn* conn = Ble::AcquireActiveConn();
    if(conn == nullptr)
        return;

    // Armed before the request, because the update may answer it before this returns.
    k_spinlock_key_t key = k_spin_lock(&policy_lock_);
    policy_.OnFastRequested();
    k_spin_unlock(&policy_lock_, key);

    int err = bt_conn_le_param_update(conn, &FAST_PARAMETERS);
    if(err != 0) {
        key = k_spin_lock(&policy_lock_);
        policy_.OnFastRequestFailed();
        k_spin_unlock(&policy_lock_, key);

        if(err != -EALREADY)
            LOG_WRN("Connection parameter update failed (err %d)", err);
    }

#ifdef CONFIG_BT_USER_PHY_UPDATE
    const bt_conn_le_phy_param phy = BT_CONN_LE_PHY_PARAM_INIT(BT_GAP_LE_PHY_2M, BT_GAP_LE_PHY_2M);
    err = bt_conn_le_phy_update(conn, &phy);
    if(err != 0 && err != -EALREADY)
        LOG_WRN("PHY update failed (err %d)", err);
#endif

    bt_conn_unref(conn);
}

void AccessPointService::RequestFallback() {
    bt_conn* conn = Ble::AcquireActiveConn();
    if(conn == nullptr)
        return;

    const int err = bt_conn_le_param_update(conn, &FALLBACK_PARAMETERS);
    if(err != 0 && err != -EALREADY)
        LOG_WRN("Fallback connection parameter update failed (err %d)", err);

    bt_conn_unref(conn);
}

void AccessPointService::RequestIdle() {
    // Decided under the lock, so a hold taken meanwhile is not overridden.
    const k_spinlock_key_t key = k_spin_lock(&policy_lock_);
    const FastLinkPolicy::Decision decision = policy_.OnIdleTimeout();
    k_spin_unlock(&policy_lock_, key);

    if(decision.request != FastLinkPolicy::LinkRequest::IDLE)
        return;

    bt_conn* conn = Ble::AcquireActiveConn();
    if(conn == nullptr)
        return;

    const int err = bt_conn_le_param_update(conn, &IDLE_PARAMETERS);
    if(err != 0 && err != -EALREADY)
        LOG_WRN("Connection parameter update failed (err %d)", err);

    bt_conn_unref(conn);
}

} // namespace eerie_leap::subsys::bluetooth::access_point
