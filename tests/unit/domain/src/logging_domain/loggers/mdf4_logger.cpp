#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef CONFIG_ARCH_POSIX
#include <fstream>
#endif

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/canbus/can_frame.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "domain/logging_domain/models/logging_configuration.h"
#include "domain/logging_domain/loggers/logger_metadata.h"
#include "domain/logging_domain/loggers/mdf4_logger_sensor_reading.h"
#include "domain/logging_domain/services/log_file_naming.h"

#include "allocation_counter.h"
#include "memory_stream_buf.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using namespace eerie_leap::domain::logging_domain::models;
using namespace eerie_leap::domain::logging_domain::loggers;
using namespace eerie_leap::domain::logging_domain::services;
using rt_core_test::MemoryStreamBuf;
using rt_core_test::GetAllocationCount;

ZTEST_SUITE(mdf4_logger, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr size_t ID_BLOCK_SIZE = 64;
constexpr size_t BLOCK_HEADER_SIZE = 24;
constexpr size_t BLOCK_LENGTH_OFFSET = 8;

constexpr size_t TIME_SIZE = 8;
constexpr size_t VALUE_SIZE = 4;
constexpr size_t INVALIDATION_SIZE = 1;
constexpr size_t CAN_FRAME_SIZE = 15;
constexpr size_t VLSD_LENGTH_SIZE = 4;

// A value group's record: id 1, time, value, the raw value when logged, then the invalidation byte
// with bit 0 for the value channel and bit 1 for the raw channel.
constexpr size_t VALUE_RECORD_SIZE = 1 + TIME_SIZE + VALUE_SIZE + INVALIDATION_SIZE;
constexpr size_t RAW_RECORD_SIZE = 1 + TIME_SIZE + 2 * VALUE_SIZE + INVALIDATION_SIZE;
constexpr uint8_t VALUE_INVALID = 0x01;
constexpr uint8_t RAW_VALUE_INVALID = 0x02;

constexpr uint8_t TEST_DEVICE_ID = 7;
constexpr uint32_t TEST_SW_VERSION = 0x01020003;

const auto START_TIME = std::chrono::system_clock::time_point(std::chrono::seconds(1'760'000'000));

std::shared_ptr<Sensor> MakeValueSensor(std::string_view id, std::string_view expression = "") {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);

    sensor->metadata.name = "Name of " + std::string(id);
    sensor->metadata.unit = "bar";
    sensor->metadata.description = "Description of " + std::string(id);

    sensor->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor->configuration.channel = 0;
    sensor->configuration.expression = expression;

    return sensor;
}

std::shared_ptr<Sensor> MakeCanRawSensor(std::string_view id, uint8_t bus_channel) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);

    sensor->metadata.name = "Raw frames";

    sensor->configuration.type = SensorType::CANBUS_RAW;
    sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), bus_channel, 0x100);
    sensor->configuration.UpdateConnectionString();

    return sensor;
}

LoggingConfiguration MakeConfiguration(
    std::span<const std::shared_ptr<Sensor>> sensors, bool log_raw_value = false, bool log_only_new_data = false) {

    LoggingConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    configuration.logging_interval_ms = 50;

    for(const auto& sensor : sensors) {
        configuration.sensor_configurations.emplace(sensor->id_hash, SensorLoggingConfiguration {
            .is_enabled = true,
            .log_raw_value = log_raw_value,
            .log_only_new_data = log_only_new_data
        });
    }

    return configuration;
}

LoggerMetadata MakeMetadata() {
    LoggerMetadata metadata;
    metadata.device_name = "unit_board";
    metadata.tool_id = "rt_core_unit";
    metadata.tool_version = "1.2.3";
    metadata.product_id = 50177;
    metadata.device_id = TEST_DEVICE_ID;
    metadata.hw_version = 0x00000001;
    metadata.sw_version = TEST_SW_VERSION;
    metadata.sensors_configuration_hash = 0xDEADBEEF;

    return metadata;
}

SensorReading MakeValueReading(
    const Sensor& sensor, std::optional<float> value, std::optional<std::chrono::system_clock::time_point> timestamp) {

    SensorReading reading(&sensor);
    reading.value = value;
    reading.timestamp = timestamp;
    reading.source = ReadingSource::PROCESSING;
    reading.status = ReadingStatus::PROCESSED;

    return reading;
}

MemoryStreamBuf MakeStream() {
    return MemoryStreamBuf{};
}

// The records appended since the header, which StartLogging() reported as the bytes written so far.
std::string RecordBytes(const MemoryStreamBuf& stream, uint64_t header_size) {
    return stream.str().substr(static_cast<size_t>(header_size));
}

template<typename T>
T ReadAt(std::string_view data, size_t offset) {
    T value{};
    zassert_true(offset + sizeof(T) <= data.size(), "read past the end of the data");
    std::memcpy(&value, data.data() + offset, sizeof(T));

    return value;
}

