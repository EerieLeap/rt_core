#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <chrono>

#include "subsys/mdf/mdf4_file.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/logging_domain/configuration/logging_configuration_manager.h"
#include "i_logger.h"

namespace eerie_leap::domain::logging_domain::loggers {

namespace mdf4 = eerie_leap::subsys::mdf::mdf4;

using eerie_leap::subsys::mdf::Mdf4File;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::logging_domain::configuration::LoggingConfigurationManager;

class Mdf4LoggerSensorReading : public ILogger<SensorReading> {
private:
    static constexpr uint8_t RECORD_ID_SIZE = 4;

    struct SensorChannelGroup {
        std::shared_ptr<mdf4::ChannelGroupBlock> channel_group;
        bool has_raw_value_channel;
    };

    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager_;

    std::unique_ptr<Mdf4File> mdf4_file_;
    std::unordered_map<size_t, SensorChannelGroup> value_channel_groups_;
    std::unordered_map<size_t, std::shared_ptr<mdf4::ChannelGroupBlock>> can_raw_channel_groups_;
    std::streambuf* stream_;
    std::chrono::system_clock::time_point start_time_;

    uint64_t next_record_id_ = 1;
    uint64_t current_file_size_bytes_ = 0;
    std::unordered_map<size_t, std::chrono::system_clock::time_point> last_reading_time_;

    bool LogValueReading(float time_delta_s, const SensorReading& reading);
    bool LogCanbusRawReading(float time_delta_s, const SensorReading& reading);

public:
    explicit Mdf4LoggerSensorReading(std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager, const std::vector<std::shared_ptr<Sensor>>& sensors);
    virtual ~Mdf4LoggerSensorReading() = default;

    const char* GetFileExtension() const override;
    bool StartLogging(std::streambuf& stream, const std::chrono::system_clock::time_point& start_time) override;
    bool StopLogging() override;
    bool LogReading(const std::chrono::system_clock::time_point& time, const SensorReading& reading) override;
};

} // namespace eerie_leap::domain::logging_domain::loggers
