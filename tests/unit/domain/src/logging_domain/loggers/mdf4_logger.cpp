#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef CONFIG_ARCH_POSIX
    #include <fstream>
#endif

#include <eerie_memory.hpp>
#include <zephyr/ztest.h>

#include "domain/logging_domain/event_bus/logging_status_reason.h"
#include "domain/logging_domain/loggers/logger_metadata.h"
#include "domain/logging_domain/loggers/mdf4_logger_sensor_reading.h"
#include "domain/logging_domain/models/logging_configuration.h"
#include "domain/logging_domain/services/log_file_naming.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "subsys/canbus/can_frame.h"
#include "utilities/memory/memory_resource_manager.h"
#include "utilities/voltage_interpolator/interpolation_method.h"

#include "allocation_counter.h"
#include "memory_stream_buf.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::canbus;
using eerie_leap::utilities::voltage_interpolator::InterpolationMethod;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using namespace eerie_leap::domain::logging_domain::models;
using namespace eerie_leap::domain::logging_domain::loggers;
using namespace eerie_leap::domain::logging_domain::services;
using rt_core_test::GetAllocationCount;
using rt_core_test::MemoryStreamBuf;

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
constexpr uint32_t TEST_UNIT_UID = 2864434397U;
constexpr uint32_t TEST_BOOT_ID = 913024577U;
constexpr uint32_t TEST_SW_VERSION = 0x01020003;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
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
    std::span<const std::shared_ptr<Sensor>> sensors,
    bool log_raw_value = false,
    bool log_only_new_data = false
) {

    LoggingConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    configuration.logging_interval_ms = 50;

    for(const auto& sensor : sensors) {
        configuration.sensor_configurations.emplace(
            sensor->id_hash,
            SensorLoggingConfiguration{
                .is_enabled = true, .log_raw_value = log_raw_value, .log_only_new_data = log_only_new_data }
        );
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
    metadata.unit_uid = TEST_UNIT_UID;
    metadata.boot_id = TEST_BOOT_ID;
    metadata.hw_version = 0x00000001;
    metadata.sw_version = TEST_SW_VERSION;
    metadata.sensors_configuration_hash = 0xDEADBEEF;

    return metadata;
}

SensorReading MakeValueReading(
    const Sensor& sensor,
    std::optional<float> value,
    std::optional<std::chrono::system_clock::time_point> timestamp
) {

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
    return stream.Str().substr(static_cast<size_t>(header_size));
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
    return std::ranges::any_of(texts, [fragment](const std::string& text) {
        return text.contains(fragment);
    });
}

constexpr size_t LINK_SIZE = 8;
constexpr size_t BLOCK_LINKS_OFFSET = 24;
constexpr size_t CN_LINK_NAME = 2;
constexpr size_t CN_LINK_SOURCE = 3;
constexpr size_t CN_DATA_OFFSET = BLOCK_LINKS_OFFSET + 8 * LINK_SIZE;
constexpr size_t CN_FLAGS_OFFSET = CN_DATA_OFFSET + 12;
constexpr size_t CN_LIMIT_MIN_OFFSET = CN_DATA_OFFSET + 40;
constexpr size_t CN_LIMIT_MAX_OFFSET = CN_DATA_OFFSET + 48;
constexpr uint32_t CN_FLAG_LIMIT_VALID = 0x10;
constexpr uint32_t CN_FLAG_INVALID_VALID = 0x02;

std::string TextAt(std::string_view file, uint64_t address) {
    const auto length = static_cast<size_t>(ReadAt<uint64_t>(file, address + BLOCK_LENGTH_OFFSET));
    const auto text = file.substr(address + BLOCK_HEADER_SIZE, length - BLOCK_HEADER_SIZE);

    return std::string(text.substr(0, text.find('\0')));
}

// The CN block named @p channel_name: its file offset, or 0 when there is none.
size_t FindChannel(std::string_view file, std::string_view channel_name) {
    size_t offset = ID_BLOCK_SIZE;

    while(offset + BLOCK_HEADER_SIZE <= file.size()) {
        const auto length = static_cast<size_t>(ReadAt<uint64_t>(file, offset + BLOCK_LENGTH_OFFSET));
        zassert_true(length >= BLOCK_HEADER_SIZE, "block at %zu has an invalid length", offset);

        const auto id = file.substr(offset, 4);
        if(id == "##DT")
            break;

        if(id == "##CN") {
            const auto name = ReadAt<uint64_t>(file, offset + BLOCK_LINKS_OFFSET + CN_LINK_NAME * LINK_SIZE);
            if(name != 0 && TextAt(file, name) == channel_name)
                return offset;
        }

        offset += length;
    }

    return 0;
}

struct ChannelSource {
    bool found = false;
    uint64_t address = 0;
    std::string name;
    uint8_t source_type = 0;
    uint8_t bus_type = 0;
};

// What the channel's cn_si_source link leads to; found is false without a link.
ChannelSource FindChannelSource(std::string_view file, std::string_view channel_name) {
    const size_t channel = FindChannel(file, channel_name);
    zassert_true(channel != 0, "channel %s must exist", std::string(channel_name).c_str());

    ChannelSource source;
    source.address = ReadAt<uint64_t>(file, channel + BLOCK_LINKS_OFFSET + CN_LINK_SOURCE * LINK_SIZE);
    if(source.address == 0)
        return source;

    zassert_equal(file.substr(source.address, 4), "##SI", "cn_si_source must lead to an SI block");
    source.found = true;
    source.name = TextAt(file, ReadAt<uint64_t>(file, source.address + BLOCK_LINKS_OFFSET));
    source.source_type = ReadAt<uint8_t>(file, source.address + BLOCK_LINKS_OFFSET + uint64_t{ 3 } * LINK_SIZE);
    source.bus_type = ReadAt<uint8_t>(file, source.address + BLOCK_LINKS_OFFSET + uint64_t{ 3 } * LINK_SIZE + 1);

    return source;
}

struct ChannelLimits {
    bool valid = false;
    double min = 0;
    double max = 0;
};

ChannelLimits FindChannelLimits(std::string_view file, std::string_view channel_name) {
    const size_t channel = FindChannel(file, channel_name);
    zassert_true(channel != 0, "channel %s must exist", std::string(channel_name).c_str());

    zassert_true(
        (ReadAt<uint32_t>(file, channel + CN_FLAGS_OFFSET) & CN_FLAG_INVALID_VALID) != 0,
        "the limit flag must not replace the invalidation flag"
    );

    return { (ReadAt<uint32_t>(file, channel + CN_FLAGS_OFFSET) & CN_FLAG_LIMIT_VALID) != 0,
        ReadAt<double>(file, channel + CN_LIMIT_MIN_OFFSET), ReadAt<double>(file, channel + CN_LIMIT_MAX_OFFSET) };
}

std::shared_ptr<Sensor> MakeTypedSensor(std::string_view id, SensorType type) {
    auto sensor = MakeValueSensor(id);
    sensor->configuration.type = type;

    return sensor;
}

std::string StartAndGetFile(Mdf4LoggerSensorReading& logger) {
    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );

    return stream.Str();
}

// native_sim links the host libc, so the file can be handed to asammdf on the host.
void DumpToHost([[maybe_unused]] const MemoryStreamBuf& stream, [[maybe_unused]] std::string_view file_name) {
#ifdef CONFIG_ARCH_POSIX
    const std::string_view dump_dir;
    if(dump_dir.empty())
        return;

    const std::string path = std::string(dump_dir) + "/" + std::string(file_name);
    const std::string content = stream.Str();

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    zassert_true(file.is_open(), "the dump file %s could not be opened", path.c_str());
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    zassert_true(file.good(), "the dump file %s could not be written", path.c_str());
#endif
}

} // namespace

ZTEST(mdf4_logger, test_ValueReadingRecordBytes) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto time = START_TIME + std::chrono::milliseconds(1500);
    zassert_true(
        logger.LogReading(time, MakeValueReading(*sensors[0], 42.5F, time)), "an enabled value reading must be logged"
    );

    const auto records = RecordBytes(stream, header_size);
    zassert_equal(
        records.size(), VALUE_RECORD_SIZE, "a value record is id 1 + time 8 + value 4 + invalidation 1 (got %zu)",
        records.size()
    );
    zassert_equal(static_cast<uint8_t>(records[0]), 1, "the first group has record id 1 in one byte");
    zassert_equal(ReadAt<double>(records, 1), 1.5, "the time is a Float64 delta from the start time in seconds");
    zassert_equal(ReadAt<float>(records, 1 + TIME_SIZE), 42.5F, "the value follows the time as Float32");
    zassert_equal(
        static_cast<uint8_t>(records[1 + TIME_SIZE + VALUE_SIZE]), 0, "a valid reading clears its invalidation bit"
    );
    zassert_equal(logger.GetBytesWritten(), header_size + records.size(), "the byte count must include the record");
}

