#include <cmath>
#include <eerie_memory.hpp>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/adc/adc_factory.hpp"
#include "subsys/adc/adc_simulator.h"
#include "subsys/adc/i_adc.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/expression_engine/expression_evaluator.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/gpio/gpio_simulator.h"
#include "subsys/gpio/i_gpio.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"

#include "domain/adc_domain/configuration/adc_configuration_manager.h"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/sensor_readers/i_sensor_reader.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_indicator.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_indicator.h"
#include "domain/sensor_domain/services/adc_sample_set.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/models/sensor.h"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"

#include "processing_test_support.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::string;
using namespace eerie_leap::utilities::voltage_interpolator;

using namespace eerie_leap::configuration::services;

using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::expression_engine;
using namespace eerie_leap::subsys::fs::services;

using namespace eerie_leap::domain::adc_domain::utilities;
using namespace eerie_leap::domain::adc_domain::models;
using namespace eerie_leap::domain::adc_domain::configuration;

using namespace eerie_leap::domain::sensor_domain::sensor_readers;

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::services;

ZTEST_SUITE(sensors_reader, NULL, NULL, NULL, NULL, NULL);

static std::vector<std::shared_ptr<Sensor>> SensorsReaderGetTestSensors() {
    const std::pmr::vector<CalibrationData> calibration_data_1{ { 0.0, 0.0 }, { 3.3, 5.0 } };
    const auto calibration_data_1_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_1);

    const auto sensor_1 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_1");

    sensor_1->metadata.name = "Sensor 1";
    sensor_1->metadata.unit = "km/h";
    sensor_1->metadata.description = "Test Sensor 1";

    sensor_1->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_1->configuration.channel = 0;
    sensor_1->configuration.sampling_rate_ms = 1000;
    sensor_1->configuration.interpolation_method = InterpolationMethod::LINEAR;
    sensor_1->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());
    sensor_1->configuration.expression = "x * 2 + sensor_2 + 1";

    const std::pmr::vector<CalibrationData> calibration_data_2{ { 0.0, 0.0 }, { 1.0, 29.0 }, { 2.0, 111.0 },
        { 2.5, 162.0 }, { 3.3, 200.0 } };
    const auto calibration_data_2_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_2);

    const auto sensor_2 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_2");

    sensor_2->metadata.name = "Sensor 2";
    sensor_2->metadata.unit = "km/h";
    sensor_2->metadata.description = "Test Sensor 2";

    sensor_2->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_2->configuration.channel = 1;
    sensor_2->configuration.sampling_rate_ms = 500;
    sensor_2->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_2->configuration.calibration_table.assign(calibration_data_2_ptr->begin(), calibration_data_2_ptr->end());
    sensor_2->configuration.expression = "x * 4 + 1.6";

    const auto sensor_3 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_3");

    sensor_3->metadata.name = "Sensor 3";
    sensor_3->metadata.unit = "km/h";
    sensor_3->metadata.description = "Test Sensor 3";

    sensor_3->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor_3->configuration.channel = std::nullopt;
    sensor_3->configuration.sampling_rate_ms = 2000;
    sensor_3->configuration.expression = "sensor_1 + 8.34";

    const auto sensor_4 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_4");

    sensor_4->metadata.name = "Sensor 4";
    sensor_4->metadata.unit = "";
    sensor_4->metadata.description = "Test Sensor 4";

    sensor_4->configuration.type = SensorType::PHYSICAL_INDICATOR;
    sensor_4->configuration.channel = 1;
    sensor_4->configuration.sampling_rate_ms = 1000;
    sensor_4->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_4->configuration.calibration_table.assign(calibration_data_2_ptr->begin(), calibration_data_2_ptr->end());

    const auto sensor_5 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_5");

    sensor_5->metadata.name = "Sensor 5";
    sensor_5->metadata.unit = "";
    sensor_5->metadata.description = "Test Sensor 5";

    sensor_5->configuration.type = SensorType::VIRTUAL_INDICATOR;
    sensor_5->configuration.sampling_rate_ms = 1000;
    sensor_5->configuration.expression = "sensor_1 < 400";

    std::vector<std::shared_ptr<Sensor>> sensors = { sensor_1, sensor_2, sensor_3, sensor_4, sensor_5 };

    return sensors;
}

