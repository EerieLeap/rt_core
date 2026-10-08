#include <cmath>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"
#include "subsys/expression_engine/expression_evaluator.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/time_service.h"

#include "subsys/adc/models/adc_configuration.h"
#include "subsys/adc/i_adc.h"
#include "subsys/adc/adc_simulator.h"
#include "subsys/gpio/i_gpio.h"
#include "subsys/gpio/gpio_simulator.h"
#include "subsys/fs/services/fs_service.h"

#include "subsys/device_tree/dt_fs.h"
#include "domain/sensor_domain/utilities/sensors_order_resolver.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/sensor_readers/i_sensor_reader.h"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_indicator.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_indicator.h"
#include "domain/sensor_domain/processors/expression_processor.h"

#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::string;
using namespace eerie_leap::utilities::voltage_interpolator;

using namespace eerie_leap::configuration::services;

using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::adc::models;
using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::expression_engine;
using namespace eerie_leap::subsys::fs::services;

using namespace eerie_leap::domain::sensor_domain::processors;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::runtime;

ZTEST_SUITE(sensor_processor, NULL, NULL, NULL, NULL, NULL);

std::vector<std::shared_ptr<Sensor>> sensor_processor_GetTestSensors() {
    std::pmr::vector<CalibrationData> calibration_data_1 {
        {0.0, 0.0},
        {3.3, 100.0}
    };
    auto calibration_data_1_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_1);

    auto sensor_1 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_1");

    sensor_1->metadata.name = "Sensor 1";
    sensor_1->metadata.unit = "km/h";
    sensor_1->metadata.description = "Test Sensor 1";

    sensor_1->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_1->configuration.channel = 0;
    sensor_1->configuration.sampling_rate_ms = 1000;
    sensor_1->configuration.interpolation_method = InterpolationMethod::LINEAR;
    sensor_1->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());
    sensor_1->configuration.expression = "x * 2 + sensor_2 + 1";

    std::pmr::vector<CalibrationData> calibration_data_2 {
        {0.0, 0.0},
        {1.0, 29.0},
        {2.0, 111.0},
        {2.5, 162.0},
        {3.3, 200.0}
    };
    auto calibration_data_2_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_2);

    auto sensor_2 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_2");

    sensor_2->metadata.name = "Sensor 2";
    sensor_2->metadata.unit = "km/h";
    sensor_2->metadata.description = "Test Sensor 2";

    sensor_2->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_2->configuration.channel = 1;
    sensor_2->configuration.sampling_rate_ms = 500;
    sensor_2->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_2->configuration.calibration_table.assign(calibration_data_2_ptr->begin(), calibration_data_2_ptr->end());
    sensor_2->configuration.expression = "x * 4 + 1.6";

    auto sensor_3 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_3");

    sensor_3->metadata.name = "Sensor 3";
    sensor_3->metadata.unit = "km/h";
    sensor_3->metadata.description = "Test Sensor 3";

    sensor_3->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor_3->configuration.channel = std::nullopt;
    sensor_3->configuration.sampling_rate_ms = 2000;
    sensor_3->configuration.expression = "sensor_1 + 8.34";

    auto sensor_4 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_4");

    sensor_4->metadata.name = "Sensor 4";
    sensor_4->metadata.unit = "";
    sensor_4->metadata.description = "Test Sensor 4";

    sensor_4->configuration.type = SensorType::PHYSICAL_INDICATOR;
    sensor_4->configuration.channel = 1;
    sensor_4->configuration.sampling_rate_ms = 1000;
    sensor_4->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_4->configuration.calibration_table.assign(calibration_data_2_ptr->begin(), calibration_data_2_ptr->end());

    auto sensor_5 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_5");

    sensor_5->metadata.name = "Sensor 5";
    sensor_5->metadata.unit = "";
    sensor_5->metadata.description = "Test Sensor 5";

    sensor_5->configuration.type = SensorType::VIRTUAL_INDICATOR;
    sensor_5->configuration.sampling_rate_ms = 1000;
    sensor_5->configuration.expression = "sensor_1 < 400";

    std::vector<std::shared_ptr<Sensor>> sensors = {
        sensor_1, sensor_2, sensor_3, sensor_4, sensor_5 };

    SensorsOrderResolver sensors_order_resolver;
    for(auto& sensor : sensors)
        sensors_order_resolver.AddSensor(sensor);

    return sensors_order_resolver.GetProcessingOrder();
}