ZTEST(mdf4_logger, test_RawValueRecord) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1", "x * 4 + 1.6") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    auto reading = MakeValueReading(*sensors[0], 9.6F, START_TIME + std::chrono::milliseconds(250));
    reading.raw_value = 2.0F;
    zassert_true(logger.LogReading(*reading.timestamp, reading), "a reading with a raw value must be logged");

    auto without_raw = MakeValueReading(*sensors[0], 1.6F, START_TIME + std::chrono::milliseconds(500));
    zassert_true(
        logger.LogReading(*without_raw.timestamp, without_raw), "a reading without a raw value is still logged"
    );

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t RECORD_SIZE = RAW_RECORD_SIZE;
    constexpr size_t MASK = RECORD_SIZE - 1;
    zassert_equal(
        records.size(), 2 * RECORD_SIZE, "two records of id + time + value + raw + invalidation (got %zu)",
        records.size()
    );
    zassert_equal(ReadAt<double>(records, 1), 0.25, "the first record is at 0.25 s");
    zassert_equal(ReadAt<float>(records, 1 + TIME_SIZE), 9.6F, "the value comes first");
    zassert_equal(ReadAt<float>(records, 1 + TIME_SIZE + VALUE_SIZE), 2.0F, "the raw value follows the value");
    zassert_equal(static_cast<uint8_t>(records[MASK]), 0, "a reading with both values clears both bits");
    zassert_equal(ReadAt<float>(records, RECORD_SIZE + 1 + TIME_SIZE), 1.6F, "a missing raw value keeps the value");
    zassert_equal(
        ReadAt<float>(records, RECORD_SIZE + 1 + TIME_SIZE + VALUE_SIZE), 0.0F, "a missing raw value is written as 0"
    );
    zassert_equal(
        static_cast<uint8_t>(records[RECORD_SIZE + MASK]), RAW_VALUE_INVALID,
        "a missing raw value sets the raw channel's bit only"
    );
}

ZTEST(mdf4_logger, test_CanFrameRecord) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeCanRawSensor("can_raw", 1) };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    CanFrame classic;
    classic.id = 0x123;
    classic.data = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };

    CanFrame fd;
    fd.id = 0x18FEF100;
    fd.is_extended = true;
    fd.is_can_fd = true;
    fd.is_bitrate_switch = true;
    fd.data = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

    CanFrame transmitted;
    transmitted.id = 0x7FF;
    transmitted.is_transmit = true;
    transmitted.data = { 0xA5 };

    const std::array<const CanFrame*, 3> frames{ &classic, &fd, &transmitted };
    for(size_t i = 0; i < frames.size(); i++) {
        SensorReading reading(sensors[0].get());
        reading.source = ReadingSource::ISR;
        reading.status = ReadingStatus::PROCESSED;
        reading.timestamp = START_TIME + std::chrono::microseconds(1000 + 250 * i);
        reading.can_frame = frames[i];

        zassert_true(logger.LogReading(*reading.timestamp, reading), "frame %zu must be logged", i);
    }

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t CAN_RECORD_SIZE = 1 + TIME_SIZE + CAN_FRAME_SIZE;
    zassert_equal(CAN_RECORD_SIZE, 24, "a CAN record is id 1 + time 8 + CAN_DataFrame 15");

    // The VLSD group is created first: id 1 for the payloads, id 2 for the CAN_DataFrame records.
    // Each payload record goes out before its frame record.
    zassert_equal(static_cast<uint8_t>(records[0]), 1, "the payload comes first, in the VLSD group");
    zassert_equal(ReadAt<uint32_t>(records, 1), 8U, "the VLSD record carries the payload length");
    zassert_equal(static_cast<uint8_t>(records[1 + VLSD_LENGTH_SIZE]), 0x11, "the payload follows its length");

    const size_t first = 1 + VLSD_LENGTH_SIZE + 8;
    zassert_equal(static_cast<uint8_t>(records[first]), 2, "the CAN group has record id 2");
    zassert_equal(ReadAt<double>(records, first + 1), 0.001, "the frame time is its own timestamp");
    zassert_equal(
        static_cast<uint8_t>(records[first + 1 + TIME_SIZE]), 2, "BusChannel is the source bus channel 1 plus 1"
    );
    zassert_equal(ReadAt<uint32_t>(records, first + 1 + TIME_SIZE + 1), 0x123U, "a standard ID is packed without IDE");
    zassert_equal(static_cast<uint8_t>(records[first + 1 + TIME_SIZE + 5]), 8, "DLC 8, receive, classic CAN");
    zassert_equal(static_cast<uint8_t>(records[first + 1 + TIME_SIZE + 6]), 8, "DataLength is the payload size");
    zassert_equal(ReadAt<uint64_t>(records, first + 1 + TIME_SIZE + 7), 0U, "the first VLSD offset is 0, 64 bits wide");

    const size_t second = first + CAN_RECORD_SIZE + 1 + VLSD_LENGTH_SIZE + 12;
    zassert_equal(static_cast<uint8_t>(records[second]), 2, "the second frame record follows its 12 byte payload");
    zassert_equal(ReadAt<double>(records, second + 1), 0.00125, "frames of one tick keep their own times");
    zassert_equal(
        ReadAt<uint32_t>(records, second + 1 + TIME_SIZE + 1), 0x18FEF100U | (1U << 31),
        "an extended ID sets IDE in bit 31"
    );
    zassert_equal(
        static_cast<uint8_t>(records[second + 1 + TIME_SIZE + 5]), 9 | (1 << 5) | (1 << 6),
        "DLC 9 for 12 bytes, EDL and BRS set"
    );
    zassert_equal(static_cast<uint8_t>(records[second + 1 + TIME_SIZE + 6]), 12, "DataLength is 12");
    zassert_equal(
        ReadAt<uint64_t>(records, second + 1 + TIME_SIZE + 7), 4U + 8U, "the offset advances by length + payload"
    );

    zassert_true(logger.StopLogging(LogFileEnd{ .time = START_TIME }), "a string buffer finalizes");
    DumpToHost(stream, "logger_unit_dump.mf4");
}

// An error reading's value is stale and a missing value unknown: both are written as 0 with the
// value channel's bit set, so readers show a gap; a missing raw value invalidates the raw channel only.
ZTEST(mdf4_logger, test_InvalidReadingsSetTheirInvalidationBits) {
    using std::chrono::milliseconds;

    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("oil_pressure", "x * 4 + 1.6"),
        MakeValueSensor("coolant_temperature") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto reading =
        [&sensors](size_t sensor, std::optional<float> value, std::optional<float> raw_value, int time_ms) {
            auto result = MakeValueReading(*sensors[sensor], value, START_TIME + milliseconds(time_ms));
            result.raw_value = raw_value;

            return result;
        };

    const auto valid = reading(0, 9.6F, 2.0F, 100);
    // The expression failed after the input was measured: the value is a leftover, the raw value real.
    auto error = reading(0, 9.6F, 2.5F, 200);
    error.SetError(ReadingError::EXPRESSION_FAILED);
    const auto without_value = reading(0, std::nullopt, 3.0F, 300);
    const auto without_raw = reading(0, 13.6F, std::nullopt, 400);
    auto failed_read = reading(0, std::nullopt, std::nullopt, 500);
    failed_read.SetError(ReadingError::READER_FAILED);
    const auto valid_again = reading(0, 15.6F, 3.5F, 600);

    const auto coolant_valid = reading(1, 80.0F, std::nullopt, 150);
    auto coolant_error = reading(1, 80.0F, std::nullopt, 250);
    coolant_error.SetError(ReadingError::READER_FAILED);
    const auto coolant_valid_again = reading(1, 81.0F, std::nullopt, 350);

    const std::array<const SensorReading*, 9> readings{ &valid, &coolant_valid, &error, &coolant_error, &without_value,
        &coolant_valid_again, &without_raw, &failed_read, &valid_again };
    for(size_t i = 0; i < readings.size(); i++)
        zassert_true(
            logger.LogReading(*readings[i]->timestamp, *readings[i]), "reading %zu must be written, not skipped", i
        );

    struct Expected {
        float value;
        float raw_value;
        uint8_t mask;
    };
    const std::array<Expected, 6> expected_oil{ { { 9.6F, 2.0F, 0 }, { 0.0F, 2.5F, VALUE_INVALID },
        { 0.0F, 3.0F, VALUE_INVALID }, { 13.6F, 0.0F, RAW_VALUE_INVALID },
        { 0.0F, 0.0F, VALUE_INVALID | RAW_VALUE_INVALID }, { 15.6F, 3.5F, 0 } } };
    const std::array<Expected, 3> expected_coolant{ { { 80.0F, 0.0F, 0 }, { 0.0F, 0.0F, VALUE_INVALID },
        { 81.0F, 0.0F, 0 } } };

    const auto records = RecordBytes(stream, header_size);
    zassert_equal(
        records.size(), expected_oil.size() * RAW_RECORD_SIZE + expected_coolant.size() * VALUE_RECORD_SIZE,
        "every reading is one record (got %zu bytes)", records.size()
    );

    size_t oil = 0;
    size_t coolant = 0;
    for(size_t position = 0; position < records.size();) {
        const auto record_id = static_cast<uint8_t>(records[position]);
        const auto value = ReadAt<float>(records, position + 1 + TIME_SIZE);

        if(record_id == 1) {
            const auto& expected = expected_oil.at(oil);
            zassert_equal(value, expected.value, "oil record %zu value", oil);
            zassert_equal(
                ReadAt<float>(records, position + 1 + TIME_SIZE + VALUE_SIZE), expected.raw_value,
                "oil record %zu raw value", oil
            );
            zassert_equal(
                static_cast<uint8_t>(records[position + RAW_RECORD_SIZE - 1]), expected.mask,
                "oil record %zu invalidation bits", oil
            );
            position += RAW_RECORD_SIZE;
            oil++;
        } else {
            zassert_equal(record_id, 2, "unknown record id %u at %zu", static_cast<unsigned>(record_id), position);
            const auto& expected = expected_coolant.at(coolant);
            zassert_equal(value, expected.value, "coolant record %zu value", coolant);
            zassert_equal(
                static_cast<uint8_t>(records[position + VALUE_RECORD_SIZE - 1]), expected.mask,
                "coolant record %zu invalidation bits", coolant
            );
            position += VALUE_RECORD_SIZE;
            coolant++;
        }
    }
    zassert_equal(oil, expected_oil.size());
    zassert_equal(coolant, expected_coolant.size());

    zassert_true(logger.StopLogging(LogFileEnd{ .time = START_TIME }), "a memory buffer finalizes");
    DumpToHost(stream, "logger_invalidation_dump.mf4");
}