static AdcConfiguration SensorsReaderGetTestConfiguration() {
    const std::pmr::vector<CalibrationData> adc_calibration_data_samples{ { 0.0, 0.0 }, { 5.0, 5.0 } };

    const auto adc_calibration_data_samples_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(
        adc_calibration_data_samples
    );
    const auto adc_calibrator = std::make_shared<AdcCalibrator>(
        InterpolationMethod::LINEAR, adc_calibration_data_samples_ptr
    );

    const auto adc_channel_configuration = std::make_shared<AdcChannelConfiguration>(adc_calibrator);

    std::vector<std::shared_ptr<AdcChannelConfiguration>> channel_configurations;
    channel_configurations.reserve(8);
    for(int i = 0; i < 8; i++)
        channel_configurations.push_back(adc_channel_configuration);

    AdcConfiguration adc_configuration;
    adc_configuration.samples = 40;
    adc_configuration.channel_configurations = std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>(
        channel_configurations
    );

    return adc_configuration;
}

struct SensorsReaderHelperInstances {
    std::shared_ptr<ITimeService> time_service;
    std::shared_ptr<IAdcManager> adc_manager; // Owns the ADCs the sample set points at.
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame;
    std::shared_ptr<SensorGeneration> generation;
    std::shared_ptr<AdcSampleSet> adc_samples;
    std::shared_ptr<std::vector<std::shared_ptr<ISensorReader>>> sensor_readers;
};

static SensorsReaderHelperInstances SensorsReaderGetReadingInstances() {
    const auto time_provider = std::make_shared<BootElapsedTimeProvider>();
    const auto rtc_provider = std::make_shared<RtcProvider>();
    const auto time_service = std::make_shared<TimeService>(time_provider, rtc_provider);

    AdcFactory adc_factory(nullptr);
    const auto adc_manager = adc_factory.Create();
    adc_manager->Initialize();

    const auto gpio = std::make_shared<GpioSimulator>();
    gpio->Initialize();

    const auto sensor_readings_frame = make_shared_pmr<SensorReadingsFrame>(Mrm::GetDefaultPmr());
    const auto sensors = SensorsReaderGetTestSensors();

    // The builder lays out the frame's slots and binds the expressions, as the processing service does.
    const auto generation = SensorPipelineBuilder(nullptr, sensor_readings_frame)
                                .Build(std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));
    generation->adc_configuration = std::make_shared<AdcConfiguration>(SensorsReaderGetTestConfiguration());

    // Analog readers reserve their slots in the sample set, as in a rate group.
    const auto adc_samples = std::make_shared<AdcSampleSet>();
    RateGroupBuildContext context{ .adc_samples = *adc_samples,
        .adc_configuration = generation->adc_configuration.get() };
    SensorReaderFactory sensor_reader_factory(time_service, gpio, adc_manager);

    const auto sensor_readers = std::make_shared<std::vector<std::shared_ptr<ISensorReader>>>();
    for(const auto& runtime : generation->runtimes) {
        std::shared_ptr<ISensorReader> sensor_reader;

        if(runtime.sensor->configuration.type == SensorType::PHYSICAL_ANALOG) {
            sensor_reader = sensor_reader_factory.Create(runtime, context);
        } else if(runtime.sensor->configuration.type == SensorType::VIRTUAL_ANALOG) {
            sensor_reader = std::make_shared<SensorReaderVirtualAnalog>(time_service, runtime);
        } else if(runtime.sensor->configuration.type == SensorType::PHYSICAL_INDICATOR) {
            sensor_reader = std::make_shared<SensorReaderPhysicalIndicator>(time_service, runtime, gpio);
        } else if(runtime.sensor->configuration.type == SensorType::VIRTUAL_INDICATOR) {
            sensor_reader = std::make_shared<SensorReaderVirtualIndicator>(time_service, runtime);
        } else {
            throw std::runtime_error("Unsupported sensor type");
        }

        sensor_readers->push_back(sensor_reader);
    }

    adc_samples->Seal();

    return SensorsReaderHelperInstances{ .time_service = time_service,
        .adc_manager = adc_manager,
        .sensor_readings_frame = sensor_readings_frame,
        .generation = generation,
        .adc_samples = adc_samples,
        .sensor_readers = sensor_readers };
}

