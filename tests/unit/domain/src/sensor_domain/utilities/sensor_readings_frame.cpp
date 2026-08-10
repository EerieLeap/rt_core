#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/guid/guid_generator.h"
#include "utilities/string/string_helpers.h"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"
#include "subsys/math_parser/expression_evaluator.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::guid;
using namespace eerie_leap::utilities::string;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::subsys::math_parser;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;

ZTEST_SUITE(sensor_readings_frame, NULL, NULL, NULL, NULL, NULL);

std::vector<std::shared_ptr<Sensor>> sensor_readings_frame_GetTestSensors() {
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
    sensor_1->configuration.voltage_interpolator = make_unique_pmr<LinearVoltageInterpolator>(Mrm::GetDefaultPmr(), calibration_data_1_ptr);
    sensor_1->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), "x * 2 + sensor_2 + 1");

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
    sensor_2->configuration.voltage_interpolator = make_unique_pmr<LinearVoltageInterpolator>(Mrm::GetDefaultPmr(), calibration_data_2_ptr);
    sensor_2->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), "x * 4 + 1.6");

    auto sensor_3 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_3");

    sensor_3->metadata.name = "Sensor 3";
    sensor_3->metadata.unit = "km/h";
    sensor_3->metadata.description = "Test Sensor 3";

    sensor_3->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor_3->configuration.channel = std::nullopt;
    sensor_3->configuration.sampling_rate_ms = 2000;
    sensor_3->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), "sensor_1 + 8.34");

    std::vector<std::shared_ptr<Sensor>> sensors = {
        sensor_1, sensor_2, sensor_3 };

    return sensors;
}

struct sensor_readings_frame_GetTestSensors_HelperInstances {
    std::shared_ptr<GuidGenerator> guid_generator;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame;
};

sensor_readings_frame_GetTestSensors_HelperInstances sensor_readings_frame_GetHelperInstances() {
    auto guid_generator = std::make_shared<GuidGenerator>();
    auto sensor_readings_frame = make_shared_pmr<SensorReadingsFrame>(Mrm::GetDefaultPmr());

    return sensor_readings_frame_GetTestSensors_HelperInstances {
        .guid_generator = guid_generator,
        .sensor_readings_frame = sensor_readings_frame
    };
}

ZTEST(sensor_readings_frame, test_AddOrUpdateReading) {
        auto helper = sensor_readings_frame_GetHelperInstances();

        auto guid_generator = helper.guid_generator;
        auto sensor_readings_frame = helper.sensor_readings_frame;
        auto sensors = sensor_readings_frame_GetTestSensors();

        SensorReading reading1(guid_generator->Generate(), sensors[1]);
        reading1.source = ReadingSource::PROCESSING;
        sensor_readings_frame->AddOrUpdateReading(reading1);

        zassert_equal(sensor_readings_frame->HasReading(sensors[1]->id_hash), true);

        auto fr_reading_1 = sensor_readings_frame->TryGetReading("sensor_2");
        zassert_equal(fr_reading_1.value().status, ReadingStatus::UNINITIALIZED);
        zassert_false(fr_reading_1.value().value.has_value());

        SensorReading reading2(guid_generator->Generate(), sensors[1]);
        reading2.source = ReadingSource::PROCESSING;
        reading2.status = ReadingStatus::PROCESSED;
        reading2.value = 1.6;
        sensor_readings_frame->AddOrUpdateReading(reading2);

        zassert_equal(sensor_readings_frame->HasReading(sensors[1]->id_hash), true);

        auto fr_reading_2 = sensor_readings_frame->TryGetReading("sensor_2");
        zassert_true(fr_reading_2.has_value());
        zassert_equal(fr_reading_2.value().status, ReadingStatus::PROCESSED);
        zassert_true(fr_reading_2.value().value.has_value());

        SensorReading reading3(guid_generator->Generate(), sensors[2]);
        reading3.source = ReadingSource::PROCESSING;
        sensor_readings_frame->AddOrUpdateReading(reading3);

        zassert_equal(sensor_readings_frame->HasReading(sensors[2]->id_hash), true);
}