// An error reading carries its own timestamp, so it is a sample like any other: new once, then held.
ZTEST(mdf4_logger, test_ErrorReadingIsASampleForLogOnlyNewData) {
    using std::chrono::milliseconds;

    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };

    auto error = MakeValueReading(*sensors[0], 5.0F, START_TIME + milliseconds(200));
    error.SetError(ReadingError::READER_FAILED);
    const auto first = MakeValueReading(*sensors[0], 1.0F, START_TIME + milliseconds(100));
    const auto recovered = MakeValueReading(*sensors[0], 2.0F, START_TIME + milliseconds(400));

    for(const bool log_only_new_data : { true, false }) {
        Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, log_only_new_data), sensors, MakeMetadata());

        auto stream = MakeStream();
        zassert_true(
            logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
            "StartLogging must succeed on a string buffer"
        );
        const uint64_t header_size = logger.GetBytesWritten();

        zassert_true(logger.LogReading(START_TIME + milliseconds(100), first), "the first sample is new");
        zassert_true(
            logger.LogReading(START_TIME + milliseconds(200), error), "an error with a new timestamp is a new sample"
        );
        zassert_equal(
            logger.LogReading(START_TIME + milliseconds(300), error), !log_only_new_data, "a held error is %s",
            log_only_new_data ? "skipped with log_only_new_data" : "written again at the tick time"
        );
        zassert_true(logger.LogReading(START_TIME + milliseconds(400), recovered), "the next sample is new");

        const auto records = RecordBytes(stream, header_size);
        const size_t record_count = log_only_new_data ? 3 : 4;
        zassert_equal(
            records.size(), record_count * VALUE_RECORD_SIZE, "%zu records (got %zu bytes)", record_count,
            records.size()
        );

        for(size_t i = 0; i < record_count; i++) {
            const bool is_error = i == 1 || (i == 2 && !log_only_new_data);
            zassert_equal(
                static_cast<uint8_t>(records[(i + 1) * VALUE_RECORD_SIZE - 1]), is_error ? VALUE_INVALID : 0,
                "record %zu must be %s", i, is_error ? "invalid" : "valid"
            );
        }

        if(!log_only_new_data) {
            zassert_within(
                ReadAt<double>(records, 2 * VALUE_RECORD_SIZE + 1), 0.3, 1e-9,
                "the held error is repeated at the tick time"
            );
            zassert_equal(
                ReadAt<float>(records, 2 * VALUE_RECORD_SIZE + 1 + TIME_SIZE), 0.0F, "and written as 0 again"
            );
        }
    }
}

// Queued frames are separate bus events even when they share the time service's millisecond.
ZTEST(mdf4_logger, test_CanFramesIgnoreLogOnlyNewData) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeCanRawSensor("can_raw", 0) };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    CanFrame frame;
    frame.id = 0x100;
    frame.data = { 1, 2 };

    SensorReading reading(sensors[0].get());
    reading.source = ReadingSource::ISR;
    reading.status = ReadingStatus::PROCESSED;
    reading.timestamp = START_TIME + std::chrono::milliseconds(10);
    reading.can_frame = &frame;

    zassert_true(logger.LogReading(*reading.timestamp, reading), "the first frame is written");
    zassert_true(logger.LogReading(*reading.timestamp, reading), "a frame with the same timestamp is written too");

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t PAIR_SIZE = 1 + VLSD_LENGTH_SIZE + 2 + 1 + TIME_SIZE + CAN_FRAME_SIZE;
    zassert_equal(records.size(), 2 * PAIR_SIZE, "two payload and frame record pairs (got %zu)", records.size());
}

ZTEST(mdf4_logger, test_DisabledSensorIsNotWritten) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1"), MakeValueSensor("sensor_2") };
    auto configuration = MakeConfiguration(sensors);
    configuration.sensor_configurations.at(sensors[1]->id_hash).is_enabled = false;

    Mdf4LoggerSensorReading logger(configuration, sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto texts = CollectBlockTexts(stream.Str(), "##TX");
    zassert_false(ContainsText(texts, "sensor_2"), "a disabled sensor must not get a channel group");

    zassert_false(
        logger.LogReading(START_TIME, MakeValueReading(*sensors[1], 1.0F, START_TIME)),
        "a reading of a disabled sensor must not be logged"
    );
    zassert_equal(logger.GetBytesWritten(), header_size, "nothing may be written for a disabled sensor");
}

// A disabled sensor never has a reading, so it gets no channel whatever its logging configuration says.
ZTEST(mdf4_logger, test_DisabledSensorGetsNoChannelEvenWithLoggingEnabled) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("disabled_value"),
        MakeCanRawSensor("disabled_raw", 0), MakeValueSensor("enabled_value") };
    sensors[0]->is_enabled = false;
    sensors[1]->is_enabled = false;

    // The logging configuration enables all three.
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto texts = CollectBlockTexts(stream.Str(), "##TX");
    zassert_true(ContainsText(texts, "enabled_value"), "an enabled sensor keeps its channel group");
    zassert_false(ContainsText(texts, "disabled_value"), "a disabled sensor must not get a channel group");
    zassert_false(ContainsText(texts, "disabled_raw"), "a disabled raw CAN sensor must not get a CAN group");

    zassert_false(
        logger.LogReading(START_TIME, MakeValueReading(*sensors[0], 1.0F, START_TIME)),
        "a reading of a disabled sensor must not be logged"
    );

    CanFrame frame;
    frame.id = 0x100;
    frame.data = { 1, 2 };

    SensorReading frame_reading(sensors[1].get());
    frame_reading.source = ReadingSource::ISR;
    frame_reading.status = ReadingStatus::PROCESSED;
    frame_reading.timestamp = START_TIME;
    frame_reading.can_frame = &frame;
    zassert_false(
        logger.LogReading(START_TIME, frame_reading), "a frame of a disabled raw CAN sensor must not be logged"
    );
    zassert_equal(logger.GetBytesWritten(), header_size, "nothing may be written for a disabled sensor");

    zassert_true(
        logger.LogReading(START_TIME, MakeValueReading(*sensors[2], 2.0F, START_TIME)), "the enabled sensor is logged"
    );

    const auto records = RecordBytes(stream, header_size);
    zassert_equal(records.size(), VALUE_RECORD_SIZE, "one value record (got %zu)", records.size());
    zassert_equal(
        static_cast<uint8_t>(records[0]), 1, "disabled sensors take no record ids, so the enabled one has the first"
    );
}

ZTEST(mdf4_logger, test_RecordIdSizeCountsOnlyEnabledSensors) {
    std::vector<std::shared_ptr<Sensor>> sensors;
    sensors.reserve(130);
    for(int i = 0; i < 130; i++)
        sensors.push_back(MakeValueSensor("sensor_" + std::to_string(i)));

    // 127 enabled sensors fit one byte record ids, as test_RecordIdSizeFollowsTheSensorCount shows.
    for(int i = 0; i < 3; i++)
        sensors[i]->is_enabled = false;

    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    zassert_true(
        logger.LogReading(START_TIME, MakeValueReading(*sensors[129], 5.0F, START_TIME)), "the last sensor is logged"
    );

    const auto records = RecordBytes(stream, header_size);
    zassert_equal(
        records.size(), VALUE_RECORD_SIZE, "127 enabled sensors need a one byte record id (got %zu)", records.size()
    );
    zassert_equal(static_cast<uint8_t>(records[0]), 127, "the last of 127 groups has record id 127");
}

ZTEST(mdf4_logger, test_LogOnlyNewDataSkipsUnchangedTimestamp) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );

    const auto first = START_TIME + std::chrono::milliseconds(100);
    const auto second = START_TIME + std::chrono::milliseconds(200);

    zassert_true(logger.LogReading(first, MakeValueReading(*sensors[0], 1.0F, first)), "the first reading is new");
    zassert_false(
        logger.LogReading(first + std::chrono::milliseconds(50), MakeValueReading(*sensors[0], 1.0F, first)),
        "the same reading timestamp again must be skipped"
    );
    zassert_true(
        logger.LogReading(second, MakeValueReading(*sensors[0], 2.0F, second)), "a changed timestamp is new data"
    );
}

ZTEST(mdf4_logger, test_ReadingWithoutTimestampUsesTheGivenTime) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto tick_time = START_TIME + std::chrono::milliseconds(750);
    zassert_true(
        logger.LogReading(tick_time, MakeValueReading(*sensors[0], 3.0F, std::nullopt)),
        "a reading without a timestamp is logged at the given time"
    );
    zassert_true(
        logger.LogReading(tick_time, MakeValueReading(*sensors[0], 3.0F, std::nullopt)),
        "a reading without a timestamp never counts as unchanged data"
    );
    zassert_true(
        logger.LogReading(tick_time, MakeValueReading(*sensors[0], std::nullopt, std::nullopt)),
        "a reading without a value is written, marked invalid"
    );

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t RECORD_SIZE = VALUE_RECORD_SIZE;
    zassert_equal(records.size(), 3 * RECORD_SIZE, "three records written (got %zu)", records.size());
    zassert_equal(ReadAt<double>(records, 1), 0.75, "the record time is the time the caller passed");
    zassert_true(ReadAt<double>(records, RECORD_SIZE + 1) > 0.75, "the master channel of a group strictly increases");
    zassert_true(
        ReadAt<double>(records, 2 * RECORD_SIZE + 1) > ReadAt<double>(records, RECORD_SIZE + 1),
        "an invalid record keeps the master channel increasing"
    );
    zassert_equal(
        ReadAt<float>(records, 2 * RECORD_SIZE + 1 + TIME_SIZE), 0.0F, "a reading without a value is written as 0"
    );
    zassert_equal(
        static_cast<uint8_t>(records[3 * RECORD_SIZE - 1]), VALUE_INVALID, "with the value channel's bit set"
    );
}