ZTEST(sensors_reader, test_Read) {
    const auto helper = SensorsReaderGetReadingInstances();

    const auto sensor_readings_frame = helper.sensor_readings_frame;
    const auto sensor_readers = helper.sensor_readers;

    const std::vector<std::string> sensor_names{ "sensor_1", "sensor_2", "sensor_3", "sensor_4", "sensor_5" };

    for(const auto& sensor_name : sensor_names) {
        const auto sensor_hash = StringHelpers::GetHash(sensor_name);
        zassert_false(sensor_readings_frame->HasReading(sensor_hash));
    }

    helper.adc_samples->Acquire();
    const TickContext tick{ helper.time_service->GetCurrentTime() };

    for(auto& i : *sensor_readers)
        sensor_readings_frame->AddOrUpdateReading(i->Read(tick));

    for(const auto& sensor_name : sensor_names) {
        const auto sensor_hash = StringHelpers::GetHash(sensor_name);
        zassert_true(sensor_readings_frame->HasReading(sensor_hash));
    }

    auto reading_2_opt = sensor_readings_frame->TryGetReading("sensor_2");
    zassert_true(reading_2_opt.has_value());
    auto& reading_2 = reading_2_opt.value();
    zassert_equal(reading_2.status, ReadingStatus::INTERPOLATED);
    zassert_true(reading_2.value.has_value());
    zassert_true(reading_2.timestamp.has_value());
    zassert_between_inclusive(reading_2.value.value(), 0.0F, 200.0F);

    auto reading_1_opt = sensor_readings_frame->TryGetReading("sensor_1");
    zassert_true(reading_1_opt.has_value());
    auto& reading_1 = reading_1_opt.value();
    zassert_equal(reading_1.status, ReadingStatus::INTERPOLATED);
    zassert_true(reading_1.value.has_value());
    zassert_true(reading_1.timestamp.has_value());
    zassert_between_inclusive(reading_1.value.value(), 0.0F, 5.0F);

    auto reading_3_opt = sensor_readings_frame->TryGetReading("sensor_3");
    zassert_true(reading_3_opt.has_value());
    const auto& reading_3 = reading_3_opt.value();
    zassert_equal(reading_3.status, ReadingStatus::UNINITIALIZED);
    zassert_true(reading_3.timestamp.has_value());
    zassert_false(reading_3.value.has_value());

    auto reading_4_opt = sensor_readings_frame->TryGetReading("sensor_4");
    zassert_true(reading_4_opt.has_value());
    auto& reading_4 = reading_4_opt.value();
    zassert_equal(reading_4.status, ReadingStatus::RAW);
    zassert_true(reading_4.timestamp.has_value());
    zassert_true(reading_4.value.has_value());
    zassert_true(reading_4.value.value() == 1 || reading_4.value.value() == 0);

    auto reading_5_opt = sensor_readings_frame->TryGetReading("sensor_5");
    zassert_true(reading_5_opt.has_value());
    const auto& reading_5 = reading_5_opt.value();
    zassert_equal(reading_5.status, ReadingStatus::UNINITIALIZED);
    zassert_true(reading_5.timestamp.has_value());
    zassert_false(reading_5.value.has_value());
}

namespace {

// Records what a reader asks for instead of converting anything.
class RecordingAdc : public IAdc {
public:
    uint16_t last_samples = 0;

    bool Initialize() override {
        return true;
    }

    void Sample(const AdcSampleRequest& request) noexcept override {
        last_samples = request.samples;
        for(auto& result : request.results)
            result = { .volts = 1.0F };
    }

    [[nodiscard]] int GetChannelCount() const override {
        return 8;
    }
    [[nodiscard]] AdcCapabilities GetCapabilities() const override {
        return {};
    }
    [[nodiscard]] AdcStats GetStats() const override {
        return {};
    }
};

} // namespace