// Texts of every block with @p block_id in the header, walking the blocks up to the DT block.
std::vector<std::string> CollectBlockTexts(std::string_view file, std::string_view block_id) {
    std::vector<std::string> texts;
    size_t offset = ID_BLOCK_SIZE;

    while(offset + BLOCK_HEADER_SIZE <= file.size()) {
        const auto id = file.substr(offset, 4);
        const auto length = static_cast<size_t>(ReadAt<uint64_t>(file, offset + BLOCK_LENGTH_OFFSET));
        zassert_true(length >= BLOCK_HEADER_SIZE, "block at %zu has an invalid length", offset);

        if(id == "##DT")
            break;

        if(id == block_id) {
            const auto text = file.substr(offset + BLOCK_HEADER_SIZE, length - BLOCK_HEADER_SIZE);
            texts.emplace_back(text.substr(0, text.find('\0')));
        }

        offset += length;
    }

    return texts;
}

bool ContainsText(const std::vector<std::string>& texts, std::string_view expected) {
    return std::ranges::find(texts, expected) != texts.end();
}

bool AnyContains(const std::vector<std::string>& texts, std::string_view fragment) {
    return std::ranges::any_of(texts, [fragment](const std::string& text) { return text.find(fragment) != std::string::npos; });
}

// native_sim links the host libc, so the file can be handed to asammdf on the host.
void DumpToHost([[maybe_unused]] const MemoryStreamBuf& stream, [[maybe_unused]] std::string_view file_name) {
#ifdef CONFIG_ARCH_POSIX
    const std::string_view dump_dir = CONFIG_LOGGING_UNIT_TEST_DUMP_DIR;
    if(dump_dir.empty())
        return;

    const std::string path = std::string(dump_dir) + "/" + std::string(file_name);
    const std::string content = stream.str();

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    zassert_true(file.is_open(), "the dump file %s could not be opened", path.c_str());
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    zassert_true(file.good(), "the dump file %s could not be written", path.c_str());
#endif
}

} // namespace

ZTEST(mdf4_logger, test_ValueReadingRecordBytes) {
    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    const auto time = START_TIME + std::chrono::milliseconds(1500);
    zassert_true(logger.LogReading(time, MakeValueReading(*sensors[0], 42.5F, time)), "an enabled value reading must be logged");

    const auto records = RecordBytes(stream, header_size);
    zassert_equal(records.size(), VALUE_RECORD_SIZE, "a value record is id 1 + time 8 + value 4 + invalidation 1 (got %zu)",
        records.size());
    zassert_equal(static_cast<uint8_t>(records[0]), 1, "the first group has record id 1 in one byte");
    zassert_equal(ReadAt<double>(records, 1), 1.5, "the time is a Float64 delta from the start time in seconds");
    zassert_equal(ReadAt<float>(records, 1 + TIME_SIZE), 42.5F, "the value follows the time as Float32");
    zassert_equal(static_cast<uint8_t>(records[1 + TIME_SIZE + VALUE_SIZE]), 0, "a valid reading clears its invalidation bit");
    zassert_equal(logger.GetBytesWritten(), header_size + records.size(), "the byte count must include the record");
}

ZTEST(mdf4_logger, test_RawValueRecord) {
    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1", "x * 4 + 1.6")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    auto reading = MakeValueReading(*sensors[0], 9.6F, START_TIME + std::chrono::milliseconds(250));
    reading.raw_value = 2.0F;
    zassert_true(logger.LogReading(*reading.timestamp, reading), "a reading with a raw value must be logged");

    auto without_raw = MakeValueReading(*sensors[0], 1.6F, START_TIME + std::chrono::milliseconds(500));
    zassert_true(logger.LogReading(*without_raw.timestamp, without_raw), "a reading without a raw value is still logged");

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t record_size = RAW_RECORD_SIZE;
    constexpr size_t mask = record_size - 1;
    zassert_equal(records.size(), 2 * record_size, "two records of id + time + value + raw + invalidation (got %zu)", records.size());
    zassert_equal(ReadAt<double>(records, 1), 0.25, "the first record is at 0.25 s");
    zassert_equal(ReadAt<float>(records, 1 + TIME_SIZE), 9.6F, "the value comes first");
    zassert_equal(ReadAt<float>(records, 1 + TIME_SIZE + VALUE_SIZE), 2.0F, "the raw value follows the value");
    zassert_equal(static_cast<uint8_t>(records[mask]), 0, "a reading with both values clears both bits");
    zassert_equal(ReadAt<float>(records, record_size + 1 + TIME_SIZE), 1.6F, "a missing raw value keeps the value");
    zassert_equal(ReadAt<float>(records, record_size + 1 + TIME_SIZE + VALUE_SIZE), 0.0F, "a missing raw value is written as 0");
    zassert_equal(static_cast<uint8_t>(records[record_size + mask]), RAW_VALUE_INVALID,
        "a missing raw value sets the raw channel's bit only");
}