// The tick offers every sensor's latest sample each time; without log_only_new_data a held sample
// is written again at the tick time, never again at its own time.
ZTEST(mdf4_logger, test_HeldSampleIsRepeatedAtTheTickTime) {
    using std::chrono::milliseconds;

    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, false), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto sample_1 = MakeValueReading(*sensors[0], 1.0F, START_TIME + milliseconds(100));
    const auto sample_2 = MakeValueReading(*sensors[0], 2.0F, START_TIME + milliseconds(350));
    // Stamped before the tick at 400 ms that still saw sample_2, published after it.
    const auto sample_3 = MakeValueReading(*sensors[0], 3.0F, START_TIME + milliseconds(390));

    zassert_true(logger.LogReading(START_TIME + milliseconds(100), sample_1), "a new sample is written");
    zassert_false(
        logger.LogReading(START_TIME + milliseconds(100), sample_1),
        "a repeat at the time of its own record would duplicate that record"
    );
    zassert_true(logger.LogReading(START_TIME + milliseconds(200), sample_1), "a held sample is written again");
    zassert_true(logger.LogReading(START_TIME + milliseconds(300), sample_1), "and again at the next tick");
    zassert_true(
        logger.LogReading(START_TIME + milliseconds(400), sample_2), "a new sample is written at its own time"
    );
    zassert_true(logger.LogReading(START_TIME + milliseconds(400), sample_2), "a held sample at a later tick time");
    zassert_true(logger.LogReading(START_TIME + milliseconds(500), sample_3), "a late new sample is still written");

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t RECORD_SIZE = VALUE_RECORD_SIZE;
    zassert_equal(records.size(), 6 * RECORD_SIZE, "six records (got %zu)", records.size());

    const std::array<double, 6> expected_times{ 0.1, 0.2, 0.3, 0.35, 0.4, 0.4 + 0.000001 };
    const std::array<float, 6> expected_values{ 1.0F, 1.0F, 1.0F, 2.0F, 2.0F, 3.0F };
    for(size_t i = 0; i < expected_times.size(); i++) {
        const auto time = ReadAt<double>(records, i * RECORD_SIZE + 1);
        zassert_within(time, expected_times[i], 1e-9, "record %zu at %f s, expected %f s", i, time, expected_times[i]);
        zassert_equal(
            ReadAt<float>(records, i * RECORD_SIZE + 1 + TIME_SIZE), expected_values[i], "record %zu value", i
        );
    }
}

// Float64 seconds keep microsecond steps a day into a session, where Float32 had 8 ms.
ZTEST(mdf4_logger, test_TimeKeepsMicrosecondsAfterADay) {
    using std::chrono::hours;
    using std::chrono::microseconds;
    using std::chrono::milliseconds;

    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto day = START_TIME + hours(24);
    const std::array<std::chrono::system_clock::time_point, 4> times{ day + milliseconds(1), day + milliseconds(2),
        day + milliseconds(2) + microseconds(1), day + milliseconds(2) + microseconds(2) };

    for(const auto& time : times)
        zassert_true(logger.LogReading(time, MakeValueReading(*sensors[0], 1.0F, time)), "every sample is new");

    const auto records = RecordBytes(stream, header_size);
    constexpr size_t RECORD_SIZE = VALUE_RECORD_SIZE;
    zassert_equal(records.size(), times.size() * RECORD_SIZE, "every sample is written (got %zu)", records.size());

    for(size_t i = 0; i < times.size(); i++) {
        const auto expected = std::chrono::duration<double>(times[i] - START_TIME).count();
        const auto time = ReadAt<double>(records, i * RECORD_SIZE + 1);
        zassert_within(time, expected, 1e-9, "record %zu at %.9f s, expected %.9f s", i, time, expected);

        if(i > 0) {
            const auto previous = ReadAt<double>(records, (i - 1) * RECORD_SIZE + 1);
            zassert_true(time > previous, "record %zu must be later than the one before", i);
        }
    }
}

ZTEST(mdf4_logger, test_ConversionRule) {
    const auto linear = Mdf4LoggerSensorReading::ToMdfFormula("x * 4 + 1.6");
    zassert_true(linear.has_value() && linear.value() == "X * 4 + 1.6", "x is renamed to X token-wise");

    // asammdf evaluates formulas with numexpr, which has no pow() and rejects "05".
    const auto power = Mdf4LoggerSensorReading::ToMdfFormula("x ^ 2 * 0.5");
    zassert_true(
        power.value_or("") == "(X * X) * 0.5", "x ^ 2 is spelled out as a product (got %s)", power.value_or("").c_str()
    );
    const auto negated_cube = Mdf4LoggerSensorReading::ToMdfFormula("1 - x ^ 3");
    zassert_true(
        negated_cube.value_or("") == "1 - (X * X * X)", "the product binds like ^ (got %s)",
        negated_cube.value_or("").c_str()
    );
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x ^ 5").has_value(), "an exponent above 4 is not spelled out");
    zassert_false(
        Mdf4LoggerSensorReading::ToMdfFormula("x ^ 0.5").has_value(), "a fractional exponent is not converted"
    );
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x ^ 0").has_value(), "a zero exponent is not converted");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x * 05").has_value(), "numexpr rejects a leading zero");
    zassert_true(
        Mdf4LoggerSensorReading::ToMdfFormula("x * 0.5 + 0").value_or("") == "X * 0.5 + 0",
        "a single zero and a zero before the point are plain literals"
    );

    zassert_false(
        Mdf4LoggerSensorReading::ToMdfFormula("x * 2 + sensor_2 + 1").has_value(),
        "an expression reading another sensor cannot be a conversion"
    );
    zassert_false(
        Mdf4LoggerSensorReading::ToMdfFormula("exp(x)").has_value(), "exp is not in the allowed function list"
    );
    zassert_false(
        Mdf4LoggerSensorReading::ToMdfFormula("max(x, 1)").has_value(), "max is not in the allowed function list"
    );

    zassert_true(
        Mdf4LoggerSensorReading::ToMdfFormula("sqrt(abs(x)) / 2").value_or("") == "sqrt(abs(X)) / 2",
        "allowed functions pass through"
    );
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("(x + 1) ^ 2").has_value(), "a grouped base is not converted");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("x ^ 2 ^ 2").has_value(), "a chained power is not converted");
    zassert_false(Mdf4LoggerSensorReading::ToMdfFormula("4.5").has_value(), "an expression without x is no conversion");

    // In the file: the formula on the raw channel's conversion, or the expression in its comment.
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1", "x * 2 + sensor_2 + 1"),
        MakeValueSensor("sensor_2", "x * 4 + 1.6") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );

    const auto file = stream.Str();
    zassert_true(
        ContainsText(CollectBlockTexts(file, "##TX"), "X * 4 + 1.6"), "the convertible expression is a formula"
    );
    zassert_false(
        ContainsText(CollectBlockTexts(file, "##TX"), "x * 2 + sensor_2 + 1"),
        "the other expression must not be written as a formula"
    );
    zassert_true(
        AnyContains(CollectBlockTexts(file, "##MD"), "<TX>expression: x * 2 + sensor_2 + 1</TX>"),
        "the other expression goes into the raw channel comment"
    );
}

ZTEST(mdf4_logger, test_ChannelAndGroupNames) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("oil_pressure", "x * 0.1"),
        MakeCanRawSensor("can_raw", 0) };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );

    const auto file = stream.Str();
    const auto texts = CollectBlockTexts(file, "##TX");
    const auto comments = CollectBlockTexts(file, "##MD");

    zassert_true(ContainsText(texts, "oil_pressure"), "group and value channel are named after the sensor id");
    zassert_true(ContainsText(texts, "oil_pressure.raw"), "the raw channel is the sensor id + .raw");
    zassert_true(ContainsText(texts, "can_raw"), "the CAN group is named after the sensor id");
    zassert_true(ContainsText(texts, "bar"), "the value channel carries the sensor unit");
    zassert_false(
        ContainsText(texts, "value") || ContainsText(texts, "raw_value") || ContainsText(texts, "Raw CAN Frame"),
        "the generic names of the old layout are gone"
    );

    zassert_true(
        AnyContains(comments, "<CNcomment><TX>Name of oil_pressure: Description of oil_pressure</TX>"),
        "the value channel comment is the sensor name and description"
    );
    zassert_true(AnyContains(comments, "<CGcomment><TX>Raw frames</TX>"), "the CAN group comment is the sensor name");
}

