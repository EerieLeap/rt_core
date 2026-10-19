#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mgmt/mcumgr/transport/smp_internal.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include "subsys/smp/smp_group_id.h"
#include "subsys/smp/smp_header.h"

// Drives MCUmgr groups in rt_core test suites, as an SMP client on a test transport.
namespace smp_test {

using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpHeader;
using eerie_leap::subsys::smp::SmpOperation;

using Bytes = std::vector<uint8_t>;

// Builds a request body: a CBOR map of unsigned integers, byte strings, text strings and lists of
// unsigned integers.
class CborMap {
private:
    std::array<uint8_t, CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE> buffer_{};
    zcbor_state_t states_[4]{};

public:
    CborMap() {
        zcbor_new_encode_state(states_, ZCBOR_ARRAY_SIZE(states_), buffer_.data(), buffer_.size(), 0);
        zcbor_map_start_encode(states_, 16);
    }

    CborMap& Put(std::string_view key, uint32_t value) {
        zassert_true(zcbor_tstr_encode_ptr(states_, key.data(), key.size()) && zcbor_uint32_put(states_, value));
        return *this;
    }

    CborMap& Put(std::string_view key, std::span<const uint8_t> value) {
        zassert_true(
            zcbor_tstr_encode_ptr(states_, key.data(), key.size())
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out text as bytes
            && zcbor_bstr_encode_ptr(states_, reinterpret_cast<const char*>(value.data()), value.size())
        );
        return *this;
    }

    CborMap& PutText(std::string_view key, std::string_view value) {
        zassert_true(
            zcbor_tstr_encode_ptr(states_, key.data(), key.size())
            && zcbor_tstr_encode_ptr(states_, value.data(), value.size())
        );
        return *this;
    }

    CborMap& Put(std::string_view key, std::span<const uint32_t> values) {
        bool ok = zcbor_tstr_encode_ptr(states_, key.data(), key.size())
            && zcbor_list_start_encode(states_, values.size());
        for(const uint32_t value : values)
            ok = ok && zcbor_uint32_put(states_, value);

        zassert_true(ok && zcbor_list_end_encode(states_, values.size()));
        return *this;
    }

    Bytes Build() {
        zassert_true(zcbor_map_end_encode(states_, 16));
        const auto size = static_cast<size_t>(states_[0].payload - buffer_.data());
        return { buffer_.begin(), buffer_.begin() + size };
    }
};

using Entry = std::map<std::string, uint32_t>;
using TextEntry = std::map<std::string, std::string>;
using FlagEntry = std::map<std::string, bool>;
using ListEntry = std::map<std::string, std::vector<std::string>>;

// A decoded response body: unsigned values, floats, booleans, byte and text strings, one list of maps
// of unsigned, text, boolean and text list values, and the group error.
struct Response {
    SmpHeader header;
    std::map<std::string, uint32_t> values;
    // Unsigned values that do not fit 32 bits.
    std::map<std::string, uint64_t> wide_values;
    std::map<std::string, float> floats;
    std::map<std::string, bool> flags;
    std::map<std::string, Bytes> bytes;
    std::map<std::string, std::string> texts;
    std::vector<Entry> entries;
    // The text values of the maps in `entries`, index for index.
    std::vector<TextEntry> entry_texts;
    // Their boolean values and their lists of text values, index for index.
    std::vector<FlagEntry> entry_flags;
    std::vector<ListEntry> entry_lists;
    // The number of maps in each list of the body, by key; says an empty list was there.
    std::map<std::string, size_t> list_sizes;
    // Lists of plain unsigned values and of text values, by key.
    std::map<std::string, std::vector<uint32_t>> uint_lists;
    std::map<std::string, std::vector<std::string>> text_lists;
    std::optional<uint32_t> error_group;
    std::optional<uint32_t> error;

    [[nodiscard]] uint32_t Value(const std::string& key) const {
        const auto it = values.find(key);
        zassert_true(it != values.end(), "Response has no \"%s\"", key.c_str());
        return it->second;
    }

    [[nodiscard]] uint64_t WideValue(const std::string& key) const {
        if(const auto it = values.find(key); it != values.end())
            return it->second;

        const auto it = wide_values.find(key);
        zassert_true(it != wide_values.end(), "Response has no \"%s\"", key.c_str());
        return it->second;
    }

    [[nodiscard]] float Float(const std::string& key) const {
        const auto it = floats.find(key);
        zassert_true(it != floats.end(), "Response has no float \"%s\"", key.c_str());
        return it->second;
    }

    [[nodiscard]] bool Flag(const std::string& key) const {
        const auto it = flags.find(key);
        zassert_true(it != flags.end(), "Response has no boolean \"%s\"", key.c_str());
        return it->second;
    }