ZTEST(sensor_readings_frame, test_GetReading) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading reading1(guid_generator->Generate(), sensors[1]);
    reading1.source = ReadingSource::PROCESSING;
    sensor_readings_frame->AddOrUpdateReading(reading1);
    SensorReading reading2(guid_generator->Generate(), sensors[2]);
    reading2.source = ReadingSource::PROCESSING;
    sensor_readings_frame->AddOrUpdateReading(reading2);

    auto rf_reading1 = sensor_readings_frame->TryGetReading("sensor_2");
    zassert_true(rf_reading1.has_value());
    zassert_str_equal(rf_reading1.value().sensor->id.c_str(), "sensor_2");

    auto rf_reading2 = sensor_readings_frame->TryGetReading("sensor_3");
    zassert_true(rf_reading2.has_value());
    zassert_str_equal(rf_reading2.value().sensor->id.c_str(), "sensor_3");
}

ZTEST(sensor_readings_frame, test_GetReading_no_sensor) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    auto reading = sensor_readings_frame->TryGetReading("sensor_2");
    zassert_false(reading.has_value());
}

ZTEST(sensor_readings_frame, test_ClearReadings) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    auto readings = sensor_readings_frame->GetProcessedReadings();
    zassert_equal(readings.size(), 0);

    SensorReading reading1(guid_generator->Generate(), sensors[0]);
    reading1.source = ReadingSource::ISR;
    reading1.value = 2.4;
    reading1.status = ReadingStatus::RAW;
    sensor_readings_frame->AddOrUpdateReading(reading1);

    SensorReading reading2(guid_generator->Generate(), sensors[1]);
    reading2.source = ReadingSource::PROCESSING;
    reading2.status = ReadingStatus::ERROR;
    sensor_readings_frame->AddOrUpdateReading(reading2);

    SensorReading reading3(guid_generator->Generate(), sensors[2]);
    reading3.source = ReadingSource::PROCESSING;
    reading3.value = 2.6;
    reading3.status = ReadingStatus::PROCESSED;
    sensor_readings_frame->AddOrUpdateReading(reading3);

    zassert_equal(sensor_readings_frame->HasIsrReading(sensors[0]->id_hash), true);
    zassert_equal(sensor_readings_frame->HasIsrReading(sensors[1]->id_hash), false);
    zassert_equal(sensor_readings_frame->HasIsrReading(sensors[2]->id_hash), false);

    zassert_equal(sensor_readings_frame->HasReading(sensors[0]->id_hash), false);
    zassert_equal(sensor_readings_frame->HasReading(sensors[1]->id_hash), true);
    zassert_equal(sensor_readings_frame->HasReading(sensors[2]->id_hash), true);

    readings = sensor_readings_frame->GetProcessedReadings();
    zassert_equal(readings.size(), 1);
    zassert_equal(readings.contains(sensors[2]->id_hash), true);

    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[0]->id_hash).has_value(), false);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).has_value(), false);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[2]->id_hash).has_value(), true);

    sensor_readings_frame->ClearReadings();

    zassert_equal(sensor_readings_frame->HasIsrReading(sensors[0]->id_hash), false);
    zassert_equal(sensor_readings_frame->HasIsrReading(sensors[1]->id_hash), false);
    zassert_equal(sensor_readings_frame->HasIsrReading(sensors[2]->id_hash), false);

    zassert_equal(sensor_readings_frame->HasReading(sensors[0]->id_hash), false);
    zassert_equal(sensor_readings_frame->HasReading(sensors[1]->id_hash), false);
    zassert_equal(sensor_readings_frame->HasReading(sensors[2]->id_hash), false);

    readings = sensor_readings_frame->GetProcessedReadings();
    zassert_equal(readings.size(), 0);

    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[0]->id_hash).has_value(), false);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).has_value(), false);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[2]->id_hash).has_value(), false);
}