AdcConfiguration sensor_processor_GetTestConfiguration() {
    std::pmr::vector<CalibrationData> adc_calibration_data_samples {
        {0.0, 0.0},
        {5.0, 5.0}
    };

    auto adc_calibration_data_samples_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(adc_calibration_data_samples);
    auto adc_calibrator = std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR, adc_calibration_data_samples_ptr);

    auto adc_channel_configuration = std::make_shared<AdcChannelConfiguration>(adc_calibrator);

    std::vector<std::shared_ptr<AdcChannelConfiguration>> channel_configurations;
    channel_configurations.reserve(8);
    for(int i = 0; i < 8; i++)
        channel_configurations.push_back(adc_channel_configuration);

    AdcConfiguration adc_configuration;
    adc_configuration.samples = 40;
    adc_configuration.channel_configurations =
        std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>(channel_configurations);

    return adc_configuration;
}

struct sensor_processor_HelperInstances {
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame;
    std::shared_ptr<SensorGeneration> generation;
    std::shared_ptr<std::vector<std::shared_ptr<ISensorReader>>> sensor_readers;
    std::vector<std::shared_ptr<Sensor>> sensors;
};

sensor_processor_HelperInstances sensor_processor_GetReadingInstances() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto time_provider = std::make_shared<BootElapsedTimeProvider>();
    auto rtc_provider = std::make_shared<RtcProvider>();
    auto time_service = std::make_shared<TimeService>(time_provider, rtc_provider);

    auto cbor_adc_config_service = std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service);

    AdcFactory adc_factory(nullptr);
    auto adc_manager = adc_factory.Create();
    adc_manager->Initialize();

    auto adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
        std::move(cbor_adc_config_service), adc_manager);

    const auto adc_configuration = sensor_processor_GetTestConfiguration();
    adc_configuration_manager->Update(adc_configuration);

    auto gpio = std::make_shared<GpioSimulator>();
    gpio->Initialize();

    auto sensor_readings_frame = make_shared_pmr<SensorReadingsFrame>(Mrm::GetDefaultPmr());
    auto sensors = sensor_processor_GetTestSensors();

    // The builder lays out the frame's slots and binds the expressions, as the processing service does.
    auto generation = SensorPipelineBuilder(nullptr, sensor_readings_frame).Build(
        std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));

    auto sensor_readers = std::make_shared<std::vector<std::shared_ptr<ISensorReader>>>();
    for(size_t i = 0; i < generation->runtimes.size(); i++) {
        const auto& runtime = generation->runtimes[i];
        std::shared_ptr<ISensorReader> sensor_reader;

        if(runtime.sensor->configuration.type == SensorType::PHYSICAL_ANALOG) {
            sensor_reader = std::make_shared<SensorReaderPhysicalAnalog>(
                time_service,
                runtime,
                adc_configuration_manager);
        } else if(runtime.sensor->configuration.type == SensorType::VIRTUAL_ANALOG) {
            sensor_reader = std::make_shared<SensorReaderVirtualAnalog>(
                time_service,
                runtime);
        } else if(runtime.sensor->configuration.type == SensorType::PHYSICAL_INDICATOR) {
            sensor_reader = std::make_shared<SensorReaderPhysicalIndicator>(
                time_service,
                runtime,
                gpio);
        } else if(runtime.sensor->configuration.type == SensorType::VIRTUAL_INDICATOR) {
            sensor_reader = std::make_shared<SensorReaderVirtualIndicator>(
                time_service,
                runtime);
        } else {
            throw std::runtime_error("Unsupported sensor type");
        }

        sensor_readers->push_back(sensor_reader);
    }

    return sensor_processor_HelperInstances {
        .sensor_readings_frame = sensor_readings_frame,
        .generation = generation,
        .sensor_readers = sensor_readers,
        .sensors = sensors
    };
}

class ReadingProcessedProcessor : public IReadingProcessor {
public:
    void Process(const SensorRuntime& runtime, SensorReading& reading) override {
        ARG_UNUSED(runtime);

        reading.status = ReadingStatus::PROCESSED;
    }
};

