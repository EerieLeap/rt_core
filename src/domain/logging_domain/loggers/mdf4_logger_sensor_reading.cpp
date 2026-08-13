#include <array>
#include <cstdint>
#include <span>

#include "subsys/mdf/mdf_value.h"
#include "subsys/mdf/mdf4/source_information_block.h"

#include "mdf4_logger_sensor_reading.h"

namespace eerie_leap::domain::logging_domain::loggers {

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::subsys::mdf;
using namespace eerie_leap::subsys::mdf::mdf4;

Mdf4LoggerSensorReading::Mdf4LoggerSensorReading(
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
    const std::vector<std::shared_ptr<Sensor>>& sensors)
        : logging_configuration_manager_(logging_configuration_manager),
        stream_(nullptr) {

    auto logging_configuration = logging_configuration_manager_->Get();

    mdf4_file_ = std::make_unique<Mdf4File>(RECORD_ID_SIZE);

    auto source_information = std::make_shared<mdf4::SourceInformationBlock>(
        mdf4::SourceInformationBlock::SourceType::IoDevice,
        mdf4::SourceInformationBlock::BusType::None);
    source_information->SetName(mdf4_file_->GetOrCreateTextBlock("Eerie Leap Sensor"));

    for(auto& sensor : sensors) {
        if(!logging_configuration->sensor_configurations.contains(sensor->id_hash)
            || !logging_configuration->sensor_configurations.at(sensor->id_hash).is_enabled) {

            continue;
        }

        if(sensor->configuration.type == SensorType::CANBUS_RAW) {
            auto vlsd_channel_group = mdf4_file_->CreateVLSDChannelGroup(next_record_id_++);
            auto channel_group = mdf4_file_->CreateCanDataFrameChannelGroup(
                vlsd_channel_group, next_record_id_++, "Raw CAN Frame");

            can_raw_channel_groups_.emplace(sensor->id_hash, channel_group);
        } else {
            auto channel_group = mdf4_file_->CreateChannelGroup(next_record_id_++, std::string(sensor->id));
            channel_group->AddSourceInformation(source_information);

            mdf4_file_->CreateDataChannel(channel_group, MdfDataType::Float32, "value", std::string(sensor->metadata.unit));

            bool has_raw_value_channel = sensor->configuration.expression_evaluator != nullptr
                && logging_configuration->sensor_configurations.at(sensor->id_hash).log_raw_value
                && (sensor->configuration.type == SensorType::PHYSICAL_ANALOG
                    || sensor->configuration.type == SensorType::PHYSICAL_INDICATOR);

            if(has_raw_value_channel) {
                auto channel_raw = mdf4_file_->CreateDataChannel(channel_group, MdfDataType::Float32, "raw_value", "");
                channel_raw->SetConversion(
                    mdf4_file_->CreateAlgebraicConversion(sensor->configuration.expression_evaluator->GetExpression()));
            }

            value_channel_groups_.emplace(sensor->id_hash, SensorChannelGroup{channel_group, has_raw_value_channel});
        }
    }
}

const char* Mdf4LoggerSensorReading::GetFileExtension() const {
    return Mdf4File::LOG_DATA_FILE_EXTENSION;
}

bool Mdf4LoggerSensorReading::StartLogging(std::streambuf& stream, const std::chrono::system_clock::time_point& start_time) {
    stream_ = &stream;
    start_time_ = start_time;

    mdf4_file_->UpdateCurrentTime(start_time);
    current_file_size_bytes_ = mdf4_file_->WriteHeaderToStream(*stream_);

    return true;
}

bool Mdf4LoggerSensorReading::StopLogging() {
    if(stream_ == nullptr)
        return false;

    bool result = true;

    // Patches the DT length, cycle counts and file identifier; without it the file
    // stays readable but flagged as unfinalized.
    try {
        current_file_size_bytes_ += mdf4_file_->FinalizeToStream(*stream_);
    } catch(const std::exception&) {
        result = false;
    }

    stream_ = nullptr;

    return result;
}

bool Mdf4LoggerSensorReading::LogValueReading(float time_delta_s, const SensorReading& reading) {
    auto channel_group = value_channel_groups_.find(reading.sensor->id_hash);
    if(channel_group == value_channel_groups_.end() || !reading.value.has_value())
        return false;

    float value = reading.value.value();

    if(channel_group->second.has_raw_value_channel) {
        float raw_value = 0;

        if(reading.sensor->configuration.type == SensorType::PHYSICAL_ANALOG) {
            auto raw_value_data = reading.metadata.GetTag<float>(ReadingMetadataTag::RAW_VALUE);
            if(raw_value_data.has_value())
                raw_value = raw_value_data.value();
        } else if(reading.sensor->configuration.type == SensorType::PHYSICAL_INDICATOR) {
            auto raw_value_data = reading.metadata.GetTag<bool>(ReadingMetadataTag::RAW_VALUE);
            if(raw_value_data.has_value())
                raw_value = raw_value_data.value() ? 1.0F : 0.0F;
        }

        std::array<MdfValue, 3> values{time_delta_s, value, raw_value};
        current_file_size_bytes_ += mdf4_file_->WriteDataRecordToStream(channel_group->second.channel_group, *stream_, values);
    } else {
        std::array<MdfValue, 2> values{time_delta_s, value};
        current_file_size_bytes_ += mdf4_file_->WriteDataRecordToStream(channel_group->second.channel_group, *stream_, values);
    }

    return true;
}

bool Mdf4LoggerSensorReading::LogCanbusRawReading(float time_delta_s, const SensorReading& reading) {
    auto channel_group = can_raw_channel_groups_.find(reading.sensor->id_hash);
    if(channel_group == can_raw_channel_groups_.end())
        return false;

    auto can_frame = reading.metadata.GetTag<CanFrame>(ReadingMetadataTag::CANBUS_DATA);
    if(!can_frame.has_value())
        return false;

    current_file_size_bytes_ += mdf4_file_->WriteCanbusDataRecordToStream(
        channel_group->second, *stream_, can_frame.value(), time_delta_s);

    return true;
}

bool Mdf4LoggerSensorReading::LogReading(const std::chrono::system_clock::time_point& time, const SensorReading& reading) {
    if(stream_ == nullptr)
        return false;

    if(!logging_configuration_manager_->Get()->sensor_configurations.contains(reading.sensor->id_hash)
        || !logging_configuration_manager_->Get()->sensor_configurations.at(reading.sensor->id_hash).is_enabled) {

        return false;
    }

    if(logging_configuration_manager_->Get()->sensor_configurations.at(reading.sensor->id_hash).log_only_new_data
        && last_reading_time_.contains(reading.sensor->id_hash)
        && last_reading_time_[reading.sensor->id_hash] == reading.timestamp.value()) {

        return false;
    }

    // Readings that predate the file start would wrap an unsigned delta.
    auto elapsed = time > start_time_ ? time - start_time_ : std::chrono::system_clock::duration::zero();
    float time_delta_s = std::chrono::duration<float>(elapsed).count();

    bool result = false;
    if(reading.sensor->configuration.type == SensorType::CANBUS_RAW)
        result = LogCanbusRawReading(time_delta_s, reading);
    else
        result = LogValueReading(time_delta_s, reading);

    if(result)
        last_reading_time_[reading.sensor->id_hash] = reading.timestamp.value();

    return result;
}

} // namespace eerie_leap::domain::logging_domain::loggers
