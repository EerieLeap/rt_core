#include <zephyr/logging/log.h>

#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/adc/adc_factory.hpp"
#include "subsys/device_tree/dt_adc.h"

#include "adc_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::device_tree;

namespace config_services = eerie_leap::configuration::services;

LOG_MODULE_REGISTER(adc_controller_logger);

AdcController::AdcController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<ITimeService> time_service)
    : fs_service_(std::move(fs_service)),
      config_work_queue_thread_(std::move(config_work_queue_thread)),
      configuration_service_(std::move(configuration_service)),
      time_service_(std::move(time_service)) {}

// The group dispatches to the calibration service, so it is unregistered before the service goes.
AdcController::~AdcController() {
#ifdef CONFIG_EERIE_LEAP_SMP
    calibration_mgmt_group_.reset();
#endif
    calibration_service_.reset();
}

int AdcController::Initialize() {
    auto adc_manager = AdcFactory(DtAdc::Get).Create();
    if(!adc_manager->Initialize()) {
        LOG_ERR("Failed to initialize ADC.");
        return -1;
    }

    adc_manager_ = std::move(adc_manager);

    auto cbor_adc_config_service = std::make_unique<config_services::CborConfigurationService<CborAdcConfig>>(
        ADC_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_);
    adc_configuration_manager_ = std::make_shared<AdcConfigurationManager>(
        std::move(cbor_adc_config_service), adc_manager_);

    if(configuration_service_ != nullptr)
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Adc, adc_configuration_manager_);

    InitializeCalibration();

    return 0;
}

void AdcController::RegisterPausedService(std::shared_ptr<IService> service) {
    paused_service_ = std::move(service);
    is_paused_service_registered_ = true;

    if(calibration_service_ == nullptr)
        return;

    calibration_service_->SetServiceToPause(paused_service_);
    RegisterCalibrationGroup();
}

// A unit whose calibration cannot start still samples, so a failure here is not fatal.
void AdcController::InitializeCalibration() {
    if(adc_manager_->GetChannelCount() <= 0 || time_service_ == nullptr)
        return;

    auto calibration_service = std::make_shared<CalibrationService>(time_service_, adc_manager_, paused_service_);
    if(!calibration_service->Initialize()) {
        LOG_ERR("Failed to initialize the calibration service.");
        return;
    }

    calibration_service_ = std::move(calibration_service);

#ifdef CONFIG_EERIE_LEAP_SMP
    calibration_mgmt_group_ = std::make_unique<CalibrationMgmtGroup>(
        calibration_service_, adc_manager_->GetChannelCount());
#endif

    if(is_paused_service_registered_)
        RegisterCalibrationGroup();
}

// A client's START before the service to pause is known would calibrate under the sensors'
// sampling, so the group is only served from then on.
void AdcController::RegisterCalibrationGroup() {
#ifdef CONFIG_EERIE_LEAP_SMP
    if(calibration_mgmt_group_ != nullptr)
        calibration_mgmt_group_->Register();
#endif
}

} // namespace eerie_leap::controllers
