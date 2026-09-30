#include <exception>
#include <span>
#include <stdexcept>
#include <vector>
#include <utility>

#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>

#include "subsys/bluetooth/ble.h"
#include "subsys/bluetooth/utilities/bt_data_builder.hpp"
#include "domain/system_domain/models/product_info.h"

#include "ble_service.h"

namespace eerie_leap::domain::ble_domain::services {

using namespace eerie_leap::subsys::bluetooth;
using namespace eerie_leap::subsys::bluetooth::utilities;
using namespace eerie_leap::domain::system_domain::models;

LOG_MODULE_REGISTER(ble_service_logger);

std::unique_ptr<BleService> BleService::instance_;
bool BleService::is_initialized_ = false;

BleService::BleService(
    std::shared_ptr<SensorsProcessingService> sensors_processing_service,
    std::shared_ptr<ISmpForwarder> smp_forwarder)
        : sensors_processing_service_(std::move(sensors_processing_service)),
        smp_forwarder_(std::move(smp_forwarder)
) {}

BleService& BleService::Create(
    std::shared_ptr<SensorsProcessingService> sensors_processing_service,
    std::shared_ptr<ISmpForwarder> smp_forwarder
) {

    instance_.reset(new BleService(std::move(sensors_processing_service), std::move(smp_forwarder)));

    return *instance_;
}

BleService& BleService::GetInstance() {
    if(!instance_)
        throw std::runtime_error("BleService instance not created. Call Create() first.");

    return *instance_;
}

bool BleService::Initialize() {
    if(is_initialized_)
        return true;

    if(!Ble::Initialize()) {
        LOG_ERR("Failed to initialize low-level Bluetooth subsystem.");
        return false;
    }

    is_initialized_ = true;

    ConfigureAdvertisingData();
    ConfigureScanResponseData();

    Ble::RegisterPairingStartedHandler([this]() {
        PairingStarted();
    });
    Ble::RegisterPairingFinishedHandler([this]() {
        PairingFinished();
    });

    return InitializeAccessPoint();
}

bool BleService::InitializeAccessPoint() {
    try {
        access_point_service_ = std::make_shared<AccessPointService>();
        smp_router_ = std::make_shared<SmpBleRouter>(access_point_service_, smp_forwarder_);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create the access point: %s", e.what());
        return false;
    }

    if(!smp_router_->Initialize())
        return false;

    const auto info = SmpBleRouter::GetInfo();
    access_point_service_->Initialize({
            .on_smp_write = [router = smp_router_](std::span<const uint8_t> data) { router->OnReceive(data); },
            .on_smp_sent = [router = smp_router_]() { router->OnNotificationSent(); },
        },
        info);

    Ble::RegisterDisconnectedHandler([router = smp_router_](bt_conn* /*conn*/) {
        router->OnDisconnected();
    });

    if(smp_forwarder_ != nullptr)
        smp_forwarder_->SetResponseSink(smp_router_);

    return true;
}

bool BleService::Start() const {
    if(!is_initialized_)
        return false;

    return Ble::Start();
}

void BleService::ConfigureAdvertisingData() const {
    BtDataBuilder ad_builder;
    // Flags: general discoverable, no BR/EDR
    const std::vector<uint8_t> flags = { BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };
    ad_builder.Add(BT_DATA_FLAGS, flags);

    // The service UUID shares the advertisement with the manufacturer data, so a scan filtered by the
    // UUID also sees the product without a scan response. Together they fill all 31 bytes.
    const std::vector<uint8_t> service_uuid = { BT_UUID_ACCESS_POINT_SERVICE_VAL };
    ad_builder.Add(BT_DATA_UUID128_ALL, service_uuid);

    // Manufacturer data
    auto manufacturer_data = GetManufacturerData();
    ad_builder.Add(BT_DATA_MANUFACTURER_DATA, manufacturer_data);

    Ble::UpdateAdvertisingData(ad_builder.Build());
}

void BleService::ConfigureScanResponseData() const {
    BtDataBuilder sd_builder;
    // Full device name
    sd_builder.Add(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1);

    Ble::UpdateScanResponseData(sd_builder.Build());
}

void BleService::PairingStarted() const {
    if(sensors_processing_service_)
        sensors_processing_service_->Pause();
}

void BleService::PairingFinished() const {
    if(sensors_processing_service_)
        sensors_processing_service_->Resume();
}

std::vector<uint8_t> BleService::GetManufacturerData() {
    // 2 (SIG company ID)
    // 1 (family)
    // 2 (product ID)
    // 2 (product features)
    // 1 (revision)
    std::vector<uint8_t> manufacturer_data;
    manufacturer_data.reserve(8);

    // Bluetooth SIG Company ID - must be first 2 bytes, little-endian
    // 0xFFFF = reserved for internal use / testing
    constexpr uint16_t company_id = 0xFFFF;
    const uint16_t company_id_le = sys_cpu_to_le16(company_id);
    const auto* cid = reinterpret_cast<const uint8_t*>(&company_id_le);
    manufacturer_data.insert(manufacturer_data.end(), cid, cid + sizeof(company_id_le));

    // Product family (1 byte)
    constexpr uint8_t product_family_le = std::to_underlying(ProductInfo::family);
    const auto* product_family_ptr = &product_family_le;
    manufacturer_data.insert(manufacturer_data.end(), product_family_ptr, product_family_ptr + sizeof(product_family_le));

    // Product ID (2 bytes, little-endian)
    constexpr uint16_t product_id_le = sys_cpu_to_le16(ProductInfo::product_id);
    const auto* product_id_ptr = reinterpret_cast<const uint8_t*>(&product_id_le);
    manufacturer_data.insert(manufacturer_data.end(), product_id_ptr, product_id_ptr + sizeof(product_id_le));

    // Product features (2 bytes, little-endian)
    constexpr uint16_t product_features_le = sys_cpu_to_le16(ProductInfo::features);
    const auto* product_features_ptr = reinterpret_cast<const uint8_t*>(&product_features_le);
    manufacturer_data.insert(manufacturer_data.end(), product_features_ptr, product_features_ptr + sizeof(product_features_le));

    // Product revision (1 byte)
    constexpr uint8_t product_revision_le = ProductInfo::revision;
    const auto* product_revision_ptr = &product_revision_le;
    manufacturer_data.insert(manufacturer_data.end(), product_revision_ptr, product_revision_ptr + sizeof(product_revision_le));

    return manufacturer_data;
}

} // namespace eerie_leap::domain::ble_domain::services
