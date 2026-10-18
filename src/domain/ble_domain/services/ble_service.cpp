#include <exception>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>

#include "domain/system_domain/models/product_info.h"
#include "subsys/bluetooth/ble.h"
#include "subsys/bluetooth/utilities/bt_data_builder.hpp"

#include "ble_service.h"

namespace eerie_leap::domain::ble_domain::services {

using namespace eerie_leap::subsys::bluetooth;
using namespace eerie_leap::subsys::bluetooth::utilities;
using namespace eerie_leap::domain::system_domain::models;

LOG_MODULE_REGISTER(ble_service_logger);

std::unique_ptr<BleService> BleService::instance;
bool BleService::is_initialized = false;

BleService::BleService(
    std::shared_ptr<SensorsProcessingService> sensors_processing_service,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<ISmpForwarder> smp_forwarder
) :
    sensors_processing_service_(std::move(sensors_processing_service)),
    sensor_readings_frame_(std::move(sensor_readings_frame)),
    smp_forwarder_(std::move(smp_forwarder)) {}

BleService& BleService::Create(
    std::shared_ptr<SensorsProcessingService> sensors_processing_service,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<ISmpForwarder> smp_forwarder
) {

    instance.reset(new BleService(
        std::move(sensors_processing_service), std::move(sensor_readings_frame), std::move(smp_forwarder)
    ));

    return *instance;
}

BleService& BleService::GetInstance() {
    if(!instance)
        throw std::runtime_error("BleService instance not created. Call Create() first.");

    return *instance;
}

bool BleService::Initialize() {
    if(is_initialized)
        return true;

    if(!Ble::Initialize()) {
        LOG_ERR("Failed to initialize low-level Bluetooth subsystem.");
        return false;
    }

    is_initialized = true;

    ConfigureAdvertisingData();
    ConfigureScanResponseData();

    Ble::RegisterPairingStartedHandler([this] {
        PairingStarted();
    });
    Ble::RegisterPairingFinishedHandler([this] {
        PairingFinished();
    });

    return InitializeAccessPoint();
}

bool BleService::InitializeAccessPoint() {
    try {
        access_point_service_ = std::make_shared<AccessPointService>();
        smp_router_ = std::make_shared<SmpBleRouter>(access_point_service_->GetSmpNotifier(), smp_forwarder_);
        live_data_service_ = std::make_shared<LiveDataService>(
            access_point_service_->GetLiveNotifier(), sensor_readings_frame_
        );
        live_mgmt_group_ = std::make_unique<LiveMgmtGroup>(live_data_service_);
        live_mgmt_group_->Register();
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create the access point: %s", e.what());
        return false;
    }

    if(!smp_router_->Initialize())
        return false;

    const auto info = SmpBleRouter::GetInfo();
    access_point_service_->Initialize(
        {
            .on_smp_write =
                [router = smp_router_](std::span<const uint8_t> data) {
                    router->OnReceive(data);
                },
            .on_smp_sent =
                [router = smp_router_] {
                    router->OnNotificationSent();
                },
            .on_live_sent =
                [live = live_data_service_] {
                    live->OnNotificationSent();
                },
        },
        info
    );

    Ble::RegisterDisconnectedHandler([router = smp_router_, live = live_data_service_](bt_conn* /*conn*/) {
        router->OnDisconnected();
        live->Unsubscribe();
    });

    if(smp_forwarder_ != nullptr)
        smp_forwarder_->SetResponseSink(smp_router_);

    return true;
}

bool BleService::Start() {
    if(!is_initialized)
        return false;

    return Ble::Start();
}

void BleService::ConfigureAdvertisingData() {
    BtDataBuilder ad_builder;
    // Flags: general discoverable, no BR/EDR
    const std::vector<uint8_t> flags = { BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };
    ad_builder.Add(BT_DATA_FLAGS, flags);

    // The service UUID shares the advertisement with the manufacturer data, so a scan filtered by the
    // UUID also sees the product without a scan response. Together they take 30 of the 31 bytes.
    const std::vector<uint8_t> service_uuid = { BT_UUID_ACCESS_POINT_SERVICE_VAL };
    ad_builder.Add(BT_DATA_UUID128_ALL, service_uuid);

    // Manufacturer data
    auto manufacturer_data = GetManufacturerData();
    ad_builder.Add(BT_DATA_MANUFACTURER_DATA, manufacturer_data);

    Ble::UpdateAdvertisingData(ad_builder.Build());
}

void BleService::ConfigureScanResponseData() {
    BtDataBuilder sd_builder;
    // Full device name
    sd_builder.Add(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1);

    Ble::UpdateScanResponseData(sd_builder.Build());
}

// Another owner (a calibration job, a restart) may hold the processing service, so pairing only
// resumes what its own pause paused. A peer that disconnects before security changes leaves no
// PairingFinished, so the next connection keeps the pause it still holds instead of losing it.
void BleService::PairingStarted() {
    if(sensors_processing_service_ && !is_processing_paused_)
        is_processing_paused_ = sensors_processing_service_->Pause();
}

void BleService::PairingFinished() {
    if(!std::exchange(is_processing_paused_, false))
        return;

    if(!sensors_processing_service_->Resume())
        LOG_WRN("Pairing finished; the processing service was no longer paused and is left as it is.");
}

std::vector<uint8_t> BleService::GetManufacturerData() {
    // 2 (SIG company ID)
    // 2 (product ID)
    // 2 (product features)
    // 1 (revision)
    std::vector<uint8_t> manufacturer_data;
    manufacturer_data.reserve(7);

    // All multi-byte fields are little-endian, whatever the byte order of the CPU.
    const auto append_le16 = [&manufacturer_data](uint16_t value) {
        manufacturer_data.push_back(static_cast<uint8_t>(value & 0xFFU));
        manufacturer_data.push_back(static_cast<uint8_t>(value >> 8U));
    };

    // Bluetooth SIG Company ID - must be first 2 bytes
    // 0xFFFF = reserved for internal use / testing
    constexpr uint16_t COMPANY_ID = 0xFFFF;
    append_le16(COMPANY_ID);

    // Product ID (2 bytes)
    append_le16(ProductInfo::PRODUCT_ID);

    // Product features (2 bytes)
    append_le16(ProductInfo::FEATURES);

    // Product revision (1 byte)
    manufacturer_data.push_back(ProductInfo::REVISION);

    return manufacturer_data;
}

} // namespace eerie_leap::domain::ble_domain::services