ZTEST(mdf4_logger, test_ValueChannelsHaveASourceByKind) {
    const auto adc = MakeTypedSensor("adc_sensor", SensorType::PHYSICAL_ANALOG);
    adc->configuration.expression = "x * 2"; // So that it has a raw channel.
    adc->configuration.channel = 3;

    const auto gpio = MakeTypedSensor("gpio_sensor", SensorType::PHYSICAL_INDICATOR);
    gpio->configuration.channel = 2;

    const auto can = MakeTypedSensor("can_sensor", SensorType::CANBUS_ANALOG);
    can->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), 1, 0x18fef100, "rpm");
    can->configuration.UpdateConnectionString();

    const auto expression = MakeTypedSensor("expression_sensor", SensorType::VIRTUAL_ANALOG);
    expression->configuration.expression = "1";

    const auto script = MakeTypedSensor("script_sensor", SensorType::USER_ANALOG);
    const auto control = MakeTypedSensor("control_sensor", SensorType::CONTROL_INDICATOR);
    const auto raw = MakeCanRawSensor("can_raw", 0);

    const std::vector<std::shared_ptr<Sensor>> sensors{ adc, gpio, can, expression, script, control, raw };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());
    const auto file = StartAndGetFile(logger);

    const auto adc_source = FindChannelSource(file, "adc_sensor");
    zassert_true(adc_source.found, "an ADC value channel has a source");
    zassert_equal(
        adc_source.name, "ADC 3", "the ADC source is named after the channel (got %s)", adc_source.name.c_str()
    );
    zassert_equal(adc_source.source_type, 3, "an ADC is an I/O device");
    zassert_equal(adc_source.bus_type, 0, "an I/O device has no bus type");

    const auto gpio_source = FindChannelSource(file, "gpio_sensor");
    zassert_equal(
        gpio_source.name, "DI 2", "the GPIO source is named after the channel (got %s)", gpio_source.name.c_str()
    );
    zassert_equal(gpio_source.source_type, 3, "a GPIO is an I/O device");

    const auto can_source = FindChannelSource(file, "can_sensor");
    zassert_equal(
        can_source.name, "CAN 2 \xC2\xB7 0x18FEF100 \xC2\xB7 rpm",
        "the CAN source names the bus (interface + 1), the frame in upper-case hex and the signal (got %s)",
        can_source.name.c_str()
    );
    zassert_equal(can_source.source_type, 2, "a CAN signal comes from a bus");
    zassert_equal(can_source.bus_type, 2, "the bus is a CAN bus");

    const auto expression_source = FindChannelSource(file, "expression_sensor");
    zassert_equal(expression_source.name, "virtual", "an expression sensor is virtual");
    zassert_equal(expression_source.source_type, 4, "a virtual sensor comes from a tool");

    const auto script_source = FindChannelSource(file, "script_sensor");
    zassert_equal(script_source.name, "script", "a script sensor is named script");
    zassert_equal(script_source.source_type, 4, "a script comes from a tool");

    const auto control_source = FindChannelSource(file, "control_sensor");
    zassert_equal(control_source.name, "control", "a control sensor is named control");
    zassert_equal(control_source.source_type, 5, "a control is a user");

    zassert_false(FindChannelSource(file, "adc_sensor.raw").found, "a raw channel has no source of its own");
}

ZTEST(mdf4_logger, test_SensorsOfOneSourceShareTheSourceBlock) {
    const auto first = MakeTypedSensor("virtual_1", SensorType::VIRTUAL_ANALOG);
    first->configuration.expression = "1";
    const auto second = MakeTypedSensor("virtual_2", SensorType::VIRTUAL_INDICATOR);
    second->configuration.expression = "2";
    const auto adc = MakeTypedSensor("adc_sensor", SensorType::PHYSICAL_ANALOG);

    const std::vector<std::shared_ptr<Sensor>> sensors{ first, second, adc };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());
    const auto file = StartAndGetFile(logger);

    const auto first_source = FindChannelSource(file, "virtual_1");
    const auto second_source = FindChannelSource(file, "virtual_2");
    zassert_true(first_source.found && second_source.found, "both virtual channels have a source");
    zassert_equal(first_source.address, second_source.address, "two virtual sensors share one SI block");
    zassert_not_equal(
        first_source.address, FindChannelSource(file, "adc_sensor").address, "another source is another block"
    );
    zassert_equal(std::ranges::count(CollectBlockTexts(file, "##TX"), "virtual"), 1, "the name is written once");
}

ZTEST(mdf4_logger, test_LimitsOnlyWhereTheConfigurationBoundsTheValue) {
    const auto indicator = MakeTypedSensor("indicator", SensorType::PHYSICAL_INDICATOR);

    const auto linear = MakeTypedSensor("linear", SensorType::PHYSICAL_ANALOG);
    linear->configuration.interpolation_method = InterpolationMethod::LINEAR;
    linear->configuration.calibration_table.push_back({ 0.5F, 10.0F });
    linear->configuration.calibration_table.push_back({ 1.5F, -2.5F });
    linear->configuration.calibration_table.push_back({ 2.5F, 40.0F });

    const auto with_expression = MakeTypedSensor("with_expression", SensorType::PHYSICAL_ANALOG);
    with_expression->configuration.interpolation_method = InterpolationMethod::LINEAR;
    with_expression->configuration.calibration_table.push_back({ 0.5F, 10.0F });
    with_expression->configuration.calibration_table.push_back({ 2.5F, 40.0F });
    with_expression->configuration.expression = "x * 2";

    const auto with_script = MakeTypedSensor("with_script", SensorType::PHYSICAL_ANALOG);
    with_script->configuration.interpolation_method = InterpolationMethod::LINEAR;
    with_script->configuration.calibration_table.push_back({ 0.5F, 10.0F });
    with_script->configuration.calibration_table.push_back({ 2.5F, 40.0F });
    with_script->configuration.script_path = "script.lua";

    const auto cubic = MakeTypedSensor("cubic", SensorType::PHYSICAL_ANALOG);
    cubic->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    cubic->configuration.calibration_table.push_back({ 0.5F, 10.0F });
    cubic->configuration.calibration_table.push_back({ 2.5F, 40.0F });

    const auto unbounded = MakeTypedSensor("unbounded", SensorType::VIRTUAL_ANALOG);
    unbounded->configuration.expression = "1";

    const auto scripted_indicator = MakeTypedSensor("scripted_indicator", SensorType::PHYSICAL_INDICATOR);
    scripted_indicator->configuration.script_path = "script.lua";

    const auto user_indicator = MakeTypedSensor("user_indicator", SensorType::USER_INDICATOR);

    const std::vector<std::shared_ptr<Sensor>> sensors{ indicator, linear, with_expression, with_script, cubic,
        unbounded, scripted_indicator, user_indicator };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());
    const auto file = StartAndGetFile(logger);

    const auto indicator_limits = FindChannelLimits(file, "indicator");
    zassert_true(indicator_limits.valid, "an indicator value is bounded");
    zassert_equal(indicator_limits.min, 0.0, "an indicator is at least 0");
    zassert_equal(indicator_limits.max, 1.0, "an indicator is at most 1");

    const auto linear_limits = FindChannelLimits(file, "linear");
    zassert_true(linear_limits.valid, "a linear table bounds the value");
    zassert_equal(linear_limits.min, -2.5, "the lowest value of the table");
    zassert_equal(linear_limits.max, 40.0, "the highest value of the table");

    zassert_false(FindChannelLimits(file, "with_expression").valid, "an expression can map anywhere");
    zassert_false(FindChannelLimits(file, "with_script").valid, "a script can map anywhere");
    zassert_false(FindChannelLimits(file, "cubic").valid, "a cubic spline can overshoot");
    zassert_false(FindChannelLimits(file, "unbounded").valid, "a virtual sensor is unbounded");
    zassert_false(FindChannelLimits(file, "scripted_indicator").valid, "a script can write any value to an indicator");
    zassert_false(FindChannelLimits(file, "user_indicator").valid, "a script sensor carries what the script wrote");
}

ZTEST(mdf4_logger, test_RawChannelHasNoLimits) {
    const auto sensor = MakeTypedSensor("indicator", SensorType::PHYSICAL_INDICATOR);
    sensor->configuration.expression = "x";

    const std::vector<std::shared_ptr<Sensor>> sensors{ sensor };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());
    const auto file = StartAndGetFile(logger);

    zassert_true(FindChannelLimits(file, "indicator").valid, "the value channel is bounded");
    zassert_false(FindChannelLimits(file, "indicator.raw").valid, "the raw channel is not");
}

ZTEST(mdf4_logger, test_ChannelCommentCarriesSamplingRateAndLogOnlyNewData) {
    const auto sampled = MakeTypedSensor("sampled", SensorType::PHYSICAL_ANALOG);
    sampled->configuration.sampling_rate_ms = 250;
    const auto event_driven = MakeTypedSensor("event_driven", SensorType::PHYSICAL_INDICATOR);

    const std::vector<std::shared_ptr<Sensor>> sensors{ sampled, event_driven };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, false, true), sensors, MakeMetadata());
    const auto file = StartAndGetFile(logger);

    const auto comments = CollectBlockTexts(file, "##MD");
    zassert_true(
        AnyContains(
            comments,
            "<e name=\"sensor_description\">Description of sampled</e><e name=\"sampling_rate_ms\">250</e>"
            "<e name=\"log_only_new_data\">true</e></common_properties></CNcomment>"
        ),
        "the facts follow the sensor properties"
    );
    zassert_true(
        AnyContains(
            comments,
            "<e name=\"sensor_description\">Description of event_driven</e>"
            "<e name=\"log_only_new_data\">true</e></common_properties></CNcomment>"
        ),
        "a sensor without a sampling rate has no sampling_rate_ms"
    );
}

// Every value channel says what kind of sensor it is, and the name and description apart; a CAN group
// does too. The text stays "name: description".
ZTEST(mdf4_logger, test_ChannelCommentsCarryTheSensorProperties) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("oil_pressure", "x * 0.1"),
        MakeCanRawSensor("can_raw", 0) };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );

    const auto comments = CollectBlockTexts(stream.Str(), "##MD");
    zassert_true(
        AnyContains(
            comments,
            "<CNcomment><TX>Name of oil_pressure: Description of oil_pressure</TX><common_properties>"
            "<e name=\"sensor_type\">PHYSICAL_ANALOG</e><e name=\"sensor_name\">Name of oil_pressure</e>"
            "<e name=\"sensor_description\">Description of oil_pressure</e><e name=\"log_only_new_data\">false</e>"
            "</common_properties></CNcomment>"
        ),
        "the value channel names its type, name and description"
    );
    zassert_true(
        AnyContains(
            comments,
            "<CGcomment><TX>Raw frames</TX><common_properties><e name=\"sensor_type\">CANBUS_RAW</e>"
            "<e name=\"sensor_name\">Raw frames</e></common_properties></CGcomment>"
        ),
        "the CAN group names its type, and leaves out the description it has none of"
    );
}