ZTEST(mdf4_logger, test_CanFrameRecord) {
    const std::vector<std::shared_ptr<Sensor>> sensors{MakeCanRawSensor("can_raw", 1)};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    CanFrame classic;
    classic.id = 0x123;
    classic.data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    CanFrame fd;
    fd.id = 0x18FEF100;
    fd.is_extended = true;
    fd.is_can_fd = true;
    fd.is_bitrate_switch = true;
    fd.data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    CanFrame transmitted;
    transmitted.id = 0x7FF;
    transmitted.is_transmit = true;
    transmitted.data = {0xA5};

    const std::array<const CanFrame*, 3> frames{&classic, &fd, &transmitted};
    for(size_t i = 0; i < frames.size(); i++) {
        SensorReading reading(sensors[0].get());
        reading.source = ReadingSource::ISR;
        reading.status = ReadingStatus::PROCESSED;
        reading.timestamp = START_TIME + std::chrono::microseconds(1000 + 250 * i);
        reading.can_frame = frames[i];

        zassert_true(logger.LogReading(*reading.timestamp, reading), "frame %zu must be logged", i);
    }

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t can_record_size = 1 + TIME_SIZE + CAN_FRAME_SIZE;
    zassert_equal(can_record_size, 24, "a CAN record is id 1 + time 8 + CAN_DataFrame 15");

    // The VLSD group is created first: id 1 for the payloads, id 2 for the CAN_DataFrame records.
    // Each payload record goes out before its frame record.
    zassert_equal(static_cast<uint8_t>(records[0]), 1, "the payload comes first, in the VLSD group");
    zassert_equal(ReadAt<uint32_t>(records, 1), 8U, "the VLSD record carries the payload length");
    zassert_equal(static_cast<uint8_t>(records[1 + VLSD_LENGTH_SIZE]), 0x11, "the payload follows its length");

    const size_t first = 1 + VLSD_LENGTH_SIZE + 8;
    zassert_equal(static_cast<uint8_t>(records[first]), 2, "the CAN group has record id 2");
    zassert_equal(ReadAt<double>(records, first + 1), 0.001, "the frame time is its own timestamp");
    zassert_equal(static_cast<uint8_t>(records[first + 1 + TIME_SIZE]), 2, "BusChannel is the source bus channel 1 plus 1");
    zassert_equal(ReadAt<uint32_t>(records, first + 1 + TIME_SIZE + 1), 0x123U, "a standard ID is packed without IDE");
    zassert_equal(static_cast<uint8_t>(records[first + 1 + TIME_SIZE + 5]), 8, "DLC 8, receive, classic CAN");
    zassert_equal(static_cast<uint8_t>(records[first + 1 + TIME_SIZE + 6]), 8, "DataLength is the payload size");
    zassert_equal(ReadAt<uint64_t>(records, first + 1 + TIME_SIZE + 7), 0U, "the first VLSD offset is 0, 64 bits wide");

    const size_t second = first + can_record_size + 1 + VLSD_LENGTH_SIZE + 12;
    zassert_equal(static_cast<uint8_t>(records[second]), 2, "the second frame record follows its 12 byte payload");
    zassert_equal(ReadAt<double>(records, second + 1), 0.00125, "frames of one tick keep their own times");
    zassert_equal(ReadAt<uint32_t>(records, second + 1 + TIME_SIZE + 1), 0x18FEF100U | (1U << 31), "an extended ID sets IDE in bit 31");
    zassert_equal(static_cast<uint8_t>(records[second + 1 + TIME_SIZE + 5]), 9 | (1 << 5) | (1 << 6), "DLC 9 for 12 bytes, EDL and BRS set");
    zassert_equal(static_cast<uint8_t>(records[second + 1 + TIME_SIZE + 6]), 12, "DataLength is 12");
    zassert_equal(ReadAt<uint64_t>(records, second + 1 + TIME_SIZE + 7), 4U + 8U, "the offset advances by length + payload");

    zassert_true(logger.StopLogging(), "a string buffer finalizes");
    DumpToHost(stream, "logger_unit_dump.mf4");
}

