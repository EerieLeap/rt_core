#include <array>
#include <cmath>
#include <optional>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"
#include "utilities/voltage_interpolator/interpolation_method.h"
#include "subsys/canbus/can_frame.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sensor_limits.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::string;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::subsys::canbus;
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
    sensor_2->configuration.interpolation_method = InterpolationMethod::LINEAR;
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

    std::vector<std::shared_ptr<Sensor>> sensors = {
        sensor_1, sensor_2, sensor_3 };

    return sensors;
}

namespace {

// The frame holds one slot per configured sensor; readings of other sensors are ignored.
std::shared_ptr<SensorReadingsFrame> MakeFrame(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    auto sensor_readings_frame = std::make_shared<SensorReadingsFrame>();
    sensor_readings_frame->Configure(sensors);

    return sensor_readings_frame;
}

SensorReading MakeReading(const std::shared_ptr<Sensor>& sensor, ReadingSource source, ReadingStatus status, std::optional<float> value = std::nullopt) {
    SensorReading reading(sensor.get());
    reading.source = source;
    reading.status = status;
    reading.value = value;

    return reading;
}

} // namespace

ZTEST(sensor_readings_frame, test_SensorReading_is_trivially_copyable_and_carries_its_hash) {
    auto sensors = sensor_readings_frame_GetTestSensors();

    SensorReading reading(sensors[1].get());

    zassert_true(std::is_trivially_copyable_v<SensorReading>);
    zassert_equal(reading.sensor_id_hash, sensors[1]->id_hash);
    zassert_equal(reading.sensor, sensors[1].get());
    zassert_equal(reading.status, ReadingStatus::UNINITIALIZED);
    zassert_equal(reading.error, ReadingError::NONE);
    zassert_false(reading.value.has_value());
    zassert_is_null(reading.can_frame);
}

ZTEST(sensor_readings_frame, test_AddOrUpdateReading) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::UNINITIALIZED));

    zassert_true(sensor_readings_frame->HasReading(sensors[1]->id_hash));

    auto fr_reading_1 = sensor_readings_frame->TryGetReading("sensor_2");
    zassert_equal(fr_reading_1.value().status, ReadingStatus::UNINITIALIZED);
    zassert_false(fr_reading_1.value().value.has_value());

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 1.6F));

    auto fr_reading_2 = sensor_readings_frame->TryGetReading("sensor_2");
    zassert_true(fr_reading_2.has_value());
    zassert_equal(fr_reading_2.value().status, ReadingStatus::PROCESSED);
    zassert_equal(fr_reading_2.value().value.value(), 1.6F);
    zassert_str_equal(fr_reading_2.value().sensor->id.c_str(), "sensor_2");

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[2], ReadingSource::PROCESSING, ReadingStatus::UNINITIALIZED));

    zassert_true(sensor_readings_frame->HasReading(sensors[2]->id_hash));
    zassert_false(sensor_readings_frame->TryGetReading("sensor_1").has_value());
}

ZTEST(sensor_readings_frame, test_AddOrUpdateReading_ignores_unset_source) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[0], ReadingSource::NONE, ReadingStatus::PROCESSED, 1.0F));

    zassert_false(sensor_readings_frame->HasReading(sensors[0]->id_hash));
    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 0);
}

ZTEST(sensor_readings_frame, test_isr_and_processing_readings_share_one_slot) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[0], ReadingSource::ISR, ReadingStatus::PROCESSED, 7.25F));

    zassert_equal(sensor_readings_frame->TryGetReading(sensors[0]->id_hash).value().source, ReadingSource::ISR);
    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 1);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[0]->id_hash).value(), 7.25F);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[0], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 4.2F));

    zassert_equal(sensor_readings_frame->TryGetReading(sensors[0]->id_hash).value().source, ReadingSource::PROCESSING);
    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 1);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[0]->id_hash).value(), 4.2F);
}

ZTEST(sensor_readings_frame, test_TryGetReading_hash_and_id_overloads_agree) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 3.5F));

    auto by_hash = sensor_readings_frame->TryGetReading(sensors[1]->id_hash);
    auto by_id = sensor_readings_frame->TryGetReading("sensor_2");

    zassert_true(by_hash.has_value());
    zassert_true(by_id.has_value());
    zassert_equal(by_hash.value().sensor_id_hash, by_id.value().sensor_id_hash);
    zassert_equal(by_hash.value().value.value(), by_id.value().value.value());

    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).value(), 3.5F);
    zassert_equal(sensor_readings_frame->TryGetReadingValue("sensor_2").value(), 3.5F);
    zassert_false(sensor_readings_frame->TryGetReadingValue("sensor_3").has_value());
}

