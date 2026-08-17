#include <utility>

#include <zephyr/logging/log.h>

#include "cdmp_device.h"

LOG_MODULE_REGISTER(cdmp_device, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::models {

CdmpDevice::CdmpDevice(
    uint32_t uid,
    CdmpDeviceType device_type,
    CdmpDeviceStatus status)
        : uid_(uid),
        device_type_(device_type),
        protocol_version_(GetCurrentProtocolVersion()),
        status_machine_(std::make_shared<CdmpStatusMachine>(status)) {

    if(uid == 0)
        throw std::invalid_argument("Unique identifier must be non-zero");

    UpdateHeartbeat();
}


void CdmpDevice::StartDiscovery() {
    status_machine_->SetStatus(CdmpDeviceStatus::INIT);
}

void CdmpDevice::ClaimId() {
    status_machine_->SetStatus(CdmpDeviceStatus::CLAIMING);
}

void CdmpDevice::GoOnline(bool force) {
    status_machine_->SetStatus(CdmpDeviceStatus::ONLINE, force);
}

void CdmpDevice::EnterVersionMismatch() {
    status_machine_->SetStatus(CdmpDeviceStatus::VERSION_MISMATCH);
}

void CdmpDevice::EnterError() {
    status_machine_->SetStatus(CdmpDeviceStatus::ERROR);
}

void CdmpDevice::Reset() {
    status_machine_->SetStatus(CdmpDeviceStatus::OFFLINE);
    health_status_ = CdmpHealthStatus::OK;
}

void CdmpDevice::UpdateHeartbeat() {
    last_heartbeat_ = k_uptime_get();
}

void CdmpDevice::SetCapability(CdmpDeviceCapabilityFlags capability, bool enabled) {
    if (enabled) {
        capability_flags_ |= std::to_underlying(capability);
    } else {
        capability_flags_ &= ~std::to_underlying(capability);
    }
}

bool CdmpDevice::IsProtocolCompatible(uint8_t other_version) const {
    uint8_t my_major = (protocol_version_ >> 4) & 0x0F;
    // uint8_t my_minor = protocol_version_ & 0x0F;
    uint8_t other_major = (other_version >> 4) & 0x0F;
    // uint8_t other_minor = other_version & 0x0F;

    // Major version must match for compatibility
    return my_major == other_major;
}

} // namespace eerie_leap::subsys::cdmp::models