// An error reading's value is stale and a missing value unknown: both are written as 0 with the
// value channel's bit set, so readers show a gap; a missing raw value invalidates the raw channel only.
ZTEST(mdf4_logger, test_InvalidReadingsSetTheirInvalidationBits) {
    using std::chrono::milliseconds;

    const std::vector<std::shared_ptr<Sensor>> sensors{
        MakeValueSensor("oil_pressure", "x * 4 + 1.6"),
        MakeValueSensor("coolant_temperature")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    auto reading = [&sensors](size_t sensor, std::optional<float> value, std::optional<float> raw_value, int time_ms) {
        auto result = MakeValueReading(*sensors[sensor], value, START_TIME + milliseconds(time_ms));
        result.raw_value = raw_value;

        return result;
    };

    auto valid = reading(0, 9.6F, 2.0F, 100);
    // The expression failed after the input was measured: the value is a leftover, the raw value real.
    auto error = reading(0, 9.6F, 2.5F, 200);
    error.SetError(ReadingError::EXPRESSION_FAILED);
    auto without_value = reading(0, std::nullopt, 3.0F, 300);
    auto without_raw = reading(0, 13.6F, std::nullopt, 400);
    auto failed_read = reading(0, std::nullopt, std::nullopt, 500);
    failed_read.SetError(ReadingError::READER_FAILED);
    auto valid_again = reading(0, 15.6F, 3.5F, 600);

    auto coolant_valid = reading(1, 80.0F, std::nullopt, 150);
    auto coolant_error = reading(1, 80.0F, std::nullopt, 250);
    coolant_error.SetError(ReadingError::READER_FAILED);
    auto coolant_valid_again = reading(1, 81.0F, std::nullopt, 350);

    const std::array<const SensorReading*, 9> readings{
        &valid, &coolant_valid, &error, &coolant_error, &without_value, &coolant_valid_again,
        &without_raw, &failed_read, &valid_again};
    for(size_t i = 0; i < readings.size(); i++)
        zassert_true(logger.LogReading(*readings[i]->timestamp, *readings[i]), "reading %zu must be written, not skipped", i);

    struct Expected {
        float value;
        float raw_value;
        uint8_t mask;
    };
    const std::array<Expected, 6> expected_oil{{
        {9.6F, 2.0F, 0},
        {0.0F, 2.5F, VALUE_INVALID},
        {0.0F, 3.0F, VALUE_INVALID},
        {13.6F, 0.0F, RAW_VALUE_INVALID},
        {0.0F, 0.0F, VALUE_INVALID | RAW_VALUE_INVALID},
        {15.6F, 3.5F, 0}}};
    const std::array<Expected, 3> expected_coolant{{{80.0F, 0.0F, 0}, {0.0F, 0.0F, VALUE_INVALID}, {81.0F, 0.0F, 0}}};

    const auto records = RecordBytes(stream, header_size);
    zassert_equal(records.size(), expected_oil.size() * RAW_RECORD_SIZE + expected_coolant.size() * VALUE_RECORD_SIZE,
        "every reading is one record (got %zu bytes)", records.size());

    size_t oil = 0;
    size_t coolant = 0;
    for(size_t position = 0; position < records.size();) {
        const auto record_id = static_cast<uint8_t>(records[position]);
        const auto value = ReadAt<float>(records, position + 1 + TIME_SIZE);

        if(record_id == 1) {
            const auto& expected = expected_oil.at(oil);
            zassert_equal(value, expected.value, "oil record %zu value", oil);
            zassert_equal(ReadAt<float>(records, position + 1 + TIME_SIZE + VALUE_SIZE), expected.raw_value, "oil record %zu raw value", oil);
            zassert_equal(static_cast<uint8_t>(records[position + RAW_RECORD_SIZE - 1]), expected.mask,
                "oil record %zu invalidation bits", oil);
            position += RAW_RECORD_SIZE;
            oil++;
        } else {
            zassert_equal(record_id, 2, "unknown record id %u at %zu", static_cast<unsigned>(record_id), position);
            const auto& expected = expected_coolant.at(coolant);
            zassert_equal(value, expected.value, "coolant record %zu value", coolant);
            zassert_equal(static_cast<uint8_t>(records[position + VALUE_RECORD_SIZE - 1]), expected.mask,
                "coolant record %zu invalidation bits", coolant);
            position += VALUE_RECORD_SIZE;
            coolant++;
        }
    }
    zassert_equal(oil, expected_oil.size());
    zassert_equal(coolant, expected_coolant.size());

    zassert_true(logger.StopLogging(), "a memory buffer finalizes");
    DumpToHost(stream, "logger_invalidation_dump.mf4");
}

// An error reading carries its own timestamp, so it is a sample like any other: new once, then held.
ZTEST(mdf4_logger, test_ErrorReadingIsASampleForLogOnlyNewData) {
    using std::chrono::milliseconds;

    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1")};

    auto error = MakeValueReading(*sensors[0], 5.0F, START_TIME + milliseconds(200));
    error.SetError(ReadingError::READER_FAILED);
    const auto first = MakeValueReading(*sensors[0], 1.0F, START_TIME + milliseconds(100));
    const auto recovered = MakeValueReading(*sensors[0], 2.0F, START_TIME + milliseconds(400));

    for(bool log_only_new_data : {true, false}) {
        Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, log_only_new_data), sensors, MakeMetadata());

        auto stream = MakeStream();
        zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
        const uint64_t header_size = logger.GetBytesWritten();

        zassert_true(logger.LogReading(START_TIME + milliseconds(100), first), "the first sample is new");
        zassert_true(logger.LogReading(START_TIME + milliseconds(200), error), "an error with a new timestamp is a new sample");
        zassert_equal(logger.LogReading(START_TIME + milliseconds(300), error), !log_only_new_data,
            "a held error is %s", log_only_new_data ? "skipped with log_only_new_data" : "written again at the tick time");
        zassert_true(logger.LogReading(START_TIME + milliseconds(400), recovered), "the next sample is new");

        const auto records = RecordBytes(stream, header_size);
        const size_t record_count = log_only_new_data ? 3 : 4;
        zassert_equal(records.size(), record_count * VALUE_RECORD_SIZE, "%zu records (got %zu bytes)", record_count, records.size());

        for(size_t i = 0; i < record_count; i++) {
            const bool is_error = i == 1 || (i == 2 && !log_only_new_data);
            zassert_equal(static_cast<uint8_t>(records[(i + 1) * VALUE_RECORD_SIZE - 1]), is_error ? VALUE_INVALID : 0,
                "record %zu must be %s", i, is_error ? "invalid" : "valid");
        }

        if(!log_only_new_data) {
            zassert_within(ReadAt<double>(records, 2 * VALUE_RECORD_SIZE + 1), 0.3, 1e-9, "the held error is repeated at the tick time");
            zassert_equal(ReadAt<float>(records, 2 * VALUE_RECORD_SIZE + 1 + TIME_SIZE), 0.0F, "and written as 0 again");
        }
    }
}