    [[nodiscard]] bool Has(const std::string& key) const {
        return values.contains(key)
            || wide_values.contains(key)
            || floats.contains(key)
            || flags.contains(key)
            || bytes.contains(key)
            || texts.contains(key);
    }
    [[nodiscard]] bool IsOk() const {
        return !error.has_value() && !values.contains("rc");
    }
};

namespace detail {

inline uint8_t MajorType(const zcbor_state_t* state) {
    return static_cast<uint8_t>(*state->payload >> 5);
}

inline uint8_t AdditionalInfo(const zcbor_state_t* state) {
    return static_cast<uint8_t>(*state->payload & 0x1F);
}

// Decodes a map of unsigned values into entry; its text values into texts, its booleans into flags
// and its lists of text values into lists, when given.
inline bool DecodeEntry(
    zcbor_state_t* zsd,
    Entry& entry,
    TextEntry* texts = nullptr,
    FlagEntry* flags = nullptr,
    ListEntry* lists = nullptr
) {

    if(!zcbor_map_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        zcbor_string key{};
        if(!zcbor_tstr_decode(zsd, &key))
            return false;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out text as bytes
        const std::string name(reinterpret_cast<const char*>(key.value), key.len);

        if(flags != nullptr && MajorType(zsd) == 7) {
            bool value = false;
            if(!zcbor_bool_decode(zsd, &value))
                return false;

            (*flags)[name] = value;
            continue;
        }

        if(lists != nullptr && MajorType(zsd) == 4) {
            std::vector<std::string>& items = (*lists)[name];
            if(!zcbor_list_start_decode(zsd))
                return false;

            while(!zcbor_array_at_end(zsd)) {
                zcbor_string item{};
                if(!zcbor_tstr_decode(zsd, &item))
                    return false;

                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out text as bytes
                items.emplace_back(reinterpret_cast<const char*>(item.value), item.len);
            }

            if(!zcbor_list_end_decode(zsd))
                return false;

            continue;
        }

        if(texts != nullptr && MajorType(zsd) == 3) {
            zcbor_string value{};
            if(!zcbor_tstr_decode(zsd, &value))
                return false;

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out text as bytes
            (*texts)[name] = std::string(reinterpret_cast<const char*>(value.value), value.len);
            continue;
        }

        uint32_t value = 0;
        if(!zcbor_uint32_decode(zsd, &value))
            return false;

        entry[name] = value;
    }

    return zcbor_map_end_decode(zsd);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one flat decoder that mirrors the response layout; splitting it would only scatter the zcbor state handling
inline bool DecodeBody(std::span<const uint8_t> body, Response& response) {
    // Deep enough for the response map, a list, its maps and their lists.
    ZCBOR_STATE_D(zsd, 5, body.data(), body.size(), 1, 0);

    if(!zcbor_map_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        zcbor_string key{};
        if(!zcbor_tstr_decode(zsd, &key))
            return false;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out text as bytes
        const std::string name(reinterpret_cast<const char*>(key.value), key.len);

        if(name == "err") {
            Entry error;
            if(!DecodeEntry(zsd, error))
                return false;

            response.error_group = error["group"];
            response.error = error["rc"];
            continue;
        }

        switch(MajorType(zsd)) {
            case 0: {
                uint64_t value = 0;
                if(!zcbor_uint64_decode(zsd, &value))
                    return false;
                if(value <= UINT32_MAX)
                    response.values[name] = static_cast<uint32_t>(value);
                else
                    response.wide_values[name] = value;
                break;
            }
            case 2: {
                zcbor_string value{};
                if(!zcbor_bstr_decode(zsd, &value))
                    return false;
                response.bytes[name] = Bytes(value.value, value.value + value.len);
                break;
            }
            case 3: {
                zcbor_string value{};
                if(!zcbor_tstr_decode(zsd, &value))
                    return false;
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out text as bytes
                response.texts[name] = std::string(reinterpret_cast<const char*>(value.value), value.len);
                break;
            }
            case 4: {
                if(!zcbor_list_start_decode(zsd))
                    return false;
                size_t size = 0;
                while(!zcbor_array_at_end(zsd)) {
                    if(MajorType(zsd) == 0) {
                        uint32_t item = 0;
                        if(!zcbor_uint32_decode(zsd, &item))
                            return false;
                        response.uint_lists[name].push_back(item);
                        size++;
                        continue;
                    }
                    if(MajorType(zsd) == 3) {
                        zcbor_string item{};
                        if(!zcbor_tstr_decode(zsd, &item))
                            return false;
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out text as bytes
                        response.text_lists[name].emplace_back(reinterpret_cast<const char*>(item.value), item.len);
                        size++;
                        continue;
                    }
                    Entry entry;
                    TextEntry texts;
                    FlagEntry flags;
                    ListEntry lists;
                    if(!DecodeEntry(zsd, entry, &texts, &flags, &lists))
                        return false;
                    response.entries.push_back(entry);
                    response.entry_texts.push_back(texts);
                    response.entry_flags.push_back(flags);
                    response.entry_lists.push_back(lists);
                    size++;
                }
                if(!zcbor_list_end_decode(zsd))
                    return false;
                response.list_sizes[name] = size;
                break;
            }
            case 7: {
                // Simple values 20 and 21 are false and true, 26 a float32.
                const uint8_t info = AdditionalInfo(zsd);
                if(info == 20 || info == 21) {
                    bool value = false;
                    if(!zcbor_bool_decode(zsd, &value))
                        return false;
                    response.flags[name] = value;
                } else if(info == 26) {
                    float value = 0.0F;
                    if(!zcbor_float32_decode(zsd, &value))
                        return false;
                    response.floats[name] = value;
                } else if(!zcbor_any_skip(zsd, nullptr)) {
                    return false;
                }
                break;
            }
            default:
                if(!zcbor_any_skip(zsd, nullptr))
                    return false;
        }
    }

    return zcbor_map_end_decode(zsd);
}

} // namespace detail

// Stands in for an SMP client: injects requests into MCUmgr and captures the responses.
class SmpTestClient {
private:
    static inline SmpTestClient* instance = nullptr;

