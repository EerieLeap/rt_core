#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <chrono>

#include "subsys/mdf/mdf4_file.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/record_writer.h"
#include "subsys/mdf/mdf4/can_record_writer.h"
#include "subsys/mdf/mdf4/source_information_block.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/logging_domain/models/logging_configuration.h"
#include "i_logger.h"
#include "logger_metadata.h"

namespace eerie_leap::domain::logging_domain::loggers {

namespace mdf4 = eerie_leap::subsys::mdf::mdf4;

using eerie_leap::subsys::mdf::Mdf4File;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::logging_domain::models::LoggingConfiguration;
using eerie_leap::domain::logging_domain::models::SensorLoggingConfiguration;

/**
 * MDF 4.10 logger for sensor readings: one channel group per enabled sensor, named after the
 * sensor ID, with a Float64 master time in seconds since StartLogging().
 *
 * Every value and raw channel has an invalidation bit. A reading with an error or without a value
 * is written with 0 and the value channel's bit set, a missing raw value with 0 and the raw
 * channel's bit set, so readers leave the sample out rather than show a stale or made-up value.
 *
 * Everything it needs from the configurations is copied at construction, so a reading is matched
 * by its sensor ID hash alone and its sensor pointer is never followed.
 */
class Mdf4LoggerSensorReading : public ILogger<SensorReading> {
private:
    static constexpr int64_t NO_TIMESTAMP = std::numeric_limits<int64_t>::min();
    // The master channel resolves microseconds; a record moved to keep it increasing moves by one.
    static constexpr auto MIN_RECORD_TIME_STEP = std::chrono::microseconds(1);

    struct LogTarget {
        enum class Kind : uint8_t {
            Value,
            ValueWithRaw,
            CanFrame
        };

        Kind kind = Kind::Value;
        bool log_only_new_data = false;
        int64_t last_timestamp = NO_TIMESTAMP;   // system_clock ticks of the last reading's own timestamp
        int64_t last_record_time = NO_TIMESTAMP; // system_clock ticks of the last record's master time
        uint8_t bus_channel = 0;                 // CanFrame: CAN interface index + 1, 0 when unknown
        // Value kinds: the invalidation bit of the value channel and of the raw channel, as masks.
        uint32_t value_invalid_mask = 0;
        uint32_t raw_value_invalid_mask = 0;
        // Owned by the file, valid from StartLogging() until the next one.
        mdf4::RecordWriter* writer = nullptr;
        mdf4::CanRecordWriter* can_writer = nullptr;
    };

    struct TargetChannelGroup {
        uint32_t sensor_id_hash;
        std::shared_ptr<mdf4::ChannelGroupBlock> channel_group;
    };

    std::unique_ptr<Mdf4File> mdf4_file_;
    std::unordered_map<uint32_t, LogTarget> targets_;
    std::vector<TargetChannelGroup> target_channel_groups_;
    std::streambuf* stream_;
    std::chrono::system_clock::time_point start_time_;

    uint64_t next_record_id_ = 1;
    uint64_t current_file_size_bytes_ = 0;

    void AddValueTarget(
        const Sensor& sensor,
        const SensorLoggingConfiguration& sensor_configuration,
        const std::shared_ptr<mdf4::SourceInformationBlock>& source_information);
    void AddCanFrameTarget(const Sensor& sensor);
    void SetFileMetadata(const LoggingConfiguration& configuration, const LoggerMetadata& metadata);

    double ToSeconds(const std::chrono::system_clock::time_point& time) const;
    void LogValueReading(const LogTarget& target, double time_delta_s, const SensorReading& reading);
    bool LogCanFrameReading(const LogTarget& target, double time_delta_s, const SensorReading& reading);

public:
    Mdf4LoggerSensorReading(
        const LoggingConfiguration& configuration,
        std::span<const std::shared_ptr<Sensor>> sensors,
        const LoggerMetadata& metadata);
    virtual ~Mdf4LoggerSensorReading() = default;

    const char* GetFileExtension() const override;
    bool StartLogging(std::streambuf& stream, const std::chrono::system_clock::time_point& start_time) override;
    bool StopLogging() override;
    bool LogReading(const std::chrono::system_clock::time_point& time, const SensorReading& reading) override;
    [[nodiscard]] uint64_t GetBytesWritten() const override { return current_file_size_bytes_; }

    /** 1 while every group ID (two per CAN sensor) fits into a byte, else 2. */
    static uint8_t GetRecordIdSizeBytes(size_t enabled_sensor_count);

    /**
     * The MDF algebraic formula of a sensor expression, when a reader can evaluate it on the raw
     * value alone: the only identifier is x, and the expression uses nothing but + - * / ( ),
     * decimal numbers and sin cos tan sqrt abs log. x becomes X, and a ^ n becomes the product
     * (a * ... * a) when a is a single token and n a whole number from 1 to 4. Nothing otherwise.
     */
    static std::optional<std::string> ToMdfFormula(std::string_view expression);
};

} // namespace eerie_leap::domain::logging_domain::loggers