// Queued frames are separate bus events even when they share the time service's millisecond.
ZTEST(mdf4_logger, test_CanFramesIgnoreLogOnlyNewData) {
    const std::vector<std::shared_ptr<Sensor>> sensors{MakeCanRawSensor("can_raw", 0)};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    CanFrame frame;
    frame.id = 0x100;
    frame.data = {1, 2};

    SensorReading reading(sensors[0].get());
    reading.source = ReadingSource::ISR;
    reading.status = ReadingStatus::PROCESSED;
    reading.timestamp = START_TIME + std::chrono::milliseconds(10);
    reading.can_frame = &frame;

    zassert_true(logger.LogReading(*reading.timestamp, reading), "the first frame is written");
    zassert_true(logger.LogReading(*reading.timestamp, reading), "a frame with the same timestamp is written too");

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t pair_size = 1 + VLSD_LENGTH_SIZE + 2 + 1 + TIME_SIZE + CAN_FRAME_SIZE;
    zassert_equal(records.size(), 2 * pair_size, "two payload and frame record pairs (got %zu)", records.size());
}

ZTEST(mdf4_logger, test_DisabledSensorIsNotWritten) {
    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1"), MakeValueSensor("sensor_2")};
    auto configuration = MakeConfiguration(sensors);
    configuration.sensor_configurations.at(sensors[1]->id_hash).is_enabled = false;

    Mdf4LoggerSensorReading logger(configuration, sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    const auto texts = CollectBlockTexts(stream.str(), "##TX");
    zassert_false(ContainsText(texts, "sensor_2"), "a disabled sensor must not get a channel group");

    zassert_false(logger.LogReading(START_TIME, MakeValueReading(*sensors[1], 1.0F, START_TIME)),
        "a reading of a disabled sensor must not be logged");
    zassert_equal(logger.GetBytesWritten(), header_size, "nothing may be written for a disabled sensor");
}

ZTEST(mdf4_logger, test_LogOnlyNewDataSkipsUnchangedTimestamp) {
    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");

    const auto first = START_TIME + std::chrono::milliseconds(100);
    const auto second = START_TIME + std::chrono::milliseconds(200);

    zassert_true(logger.LogReading(first, MakeValueReading(*sensors[0], 1.0F, first)), "the first reading is new");
    zassert_false(logger.LogReading(first + std::chrono::milliseconds(50), MakeValueReading(*sensors[0], 1.0F, first)),
        "the same reading timestamp again must be skipped");
    zassert_true(logger.LogReading(second, MakeValueReading(*sensors[0], 2.0F, second)), "a changed timestamp is new data");
}

ZTEST(mdf4_logger, test_ReadingWithoutTimestampUsesTheGivenTime) {
    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    const auto tick_time = START_TIME + std::chrono::milliseconds(750);
    zassert_true(logger.LogReading(tick_time, MakeValueReading(*sensors[0], 3.0F, std::nullopt)),
        "a reading without a timestamp is logged at the given time");
    zassert_true(logger.LogReading(tick_time, MakeValueReading(*sensors[0], 3.0F, std::nullopt)),
        "a reading without a timestamp never counts as unchanged data");
    zassert_true(logger.LogReading(tick_time, MakeValueReading(*sensors[0], std::nullopt, std::nullopt)),
        "a reading without a value is written, marked invalid");

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t record_size = VALUE_RECORD_SIZE;
    zassert_equal(records.size(), 3 * record_size, "three records written (got %zu)", records.size());
    zassert_equal(ReadAt<double>(records, 1), 0.75, "the record time is the time the caller passed");
    zassert_true(ReadAt<double>(records, record_size + 1) > 0.75, "the master channel of a group strictly increases");
    zassert_true(ReadAt<double>(records, 2 * record_size + 1) > ReadAt<double>(records, record_size + 1),
        "an invalid record keeps the master channel increasing");
    zassert_equal(ReadAt<float>(records, 2 * record_size + 1 + TIME_SIZE), 0.0F, "a reading without a value is written as 0");
    zassert_equal(static_cast<uint8_t>(records[3 * record_size - 1]), VALUE_INVALID, "with the value channel's bit set");
}

// The tick offers every sensor's latest sample each time; without log_only_new_data a held sample
// is written again at the tick time, never again at its own time.
ZTEST(mdf4_logger, test_HeldSampleIsRepeatedAtTheTickTime) {
    using std::chrono::milliseconds;

    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, false), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    const auto sample_1 = MakeValueReading(*sensors[0], 1.0F, START_TIME + milliseconds(100));
    const auto sample_2 = MakeValueReading(*sensors[0], 2.0F, START_TIME + milliseconds(350));
    // Stamped before the tick at 400 ms that still saw sample_2, published after it.
    const auto sample_3 = MakeValueReading(*sensors[0], 3.0F, START_TIME + milliseconds(390));

    zassert_true(logger.LogReading(START_TIME + milliseconds(100), sample_1), "a new sample is written");
    zassert_false(logger.LogReading(START_TIME + milliseconds(100), sample_1),
        "a repeat at the time of its own record would duplicate that record");
    zassert_true(logger.LogReading(START_TIME + milliseconds(200), sample_1), "a held sample is written again");
    zassert_true(logger.LogReading(START_TIME + milliseconds(300), sample_1), "and again at the next tick");
    zassert_true(logger.LogReading(START_TIME + milliseconds(400), sample_2), "a new sample is written at its own time");
    zassert_true(logger.LogReading(START_TIME + milliseconds(400), sample_2), "a held sample at a later tick time");
    zassert_true(logger.LogReading(START_TIME + milliseconds(500), sample_3), "a late new sample is still written");

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t record_size = VALUE_RECORD_SIZE;
    zassert_equal(records.size(), 6 * record_size, "six records (got %zu)", records.size());

    const std::array<double, 6> expected_times{0.1, 0.2, 0.3, 0.35, 0.4, 0.4 + 0.000001};
    const std::array<float, 6> expected_values{1.0F, 1.0F, 1.0F, 2.0F, 2.0F, 3.0F};
    for(size_t i = 0; i < expected_times.size(); i++) {
        const auto time = ReadAt<double>(records, i * record_size + 1);
        zassert_within(time, expected_times[i], 1e-9, "record %zu at %f s, expected %f s", i, time, expected_times[i]);
        zassert_equal(ReadAt<float>(records, i * record_size + 1 + TIME_SIZE), expected_values[i], "record %zu value", i);
    }
}

