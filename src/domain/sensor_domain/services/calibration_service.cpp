#include <memory>
#include <vector>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/time/time_helpers.hpp"
#include "subsys/adc/utilities/adc_calibrator.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_analog_calibrator.h"
#include "calibration_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::adc::utilities;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;

LOG_MODULE_REGISTER(calibration_logger);

CalibrationService::CalibrationService(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<GuidGenerator> guid_generator,
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager,
    std::shared_ptr<SensorsProcessingService> sensors_processing_service)
    : work_queue_thread_(nullptr),
    time_service_(std::move(time_service)),
    guid_generator_(std::move(guid_generator)),
    adc_configuration_manager_(std::move(adc_configuration_manager)),
    sensors_processing_service_(std::move(sensors_processing_service)) {};

void CalibrationService::Initialize() {
    work_queue_thread_ = std::make_unique<WorkQueueThread>(
        "calibration_service_",
        thread_stack_size_,
        thread_priority_);
    work_queue_thread_->Initialize();
}

WorkQueueTaskResult CalibrationService::ProcessCalibrationWorkTask(SensorTask* task) {
    try {
        task->reader->Read();
        auto reading_optional = task->readings_frame->TryGetReading(task->sensor->id_hash);
        if(reading_optional) {
            auto reading = reading_optional.value();

            LOG_INF("ADC Calibration Reading: Value: %.3f, Time: %s\n",
                reading.value.value_or(0.0f),
                TimeHelpers::GetFormattedString(reading.timestamp.value()).c_str());
        }
    } catch (const std::exception& e) {
        LOG_ERR("Error processing calibrator on channel %d, Error: %s",
            task->sensor->configuration.channel.value_or(-1),
            e.what());
    }

    return WorkQueueTaskResult {
        .reschedule = true,
        .delay = task->sampling_rate_ms
    };
}

std::unique_ptr<SensorTask> CalibrationService::CreateCalibrationTask(int channel) {
    auto sensor = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), "CalibrationSensor");
    sensor->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor->configuration.channel = channel;
    sensor->configuration.sampling_rate_ms = CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS;

    auto sensor_readings_frame = std::make_shared<SensorReadingsFrame>();

    auto task = std::make_unique<SensorTask>();
    task->sampling_rate_ms = K_MSEC(sensor->configuration.sampling_rate_ms.value());
    task->sensor = sensor;
    task->readings_frame = sensor_readings_frame;

    task->reader = std::make_unique<SensorReaderPhysicalAnalogCalibrator>(
        time_service_,
        guid_generator_,
        sensor_readings_frame,
        sensor,
        adc_configuration_manager_);

    return task;
}

void CalibrationService::Start(int channel) {
    sensors_processing_service_->Pause();

    auto adc_manager = adc_configuration_manager_->Get();
    adc_manager->UpdateSamplesCount(CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLES_COUNT);

    auto calibration_task = CreateCalibrationTask(channel);

    if(calibration_task == nullptr)
        return;

    calibration_task_ = work_queue_thread_->CreateTask(ProcessCalibrationWorkTask, std::move(calibration_task));
    calibration_task_.value().Schedule();

    k_sleep(K_MSEC(1));

    LOG_INF("Calibration Service started");
}

void CalibrationService::Stop() {

    if(calibration_task_.has_value()) {
        while(calibration_task_.value().Cancel())
            k_sleep(K_MSEC(1));
    }

    LOG_INF("Calibration Service stopped");

    auto adc_manager = adc_configuration_manager_->Get();
    adc_manager->ResetSamplesCount();

    sensors_processing_service_->Resume();
}

} // namespace eerie_leap::domain::sensor_domain::services
