#include <utility>

#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"

#include "subsys/cdmp/models/cdmp_device.h"
#include "cdmp_network_service.h"
#include "cdmp_heartbeat_service.h"
// #include "cdmp_state_service.h"

#include "cdmp_service.h"

LOG_MODULE_REGISTER(cdmp_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::services {

using namespace eerie_leap::utilities::memory;

CdmpService::CdmpService(
    CdmpDeviceType device_type,
    uint32_t uid,
    uint32_t base_can_id)
        : base_can_id_(base_can_id) {

    can_id_manager_ = std::make_shared<CdmpCanIdManager>(base_can_id_);
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

    auto network_service = std::make_shared<CdmpNetworkService>(
        can_id_manager_, device_, work_queue_thread_);
    canbus_services_.push_back(network_service);

    canbus_services_.emplace_back(std::make_shared<CdmpHeartbeatService>(
        can_id_manager_, device_, work_queue_thread_, network_service));

    command_service_ = std::make_shared<CdmpCommandService>(
        can_id_manager_, device_, work_queue_thread_);
    canbus_services_.push_back(command_service_);

    // canbus_services_.emplace_back(std::make_shared<CdmpStateService>(
    //     canbus_, can_id_manager_, device_));
    // TODO: Add IsoTp Service
}

CdmpService::~CdmpService() {
    Reset();

    work_queue_thread_->Stop();
    thread_->Join();
}

void CdmpService::ThreadEntry() {
    LOG_INF("CDMP service started");

    for(const auto& service : canbus_services_)
        service->Start();

    if(auto_discovery_enabled_)
        device_->StartDiscovery();

    atomic_set(&is_running_, 1);

    while(atomic_get(&is_running_)) {
        k_sleep(K_MSEC(100));
    }
}

bool CdmpService::Initialize() {
    work_queue_thread_->Initialize();
    thread_->Initialize();

    for(const auto& service : canbus_services_)
        service->Initialize();

    LOG_INF("CDMP service initialized with device type %d, unique ID 0x%08X",
        std::to_underlying(device_->GetDeviceType()), device_->GetUniqueIdentifier());

    return true;
}

void CdmpService::Configure(std::shared_ptr<Canbus> canbus) {
    if(atomic_get(&is_running_) != 0)
        return;

    canbus_ = std::move(canbus);

    if(canbus_ == nullptr)
        throw std::runtime_error("Canbus interface is undefined");

    for(const auto& service : canbus_services_)
        service->Configure(canbus_);
}

void CdmpService::Start() {
    thread_->Start();
}

void CdmpService::Reset() {
    if(atomic_get(&is_running_) == 0) {
        atomic_set(&is_running_, 0);
        thread_->Join();
    }

    for(const auto& service : canbus_services_)
        service->Stop();

    for(const auto& service : canbus_services_)
        service->Reset();

    if(device_)
        device_->Reset();

    canbus_.reset();

    LOG_INF("CDMP service reset");
}

bool CdmpService::IsRunning() const {
    return atomic_get(&is_running_) != 0;
}

void CdmpService::SetAutoDiscovery(bool enabled) {
    auto_discovery_enabled_ = enabled;
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