// Float64 seconds keep microsecond steps a day into a session, where Float32 had 8 ms.
ZTEST(mdf4_logger, test_TimeKeepsMicrosecondsAfterADay) {
    using std::chrono::hours;
    using std::chrono::milliseconds;
    using std::chrono::microseconds;

    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    const auto day = START_TIME + hours(24);
    const std::array<std::chrono::system_clock::time_point, 4> times{
        day + milliseconds(1), day + milliseconds(2), day + milliseconds(2) + microseconds(1), day + milliseconds(2) + microseconds(2)};

    for(const auto& time : times)
        zassert_true(logger.LogReading(time, MakeValueReading(*sensors[0], 1.0F, time)), "every sample is new");

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t record_size = VALUE_RECORD_SIZE;
    zassert_equal(records.size(), times.size() * record_size, "every sample is written (got %zu)", records.size());

    for(size_t i = 0; i < times.size(); i++) {
        const auto expected = std::chrono::duration<double>(times[i] - START_TIME).count();
        const auto time = ReadAt<double>(records, i * record_size + 1);
        zassert_within(time, expected, 1e-9, "record %zu at %.9f s, expected %.9f s", i, time, expected);

        if(i > 0) {
            const auto previous = ReadAt<double>(records, (i - 1) * record_size + 1);
            zassert_true(time > previous, "record %zu must be later than the one before", i);
        }
    }
}

