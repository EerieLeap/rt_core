#pragma once

#include <memory>
#include <vector>

#include "subsys/bluetooth/access_point/access_point_service.h"
#include "subsys/smp/i_smp_forwarder.h"
#include "subsys/smp/ble/smp_ble_router.h"
#include "domain/sensor_domain/services/sensors_processing_service.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/ble_domain/services/live_data_service.h"
#include "domain/ble_domain/smp/live_mgmt_group.h"

namespace eerie_leap::domain::ble_domain::services {

using eerie_leap::domain::ble_domain::smp::LiveMgmtGroup;
using eerie_leap::domain::sensor_domain::services::SensorsProcessingService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::bluetooth::access_point::AccessPointService;
using eerie_leap::subsys::smp::ISmpForwarder;
using eerie_leap::subsys::smp::ble::SmpBleRouter;

/** @brief Advertising, pairing and the access point: SMP routed to this and other units, and live data. */
class BleService {
private:
    static std::unique_ptr<BleService> instance_;

    std::shared_ptr<SensorsProcessingService> sensors_processing_service_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<ISmpForwarder> smp_forwarder_;
    std::shared_ptr<AccessPointService> access_point_service_;
    std::shared_ptr<SmpBleRouter> smp_router_;
    std::shared_ptr<LiveDataService> live_data_service_;
    std::unique_ptr<LiveMgmtGroup> live_mgmt_group_;

    static bool is_initialized_;

    static std::vector<uint8_t> GetManufacturerData();

    void ConfigureAdvertisingData() const;
    void ConfigureScanResponseData() const;
    void PairingStarted() const;
    void PairingFinished() const;
    bool InitializeAccessPoint();

    BleService(
        std::shared_ptr<SensorsProcessingService> sensors_processing_service,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<ISmpForwarder> smp_forwarder);

public:
    BleService& operator=(const BleService&) = delete;

    /**
     * @param sensor_readings_frame Source of the live values.
     * @param smp_forwarder Reaches the other units; nullptr serves only this one.
     */
    static BleService& Create(
        std::shared_ptr<SensorsProcessingService> sensors_processing_service,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<ISmpForwarder> smp_forwarder);
    static BleService& GetInstance();

    bool Initialize();
    bool Start() const;
};

} // namespace eerie_leap::domain::ble_domain::services
