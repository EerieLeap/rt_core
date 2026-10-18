#pragma once

#include <memory>

#include "subsys/adc/i_adc_manager.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/i_service.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"

#include "domain/configuration_domain/services/configuration_service.h"

#include "domain/adc_domain/configuration/adc_configuration_manager.h"
#include "domain/adc_domain/services/calibration_service.h"

#ifdef CONFIG_EERIE_LEAP_SMP
    #include "domain/adc_domain/smp/calibration_mgmt_group.h"
#endif

namespace eerie_leap::controllers {

using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::ITimeService;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;

using eerie_leap::domain::adc_domain::configuration::AdcConfigurationManager;
using eerie_leap::domain::adc_domain::services::CalibrationService;

#ifdef CONFIG_EERIE_LEAP_SMP
using eerie_leap::domain::adc_domain::smp::CalibrationMgmtGroup;
#endif

class AdcController {
private:
    static constexpr const char* ADC_CONFIGURATION_NAME = "adc_config";

    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<ITimeService> time_service_;

    std::shared_ptr<IAdcManager> adc_manager_;
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager_;

    // Kept for a calibration service created after the registration. nullptr is a registration
    // too: a unit with nothing to pause.
    std::shared_ptr<IService> paused_service_;
    bool is_paused_service_registered_ = false;
    std::shared_ptr<CalibrationService> calibration_service_;
#ifdef CONFIG_EERIE_LEAP_SMP
    std::unique_ptr<CalibrationMgmtGroup> calibration_mgmt_group_;
#endif

    void InitializeCalibration();
    void RegisterCalibrationGroup();

public:
    /** @param time_service Stamps calibration results; nullptr leaves the unit without calibration. */
    AdcController(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<ITimeService> time_service
    );
    ~AdcController();

    int Initialize();

    /**
     * @brief Hands calibration the service that samples the ADC otherwise, paused while a job runs.
     *
     * May be called before or after Initialize(). The SMP calibration group is only registered once
     * both have run, so no client starts a job that pauses nothing; a unit with nothing to pause
     * passes nullptr to expose it.
     */
    void RegisterPausedService(std::shared_ptr<IService> service);

    std::shared_ptr<IAdcManager> GetManager() const {
        return adc_manager_;
    }
    std::shared_ptr<AdcConfigurationManager> GetConfigurationManager() const {
        return adc_configuration_manager_;
    }
    /** @return nullptr on a unit without ADC channels or without a time service. */
    std::shared_ptr<CalibrationService> GetCalibrationService() const {
        return calibration_service_;
    }
};

} // namespace eerie_leap::controllers
