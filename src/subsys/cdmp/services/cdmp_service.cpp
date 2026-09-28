#include <utility>

#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"

#include "subsys/cdmp/models/cdmp_device.h"
#include "cdmp_heartbeat_service.h"
// #include "cdmp_state_service.h"

#include "cdmp_service.h"

LOG_MODULE_REGISTER(cdmp_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::services {

using namespace eerie_leap::utilities::memory;

CdmpService::CdmpService(CdmpDeviceType device_type, uint32_t uid) {
    can_id_manager_ = std::make_shared<CdmpCanIdManager>();
    device_ = std::make_shared<CdmpDevice>(uid, device_type);

    thread_ = std::make_unique<Thread>(
        "cdmp_service_thread",
        this,
        CONFIG_EERIE_LEAP_CDMP_SERVICE_THREAD_STACK_SIZE,
        CONFIG_EERIE_LEAP_CDMP_SERVICE_THREAD_PRIORITY,
        false,
        Mrm::GetExtPmr());
    work_queue_thread_ = std::make_shared<WorkQueueThread>(
        "cdmp_work_queue",
        CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_STACK_SIZE,
        CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_PRIORITY);

    network_service_ = std::make_shared<CdmpNetworkService>(
        can_id_manager_, device_, work_queue_thread_);
    canbus_services_.push_back(network_service_);

    canbus_services_.emplace_back(std::make_shared<CdmpHeartbeatService>(
        can_id_manager_, device_, work_queue_thread_, network_service_));

    command_service_ = std::make_shared<CdmpCommandService>(
        can_id_manager_, device_, work_queue_thread_);
    canbus_services_.push_back(command_service_);

    // canbus_services_.emplace_back(std::make_shared<CdmpStateService>(
    //     canbus_, can_id_manager_, device_));
}

CdmpService::~CdmpService() {
    Stop();
}

void CdmpService::ThreadEntry() {
    LOG_INF("CDMP service started");

    for(const auto& service : canbus_services_)
        service->Start();

    if(auto_discovery_enabled_)
        device_->StartDiscovery();

    while(thread_->IsRunning()) {
        k_sleep(K_MSEC(100));
    }
}

bool CdmpService::DoInitialize() {
    work_queue_thread_->Initialize();
    thread_->Initialize();

    for(const auto& service : canbus_services_)
        service->Initialize();

    LOG_INF("CDMP service initialized with device type %d, unique ID 0x%08X",
        std::to_underlying(device_->GetDeviceType()), device_->GetUniqueIdentifier());

    return true;
}

void CdmpService::Configure(std::shared_ptr<CanbusProxy> canbus, uint32_t base_can_id) {
    if(thread_->IsRunning()) {
        LOG_ERR("Cannot configure while service is running.");
        return;
    }

    canbus_ = std::move(canbus);

    if(canbus_ == nullptr)
        throw std::runtime_error("Canbus interface is undefined");

    can_id_manager_->SetBaseCanId(base_can_id);

    for(const auto& service : canbus_services_)
        service->Configure(canbus_);
}

bool CdmpService::DoStart() {
    if(!thread_->Start()) {
        LOG_ERR("Failed to start CDMP service thread.");
        return false;
    }

    return true;
}

bool CdmpService::DoStop() {
    thread_->Join();

    for(const auto& service : canbus_services_)
        service->Stop();

    if(device_)
        device_->Reset();

    LOG_INF("CDMP service stopped");

    return true;
}

void CdmpService::SetAutoDiscovery(bool enabled) {
    auto_discovery_enabled_ = enabled;
}

size_t CdmpService::GetNetworkDevices(std::span<CdmpDeviceInfo> devices) const {
    return network_service_->GetNetworkDevices(devices);
}

size_t CdmpService::GetNetworkDeviceCount() const {
    return network_service_->GetDeviceCount();
}

CdmpDeviceInfo CdmpService::GetDeviceInfo() const {
    return {
        .device_id = device_->GetDeviceId(),
        .uid = device_->GetUniqueIdentifier(),
        .device_type = device_->GetDeviceType(),
        .status = device_->GetStatus(),
    };
}

int CdmpService::RegisterStatusChangedHandler(StatusChangedHandler handler) {
    return device_->GetStatusMachine().RegisterStatusChangeHandler(std::move(handler));
}

void CdmpService::UnregisterStatusChangedHandler(int handler_id) {
    device_->GetStatusMachine().UnregisterStatusChangeHandler(handler_id);
}

void CdmpService::PrintDeviceStatus() const {
    if(!device_) {
        LOG_INF("Device not initialized");
        return;
    }

    LOG_INF("CDMP Device Status:");
    LOG_INF("  Device ID: %d", device_->GetDeviceId());
    LOG_INF("  Device Type: %d", std::to_underlying(device_->GetDeviceType()));
    LOG_INF("  Unique ID: 0x%08X", device_->GetUniqueIdentifier());
    LOG_INF("  Protocol Version: 0x%02X", device_->GetProtocolVersion());
    LOG_INF("  Status: %d", static_cast<int>(device_->GetStatus()));
    LOG_INF("  Health: %d", static_cast<int>(device_->GetHealthStatus()));
    LOG_INF("  Capability Flags: 0x%08X", device_->GetCapabilityFlags());
    LOG_INF("  Uptime: %d", device_->GetUptimeCounter());
}

} // namespace eerie_leap::subsys::cdmp::services
