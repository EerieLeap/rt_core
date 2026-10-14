#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "subsys/mdf/mdf_property.h"
#include "subsys/mdf/mdf_value.h"

#include "mdf4_logger_sensor_reading.h"

namespace eerie_leap::domain::logging_domain::loggers {

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::subsys::mdf;
using namespace eerie_leap::subsys::mdf::mdf4;

namespace {

constexpr const char* TOOL_VENDOR = "Eerie Leap";
constexpr const char* HEADER_COMMENT = "Eerie Leap sensor log";
constexpr size_t MAX_ONE_BYTE_RECORD_ID = 255;
constexpr char MAX_EXPANDED_EXPONENT = '4';

// Functions every MDF reader evaluates the same way; ln, exp, max and the like are left out.
constexpr std::array<std::string_view, 6> FORMULA_FUNCTIONS{"sin", "cos", "tan", "sqrt", "abs", "log"};

struct FormulaToken {
    enum class Kind : uint8_t {
        Number,
        Input,
        Function,
        Operator,
        Power,
        Open,
        Close
    };

    Kind kind;
    std::string_view text;
    size_t begin;
};

bool IsDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool IsIdentifierStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsIdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

size_t SkipSpaces(std::string_view text, size_t pos) {
    while(pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0)
        pos++;

    return pos;
}

size_t SkipDigits(std::string_view text, size_t pos) {
    while(pos < text.size() && IsDigit(text[pos]))
        pos++;

    return pos;
}

// Nothing when the expression holds anything outside the formula subset.
std::optional<std::vector<FormulaToken>> TokenizeFormula(std::string_view expression) {
    std::vector<FormulaToken> tokens;

    for(size_t pos = SkipSpaces(expression, 0); pos < expression.size(); pos = SkipSpaces(expression, pos)) {
        const size_t begin = pos;
        const char c = expression[pos];
        FormulaToken::Kind kind;

        if(IsIdentifierStart(c)) {
            while(pos < expression.size() && IsIdentifierChar(expression[pos]))
                pos++;

            const auto name = expression.substr(begin, pos - begin);
            const size_t next = SkipSpaces(expression, pos);
            const bool is_call = next < expression.size() && expression[next] == '(';

            if(name == "x" && !is_call)
                kind = FormulaToken::Kind::Input;
            else if(is_call && std::ranges::find(FORMULA_FUNCTIONS, name) != FORMULA_FUNCTIONS.end())
                kind = FormulaToken::Kind::Function;
            else
                return std::nullopt;
        } else if(IsDigit(c) || c == '.') {
            const size_t integer_end = SkipDigits(expression, pos);
            size_t digit_count = integer_end - pos;

            // Python, and so numexpr, rejects "05" as a literal.
            if(digit_count > 1 && c == '0')
                return std::nullopt;

            pos = integer_end;

            if(pos < expression.size() && expression[pos] == '.') {
                const size_t fraction_end = SkipDigits(expression, pos + 1);
                digit_count += fraction_end - (pos + 1);
                pos = fraction_end;
            }

            if(digit_count == 0)
                return std::nullopt;

            if(pos < expression.size() && (expression[pos] == 'e' || expression[pos] == 'E')) {
                pos++;
                if(pos < expression.size() && (expression[pos] == '+' || expression[pos] == '-'))
                    pos++;

                const size_t exponent_end = SkipDigits(expression, pos);
                if(exponent_end == pos)
                    return std::nullopt;
                pos = exponent_end;
            }

            // "2x", "1.2.3" and hexadecimal literals are not decimal numbers.
            if(pos < expression.size() && (IsIdentifierChar(expression[pos]) || expression[pos] == '.'))
                return std::nullopt;

            kind = FormulaToken::Kind::Number;
        } else {
            switch(c) {
            case '+':
            case '-':
                kind = FormulaToken::Kind::Operator;
                break;
            case '*':
            case '/':
                // Both are binary only; "**" is power in some readers and an error in others.
                if(tokens.empty()
                    || tokens.back().kind == FormulaToken::Kind::Operator
                    || tokens.back().kind == FormulaToken::Kind::Power
                    || tokens.back().kind == FormulaToken::Kind::Open) {

                    return std::nullopt;
                }
                kind = FormulaToken::Kind::Operator;
                break;
            case '^':
                kind = FormulaToken::Kind::Power;
                break;
            case '(':
                kind = FormulaToken::Kind::Open;
                break;
            case ')':
                kind = FormulaToken::Kind::Close;
                break;
            default:
                return std::nullopt;
            }

            pos++;
        }

        tokens.push_back(FormulaToken{kind, expression.substr(begin, pos - begin), begin});
    }

    return tokens;
}

std::string GetSensorComment(const Sensor& sensor) {
    std::string comment(sensor.metadata.name);

    if(!sensor.metadata.description.empty()) {
        if(!comment.empty())
            comment += ": ";
        comment += sensor.metadata.description;
    }

    return comment;
}

int64_t ToTicks(const std::chrono::system_clock::time_point& time) {
    return static_cast<int64_t>(time.time_since_epoch().count());
}

std::chrono::system_clock::time_point FromTicks(int64_t ticks) {
    return std::chrono::system_clock::time_point(std::chrono::system_clock::duration(ticks));
}

} // namespace

double Mdf4LoggerSensorReading::ToSeconds(const std::chrono::system_clock::time_point& time) const {
    return std::chrono::duration<double>(time - start_time_).count();
}

uint8_t Mdf4LoggerSensorReading::GetRecordIdSizeBytes(size_t enabled_sensor_count) {
    // A CAN sensor takes two IDs (its group and the VLSD group), and IDs start at 1.
    return 2 * enabled_sensor_count + 1 <= MAX_ONE_BYTE_RECORD_ID ? 1 : 2;
}

std::optional<std::string> Mdf4LoggerSensorReading::ToMdfFormula(std::string_view expression) {
    const auto tokens = TokenizeFormula(expression);
    if(!tokens.has_value())
        return std::nullopt;

    const bool uses_input = std::ranges::any_of(tokens.value(),
        [](const FormulaToken& token) { return token.kind == FormulaToken::Kind::Input; });
    if(!uses_input)
        return std::nullopt;

    auto token_text = [](const FormulaToken& token) -> std::string_view {
        return token.kind == FormulaToken::Kind::Input ? "X" : token.text;
    };

    std::string formula;
    size_t pos = tokens->front().begin;

    for(size_t i = 0; i < tokens->size(); i++) {
        const auto& token = tokens->at(i);

        // Only reached when its left operand is not a single token: handled below otherwise.
        if(token.kind == FormulaToken::Kind::Power)
            return std::nullopt;

        formula.append(expression.substr(pos, token.begin - pos));

        const bool is_power_base = i + 1 < tokens->size() && tokens->at(i + 1).kind == FormulaToken::Kind::Power;
        if(!is_power_base) {
            formula.append(token_text(token));
            pos = token.begin + token.text.size();
            continue;
        }

        // ^ is power here and XOR in MCD-2 MC formulas, and numexpr (asammdf) has no pow(), so a
        // small whole exponent is spelled out as a product, which every reader evaluates. A chained,
        // grouped or other exponent would need the engine's precedence rules: not converted.
        const bool is_single_base = token.kind == FormulaToken::Kind::Input || token.kind == FormulaToken::Kind::Number;
        const bool is_small_exponent = i + 2 < tokens->size()
            && tokens->at(i + 2).kind == FormulaToken::Kind::Number
            && tokens->at(i + 2).text.size() == 1
            && tokens->at(i + 2).text[0] >= '1'
            && tokens->at(i + 2).text[0] <= MAX_EXPANDED_EXPONENT
            && (i + 3 >= tokens->size() || tokens->at(i + 3).kind != FormulaToken::Kind::Power);

        if(!is_single_base || !is_small_exponent)
            return std::nullopt;

        const auto& exponent = tokens->at(i + 2);
        const int factor_count = exponent.text[0] - '0';

        formula += "(";
        for(int factor = 0; factor < factor_count; factor++) {
            if(factor > 0)
                formula += " * ";
            formula += token_text(token);
        }
        formula += ")";

        pos = exponent.begin + exponent.text.size();
        i += 2;
    }

    return formula;
}

Mdf4LoggerSensorReading::Mdf4LoggerSensorReading(
    const LoggingConfiguration& configuration,
    std::span<const std::shared_ptr<Sensor>> sensors,
    const LoggerMetadata& metadata)
        : stream_(nullptr) {

    // A disabled sensor never has a reading, so it gets no channel whatever its logging configuration says.
    auto find_enabled = [&configuration](const Sensor& sensor) -> const SensorLoggingConfiguration* {
        if(!sensor.is_enabled)
            return nullptr;

        auto sensor_configuration = configuration.sensor_configurations.find(sensor.id_hash);
        if(sensor_configuration == configuration.sensor_configurations.end() || !sensor_configuration->second.is_enabled)
            return nullptr;

        return &sensor_configuration->second;
    };

    const size_t enabled_sensor_count = std::ranges::count_if(sensors,
        [&find_enabled](const auto& sensor) { return find_enabled(*sensor) != nullptr; });

    mdf4_file_ = std::make_unique<Mdf4File>(Mdf4FileOptions{
        .record_id_size_bytes = GetRecordIdSizeBytes(enabled_sensor_count)
    });

    SetFileMetadata(configuration, metadata);

    auto source_information = std::make_shared<mdf4::SourceInformationBlock>(
        mdf4::SourceInformationBlock::SourceType::IoDevice,
        mdf4::SourceInformationBlock::BusType::None);
    source_information->SetName(mdf4_file_->GetOrCreateTextBlock("Eerie Leap Sensor"));

    for(const auto& sensor : sensors) {
        const auto* sensor_configuration = find_enabled(*sensor);
        if(sensor_configuration == nullptr)
            continue;

        if(sensor->configuration.type == SensorType::CANBUS_RAW)
            AddCanFrameTarget(*sensor);
        else
            AddValueTarget(*sensor, *sensor_configuration, source_information);
    }
}

void Mdf4LoggerSensorReading::SetFileMetadata(const LoggingConfiguration& configuration, const LoggerMetadata& metadata) {
    mdf4_file_->SetFileHistory(metadata.tool_id, TOOL_VENDOR, metadata.tool_version);

    const std::array<MdfProperty, 7> properties{{
        {"device_name", metadata.device_name},
        {"product_id", std::to_string(metadata.product_id)},
        {"device_id", std::to_string(static_cast<uint32_t>(metadata.device_id))},
        {"hw_version", std::to_string(metadata.hw_version)},
        {"sw_version", std::to_string(metadata.sw_version)},
        {"sensors_configuration_hash", std::to_string(metadata.sensors_configuration_hash)},
        {"logging_interval_ms", std::to_string(configuration.logging_interval_ms)}
    }};

    mdf4_file_->SetHeaderComment(HEADER_COMMENT, properties);
}

void Mdf4LoggerSensorReading::AddValueTarget(
    const Sensor& sensor,
    const SensorLoggingConfiguration& sensor_configuration,
    const std::shared_ptr<mdf4::SourceInformationBlock>& source_information) {

    const std::string sensor_id(sensor.id);

    auto channel_group = mdf4_file_->CreateChannelGroup(next_record_id_++, sensor_id);
    channel_group->AddSourceInformation(source_information);

    auto value_channel = mdf4_file_->CreateDataChannel(
        channel_group, MdfDataType::Float32, sensor_id, std::string(sensor.metadata.unit), true);

    const auto comment = GetSensorComment(sensor);
    if(!comment.empty())
        mdf4_file_->SetChannelComment(value_channel, comment);

    const bool has_raw_value_channel = sensor.configuration.HasExpression()
        && sensor_configuration.log_raw_value
        && (sensor.configuration.type == SensorType::PHYSICAL_ANALOG
            || sensor.configuration.type == SensorType::PHYSICAL_INDICATOR);

    LogTarget target{
        .kind = has_raw_value_channel ? LogTarget::Kind::ValueWithRaw : LogTarget::Kind::Value,
        .log_only_new_data = sensor_configuration.log_only_new_data,
        .value_invalid_mask = 1U << value_channel->GetInvalidationBitPosition()
    };

    if(has_raw_value_channel) {
        auto raw_channel = mdf4_file_->CreateDataChannel(channel_group, MdfDataType::Float32, sensor_id + ".raw", "", true);
        target.raw_value_invalid_mask = 1U << raw_channel->GetInvalidationBitPosition();

        // A formula naming other sensors or engine-only functions would be evaluated wrongly or
        // not at all by readers, so those expressions are documented instead.
        const auto formula = ToMdfFormula(sensor.configuration.expression);
        if(formula.has_value())
            raw_channel->SetConversion(mdf4_file_->CreateAlgebraicConversion(formula.value()));
        else
            mdf4_file_->SetChannelComment(raw_channel, "expression: " + std::string(sensor.configuration.expression));
    }

    targets_.emplace(sensor.id_hash, target);
    target_channel_groups_.push_back(TargetChannelGroup{sensor.id_hash, channel_group});
}

void Mdf4LoggerSensorReading::AddCanFrameTarget(const Sensor& sensor) {
    const std::string sensor_id(sensor.id);

    auto vlsd_channel_group = mdf4_file_->CreateVLSDChannelGroup(next_record_id_++);
    auto channel_group = mdf4_file_->CreateCanDataFrameChannelGroup(vlsd_channel_group, next_record_id_++, sensor_id);

    const auto comment = GetSensorComment(sensor);
    if(!comment.empty())
        mdf4_file_->SetChannelGroupComment(channel_group, comment);

    // BusChannel numbers interfaces from 1; 0 tells readers the bus is unknown.
    const auto& canbus_source = sensor.configuration.canbus_source;
    const uint8_t bus_channel = canbus_source != nullptr && canbus_source->bus_channel < UINT8_MAX
        ? static_cast<uint8_t>(canbus_source->bus_channel + 1)
        : 0;

    // Every queued frame is a bus event of its own, so log_only_new_data does not apply: two frames
    // can share a timestamp when they arrive within the time service's resolution.
    targets_.emplace(sensor.id_hash, LogTarget{
        .kind = LogTarget::Kind::CanFrame,
        .log_only_new_data = false,
        .bus_channel = bus_channel
    });
    target_channel_groups_.push_back(TargetChannelGroup{sensor.id_hash, channel_group});
}

const char* Mdf4LoggerSensorReading::GetFileExtension() const {
    return Mdf4File::LOG_DATA_FILE_EXTENSION;
}

bool Mdf4LoggerSensorReading::StartLogging(std::streambuf& stream, const std::chrono::system_clock::time_point& start_time) {
    // Set once the header is out: until then the writers the targets point at may not exist.
    stream_ = nullptr;
    start_time_ = start_time;

    for(auto& [_, target] : targets_) {
        target.last_timestamp = NO_TIMESTAMP;
        target.last_record_time = NO_TIMESTAMP;
    }

    mdf4_file_->UpdateCurrentTime(start_time);
    current_file_size_bytes_ = mdf4_file_->WriteHeaderToStream(stream);

    // Every header builds new writers, so the previous file's pointers are stale now.
    for(const auto& [sensor_id_hash, channel_group] : target_channel_groups_) {
        auto& target = targets_.at(sensor_id_hash);

        if(target.kind == LogTarget::Kind::CanFrame)
            target.can_writer = &mdf4_file_->GetCanRecordWriter(channel_group);
        else
            target.writer = &mdf4_file_->GetRecordWriter(channel_group);
    }

    // The header sits in the stream's write buffer until a sync; without one a volume that refuses
    // writes would only be noticed a sync interval later, with the file at 0 bytes.
    if(stream.pubsync() != 0)
        return false;

    stream_ = &stream;

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

void Mdf4LoggerSensorReading::LogValueReading(const LogTarget& target, double time_delta_s, const SensorReading& reading) {
    // An error reading may still carry the value of an earlier step; it is not the sensor's value.
    // Its raw value, when it has one, is the input that was measured, so it stays valid.
    const bool is_value_valid = reading.value.has_value() && !reading.HasError();
    const float value = is_value_valid ? reading.value.value() : 0.0F;
    uint32_t invalid_mask = is_value_valid ? 0 : target.value_invalid_mask;

    if(target.kind == LogTarget::Kind::ValueWithRaw) {
        if(!reading.raw_value.has_value())
            invalid_mask |= target.raw_value_invalid_mask;

        const std::array<MdfValue, 3> values{time_delta_s, value, reading.raw_value.value_or(0.0F)};
        current_file_size_bytes_ += target.writer->Write(*stream_, values, invalid_mask);
    } else {
        const std::array<MdfValue, 2> values{time_delta_s, value};
        current_file_size_bytes_ += target.writer->Write(*stream_, values, invalid_mask);
    }
}

bool Mdf4LoggerSensorReading::LogCanFrameReading(const LogTarget& target, double time_delta_s, const SensorReading& reading) {
    if(reading.can_frame == nullptr)
        return false;

    current_file_size_bytes_ += target.can_writer->Write(*stream_, *reading.can_frame, time_delta_s, target.bus_channel);

    return true;
}

bool Mdf4LoggerSensorReading::LogReading(const std::chrono::system_clock::time_point& time, const SensorReading& reading) {
    if(stream_ == nullptr)
        return false;

    auto entry = targets_.find(reading.sensor_id_hash);
    if(entry == targets_.end())
        return false;

    auto& target = entry->second;

    // Readings that predate the file start would give a negative time.
    auto record_time = std::max(reading.timestamp.value_or(time), start_time_);

    if(target.kind == LogTarget::Kind::CanFrame)
        return LogCanFrameReading(target, ToSeconds(record_time), reading);

    // A reading with an error or without a value is a sample like any other: its own timestamp makes
    // it new or held, and it stays invalid when held.
    const int64_t timestamp = reading.timestamp.has_value() ? ToTicks(reading.timestamp.value()) : NO_TIMESTAMP;
    const bool is_repeat = timestamp != NO_TIMESTAMP && timestamp == target.last_timestamp;

    if(is_repeat) {
        if(target.log_only_new_data)
            return false;

        // Sample and hold: a sample already in the file is written again at the caller's time.
        record_time = time;
    }

    // The master channel of a group strictly increases. A repeat at or before the last record says
    // nothing new. A new sample stamped before the tick that last repeated its predecessor was still
    // being processed then; it goes right after that record.
    if(target.last_record_time != NO_TIMESTAMP && ToTicks(record_time) <= target.last_record_time) {
        if(is_repeat)
            return false;

        record_time = FromTicks(target.last_record_time) + MIN_RECORD_TIME_STEP;
    }

    LogValueReading(target, ToSeconds(record_time), reading);

    if(timestamp != NO_TIMESTAMP)
        target.last_timestamp = timestamp;
    target.last_record_time = ToTicks(record_time);

    return true;
}

} // namespace eerie_leap::domain::logging_domain::loggers
