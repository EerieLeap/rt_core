#include <algorithm>
#include <cerrno>
#include <stdexcept>

#include <zephyr/logging/log.h>

#include "subsys/bluetooth/ble.h"

#include "access_point_service.h"

LOG_MODULE_REGISTER(access_point_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::bluetooth::access_point {

#define BT_UUID_ACCESS_POINT_SERVICE BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_SERVICE_VAL)
#define BT_UUID_ACCESS_POINT_INFO BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_ENCODE(1))
#define BT_UUID_ACCESS_POINT_SMP BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_ENCODE(2))
#define BT_UUID_ACCESS_POINT_LIVE BT_UUID_DECLARE_128(BT_UUID_ACCESS_POINT_ENCODE(3))

namespace {

// 7.5-15 ms while SMP traffic flows, 30-50 ms otherwise, both with a 4 s supervision timeout.
constexpr bt_le_conn_param FAST_PARAMETERS = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400);
constexpr bt_le_conn_param IDLE_PARAMETERS = BT_LE_CONN_PARAM_INIT(24, 40, 0, 400);

// The service, then declaration, value and CCC of `smp`, then those of `live`.
constexpr size_t SMP_VALUE_INDEX = 2;
constexpr size_t LIVE_VALUE_INDEX = 5;

} // namespace

std::atomic<AccessPointService*> AccessPointService::instance_ = nullptr;

const bt_gatt_attr AccessPointService::attributes_[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_ACCESS_POINT_SERVICE),

    // Requests change settings, so writing needs an encrypted link.
    BT_GATT_CHARACTERISTIC(
        BT_UUID_ACCESS_POINT_SMP,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_WRITE_ENCRYPT,
        nullptr, &SmpWriteCallback, nullptr),
    // Centrals subscribe while connecting, possibly before pairing is done, so subscribing stays open.
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_ACCESS_POINT_LIVE,
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        nullptr, nullptr, nullptr),
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // Readable without pairing, so a client can check the protocol version first.
    BT_GATT_CHARACTERISTIC(
        BT_UUID_ACCESS_POINT_INFO,
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        &InfoReadCallback, nullptr, nullptr),
};

const STRUCT_SECTION_ITERABLE(bt_gatt_service_static, AccessPointService::gatt_service_) =
    BT_GATT_SERVICE(attributes_);

AccessPointService::AccessPointService()
    : smp_notifier_(std::make_shared<Notifier>(&attributes_[SMP_VALUE_INDEX], SmpSentCallback)),
    live_notifier_(std::make_shared<Notifier>(&attributes_[LIVE_VALUE_INDEX], LiveSentCallback)) {

    AccessPointService* expected = nullptr;
    if(!instance_.compare_exchange_strong(expected, this))
        throw std::logic_error("Only one AccessPointService may exist");

    k_work_init(&fast_work_, FastWorkHandler);
    k_work_init_delayable(&idle_work_, IdleWorkHandler);
}

AccessPointService::~AccessPointService() {
    if(connected_handler_id_ >= 0)
        Ble::UnregisterConnectedHandler(connected_handler_id_);
    if(disconnected_handler_id_ >= 0)
        Ble::UnregisterDisconnectedHandler(disconnected_handler_id_);

    instance_.store(nullptr);

    k_work_sync sync;
    k_work_cancel_sync(&fast_work_, &sync);
    k_work_cancel_delayable_sync(&idle_work_, &sync);
}

void AccessPointService::Initialize(Callbacks callbacks, std::span<const uint8_t> info) {
    callbacks_ = std::move(callbacks);
    info_size_ = std::min(info.size(), info_.size());
    std::copy_n(info.begin(), info_size_, info_.begin());

    // A new connection gets the long interval unless SMP traffic starts first.
    connected_handler_id_ = Ble::RegisterConnectedHandler([this](bt_conn* /*conn*/) {
        k_work_reschedule(&idle_work_, K_MSEC(IDLE_TIMEOUT_MS));
    });
    disconnected_handler_id_ = Ble::RegisterDisconnectedHandler([this](bt_conn* /*conn*/) {
        is_fast_ = false;
        k_work_cancel_delayable(&idle_work_);
    });
}

AccessPointService::Notifier::Notifier(const bt_gatt_attr* attribute, bt_gatt_complete_func_t on_sent)
    : attribute_(attribute), on_sent_(on_sent) {}

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
    uint8_t /*flags*/) {

    if(offset != 0)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);

    AccessPointService* service = instance_.load();
    if(service != nullptr && service->callbacks_.on_smp_write) {
        service->callbacks_.on_smp_write({static_cast<const uint8_t*>(buf), len});
        service->OnSmpActivity();
    }

    return len;
}

ssize_t AccessPointService::InfoReadCallback(
    bt_conn* conn, const bt_gatt_attr* attr, void* buf, uint16_t len, uint16_t offset) {

    AccessPointService* service = instance_.load();
    if(service == nullptr)
        return bt_gatt_attr_read(conn, attr, buf, len, offset, nullptr, 0);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, service->info_.data(), service->info_size_);
}

void AccessPointService::SmpSentCallback(bt_conn* /*conn*/, void* /*user_data*/) {
    AccessPointService* service = instance_.load();
    if(service != nullptr && service->callbacks_.on_smp_sent)
        service->callbacks_.on_smp_sent();
}

void AccessPointService::LiveSentCallback(bt_conn* /*conn*/, void* /*user_data*/) {
    AccessPointService* service = instance_.load();
    if(service != nullptr && service->callbacks_.on_live_sent)
        service->callbacks_.on_live_sent();
}

void AccessPointService::OnSmpActivity() {
    if(!is_fast_.exchange(true))
        k_work_submit(&fast_work_);

    k_work_reschedule(&idle_work_, K_MSEC(IDLE_TIMEOUT_MS));
}

void AccessPointService::FastWorkHandler(k_work* /*work*/) {
    RequestConnectionParameters(true);
}

void AccessPointService::IdleWorkHandler(k_work* /*work*/) {
    AccessPointService* service = instance_.load();
    if(service != nullptr)
        service->is_fast_ = false;

    RequestConnectionParameters(false);
}

void AccessPointService::RequestConnectionParameters(bool is_fast) {
    bt_conn* conn = Ble::AcquireActiveConn();
    if(conn == nullptr)
        return;

    int err = bt_conn_le_param_update(conn, is_fast ? &FAST_PARAMETERS : &IDLE_PARAMETERS);
    if(err != 0 && err != -EALREADY)
        LOG_WRN("Connection parameter update failed (err %d)", err);

#if defined(CONFIG_BT_USER_PHY_UPDATE)
    if(is_fast) {
        const bt_conn_le_phy_param phy = BT_CONN_LE_PHY_PARAM_INIT(BT_GAP_LE_PHY_2M, BT_GAP_LE_PHY_2M);
        err = bt_conn_le_phy_update(conn, &phy);
        if(err != 0 && err != -EALREADY)
            LOG_WRN("PHY update failed (err %d)", err);
    }
#endif

    bt_conn_unref(conn);
}

} // namespace eerie_leap::subsys::bluetooth::access_point