ZTEST(mdf4_logger, test_AChannelWithoutNameOrDescriptionKeepsOnlyItsType) {
    const auto sensor = MakeValueSensor("bare");
    sensor->metadata.name = "";
    sensor->metadata.description = "";
    sensor->configuration.type = SensorType::VIRTUAL_INDICATOR;
    sensor->configuration.expression = "1";

    const std::vector<std::shared_ptr<Sensor>> sensors{ sensor };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );

    const auto comments = CollectBlockTexts(stream.Str(), "##MD");
    zassert_true(
        AnyContains(
            comments,
            "<CNcomment><TX></TX><common_properties><e name=\"sensor_type\">VIRTUAL_INDICATOR</e>"
            "<e name=\"log_only_new_data\">false</e></common_properties></CNcomment>"
        ),
        "a sensor without a name or description still has its type"
    );
    zassert_false(AnyContains(comments, "sensor_name"), "an empty name is left out");
    zassert_false(AnyContains(comments, "sensor_description"), "an empty description is left out");
}

ZTEST(mdf4_logger, test_EverySensorTypeIsNamedInTheChannelComment) {
    for(uint32_t type = 0; type < static_cast<uint32_t>(SensorType::COUNT); type++) {
        if(static_cast<SensorType>(type) == SensorType::CANBUS_RAW)
            continue;

        const auto sensor = MakeValueSensor("typed");
        sensor->configuration.type = static_cast<SensorType>(type);

        const std::vector<std::shared_ptr<Sensor>> sensors{ sensor };
        Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

        auto stream = MakeStream();
        zassert_true(
            logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
            "StartLogging must succeed on a string buffer"
        );

        const std::string expected = std::string("<e name=\"sensor_type\">")
            + GetSensorTypeName(static_cast<SensorType>(type)) + "</e>";
        zassert_true(
            AnyContains(CollectBlockTexts(stream.Str(), "##MD"), expected), "type %u is named in the comment", type
        );
    }
}

ZTEST(mdf4_logger, test_RecordIdSizeFollowsTheSensorCount) {
    zassert_equal(Mdf4LoggerSensorReading::GetRecordIdSizeBytes(127), 1, "255 record ids still fit into one byte");
    zassert_equal(Mdf4LoggerSensorReading::GetRecordIdSizeBytes(128), 2, "257 record ids need two bytes");

    std::vector<std::shared_ptr<Sensor>> sensors;
    sensors.reserve(130);
    for(int i = 0; i < 130; i++)
        sensors.push_back(MakeValueSensor("sensor_" + std::to_string(i)));

    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    zassert_true(
        logger.LogReading(START_TIME, MakeValueReading(*sensors[129], 5.0F, START_TIME)), "the last sensor is logged"
    );

    const auto records = RecordBytes(stream, header_size);
    zassert_equal(
        records.size(), 2 + TIME_SIZE + VALUE_SIZE + INVALIDATION_SIZE,
        "130 sensors need a two byte record id (got %zu)", records.size()
    );
    zassert_equal(ReadAt<uint16_t>(records, 0), 130, "the last of 130 groups has record id 130");
}

ZTEST(mdf4_logger, test_HeaderPropertiesAreWritten) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );

    const auto comments = CollectBlockTexts(stream.Str(), "##MD");

    zassert_true(
        AnyContains(comments, "<HDcomment><TX>Eerie Leap sensor log</TX><common_properties>"),
        "the header comment opens with its text and the properties"
    );
    zassert_true(AnyContains(comments, "<e name=\"device_id\">7</e>"), "the device id is a header property");
    zassert_true(
        AnyContains(comments, "<e name=\"unit_uid\">" + std::to_string(TEST_UNIT_UID) + "</e>"),
        "the unit UID is a header property"
    );
    zassert_true(
        AnyContains(comments, "<e name=\"boot_id\">" + std::to_string(TEST_BOOT_ID) + "</e>"),
        "the boot ID is a header property"
    );
    zassert_true(AnyContains(comments, "<e name=\"session_id\">0</e>"), "without a session the session ID is 0");
    zassert_true(AnyContains(comments, "<e name=\"file_index\">0</e>"), "without a session the file index is 0");
    zassert_true(
        AnyContains(comments, "<e name=\"sw_version\">" + std::to_string(TEST_SW_VERSION) + "</e>"),
        "the software version is a header property"
    );
    zassert_true(
        AnyContains(comments, "<e name=\"device_name\">unit_board</e>"), "the device name is a header property"
    );
    zassert_true(
        AnyContains(comments, "<e name=\"sensors_configuration_hash\">3735928559</e>"),
        "the sensors configuration hash is a header property"
    );
    zassert_true(
        AnyContains(comments, "<e name=\"logging_interval_ms\">50</e>"), "the logging interval is a header property"
    );
    zassert_true(AnyContains(comments, "<tool_id>rt_core_unit</tool_id>"), "the file history names the tool");
    zassert_true(AnyContains(comments, "<tool_vendor>Eerie Leap</tool_vendor>"), "the file history names the vendor");
    zassert_true(
        AnyContains(comments, "<tool_version>1.2.3</tool_version>"), "the file history carries the tool version"
    );
}

ZTEST(mdf4_logger, test_TheStartCauseIsWrittenFromTheSession) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };

    {
        Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());
        auto stream = MakeStream();
        zassert_true(
            logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }), "StartLogging must succeed"
        );
        zassert_true(
            AnyContains(CollectBlockTexts(stream.Str(), "##MD"), "<e name=\"start_cause\">unknown</e>"),
            "without a session the start cause is unknown"
        );
    }

    auto metadata = MakeMetadata();
    metadata.session = std::make_shared<LogSession>();
    metadata.session->id = 9;
    metadata.session->cause = LoggingStartCause::Trigger;
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, metadata);
    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }), "StartLogging must succeed");
    zassert_true(
        AnyContains(CollectBlockTexts(stream.Str(), "##MD"), "<e name=\"start_cause\">trigger</e>"),
        "the session's cause is written by name"
    );
}

namespace {

// The attachments of a file, walked from the header's hd_at_first link.
struct WrittenAttachment {
    std::string file_name;
    std::string mime_type;
    std::string comment;
    std::string data;
    uint64_t original_size = 0;
    uint64_t embedded_size = 0;
    uint64_t address = 0;
    uint64_t length = 0;
};

std::vector<WrittenAttachment> ReadAttachments(std::string_view file) {
    constexpr size_t HD_LINK_ATTACHMENT_FIRST = 3;
    constexpr size_t AT_FIXED_DATA = 2 + 2 + 4 + 16 + 8 + 8;

    std::vector<WrittenAttachment> attachments;
    auto address = ReadAt<uint64_t>(file, ID_BLOCK_SIZE + BLOCK_LINKS_OFFSET + HD_LINK_ATTACHMENT_FIRST * LINK_SIZE);

    while(address != 0) {
        zassert_equal(file.substr(address, 4), "##AT");
        WrittenAttachment attachment;
        attachment.address = address;
        attachment.length = ReadAt<uint64_t>(file, address + BLOCK_LENGTH_OFFSET);
        zassert_equal(attachment.length % 8, 0, "the block length is a multiple of 8");
        zassert_equal(ReadAt<uint64_t>(file, address + 16), 4, "four links");

        const auto data_offset = address + BLOCK_LINKS_OFFSET + uint64_t{ 4 } * LINK_SIZE;
        attachment.file_name = TextAt(file, ReadAt<uint64_t>(file, address + BLOCK_LINKS_OFFSET + LINK_SIZE));
        attachment.mime_type = TextAt(
            file, ReadAt<uint64_t>(file, address + BLOCK_LINKS_OFFSET + uint64_t{ 2 } * LINK_SIZE)
        );
        attachment.comment = TextAt(
            file, ReadAt<uint64_t>(file, address + BLOCK_LINKS_OFFSET + uint64_t{ 3 } * LINK_SIZE)
        );
        zassert_equal(ReadAt<uint16_t>(file, data_offset), 1, "embedded, no MD5");
        zassert_equal(ReadAt<uint16_t>(file, data_offset + 2), 0, "creator index");
        attachment.original_size = ReadAt<uint64_t>(file, data_offset + 24);
        attachment.embedded_size = ReadAt<uint64_t>(file, data_offset + 32);
        attachment.data = std::string(file.substr(data_offset + AT_FIXED_DATA, attachment.embedded_size));
        zassert_equal(
            attachment.length,
            BLOCK_LINKS_OFFSET + 4 * LINK_SIZE + AT_FIXED_DATA + ((attachment.embedded_size + 7) & ~7ULL),
            "the data are padded to 8"
        );

        attachments.push_back(std::move(attachment));
        address = ReadAt<uint64_t>(file, address + BLOCK_LINKS_OFFSET);
    }

    return attachments;
}

LoggerAttachment MakeAttachment(std::string name, std::vector<uint8_t> bytes, std::string type, std::string crc) {
    LoggerAttachment attachment;
    attachment.file_name = std::move(name);
    attachment.mime_type = "application/cbor";
    attachment.comment_text = "Eerie Leap test configuration";
    attachment.properties = { { "configuration_type", std::move(type) }, { "crc", std::move(crc) } };
    attachment.bytes = std::move(bytes);

    return attachment;
}

} // namespace