ZTEST(sensor_readings_frame, test_processed_reading_without_a_value_has_no_value) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED));

    zassert_true(sensor_readings_frame->HasReading(sensors[1]->id_hash));
    // A raw CAN frame reading is processed without a value; it reaches the loggers, not the value store.
    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 1);
    zassert_false(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).has_value());
}

ZTEST(sensor_readings_frame, test_failed_update_keeps_last_processed_value) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 5.5F));

    SensorReading failed = MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::UNINITIALIZED);
    failed.SetError(ReadingError::EXPRESSION_FAILED);
    sensor_readings_frame->AddOrUpdateReading(failed);

    const auto reading = sensor_readings_frame->TryGetReading(sensors[1]->id_hash).value();
    zassert_equal(reading.status, ReadingStatus::ERROR);
    zassert_equal(reading.error, ReadingError::EXPRESSION_FAILED);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).value(), 5.5F);

    std::array<SensorReading, 4> snapshot;
    zassert_equal(sensor_readings_frame->SnapshotProcessedReadings(snapshot), 1);
    zassert_equal(snapshot[0].value.value(), 5.5F, "The last processed reading survives a failed update");
}

ZTEST(sensor_readings_frame, test_SnapshotProcessedReadings_copies_every_processed_reading) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[0], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 1.0F));
    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::RAW, 2.0F));
    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[2], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 3.0F));

    std::array<SensorReading, 4> snapshot;
    const size_t count = sensor_readings_frame->SnapshotProcessedReadings(snapshot);

    zassert_equal(count, 2);
    for(size_t i = 0; i < count; i++)
        zassert_true(snapshot[i].sensor_id_hash == sensors[0]->id_hash || snapshot[i].sensor_id_hash == sensors[2]->id_hash);

    // Snapshots are copies and repeatable.
    snapshot[0].value = 99.0F;
    zassert_equal(sensor_readings_frame->SnapshotProcessedReadings(snapshot), 2);
    zassert_not_equal(snapshot[0].value.value(), 99.0F);

    // A buffer that is too small copies what fits.
    std::array<SensorReading, 1> small;
    zassert_equal(sensor_readings_frame->SnapshotProcessedReadings(small), 1);
}

ZTEST(sensor_readings_frame, test_TakeProcessedReadings_returns_each_update_once) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    std::array<SensorReading, 4> taken;
    zassert_equal(sensor_readings_frame->TakeProcessedReadings(taken), 0);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 2.0F));

    zassert_equal(sensor_readings_frame->TakeProcessedReadings(taken), 1);
    zassert_equal(taken[0].sensor_id_hash, sensors[1]->id_hash);
    zassert_equal(taken[0].value.value(), 2.0F);

    // Already taken, and the latest processed reading is still available to other consumers.
    zassert_equal(sensor_readings_frame->TakeProcessedReadings(taken), 0);
    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 1);
    zassert_equal(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).value(), 2.0F);

    // A new reading of the same sensor is reported again, with its latest value; unprocessed ones are not.
    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 3.0F));
    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[2], ReadingSource::PROCESSING, ReadingStatus::RAW, 1.0F));

    zassert_equal(sensor_readings_frame->TakeProcessedReadings(taken), 1);
    zassert_equal(taken[0].value.value(), 3.0F);
}

ZTEST(sensor_readings_frame, test_TakeProcessedReadings_keeps_what_does_not_fit_pending) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    for(const auto& sensor : sensors)
        sensor_readings_frame->AddOrUpdateReading(MakeReading(sensor, ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 1.0F));

    std::array<SensorReading, 2> taken;
    zassert_equal(sensor_readings_frame->TakeProcessedReadings(taken), 2);
    zassert_equal(sensor_readings_frame->TakeProcessedReadings(taken), 1);
    zassert_equal(sensor_readings_frame->TakeProcessedReadings(taken), 0);
}