ZTEST(mdf4_logger, test_ConversionRule) {
    const auto linear = Mdf4LoggerSensorReading::ToMdfFormula("x * 4 + 1.6");
    zassert_true(linear.has_value() && linear.value() == "X * 4 + 1.6", "x is renamed to X token-wise");

    // asammdf evaluates formulas with numexpr, which has no pow() and rejects "05".
    const auto power = Mdf4LoggerSensorReading::ToMdfFormula("x ^ 2 * 0.5");
    zassert_true(power.value_or("") == "(X * X) * 0.5", "x ^ 2 is spelled out as a product (got %s)", power.value_or("").c_str());
    const auto negated_cube = Mdf4LoggerSensorReading::ToMdfFormula("1 - x ^ 3");
    zassert_true(negated_cube.value_or("") == "1 - (X * X * X)", "the product binds like ^ (got %s)", negated_cube.value_or("").c_str());
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x ^ 5").has_value(), "an exponent above 4 is not spelled out");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x ^ 0.5").has_value(), "a fractional exponent is not converted");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x ^ 0").has_value(), "a zero exponent is not converted");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x * 05").has_value(), "numexpr rejects a leading zero");
    zassert_true(Mdf4LoggerSensorReading::ToMdfFormula("x * 0.5 + 0").value_or("") == "X * 0.5 + 0",
        "a single zero and a zero before the point are plain literals");

    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x * 2 + sensor_2 + 1").has_value(),
        "an expression reading another sensor cannot be a conversion");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("exp(x)").has_value(), "exp is not in the allowed function list");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("max(x, 1)").has_value(), "max is not in the allowed function list");

    zassert_true(Mdf4LoggerSensorReading::ToMdfFormula("sqrt(abs(x)) / 2").value_or("") == "sqrt(abs(X)) / 2",
        "allowed functions pass through");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("(x + 1) ^ 2").has_value(), "a grouped base is not converted");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x ^ 2 ^ 2").has_value(), "a chained power is not converted");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("4.5").has_value(), "an expression without x is no conversion");

    // In the file: the formula on the raw channel's conversion, or the expression in its comment.
    const std::vector<std::shared_ptr<Sensor>> sensors{
        MakeValueSensor("sensor_1", "x * 2 + sensor_2 + 1"),
        MakeValueSensor("sensor_2", "x * 4 + 1.6")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");

    const auto file = stream.str();
    zassert_true(ContainsText(CollectBlockTexts(file, "##TX"), "X * 4 + 1.6"), "the convertible expression is a formula");
    zassert_false(ContainsText(CollectBlockTexts(file, "##TX"), "x * 2 + sensor_2 + 1"),
        "the other expression must not be written as a formula");
    zassert_true(AnyContains(CollectBlockTexts(file, "##MD"), "<TX>expression: x * 2 + sensor_2 + 1</TX>"),
        "the other expression goes into the raw channel comment");
}

ZTEST(mdf4_logger, test_ChannelAndGroupNames) {
    const std::vector<std::shared_ptr<Sensor>> sensors{
        MakeValueSensor("oil_pressure", "x * 0.1"),
        MakeCanRawSensor("can_raw", 0)};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");

    const auto file = stream.str();
    const auto texts = CollectBlockTexts(file, "##TX");
    const auto comments = CollectBlockTexts(file, "##MD");

    zassert_true(ContainsText(texts, "oil_pressure"), "group and value channel are named after the sensor id");
    zassert_true(ContainsText(texts, "oil_pressure.raw"), "the raw channel is the sensor id + .raw");
    zassert_true(ContainsText(texts, "can_raw"), "the CAN group is named after the sensor id");
    zassert_true(ContainsText(texts, "bar"), "the value channel carries the sensor unit");
    zassert_false(ContainsText(texts, "value") || ContainsText(texts, "raw_value") || ContainsText(texts, "Raw CAN Frame"),
        "the generic names of the old layout are gone");

    zassert_true(AnyContains(comments, "<CNcomment><TX>Name of oil_pressure: Description of oil_pressure</TX></CNcomment>"),
        "the value channel comment is the sensor name and description");
    zassert_true(AnyContains(comments, "<CGcomment><TX>Raw frames</TX></CGcomment>"),
        "the CAN group comment is the sensor name");
}

ZTEST(mdf4_logger, test_RecordIdSizeFollowsTheSensorCount) {
    zassert_equal(Mdf4LoggerSensorReading::GetRecordIdSizeBytes(127), 1, "255 record ids still fit into one byte");
    zassert_equal(Mdf4LoggerSensorReading::GetRecordIdSizeBytes(128), 2, "257 record ids need two bytes");

    std::vector<std::shared_ptr<Sensor>> sensors;
    for(int i = 0; i < 130; i++)
        sensors.push_back(MakeValueSensor("sensor_" + std::to_string(i)));

    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");
    const uint64_t header_size = logger.GetBytesWritten();

    zassert_true(logger.LogReading(START_TIME, MakeValueReading(*sensors[129], 5.0F, START_TIME)), "the last sensor is logged");

    const auto records = RecordBytes(stream, header_size);
    zassert_equal(records.size(), 2 + TIME_SIZE + VALUE_SIZE + INVALIDATION_SIZE, "130 sensors need a two byte record id (got %zu)",
        records.size());
    zassert_equal(ReadAt<uint16_t>(records, 0), 130, "the last of 130 groups has record id 130");
}

ZTEST(mdf4_logger, test_HeaderPropertiesAreWritten) {
    const std::vector<std::shared_ptr<Sensor>> sensors{MakeValueSensor("sensor_1")};
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a string buffer");

    const auto comments = CollectBlockTexts(stream.str(), "##MD");

    zassert_true(AnyContains(comments, "<HDcomment><TX>Eerie Leap sensor log</TX><common_properties>"),
        "the header comment opens with its text and the properties");
    zassert_true(AnyContains(comments, "<e name=\"device_id\">7</e>"), "the device id is a header property");
    zassert_true(AnyContains(comments, "<e name=\"sw_version\">" + std::to_string(TEST_SW_VERSION) + "</e>"),
        "the software version is a header property");
    zassert_true(AnyContains(comments, "<e name=\"device_name\">unit_board</e>"), "the device name is a header property");
    zassert_true(AnyContains(comments, "<e name=\"sensors_configuration_hash\">3735928559</e>"),
        "the sensors configuration hash is a header property");
    zassert_true(AnyContains(comments, "<e name=\"logging_interval_ms\">50</e>"), "the logging interval is a header property");
    zassert_true(AnyContains(comments, "<tool_id>rt_core_unit</tool_id>"), "the file history names the tool");
    zassert_true(AnyContains(comments, "<tool_vendor>Eerie Leap</tool_vendor>"), "the file history names the vendor");
    zassert_true(AnyContains(comments, "<tool_version>1.2.3</tool_version>"), "the file history carries the tool version");
}