ZTEST(mdf4_logger, test_TheMetadataAttachmentsAreEmbeddedInEveryFile) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };

    {
        Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());
        auto stream = MakeStream();
        zassert_true(logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }));
        zassert_true(ReadAttachments(stream.Str()).empty(), "without attachments the header links none");
    }

    auto metadata = MakeMetadata();
    metadata.attachments.push_back(MakeAttachment("sensors_config.cbor", { 0xA1, 0x01, 0x02, 0x03, 0x04 }, "4", "99"));
    metadata.attachments.push_back(MakeAttachment("logging_config.cbor", std::vector<uint8_t>(16, 0x5A), "5", "7"));
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, metadata);

    // Every file of the logger carries them, as the header is written again for each.
    for(int file = 0; file < 2; file++) {
        auto stream = MakeStream();
        zassert_true(logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }), "file %d", file);
        const std::string content = stream.Str();

        const auto attachments = ReadAttachments(content);
        zassert_equal(attachments.size(), 2, "file %d", file);
        zassert_equal(attachments[0].file_name, "sensors_config.cbor");
        zassert_equal(attachments[0].mime_type, "application/cbor");
        zassert_true(attachments[0].comment.contains("<TX>Eerie Leap test configuration</TX>"));
        zassert_true(attachments[0].comment.contains("<e name=\"configuration_type\">4</e>"));
        zassert_true(attachments[0].comment.contains("<e name=\"crc\">99</e>"));
        zassert_equal(attachments[0].original_size, 5);
        zassert_equal(attachments[0].embedded_size, 5);
        zassert_equal(attachments[0].data, std::string("\xA1\x01\x02\x03\x04", 5));
        zassert_equal(attachments[1].file_name, "logging_config.cbor");
        zassert_equal(attachments[1].data, std::string(16, '\x5A'));
        zassert_true(attachments[1].comment.contains("<e name=\"configuration_type\">5</e>"));

        // The attachments come before the data group, so the DT block is still the header's last block.
        const auto dt = content.find("##DT");
        zassert_true(dt != std::string::npos && dt > attachments[1].address + attachments[1].length);
        zassert_equal(logger.GetBytesWritten(), dt + 24, "the header ends with the DT block");

        zassert_true(logger.StopLogging(LogFileEnd{ .time = START_TIME }), "file %d is finalized", file);
    }
}

ZTEST(mdf4_logger, test_TheSessionIsWrittenAndNumbersTheFiles) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    auto metadata = MakeMetadata();
    metadata.session = std::make_shared<LogSession>();
    metadata.session->id = 40211837;

    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, metadata);

    // A rotation: the same logger starts the next file.
    auto first = MakeStream();
    zassert_true(
        logger.StartLogging(first, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    zassert_true(logger.StopLogging(LogFileEnd{ .time = START_TIME }), "the first file is finalized");

    auto second = MakeStream();
    zassert_true(
        logger.StartLogging(second, LogFileStart{ START_TIME, std::nullopt }), "the second StartLogging must succeed"
    );

    const auto first_comments = CollectBlockTexts(first.Str(), "##MD");
    const auto second_comments = CollectBlockTexts(second.Str(), "##MD");
    zassert_true(AnyContains(first_comments, "<e name=\"session_id\">40211837</e>"), "the session ID");
    zassert_true(AnyContains(first_comments, "<e name=\"file_index\">0</e>"), "the first file is 0");
    zassert_true(AnyContains(second_comments, "<e name=\"session_id\">40211837</e>"), "the same session");
    zassert_true(AnyContains(second_comments, "<e name=\"file_index\">1</e>"), "the rotation's file is 1");

    // A restart: another logger of the same session goes on counting.
    Mdf4LoggerSensorReading restarted(MakeConfiguration(sensors), sensors, metadata);
    auto third = MakeStream();
    zassert_true(
        restarted.StartLogging(third, LogFileStart{ START_TIME, std::nullopt }), "the restarted logger must start"
    );
    zassert_true(
        AnyContains(CollectBlockTexts(third.Str(), "##MD"), "<e name=\"file_index\">2</e>"),
        "a logger of the same session continues the numbering"
    );
    zassert_equal(metadata.session->next_file_index.load(), 3, "the session counts three files");
}

namespace {

// A stream whose sync fails: the header reached the buffer but not the medium.
class UnsyncableStreamBuf : public MemoryStreamBuf {
protected:
    int sync() override {
        return -1;
    }
};

} // namespace

ZTEST(mdf4_logger, test_AFailedStartTakesNoFileIndex) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    auto metadata = MakeMetadata();
    metadata.session = std::make_shared<LogSession>();
    metadata.session->id = 7;

    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, metadata);

    UnsyncableStreamBuf failing;
    zassert_false(
        logger.StartLogging(failing, LogFileStart{ START_TIME, std::nullopt }),
        "a header that does not sync fails the start"
    );
    zassert_equal(metadata.session->next_file_index.load(), 0, "a failed header takes no index");

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }), "the next start succeeds");
    zassert_true(
        AnyContains(CollectBlockTexts(stream.Str(), "##MD"), "<e name=\"file_index\">0</e>"),
        "the file after a failed start is still the first"
    );
}

ZTEST(mdf4_logger, test_LogFileNaming) {
    using namespace std::chrono;

    const system_clock::time_point time = sys_days{ year{ 2026 } / October / 11 } + hours{ 9 } + minutes{ 30 };

    const std::string directory = LogFileNaming::LOGS_DIR;

    const auto path = LogFileNaming::GetFilePath(directory, "log_data", time, 4242, "mf4");
    zassert_true(
        path.ToString() == directory + "/log_data_20261011T093000Z_4242.mf4", "unexpected file name %s", path.CStr()
    );

    const auto without_prefix = LogFileNaming::GetFilePath(directory, "", time + seconds{ 5 }, 123456, "mf4");
    zassert_true(
        without_prefix.ToString() == directory + "/20261011T093005Z_3456.mf4",
        "an empty prefix drops its underscore and the suffix keeps four digits (got %s)", without_prefix.CStr()
    );
}

ZTEST(mdf4_logger, test_AllocationCounterCounts) {
    const size_t before = GetAllocationCount();
    const auto block = std::make_unique<uint8_t[]>(32);

    zassert_equal(
        GetAllocationCount() - before, 1,
        "the replaced operator new must count, or test_ATickAllocatesNothing proves nothing"
    );
}

// The writers, their buffers and the targets are built by StartLogging(); a tick only finds the
// target and hands the record to its writer.
ZTEST(mdf4_logger, test_ATickAllocatesNothing) {
    constexpr size_t VALUE_SENSOR_COUNT = 64;
    constexpr size_t CAN_SENSOR_COUNT = 8;
    constexpr size_t FRAME_COUNT = 32;

    std::vector<std::shared_ptr<Sensor>> sensors;
    sensors.reserve(VALUE_SENSOR_COUNT);
    for(size_t i = 0; i < VALUE_SENSOR_COUNT; i++) {
        // Every other sensor gets a raw value channel, so both value record layouts are written.
        sensors.push_back(MakeValueSensor("sensor_" + std::to_string(i), i % 2 == 0 ? "x * 2" : ""));
    }
    for(size_t i = 0; i < CAN_SENSOR_COUNT; i++)
        sensors.push_back(MakeCanRawSensor("can_" + std::to_string(i), static_cast<uint8_t>(i % 2)));

    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors, true), sensors, MakeMetadata());

    const auto reading_time = START_TIME + std::chrono::milliseconds(90);
    const auto tick_time = START_TIME + std::chrono::milliseconds(100);

    std::vector<SensorReading> value_readings;
    for(size_t i = 0; i < VALUE_SENSOR_COUNT; i++) {
        auto reading = MakeValueReading(*sensors[i], static_cast<float>(i), reading_time);
        reading.raw_value = static_cast<float>(i) / 2.0F;

        // Invalid readings take the same path and must not allocate either.
        if(i % 3 == 1)
            reading.SetError(ReadingError::EXPRESSION_FAILED);
        if(i % 4 == 2)
            reading.raw_value.reset();

        value_readings.push_back(reading);
    }

    std::array<CanFrame, FRAME_COUNT> frames;
    std::vector<SensorReading> frame_readings;
    for(size_t i = 0; i < FRAME_COUNT; i++) {
        frames[i].id = static_cast<uint32_t>(0x100 + i);
        frames[i].is_extended = i % 2 == 1;
        frames[i].is_can_fd = i % 4 == 0;
        frames[i].data = std::vector<uint8_t>(i % 4 == 0 ? 64 : 8, static_cast<uint8_t>(i));

        SensorReading reading(sensors[VALUE_SENSOR_COUNT + i % CAN_SENSOR_COUNT].get());
        reading.source = ReadingSource::ISR;
        reading.status = ReadingStatus::PROCESSED;
        reading.timestamp = reading_time + std::chrono::microseconds(i);
        reading.can_frame = &frames[i];
        frame_readings.push_back(reading);
    }

    auto stream = MakeStream();
    stream.Reserve(192 * 1024);
    const size_t capacity = stream.Capacity();

    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a memory buffer"
    );

    const size_t allocations_before = GetAllocationCount();
    size_t logged = 0;

    for(const auto& reading : value_readings)
        logged += logger.LogReading(tick_time, reading) ? 1 : 0;
    for(const auto& reading : frame_readings)
        logged += logger.LogReading(tick_time, reading) ? 1 : 0;

    const size_t allocations = GetAllocationCount() - allocations_before;

    zassert_equal(
        logged, VALUE_SENSOR_COUNT + FRAME_COUNT, "every reading of the tick must be logged (got %zu)", logged
    );
    zassert_true(
        logger.GetBytesWritten() <= capacity,
        "the %llu bytes written outgrew the %zu reserved, so the stream itself allocated",
        static_cast<unsigned long long>(logger.GetBytesWritten()), capacity
    );
    zassert_equal(allocations, 0, "%zu allocations while logging 64 value readings and 32 frames", allocations);
}