ZTEST(sensor_readings_frame, test_GetReadingValuePtr) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    // A slot exists before the first reading, so an evaluator never binds a null pointer.
    float* value_ptr = sensor_readings_frame->GetReadingValuePtr("sensor_2");
    zassert_not_null(value_ptr);
    zassert_true(std::isnan(*value_ptr));
    zassert_false(sensor_readings_frame->TryGetReadingValue("sensor_2").has_value(), "An empty slot is not a value");

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 1.5F));

    zassert_equal(sensor_readings_frame->GetReadingValuePtr("sensor_2"), value_ptr);
    zassert_equal(*value_ptr, 1.5F);
    zassert_equal(sensor_readings_frame->TryGetReadingValue("sensor_2").value(), 1.5F);

    // The pointer aliases the stored value so evaluators observe later updates.
    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 9.5F));

    zassert_equal(*value_ptr, 9.5F);
    zassert_equal(sensor_readings_frame->GetReadingValuePtr("sensor_2"), value_ptr);

    // Clearing resets the slot instead of freeing it, so bound evaluators stay valid.
    sensor_readings_frame->ClearReadings();

    zassert_equal(sensor_readings_frame->GetReadingValuePtr("sensor_2"), value_ptr);
    zassert_true(std::isnan(*value_ptr));
    zassert_false(sensor_readings_frame->TryGetReadingValue("sensor_2").has_value());
}

ZTEST(sensor_readings_frame, test_GetReadingValues_reads_each_hash_in_order) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[0], ReadingSource::ISR, ReadingStatus::PROCESSED, 1.5F));
    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::UNINITIALIZED));
    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[2], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 3.25F));

    const std::array<uint32_t, 4> hashes = {
        sensors[2]->id_hash, sensors[1]->id_hash, StringHelpers::GetHash("unknown"), sensors[0]->id_hash };
    std::array<std::optional<float>, 4> values;
    values.fill(0.0F);

    sensor_readings_frame->GetReadingValues(hashes, values);

    zassert_equal(values[0].value(), 3.25F);
    zassert_false(values[1].has_value(), "A sensor without a processed value is empty");
    zassert_false(values[2].has_value());
    zassert_equal(values[3].value(), 1.5F);
}

ZTEST(sensor_readings_frame, test_can_frame_is_stored_apart_from_the_reading) {
    auto sensors = sensor_readings_frame_GetTestSensors();

    // Only a raw CAN sensor gets a frame slot.
    auto raw = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "raw");
    raw->configuration.type = SensorType::CANBUS_RAW;
    auto sensor_readings_frame = MakeFrame({ raw, sensors[1] });

    CanFrame can_frame;
    can_frame.id = 0x123;
    can_frame.data = { 1, 2, 3, 4 };

    SensorReading reading = MakeReading(raw, ReadingSource::ISR, ReadingStatus::PROCESSED);
    reading.can_frame = &can_frame;
    sensor_readings_frame->AddOrUpdateReading(reading);

    // The store keeps a copy and never hands the caller's pointer back out.
    can_frame.data = { 9 };
    zassert_is_null(sensor_readings_frame->TryGetReading(raw->id_hash).value().can_frame);

    CanFrame stored;
    zassert_true(sensor_readings_frame->TryGetCanFrame(raw->id_hash, stored));
    zassert_equal(stored.id, 0x123);
    zassert_equal(stored.data.size(), 4);
    zassert_equal(stored.data[3], 4);

    // A sensor of another kind has no frame slot, even with a frame on its reading.
    SensorReading other = MakeReading(sensors[1], ReadingSource::ISR, ReadingStatus::PROCESSED, 1.0F);
    other.can_frame = &can_frame;
    sensor_readings_frame->AddOrUpdateReading(other);
    zassert_false(sensor_readings_frame->TryGetCanFrame(sensors[1]->id_hash, stored));

    sensor_readings_frame->ClearReadings();
    zassert_false(sensor_readings_frame->TryGetCanFrame(raw->id_hash, stored));
}