ZTEST(sensors_reader, test_PhysicalAnalog_rebuilt_reader_takes_the_new_adc_samples) {
    DtFs::InitInternalFs();
    const auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    const auto recording_adc = std::make_shared<RecordingAdc>();
    const auto adc_manager = std::make_shared<AdcManager>(std::vector<std::shared_ptr<IAdc>>{ recording_adc });
    adc_manager->Initialize();

    auto adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
        std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service), adc_manager
    );

    auto adc_configuration = SensorsReaderGetTestConfiguration();
    adc_configuration.samples = 40;
    zassert_true(adc_configuration_manager->Update(adc_configuration));

    const auto time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>()
    );

    const auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_1");
    sensor->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor->configuration.channel = 0;
    sensor->configuration.sampling_rate_ms = 1000;
    sensor->configuration.interpolation_method = InterpolationMethod::LINEAR;
    sensor->configuration.calibration_table = { { 0.0F, 0.0F }, { 3.3F, 5.0F } };

    const auto sensor_readings_frame = make_shared_pmr<SensorReadingsFrame>(Mrm::GetDefaultPmr());
    auto generation = SensorPipelineBuilder(nullptr, sensor_readings_frame)
                          .Build(
                              std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(
                                  std::vector<std::shared_ptr<Sensor>>{ sensor }
                              )
                          );

    // Readers reserve their slot with the samples count of the configuration they are built with,
    // so the handler rebuilds the sample set and the reader, as a processing restart does.
    SensorReaderFactory sensor_reader_factory(time_service, nullptr, adc_manager);
    std::unique_ptr<AdcSampleSet> adc_samples;
    std::unique_ptr<ISensorReader> reader;
    const auto build_reader = [&] {
        reader = nullptr;
        adc_samples = std::make_unique<AdcSampleSet>();

        RateGroupBuildContext context{ .adc_samples = *adc_samples,
            .adc_configuration = adc_configuration_manager->Get().get() };
        reader = sensor_reader_factory.Create(generation->runtimes[0], context);
        adc_samples->Seal();
    };

    build_reader();
    adc_configuration_manager->RegisterConfigurationUpdatedHandler(build_reader);

    adc_samples->Acquire();
    reader->Read({ time_service->GetCurrentTime() });
    zassert_equal(recording_adc->last_samples, 40);

    adc_configuration.samples = 10;
    zassert_true(adc_configuration_manager->Update(adc_configuration));

    adc_samples->Acquire();
    reader->Read({ time_service->GetCurrentTime() });
    zassert_equal(recording_adc->last_samples, 10, "the rebuilt reader asks for the new samples count");

    adc_configuration_manager->RegisterConfigurationUpdatedHandler(nullptr);
}

namespace {

// One analog sensor on channel 1 of a scripted four-channel ADC, its reader built the way a rate
// group builds it, against a configuration made by hand.
struct SensorsReaderAnalogReader {
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<processing_test_support::FakeAdc> adc;
    std::shared_ptr<SensorGeneration> generation;
    std::unique_ptr<AdcSampleSet> adc_samples;
    std::unique_ptr<ISensorReader> reader;
};

SensorsReaderAnalogReader SensorsReaderMakeAnalogReader(const std::shared_ptr<AdcCalibrator>& calibrator) {
    SensorsReaderAnalogReader fixture;

    fixture.time_service = processing_test_support::MakeTimeService();
    fixture.adc = std::make_shared<processing_test_support::FakeAdc>(4);
    const auto adc_manager = processing_test_support::MakeAdcManager(fixture.adc);

    const auto sensor_readings_frame = make_shared_pmr<SensorReadingsFrame>(Mrm::GetDefaultPmr());
    fixture.generation =
        SensorPipelineBuilder(nullptr, sensor_readings_frame)
            .Build(
                std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(std::vector<std::shared_ptr<Sensor>>{
                    processing_test_support::MakeAnalogSensor("analog", 1, 100) })
            );
    fixture.generation->adc_configuration = processing_test_support::MakeAdcConfiguration(4, 8, calibrator);

    fixture.adc_samples = std::make_unique<AdcSampleSet>();
    RateGroupBuildContext context{ .adc_samples = *fixture.adc_samples,
        .adc_configuration = fixture.generation->adc_configuration.get() };

    SensorReaderFactory sensor_reader_factory(fixture.time_service, nullptr, adc_manager);
    fixture.reader = sensor_reader_factory.Create(fixture.generation->runtimes[0], context);
    zassert_not_null(fixture.reader.get());

    fixture.adc_samples->Seal();

    return fixture;
}

} // namespace

ZTEST(sensors_reader, test_Read_analog_reports_a_failed_sample) {
    auto fixture = SensorsReaderMakeAnalogReader(
        processing_test_support::MakeCalibrator({ { 0.0F, 0.0F }, { 5.0F, 5.0F } })
    );
    fixture.adc->failing_channel = 1;

    fixture.adc_samples->Acquire();
    zassert_equal(fixture.adc->requests, 1);

    const TickContext tick{ fixture.time_service->GetCurrentTime() };
    const SensorReading reading = fixture.reader->Read(tick);

    zassert_equal(reading.status, ReadingStatus::ERROR);
    zassert_equal(reading.error, ReadingError::READER_FAILED);
    zassert_false(reading.value.has_value(), "A failed conversion has no value, not 0");
    zassert_false(reading.calibrated_voltage.has_value());
    zassert_false(reading.adc_voltage.has_value());
    zassert_equal(reading.source, ReadingSource::PROCESSING);
    zassert_true(reading.timestamp.has_value());
    zassert_equal(reading.timestamp.value(), tick.timestamp);
}