namespace {

// The HD block's start time (ns) and time class, read from the first block after the ID block.
struct HeaderStart {
    uint64_t start_time_ns;
    uint8_t time_class;
};

HeaderStart ReadHeaderStart(std::string_view file) {
    zassert_equal(file.substr(ID_BLOCK_SIZE, 4), "##HD", "the header block follows the ID block");

    const auto link_count = ReadAt<uint64_t>(file, ID_BLOCK_SIZE + 16);
    const size_t fields = ID_BLOCK_SIZE + BLOCK_HEADER_SIZE + static_cast<size_t>(link_count) * 8;

    return HeaderStart{ .start_time_ns = ReadAt<uint64_t>(file, fields),
        .time_class = ReadAt<uint8_t>(file, fields + 8 + 2 + 2 + 1) };
}

constexpr uint64_t START_TIME_NS = 1'760'000'000ULL * 1'000'000'000ULL;

} // namespace

ZTEST(mdf4_logger, test_FileOnRealTimeStampsStartClassAndClock) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());
    const auto offset = std::chrono::seconds(1000);

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, offset }), "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto header = ReadHeaderStart(stream.Str());
    zassert_equal(header.start_time_ns, START_TIME_NS + 1000ULL * 1'000'000'000ULL, "the start is time + offset");
    zassert_equal(header.time_class, 10, "a file on real time has the external time source class");
    zassert_true(AnyContains(CollectBlockTexts(stream.Str(), "##MD"), "<e name=\"clock\">set</e>"), "the clock is set");

    // Records stay relative to the unit's clock at the start.
    const auto time = START_TIME + std::chrono::milliseconds(1500);
    zassert_true(logger.LogReading(time, MakeValueReading(*sensors[0], 1.0F, time)), "the reading must be logged");
    zassert_equal(ReadAt<double>(RecordBytes(stream, header_size), 1), 1.5, "the record time ignores the offset");
}

ZTEST(mdf4_logger, test_FileOnUptimeClockKeepsClassZero) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(
        logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }),
        "StartLogging must succeed on a string buffer"
    );
    const uint64_t header_size = logger.GetBytesWritten();

    const auto header = ReadHeaderStart(stream.Str());
    zassert_equal(header.start_time_ns, START_TIME_NS, "the start is the unit's clock");
    zassert_equal(header.time_class, 0, "a file on the unit's clock has class 0");
    zassert_true(
        AnyContains(CollectBlockTexts(stream.Str(), "##MD"), "<e name=\"clock\">uptime</e>"), "the clock is uptime"
    );

    const auto time = START_TIME + std::chrono::milliseconds(1500);
    zassert_true(logger.LogReading(time, MakeValueReading(*sensors[0], 1.0F, time)), "the reading must be logged");
    zassert_equal(ReadAt<double>(RecordBytes(stream, header_size), 1), 1.5, "the record time is the same");
}

ZTEST(mdf4_logger, test_NextFileTakesItsOwnClock) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto first = MakeStream();
    zassert_true(logger.StartLogging(first, LogFileStart{ START_TIME, std::chrono::seconds(5) }), "first start");
    zassert_equal(ReadHeaderStart(first.Str()).time_class, 10, "the first file is on real time");

    auto second = MakeStream();
    zassert_true(logger.StartLogging(second, LogFileStart{ START_TIME, std::nullopt }), "second start");
    zassert_equal(ReadHeaderStart(second.Str()).time_class, 0, "the second file is on the unit's clock again");
    zassert_true(AnyContains(CollectBlockTexts(second.Str(), "##MD"), "<e name=\"clock\">uptime</e>"), "and says so");
}

namespace {

using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;

constexpr size_t HD_LINK_EVENT_FIRST = 4;
constexpr size_t EV_LINK_NAME = 3;
constexpr size_t EV_LINK_COMMENT = 4;
constexpr size_t EV_DATA_OFFSET = BLOCK_LINKS_OFFSET + 5 * LINK_SIZE;

// A buffer whose syncs can be made to fail, which fails the finalize.
class SyncFailingStreamBuf : public MemoryStreamBuf {
protected:
    int sync() override {
        return fail_sync ? -1 : 0;
    }

public:
    bool fail_sync = false;
};

struct EndEvent {
    size_t address = 0;
    uint8_t type = 0;
    uint8_t sync_type = 0;
    uint8_t range_type = 0;
    uint8_t cause = 0;
    int64_t base_value = 0;
    double factor = 0.0;
    std::string name;
    std::string comment;
};

// The event the header links, or an address of 0.
EndEvent ReadEndEvent(std::string_view file) {
    EndEvent event;
    event.address = static_cast<size_t>(
        ReadAt<uint64_t>(file, ID_BLOCK_SIZE + BLOCK_LINKS_OFFSET + HD_LINK_EVENT_FIRST * LINK_SIZE)
    );
    if(event.address == 0)
        return event;

    zassert_equal(event.address % 8, 0U, "the event sits on an 8 byte boundary");
    zassert_equal(file.substr(event.address, 4), "##EV", "the header links an event block");

    const size_t fields = event.address + EV_DATA_OFFSET;
    event.type = ReadAt<uint8_t>(file, fields);
    event.sync_type = ReadAt<uint8_t>(file, fields + 1);
    event.range_type = ReadAt<uint8_t>(file, fields + 2);
    event.cause = ReadAt<uint8_t>(file, fields + 3);
    event.base_value = ReadAt<int64_t>(file, fields + 16);
    event.factor = ReadAt<double>(file, fields + 24);
    event.name = TextAt(file, ReadAt<uint64_t>(file, event.address + BLOCK_LINKS_OFFSET + EV_LINK_NAME * LINK_SIZE));
    event.comment = TextAt(
        file, ReadAt<uint64_t>(file, event.address + BLOCK_LINKS_OFFSET + EV_LINK_COMMENT * LINK_SIZE)
    );

    return event;
}

EndEvent StopAndReadEvent(const LogFileEnd& end) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    auto stream = MakeStream();
    zassert_true(logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }), "start");
    const auto time = START_TIME + std::chrono::milliseconds(100);
    zassert_true(logger.LogReading(time, MakeValueReading(*sensors[0], 1.0F, time)), "the reading is logged");

    zassert_true(logger.StopLogging(end), "the file finalizes");

    return ReadEndEvent(stream.Str());
}

} // namespace

ZTEST(mdf4_logger, test_AStopEndsTheFileWithAnEventForTheUser) {
    const auto event = StopAndReadEvent(
        LogFileEnd{ .time = START_TIME + std::chrono::milliseconds(2500),
            .stop_reason = LoggingStatusReason::Stopped,
            .overruns = 3,
            .frames_dropped = 7 }
    );

    zassert_not_equal(event.address, 0U, "the file has an end event");
    zassert_equal(event.type, 4, "a stop recording trigger");
    zassert_equal(event.sync_type, 1, "synced on time");
    zassert_equal(event.range_type, 0, "a point");
    zassert_equal(event.cause, 4, "the user stopped it");
    zassert_equal(event.base_value, 2'500'000, "microseconds from the file's start");
    zassert_equal(event.factor, 1e-6, "the factor makes them seconds");
    zassert_equal(event.name, "Logging stopped", "the name");
    zassert_true(event.comment.contains("<e name=\"stop_reason\">0</e>"), "the reason is the status value");
    zassert_true(event.comment.contains("<e name=\"overruns\">3</e>"), "the overruns");
    zassert_true(event.comment.contains("<e name=\"frames_dropped\">7</e>"), "the dropped frames");
    zassert_false(event.comment.contains("rotated"), "a stop is not a rotation");
}

ZTEST(mdf4_logger, test_ARotationEndsTheFileWithoutAReason) {
    const auto event = StopAndReadEvent(LogFileEnd{ .time = START_TIME, .stop_reason = std::nullopt });

    zassert_not_equal(event.address, 0U, "the file has an end event");
    zassert_equal(event.type, 4, "a stop recording trigger");
    zassert_equal(event.cause, 2, "the tool closed it");
    zassert_equal(event.base_value, 0, "a file without a tick ends where it began");
    zassert_equal(event.name, "Next file", "the name");
    zassert_true(event.comment.contains("<e name=\"rotated\">true</e>"), "it says it rotated");
    zassert_false(event.comment.contains("stop_reason"), "a rotation has no reason");
    zassert_true(event.comment.contains("<e name=\"overruns\">0</e>"), "the overruns");
}

ZTEST(mdf4_logger, test_AFailureEndsTheFileWithAnErrorEvent) {
    const auto event = StopAndReadEvent(LogFileEnd{ .time = START_TIME, .stop_reason = LoggingStatusReason::NoSpace });

    zassert_equal(event.cause, 1, "an error");
    zassert_equal(event.name, "Logging stopped: no space", "the name says why");
    zassert_true(event.comment.contains("<e name=\"stop_reason\">2</e>"), "the reason is NoSpace's status value, 2");

    const auto update = StopAndReadEvent(
        LogFileEnd{ .time = START_TIME, .stop_reason = LoggingStatusReason::FirmwareUpdate }
    );
    zassert_equal(update.cause, 2, "a firmware update is the tool's doing");
}

ZTEST(mdf4_logger, test_AFileThatCouldNotBeFinalizedGetsNoEvent) {
    const std::vector<std::shared_ptr<Sensor>> sensors{ MakeValueSensor("sensor_1") };
    Mdf4LoggerSensorReading logger(MakeConfiguration(sensors), sensors, MakeMetadata());

    SyncFailingStreamBuf stream;
    zassert_true(logger.StartLogging(stream, LogFileStart{ START_TIME, std::nullopt }), "start");

    stream.fail_sync = true;
    const auto size_before = stream.Str().size();
    zassert_false(
        logger.StopLogging(LogFileEnd{ .time = START_TIME, .stop_reason = LoggingStatusReason::Stopped }),
        "the finalize fails"
    );

    zassert_equal(ReadEndEvent(stream.Str()).address, 0U, "no event is linked");
    zassert_equal(stream.Str().size(), size_before, "nothing is appended to an unfinalized file");
}