ZTEST(sensor_readings_frame, test_TryGetIsrReading) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    zassert_false(sensor_readings_frame->TryGetIsrReading(sensors[0]->id_hash).has_value());
    zassert_false(sensor_readings_frame->TryGetIsrReading("sensor_1").has_value());

    SensorReading reading(guid_generator->Generate(), sensors[0]);
    reading.source = ReadingSource::ISR;
    reading.status = ReadingStatus::RAW;
    reading.value = 2.4;
    sensor_readings_frame->AddOrUpdateReading(reading);

    auto by_hash = sensor_readings_frame->TryGetIsrReading(sensors[0]->id_hash);
    auto by_id = sensor_readings_frame->TryGetIsrReading("sensor_1");

    zassert_true(by_hash.has_value());
    zassert_true(by_id.has_value());
    zassert_str_equal(by_hash.value().sensor->id.c_str(), "sensor_1");
    zassert_equal(by_hash.value().status, ReadingStatus::RAW);
    zassert_equal(by_id.value().id.AsUint64(), by_hash.value().id.AsUint64());

    // An ISR reading is not visible through the processing readings.
    zassert_false(sensor_readings_frame->TryGetReading("sensor_1").has_value());
}

ZTEST(sensor_readings_frame, test_TryGetReading_hash_and_id_overloads_agree) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading reading(guid_generator->Generate(), sensors[1]);
    reading.source = ReadingSource::PROCESSING;
    reading.status = ReadingStatus::PROCESSED;
    reading.value = 3.5;
    sensor_readings_frame->AddOrUpdateReading(reading);

    auto by_hash = sensor_readings_frame->TryGetReading(sensors[1]->id_hash);
    auto by_id = sensor_readings_frame->TryGetReading("sensor_2");

    zassert_true(by_hash.has_value());
    zassert_true(by_id.has_value());
    zassert_equal(by_hash.value().id.AsUint64(), by_id.value().id.AsUint64());

    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).value(), 3.5F);
    zassert_equal(sensor_readings_frame->TryGetReadingValue("sensor_2").value(), 3.5F);
    zassert_false(sensor_readings_frame->TryGetReadingValue("sensor_3").has_value());
}

ZTEST(sensor_readings_frame, test_GetReadingValuePtr) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    zassert_is_null(sensor_readings_frame->GetReadingValuePtr("sensor_2"));

    SensorReading reading(guid_generator->Generate(), sensors[1]);
    reading.source = ReadingSource::PROCESSING;
    reading.status = ReadingStatus::PROCESSED;
    reading.value = 1.5;
    sensor_readings_frame->AddOrUpdateReading(reading);

    float* value_ptr = sensor_readings_frame->GetReadingValuePtr("sensor_2");

    zassert_not_null(value_ptr);
    zassert_equal(*value_ptr, 1.5F);

    // The pointer aliases the stored value so evaluators observe later updates.
    SensorReading update(guid_generator->Generate(), sensors[1]);
    update.source = ReadingSource::PROCESSING;
    update.status = ReadingStatus::PROCESSED;
    update.value = 9.5;
    sensor_readings_frame->AddOrUpdateReading(update);

    zassert_equal(*value_ptr, 9.5F);
    zassert_equal(sensor_readings_frame->GetReadingValuePtr("sensor_2"), value_ptr);
}

ZTEST(sensor_readings_frame, test_AddOrUpdateReading_ignores_unset_source) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading reading(guid_generator->Generate(), sensors[0]);
    reading.status = ReadingStatus::PROCESSED;
    reading.value = 1.0;
    sensor_readings_frame->AddOrUpdateReading(reading);

    zassert_false(sensor_readings_frame->HasReading(sensors[0]->id_hash));
    zassert_false(sensor_readings_frame->HasIsrReading(sensors[0]->id_hash));
    zassert_equal(sensor_readings_frame->GetProcessedReadings().size(), 0);
}