ZTEST(sensor_readings_frame, test_ClearReadings) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 0);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[0], ReadingSource::ISR, ReadingStatus::RAW, 2.4F));

    SensorReading failed = MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::UNINITIALIZED);
    failed.SetError(ReadingError::READER_FAILED);
    sensor_readings_frame->AddOrUpdateReading(failed);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[2], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 2.6F));

    for(const auto& sensor : sensors)
        zassert_true(sensor_readings_frame->HasReading(sensor->id_hash));

    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 1);
    zassert_false(sensor_readings_frame->TryGetReadingValue(sensors[0]->id_hash).has_value());
    zassert_false(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).has_value());
    zassert_true(sensor_readings_frame->TryGetReadingValue(sensors[2]->id_hash).has_value());

    sensor_readings_frame->ClearReadings();

    for(const auto& sensor : sensors) {
        zassert_false(sensor_readings_frame->HasReading(sensor->id_hash));
        zassert_false(sensor_readings_frame->TryGetReadingValue(sensor->id_hash).has_value());
    }

    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 0);
}

ZTEST(sensor_readings_frame, test_ClearProcessedReadings) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 2.0F));

    sensor_readings_frame->ClearProcessedReadings();

    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 0);

    // Only the processed set is dropped; the reading and its value survive.
    zassert_true(sensor_readings_frame->HasReading(sensors[1]->id_hash));
    zassert_true(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).has_value());
}

ZTEST(sensor_readings_frame, test_Configure_lays_out_one_slot_per_sensor) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = std::make_shared<SensorReadingsFrame>();

    zassert_equal(sensor_readings_frame->GetGeneration(), 0);
    zassert_equal(sensor_readings_frame->GetSlotCount(), 0);
    zassert_false(sensor_readings_frame->FindSlot(sensors[0]->id_hash).has_value());

    zassert_equal(sensor_readings_frame->Configure(sensors), 1);
    zassert_equal(sensor_readings_frame->GetGeneration(), 1);
    zassert_equal(sensor_readings_frame->GetSlotCount(), sensors.size());

    for(size_t i = 0; i < sensors.size(); i++) {
        const auto slot = sensor_readings_frame->FindSlot(sensors[i]->id_hash);
        zassert_true(slot.has_value());
        zassert_equal(slot->index, i, "Slots follow the configuration order");
        zassert_equal(sensor_readings_frame->FindSlot(std::string_view(sensors[i]->id))->index, i);
        zassert_not_null(sensor_readings_frame->GetValueAddress(slot.value()));
    }

    zassert_false(sensor_readings_frame->FindSlot(StringHelpers::GetHash("unknown")).has_value());
    zassert_is_null(sensor_readings_frame->GetReadingValuePtr("unknown"));
}

ZTEST(sensor_readings_frame, test_readings_of_unconfigured_sensors_are_ignored) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame({ sensors[0], sensors[1] });

    zassert_true(sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[0], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 1.0F)));
    zassert_false(sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[2], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 1.0F)));

    zassert_false(sensor_readings_frame->HasReading(sensors[2]->id_hash));
    zassert_false(sensor_readings_frame->TryGetReadingValue(sensors[2]->id_hash).has_value());
    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 1);
}

ZTEST(sensor_readings_frame, test_Configure_again_starts_a_new_generation) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    float* value_ptr = sensor_readings_frame->GetReadingValuePtr("sensor_2");
    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 2.0F));
    zassert_equal(*value_ptr, 2.0F);

    // A new layout: the old readings are gone and the slots follow the new order.
    const std::vector<std::shared_ptr<Sensor>> reordered = { sensors[2], sensors[1] };
    zassert_equal(sensor_readings_frame->Configure(reordered), 2);

    zassert_equal(sensor_readings_frame->GetSlotCount(), 2);
    zassert_equal(sensor_readings_frame->FindSlot(sensors[1]->id_hash)->index, 1);
    zassert_false(sensor_readings_frame->FindSlot(sensors[0]->id_hash).has_value());
    zassert_false(sensor_readings_frame->HasReading(sensors[1]->id_hash));
    zassert_false(sensor_readings_frame->TryGetReadingValue(sensors[1]->id_hash).has_value());
    zassert_equal(sensor_readings_frame->GetProcessedReadingCount(), 0);
}