ZTEST(mdf4_logger, test_LogFileNaming) {
    using namespace std::chrono;

    const system_clock::time_point time = sys_days{year{2026} / October / 11} + hours{9} + minutes{30};

    const auto path = LogFileNaming::GetFilePath("data_logs", "log_data", time, 4242, "mf4");
    zassert_true(path == "data_logs/log_data_20261011T093000Z_4242.mf4", "unexpected file name %s", path.CStr());

    const auto without_prefix = LogFileNaming::GetFilePath("data_logs", "", time + seconds{5}, 123456, "mf4");
    zassert_true(without_prefix == "data_logs/20261011T093005Z_3456.mf4",
        "an empty prefix drops its underscore and the suffix keeps four digits (got %s)", without_prefix.CStr());
}

ZTEST(mdf4_logger, test_AllocationCounterCounts) {
    const size_t before = GetAllocationCount();
    auto block = std::make_unique<uint8_t[]>(32);

    zassert_equal(GetAllocationCount() - before, 1,
        "the replaced operator new must count, or test_ATickAllocatesNothing proves nothing");
}

// The writers, their buffers and the targets are built by StartLogging(); a tick only finds the
// target and hands the record to its writer.
ZTEST(mdf4_logger, test_ATickAllocatesNothing) {
    constexpr size_t value_sensor_count = 64;
    constexpr size_t can_sensor_count = 8;
    constexpr size_t frame_count = 32;

    std::vector<std::shared_ptr<Sensor>> sensors;
    for(size_t i = 0; i < value_sensor_count; i++) {
        // Every other sensor gets a raw value channel, so both value record layouts are written.
        sensors.push_back(MakeValueSensor("sensor_" + std::to_string(i), i % 2 == 0 ? "x * 2" : ""));
    }
    for(size_t i = 0; i < can_sensor_count; i++)
        sensors.push_back(MakeCanRawSensor("can_" + std::to_string(i), static_cast<uint8_t>(i % 2)));

    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    const auto reading_time = START_TIME + std::chrono::milliseconds(90);
    const auto tick_time = START_TIME + std::chrono::milliseconds(100);

    std::vector<SensorReading> value_readings;
    for(size_t i = 0; i < value_sensor_count; i++) {
        auto reading = MakeValueReading(*sensors[i], static_cast<float>(i), reading_time);
        reading.raw_value = static_cast<float>(i) / 2.0F;

        // Invalid readings take the same path and must not allocate either.
        if(i % 3 == 1)
            reading.SetError(ReadingError::EXPRESSION_FAILED);
        if(i % 4 == 2)
            reading.raw_value.reset();

        value_readings.push_back(reading);
    }

    std::array<CanFrame, frame_count> frames;
    std::vector<SensorReading> frame_readings;
    for(size_t i = 0; i < frame_count; i++) {
        frames[i].id = static_cast<uint32_t>(0x100 + i);
        frames[i].is_extended = i % 2 == 1;
        frames[i].is_can_fd = i % 4 == 0;
        frames[i].data = std::vector<uint8_t>(i % 4 == 0 ? 64 : 8, static_cast<uint8_t>(i));

        SensorReading reading(sensors[value_sensor_count + i % can_sensor_count].get());
        reading.source = ReadingSource::ISR;
        reading.status = ReadingStatus::PROCESSED;
        reading.timestamp = reading_time + std::chrono::microseconds(i);
        reading.can_frame = &frames[i];
        frame_readings.push_back(reading);
    }

    auto stream = MakeStream();
    stream.Reserve(192 * 1024);
    const size_t capacity = stream.Capacity();

    zassert_true(logger.StartLogging(stream, START_TIME), "StartLogging must succeed on a memory buffer");

    const size_t allocations_before = GetAllocationCount();
    size_t logged = 0;

    for(const auto& reading : value_readings)
        logged += logger.LogReading(tick_time, reading) ? 1 : 0;
    for(const auto& reading : frame_readings)
        logged += logger.LogReading(tick_time, reading) ? 1 : 0;

    const size_t allocations = GetAllocationCount() - allocations_before;

    zassert_equal(logged, value_sensor_count + frame_count, "every reading of the tick must be logged (got %zu)", logged);
    zassert_true(logger.GetBytesWritten() <= capacity,
        "the %llu bytes written outgrew the %zu reserved, so the stream itself allocated",
        static_cast<unsigned long long>(logger.GetBytesWritten()), capacity);
    zassert_equal(allocations, 0, "%zu allocations while logging 64 value readings and 32 frames", allocations);
}
