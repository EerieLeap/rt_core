#include "ble_settings_command_end_write.h"

LOG_MODULE_DECLARE(ble_settings_logger);

namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command {

BleSettingsCommandEndWrite::BleSettingsCommandEndWrite(
    std::shared_ptr<BleSettingsStatus> status)
        : BleSettingsCommandRequestBase(status) {}

void BleSettingsCommandEndWrite::Initialize(const ApplyRequestedHandler& apply_requested_handler) {
    apply_requested_handler_ = apply_requested_handler;
}

void BleSettingsCommandEndWrite::Process(std::span<const uint8_t> data) {
    if(status_->GetState() != BleSettingsState::Writing) {
        LOG_ERR("END_WRITE: not in writing state");
        status_->SetErrorCode(BleSettingsErrorCode::InvalidState);
        status_->SetState(BleSettingsState::Error);

        return;
    }

    if(status_->GetTransferredBytes() != status_->GetTotalBytes()) {
        LOG_ERR("Incomplete transfer: %u/%u bytes",
            status_->GetTransferredBytes(), status_->GetTotalBytes());
        status_->SetErrorCode(BleSettingsErrorCode::IncompleteTransfer);
        status_->SetState(BleSettingsState::Error);

        return;
    }

    if(!apply_requested_handler_) {
        LOG_ERR("END_WRITE: no apply handler registered");
        status_->SetErrorCode(BleSettingsErrorCode::HandlerFailed);
        status_->SetState(BleSettingsState::Error);

        return;
    }

    status_->SetState(BleSettingsState::Applying);

    LOG_INF("END_WRITE: applying settings_id=%u", status_->GetSettingsId());

    apply_requested_handler_(status_->GetSettingsId());
}

} // namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command
