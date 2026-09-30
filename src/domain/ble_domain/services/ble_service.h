#pragma once

#include <memory>
#include <vector>

#include "subsys/bluetooth/access_point/access_point_service.h"
#include "subsys/smp/i_smp_forwarder.h"
#include "subsys/smp/ble/smp_ble_router.h"
#include "domain/sensor_domain/services/sensors_processing_service.h"

namespace eerie_leap::domain::ble_domain::services {

using eerie_leap::domain::sensor_domain::services::SensorsProcessingService;
using eerie_leap::subsys::bluetooth::access_point::AccessPointService;
using eerie_leap::subsys::smp::ISmpForwarder;
using eerie_leap::subsys::smp::ble::SmpBleRouter;

/** @brief Advertising, pairing and the access point: SMP over BLE, routed to this and other units. */
class BleService {
private:
    static std::unique_ptr<BleService> instance_;

    std::shared_ptr<SensorsProcessingService> sensors_processing_service_;
    std::shared_ptr<ISmpForwarder> smp_forwarder_;
    std::shared_ptr<AccessPointService> access_point_service_;
    std::shared_ptr<SmpBleRouter> smp_router_;

    static bool is_initialized_;

    static std::vector<uint8_t> GetManufacturerData();

    void ConfigureAdvertisingData() const;
    void ConfigureScanResponseData() const;
    void PairingStarted() const;
    void PairingFinished() const;
    bool InitializeAccessPoint();

    BleService(
        std::shared_ptr<SensorsProcessingService> sensors_processing_service,
        std::shared_ptr<ISmpForwarder> smp_forwarder);

public:
    BleService& operator=(const BleService&) = delete;

    /** @param smp_forwarder Reaches the other units; nullptr serves only this one. */
    static BleService& Create(
        std::shared_ptr<SensorsProcessingService> sensors_processing_service,
        std::shared_ptr<ISmpForwarder> smp_forwarder);
    static BleService& GetInstance();

    bool Initialize();
    bool Start() const;
};

} // namespace eerie_leap::domain::ble_domain::services