ZTEST(sensor_readings_frame, test_AreValuesAvailable) {
    auto sensors = sensor_readings_frame_GetTestSensors();
    auto sensor_readings_frame = MakeFrame(sensors);

    const std::array<SensorSlot, 2> slots = {
        sensor_readings_frame->FindSlot(sensors[0]->id_hash).value(),
        sensor_readings_frame->FindSlot(sensors[1]->id_hash).value() };

    zassert_false(sensor_readings_frame->AreValuesAvailable(slots));

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[0], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 1.0F));
    zassert_false(sensor_readings_frame->AreValuesAvailable(slots), "One of two is not enough");

    sensor_readings_frame->AddOrUpdateReading(MakeReading(sensors[1], ReadingSource::PROCESSING, ReadingStatus::PROCESSED, 1.0F));
    zassert_true(sensor_readings_frame->AreValuesAvailable(slots));

    const std::array<SensorSlot, 1> invalid = { SensorSlot{} };
    zassert_false(sensor_readings_frame->AreValuesAvailable(invalid));
}

ZTEST(sensor_readings_frame, test_raw_can_frames_are_queued_for_the_log_writer) {
    auto sensors = sensor_readings_frame_GetTestSensors();

    auto raw = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "raw");
    raw->configuration.type = SensorType::CANBUS_RAW;
    auto sensor_readings_frame = MakeFrame({ raw, sensors[1] });

    std::array<SensorReading, 4> readings;
    std::array<CanFrame, 4> frames;
    zassert_equal(sensor_readings_frame->TakeRawCanFrames(readings, frames), 0);

    // Every frame is queued, not only the latest one.
    for(uint8_t i = 1; i <= 3; i++) {
        CanFrame can_frame;
        can_frame.id = 0x100 + i;
        can_frame.data = { i };

        SensorReading reading = MakeReading(raw, ReadingSource::ISR, ReadingStatus::PROCESSED);
        reading.timestamp = std::chrono::system_clock::time_point(std::chrono::seconds(i));
        reading.can_frame = &can_frame;
        sensor_readings_frame->AddOrUpdateReading(reading);
    }

    const size_t count = sensor_readings_frame->TakeRawCanFrames(readings, frames);
    zassert_equal(count, 3);
    for(size_t i = 0; i < count; i++) {
        zassert_equal(readings[i].sensor_id_hash, raw->id_hash);
        zassert_equal(readings[i].status, ReadingStatus::PROCESSED);
        zassert_equal(readings[i].can_frame, &frames[i], "The reading refers to the caller's copy");
        zassert_equal(frames[i].id, 0x100 + i + 1, "Oldest first");
        zassert_equal(frames[i].data[0], i + 1);
        zassert_equal(readings[i].timestamp.value().time_since_epoch(), std::chrono::seconds(i + 1));
    }

    zassert_equal(sensor_readings_frame->TakeRawCanFrames(readings, frames), 0, "Taken once");
    zassert_equal(sensor_readings_frame->GetDroppedRawCanFrameCount(), 0);
}

ZTEST(sensor_readings_frame, test_raw_can_queue_drops_and_counts_when_full) {
    auto raw = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "raw");
    raw->configuration.type = SensorType::CANBUS_RAW;
    auto sensor_readings_frame = MakeFrame({ raw });

    CanFrame can_frame;
    can_frame.id = 0x100;
    can_frame.data = { 1 };

    SensorReading reading = MakeReading(raw, ReadingSource::ISR, ReadingStatus::PROCESSED);
    reading.can_frame = &can_frame;

    for(size_t i = 0; i < SensorLimits::kRawCanQueueSize + 5; i++)
        sensor_readings_frame->AddOrUpdateReading(reading);

    zassert_equal(sensor_readings_frame->GetDroppedRawCanFrameCount(), 5);

    // Draining in chunks returns everything that was kept, and nothing more.
    std::array<SensorReading, 8> readings;
    std::array<CanFrame, 8> frames;
    size_t total = 0;
    size_t count = 0;
    do {
        count = sensor_readings_frame->TakeRawCanFrames(readings, frames);
        total += count;
    } while(count == readings.size());

    zassert_equal(total, SensorLimits::kRawCanQueueSize);

    // The latest frame is still available for diagnostics, and ClearReadings() empties the queue.
    CanFrame latest;
    zassert_true(sensor_readings_frame->TryGetCanFrame(raw->id_hash, latest));

    sensor_readings_frame->AddOrUpdateReading(reading);
    sensor_readings_frame->ClearReadings();
    zassert_equal(sensor_readings_frame->TakeRawCanFrames(readings, frames), 0);
}