ZTEST(sensor_processor, test_ProcessReading) {
    auto helper = sensor_processor_GetReadingInstances();

    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensor_readers = helper.sensor_readers;
    auto sensors = helper.sensors;

    for(int i = 0; i < sensor_readers->size(); i++)
        sensor_readings_frame->AddOrUpdateReading(sensor_readers->at(i)->Read());

    auto reading_2_opt = sensor_readings_frame->TryGetReading("sensor_2");
    zassert_true(reading_2_opt.has_value());
    auto& reading_2 = reading_2_opt.value();

    auto reading_1_opt = sensor_readings_frame->TryGetReading("sensor_1");
    zassert_true(reading_1_opt.has_value());
    auto& reading_1 = reading_1_opt.value();

    float reading_1_value = reading_1.value.value();
    float reading_2_value = reading_2.value.value();

    std::vector<std::shared_ptr<IReadingProcessor>> reading_processors;
    reading_processors.push_back(std::make_shared<ExpressionProcessor>(sensor_readings_frame));
    reading_processors.push_back(std::make_shared<ReadingProcessedProcessor>());

    // The stages work on a local copy and the result is committed once, as the services do.
    for(const auto& runtime : helper.generation->runtimes) {
        auto reading = sensor_readings_frame->TryGetReading(runtime.sensor->id_hash).value();
        for(auto& reading_processor : reading_processors)
            reading_processor->Process(runtime, reading);

        sensor_readings_frame->AddOrUpdateReading(reading);
    }

    auto proc_reading_2_opt = sensor_readings_frame->TryGetReading("sensor_2");
    zassert_true(proc_reading_2_opt.has_value());
    auto& proc_reading_2 = proc_reading_2_opt.value();
    zassert_equal(proc_reading_2.status, ReadingStatus::PROCESSED);
    zassert_true(proc_reading_2.value.has_value());
    float proc_reading_2_value = reading_2_value * 4 + 1.6;
    zassert_equal(proc_reading_2.value.value(), proc_reading_2_value);
    zassert_true(proc_reading_2.voltage.has_value());
    zassert_between_inclusive(proc_reading_2.voltage.value(), 0.0f, 3.3f);
    zassert_true(proc_reading_2.raw_value.has_value());
    zassert_between_inclusive(proc_reading_2.raw_value.value(), 0.0f, 200.0f);

    auto proc_reading_1_opt = sensor_readings_frame->TryGetReading("sensor_1");
    zassert_true(proc_reading_1_opt.has_value());
    auto& proc_reading_1 = proc_reading_1_opt.value();
    zassert_equal(proc_reading_1.status, ReadingStatus::PROCESSED);
    zassert_true(proc_reading_1.value.has_value());
    float proc_reading_1_value = reading_1_value * 2 + proc_reading_2_value + 1;
    zassert_equal(proc_reading_1.value.value(), proc_reading_1_value);
    zassert_true(proc_reading_1.voltage.has_value());
    zassert_between_inclusive(proc_reading_1.voltage.value(), 0.0f, 3.3f);
    zassert_true(proc_reading_1.raw_value.has_value());
    zassert_between_inclusive(proc_reading_1.raw_value.value(), 0.0f, 100.0f);

    auto proc_reading_3_opt = sensor_readings_frame->TryGetReading("sensor_3");
    zassert_true(proc_reading_3_opt.has_value());
    auto& proc_reading_3 = proc_reading_3_opt.value();
    zassert_equal(proc_reading_3.status, ReadingStatus::PROCESSED);
    zassert_true(proc_reading_3.value.has_value());
    float proc_reading_3_value = proc_reading_1_value + 8.34;
    // Expression evaluator parses "8.34" at runtime independently of the compiler's constant
    // folding, so the result can differ by a ULP or two across platforms/libc - compare with tolerance.
    zassert_true(std::fabs(proc_reading_3.value.value() - proc_reading_3_value) < 0.0001f);
    zassert_false(proc_reading_3.raw_value.has_value(), "A virtual sensor has no raw value");

    auto proc_reading_4_opt = sensor_readings_frame->TryGetReading("sensor_4");
    zassert_true(proc_reading_4_opt.has_value());
    auto& proc_reading_4 = proc_reading_4_opt.value();
    zassert_equal(proc_reading_4.status, ReadingStatus::PROCESSED);
    zassert_true(proc_reading_4.value.has_value());
    zassert_true(proc_reading_4.value.value() == 1 || proc_reading_4.value.value() == 0);
    zassert_true(proc_reading_4.raw_value.has_value());

    auto proc_reading_5_opt = sensor_readings_frame->TryGetReading("sensor_5");
    zassert_true(proc_reading_5_opt.has_value());
    auto& proc_reading_5 = proc_reading_5_opt.value();
    zassert_equal(proc_reading_5.status, ReadingStatus::PROCESSED);
    zassert_true(proc_reading_5.value.has_value());
    zassert_true(proc_reading_5.value.value() ==  proc_reading_1_value < 400);
    zassert_false(proc_reading_5.raw_value.has_value(), "A virtual sensor has no raw value");
}