ZTEST(sensors_reader, test_Read_analog_calibrates_a_valid_sample) {
    // Identity range, and a calibration that recorded 2.5 V for a true 5 V input: 2 V at the pin is 4 V.
    auto fixture = SensorsReaderMakeAnalogReader(
        processing_test_support::MakeCalibrator(
            { { 0.0F, 0.0F }, { 5.0F, 2.5F } }, { .adc_max_v = 5.0F, .sensor_max_v = 5.0F }
        )
    );
    fixture.adc->volts_base = 2.0F;

    fixture.adc_samples->Acquire();
    zassert_equal(fixture.adc->requests, 1);
    zassert_equal(fixture.adc->last_channel_count, 1);
    zassert_equal(fixture.adc->last_channels[0], 1);
    zassert_equal(fixture.adc->last_samples, 8, "The configuration's samples count");

    const TickContext tick{ fixture.time_service->GetCurrentTime() };
    const SensorReading reading = fixture.reader->Read(tick);

    zassert_equal(reading.status, ReadingStatus::INTERPOLATED);
    zassert_true(reading.calibrated_voltage.has_value());
    zassert_within(reading.calibrated_voltage.value(), 4.0F, 1e-4F);
    // The sample as the ADC measured it, before the calibration moved it.
    zassert_true(reading.adc_voltage.has_value());
    zassert_within(reading.adc_voltage.value(), 2.0F, 1e-4F);
    zassert_true(std::abs(reading.adc_voltage.value() - reading.calibrated_voltage.value()) > 1.0F);
    // The sensor reads 0..5 V as 0..100.
    zassert_true(reading.value.has_value());
    zassert_within(reading.value.value(), 80.0F, 1e-3F);
    zassert_equal(reading.raw_value.value(), reading.value.value());
    zassert_equal(reading.timestamp.value(), tick.timestamp);
}

// The PM board's split: global channel 0 on a one-channel ADC, global channels 1-3 on a second
// ADC. The sample set keeps the channel each sensor is configured with, not the ADC-local one.
ZTEST(sensors_reader, test_Create_analog_reserves_the_configured_global_channel) {
    const auto time_service = processing_test_support::MakeTimeService();
    const auto unit_1 = std::make_shared<processing_test_support::FakeAdc>(1);
    const auto unit_2 = std::make_shared<processing_test_support::FakeAdc>(3);
    const auto adc_manager = std::make_shared<AdcManager>(std::vector<std::shared_ptr<IAdc>>{ unit_1, unit_2 });
    zassert_true(adc_manager->Initialize());

    const auto sensor_readings_frame = make_shared_pmr<SensorReadingsFrame>(Mrm::GetDefaultPmr());
    const auto generation =
        SensorPipelineBuilder(nullptr, sensor_readings_frame)
            .Build(
                std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(std::vector<std::shared_ptr<Sensor>>{
                    processing_test_support::MakeAnalogSensor("analog_3", 3, 100),
                    processing_test_support::MakeAnalogSensor("analog_0", 0, 100),
                    processing_test_support::MakeAnalogSensor("analog_1", 1, 100) })
            );
    generation->adc_configuration = processing_test_support::MakeAdcConfiguration(
        4, 8, processing_test_support::MakeCalibrator({ { 0.0F, 0.0F }, { 5.0F, 5.0F } })
    );

    AdcSampleSet adc_samples;
    RateGroupBuildContext context{ .adc_samples = adc_samples,
        .adc_configuration = generation->adc_configuration.get() };

    SensorReaderFactory sensor_reader_factory(time_service, nullptr, adc_manager);
    std::vector<std::unique_ptr<ISensorReader>> readers;
    for(const auto& runtime : generation->runtimes)
        readers.push_back(sensor_reader_factory.Create(runtime, context));

    adc_samples.Seal();
    zassert_equal(adc_samples.GetAdcCount(), 2);

    // Whichever ADC the pipeline's order reserved first.
    const bool is_second_unit_first = adc_samples.GetAdc(0).adc == unit_2.get();
    const auto second_unit = adc_samples.GetAdc(is_second_unit_first ? 0 : 1);
    const auto first_unit = adc_samples.GetAdc(is_second_unit_first ? 1 : 0);

    zassert_equal(second_unit.adc, unit_2.get());
    zassert_equal(second_unit.channels.size(), 2);
    zassert_equal(second_unit.channels[0], 0);
    zassert_equal(second_unit.global_channels[0], 1);
    zassert_equal(second_unit.channels[1], 2);
    zassert_equal(second_unit.global_channels[1], 3);

    zassert_equal(first_unit.adc, unit_1.get());
    zassert_equal(first_unit.channels.size(), 1);
    zassert_equal(first_unit.channels[0], 0);
    zassert_equal(first_unit.global_channels[0], 0);
}