    smp_transport transport_{};
    k_sem received_{};
    k_mutex lock_{};
    Bytes response_;
    uint8_t sequence_ = 0;

    static int Output(net_buf* packet) {
        SmpTestClient* client = instance;
        if(client != nullptr) {
            k_mutex_lock(&client->lock_, K_FOREVER);
            client->response_.assign(packet->data, packet->data + packet->len);
            k_mutex_unlock(&client->lock_);
            k_sem_give(&client->received_);
        }

        smp_packet_free(packet);

        return 0;
    }

public:
    SmpTestClient() {
        k_sem_init(&received_, 0, 1);
        k_mutex_init(&lock_);
        transport_.functions.output = Output;
        zassert_equal(smp_transport_init(&transport_), 0);
        instance = this;
    }

    SmpTestClient(const SmpTestClient&) = delete;
    SmpTestClient& operator=(const SmpTestClient&) = delete;
    SmpTestClient(SmpTestClient&&) = delete;
    SmpTestClient& operator=(SmpTestClient&&) = delete;

    ~SmpTestClient() {
        k_work_sync sync{};
        k_work_cancel_sync(&transport_.work, &sync);
        instance = nullptr;
    }

    std::optional<Response> Send(SmpOperation operation, SmpGroupId group, uint8_t command, const Bytes& body) {
        net_buf* packet = smp_packet_alloc();
        zassert_not_null(packet, "SMP pool exhausted");

        const SmpHeader header{
            .operation = operation,
            .version = 1,
            .length = static_cast<uint16_t>(body.size()),
            .group = std::to_underlying(group),
            .sequence = ++sequence_,
            .command_id = command,
        };
        std::array<uint8_t, SmpHeader::SIZE> raw_header{};
        header.Encode(raw_header);
        net_buf_add_mem(packet, raw_header.data(), raw_header.size());
        net_buf_add_mem(packet, body.data(), body.size());

        smp_rx_req(&transport_, packet);

        if(k_sem_take(&received_, K_MSEC(2000)) != 0)
            return std::nullopt;

        k_mutex_lock(&lock_, K_FOREVER);
        const Bytes raw = response_;
        k_mutex_unlock(&lock_);

        auto response_header = SmpHeader::Parse(raw);
        if(!response_header.has_value() || response_header->sequence != header.sequence)
            return std::nullopt;

        Response response{ .header = *response_header };
        if(!detail::DecodeBody(std::span(raw).subspan(SmpHeader::SIZE), response))
            return std::nullopt;

        return response;
    }

    Response Read(SmpGroupId group, uint8_t command, const Bytes& body = CborMap().Build()) {
        auto response = Send(SmpOperation::READ, group, command, body);
        zassert_true(response.has_value(), "No response to read %u/%u", std::to_underlying(group), command);
        return *response;
    }

    Response Write(SmpGroupId group, uint8_t command, const Bytes& body) {
        auto response = Send(SmpOperation::WRITE, group, command, body);
        zassert_true(response.has_value(), "No response to write %u/%u", std::to_underlying(group), command);
        return *response;
    }
};

} // namespace smp_test
