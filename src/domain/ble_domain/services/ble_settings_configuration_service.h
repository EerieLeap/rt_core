#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/work_queue_thread.h"
#include "domain/configuration_domain/services/configuration_service.h"

namespace eerie_leap::domain::ble_domain::services {

using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::subsys::threading::WorkQueueThread;

class BleSettingsConfigurationService {
private:
    static std::unique_ptr<BleSettingsConfigurationService> instance_;
    static bool is_initialized_;
    static std::pmr::vector<uint8_t> cbor_buffer_;

    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;

    BleSettingsConfigurationService(
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread);

    bool HandleConfigWrite(uint8_t settings_id, std::span<const uint8_t> data) const;
    std::span<const uint8_t> HandleConfigRead(uint8_t settings_id) const;

public:
    BleSettingsConfigurationService& operator=(const BleSettingsConfigurationService&) = delete;

    static BleSettingsConfigurationService& Create(
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread);
    static BleSettingsConfigurationService& GetInstance();

    bool Initialize();
};

} // namespace eerie_leap::domain::ble_domain::services