ZTEST(sensor_readings_frame, test_processing_reading_supersedes_isr_reading) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading isr_reading(guid_generator->Generate(), sensors[0]);
    isr_reading.source = ReadingSource::ISR;
    isr_reading.status = ReadingStatus::RAW;
    sensor_readings_frame->AddOrUpdateReading(isr_reading);

    zassert_true(sensor_readings_frame->HasIsrReading(sensors[0]->id_hash));

    SensorReading processing_reading(guid_generator->Generate(), sensors[0]);
    processing_reading.source = ReadingSource::PROCESSING;
    processing_reading.status = ReadingStatus::PROCESSED;
    processing_reading.value = 4.2;
    sensor_readings_frame->AddOrUpdateReading(processing_reading);

    zassert_false(sensor_readings_frame->HasIsrReading(sensors[0]->id_hash));
    zassert_true(sensor_readings_frame->HasReading(sensors[0]->id_hash));
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[0]->id_hash).value(), 4.2F);
}

ZTEST(sensor_readings_frame, test_isr_reading_publishes_processed_value) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading reading(guid_generator->Generate(), sensors[0]);
    reading.source = ReadingSource::ISR;
    reading.status = ReadingStatus::PROCESSED;
    reading.value = 7.25;
    sensor_readings_frame->AddOrUpdateReading(reading);

    zassert_true(sensor_readings_frame->HasIsrReading(sensors[0]->id_hash));
    zassert_equal(sensor_readings_frame->GetProcessedReadings().size(), 1);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[0]->id_hash).value(), 7.25F);
}

ZTEST(sensor_readings_frame, test_processed_reading_requires_a_value) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading reading(guid_generator->Generate(), sensors[1]);
    reading.source = ReadingSource::PROCESSING;
    reading.status = ReadingStatus::PROCESSED;
    sensor_readings_frame->AddOrUpdateReading(reading);

    zassert_true(sensor_readings_frame->HasReading(sensors[1]->id_hash));
    zassert_equal(sensor_readings_frame->GetProcessedReadings().size(), 0);
    zassert_false(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).has_value());
}

ZTEST(sensor_readings_frame, test_failed_update_keeps_last_processed_value) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading processed(guid_generator->Generate(), sensors[1]);
    processed.source = ReadingSource::PROCESSING;
    processed.status = ReadingStatus::PROCESSED;
    processed.value = 5.5;
    sensor_readings_frame->AddOrUpdateReading(processed);

    SensorReading failed(guid_generator->Generate(), sensors[1]);
    failed.source = ReadingSource::PROCESSING;
    failed.status = ReadingStatus::ERROR;
    sensor_readings_frame->AddOrUpdateReading(failed);

    zassert_equal(sensor_readings_frame->TryGetReading(sensors[1]->id_hash).value().status, ReadingStatus::ERROR);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).value(), 5.5F);
    zassert_equal(sensor_readings_frame->GetProcessedReadings().size(), 1);
}

ZTEST(sensor_readings_frame, test_GetProcessedReadings_returns_a_snapshot) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading reading(guid_generator->Generate(), sensors[1]);
    reading.source = ReadingSource::PROCESSING;
    reading.status = ReadingStatus::PROCESSED;
    reading.value = 2.0;
    sensor_readings_frame->AddOrUpdateReading(reading);

    auto snapshot = sensor_readings_frame->GetProcessedReadings();
    snapshot.clear();

    zassert_equal(sensor_readings_frame->GetProcessedReadings().size(), 1);
}

ZTEST(sensor_readings_frame, test_ClearProcessedReadings) {
    auto helper = sensor_readings_frame_GetHelperInstances();

    auto guid_generator = helper.guid_generator;
    auto sensor_readings_frame = helper.sensor_readings_frame;
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading reading(guid_generator->Generate(), sensors[1]);
    reading.source = ReadingSource::PROCESSING;
    reading.status = ReadingStatus::PROCESSED;
    reading.value = 2.0;
    sensor_readings_frame->AddOrUpdateReading(reading);

    sensor_readings_frame->ClearProcessedReadings();

    zassert_equal(sensor_readings_frame->GetProcessedReadings().size(), 0);

    // Only the processed snapshot is dropped; the reading and its value survive.
    zassert_true(sensor_readings_frame->HasReading(sensors[1]->id_hash));
    zassert_true(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).has_value());
}
