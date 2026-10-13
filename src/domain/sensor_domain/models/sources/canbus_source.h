#pragma once

#include <charconv>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "utilities/string/string_helpers.h"

namespace eerie_leap::domain::sensor_domain::models::sources {

using eerie_leap::utilities::string::StringHelpers;

// A CAN sensor's source, written in its connection string as "bus_channel/frame_id[/signal_name]".
struct CanbusSource {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    /// The largest frame ID, an extended (29-bit) identifier.
    static constexpr uint32_t MAX_FRAME_ID = 0x1FFFFFFF;

    /// What a connection string names; signal_name points into the parsed text.
    struct Parts {
        uint8_t bus_channel = 0;
        uint32_t frame_id = 0;
        std::string_view signal_name;
    };

    uint8_t bus_channel;
    uint32_t frame_id;
    std::pmr::string signal_name;
    uint32_t signal_name_hash = 0;

    CanbusSource(std::allocator_arg_t, allocator_type alloc, uint8_t bus_channel, uint32_t frame_id, std::string_view signal_name)
        : bus_channel(bus_channel), frame_id(frame_id), signal_name(signal_name, alloc) {

        if(!signal_name.empty())
            signal_name_hash = StringHelpers::GetHash(signal_name);
    }

    CanbusSource(std::allocator_arg_t alloc_arg, allocator_type alloc, uint8_t bus_channel, uint32_t frame_id)
        : CanbusSource(alloc_arg, alloc, bus_channel, frame_id, "") {}


    CanbusSource(const CanbusSource&) = delete;
    CanbusSource& operator=(const CanbusSource&) = delete;

    CanbusSource(CanbusSource&&) noexcept = default;

    CanbusSource(CanbusSource&& other, allocator_type alloc) noexcept
        : bus_channel(other.bus_channel),
        frame_id(other.frame_id),
        signal_name(other.signal_name, alloc),
        signal_name_hash(other.signal_name_hash) {}

    /// Both numbers in decimal.
    std::string ToConnectionString() const {
        std::string connection_string = std::to_string(bus_channel);
        connection_string += "/" + std::to_string(frame_id);
        if(!signal_name.empty())
            connection_string += "/" + signal_name;

        return connection_string;
    }

    /**
     * @brief Reads "bus_channel/frame_id[/signal_name]". Each number is decimal digits, or "0x" and
     *        hex digits, with nothing else in it (a leading 0 is still decimal); the bus channel is
     *        at most 255 and the frame ID at most MAX_FRAME_ID. The signal name is all of the rest,
     *        and a second "/" needs one after it.
     * @return The parts, or a sentence saying what is wrong, for a validation message.
     */
    static std::expected<Parts, const char*> Parse(std::string_view connection_string) noexcept {
        static constexpr const char* FORMAT_ERROR =
            "Connection string must be bus_channel/frame_id or bus_channel/frame_id/signal_name.";

        if(connection_string.empty())
            return std::unexpected("Connection string cannot be empty.");

        const size_t bus_end = connection_string.find('/');
        if(bus_end == std::string_view::npos)
            return std::unexpected(FORMAT_ERROR);

        const auto bus_channel = ParseNumber(connection_string.substr(0, bus_end), UINT8_MAX);
        if(!bus_channel.has_value())
            return std::unexpected("Connection string bus channel must be a number from 0 to 255.");

        const std::string_view rest = connection_string.substr(bus_end + 1);
        const size_t frame_end = rest.find('/');

        const auto frame_id = ParseNumber(rest.substr(0, frame_end), MAX_FRAME_ID);
        if(!frame_id.has_value())
            return std::unexpected("Connection string frame ID must be a number from 0 to 0x1FFFFFFF.");

        Parts parts{.bus_channel = static_cast<uint8_t>(*bus_channel), .frame_id = *frame_id};

        if(frame_end != std::string_view::npos) {
            parts.signal_name = rest.substr(frame_end + 1);
            if(parts.signal_name.empty())
                return std::unexpected(FORMAT_ERROR);
        }

        return parts;
    }

    /** @brief Parse() into a source; throws std::invalid_argument with its error. */
    static CanbusSource FromConnectionString(allocator_type alloc, std::string_view connection_string) {
        const auto parts = Parse(connection_string);
        if(!parts.has_value())
            throw std::invalid_argument(parts.error());

        return {std::allocator_arg, alloc, parts->bus_channel, parts->frame_id, parts->signal_name};
    }

private:
    // A whole number in decimal, or in hex after "0x", up to max; nothing for any other text.
    static std::optional<uint32_t> ParseNumber(std::string_view text, uint32_t max) noexcept {
        int base = 10;
        if(text.starts_with("0x") || text.starts_with("0X")) {
            base = 16;
            text.remove_prefix(2);
        }

        if(text.empty())
            return std::nullopt;

        uint32_t value = 0;
        const char* end = text.data() + text.size();
        const auto [parsed_end, error] = std::from_chars(text.data(), end, value, base);
        if(error != std::errc() || parsed_end != end || value > max)
            return std::nullopt;

        return value;
    }
};

}

// namespace eerie_leap::domain::sensor_domain::models::sources
