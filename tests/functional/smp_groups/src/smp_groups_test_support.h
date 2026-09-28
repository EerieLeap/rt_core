#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <mgmt/mcumgr/transport/smp_internal.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include "subsys/smp/smp_header.h"
#include "subsys/smp/smp_group_id.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"
#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"

namespace smp_groups_test {

using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpHeader;
using eerie_leap::subsys::smp::SmpOperation;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;
using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;

using Bytes = std::vector<uint8_t>;

inline Bytes Pattern(size_t size, uint8_t seed) {
    Bytes bytes(size);
    for(size_t i = 0; i < size; i++)
        bytes[i] = static_cast<uint8_t>(seed + i * 13);

    return bytes;
}

// Builds a request body: a CBOR map of unsigned integers and byte strings.
class CborMap {
private:
    std::array<uint8_t, CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE> buffer_{};
    zcbor_state_t states_[4];

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
        zassert_true(zcbor_tstr_encode_ptr(states_, key.data(), key.size())
            && zcbor_bstr_encode_ptr(states_, reinterpret_cast<const char*>(value.data()), value.size()));
        return *this;
    }

    Bytes Build() {
        zassert_true(zcbor_map_end_encode(states_, 16));
        const auto size = static_cast<size_t>(states_[0].payload - buffer_.data());
        return Bytes(buffer_.begin(), buffer_.begin() + size);
    }
};

using Entry = std::map<std::string, uint32_t>;

// A decoded response body: unsigned values, byte strings, one list of maps and the group error.
struct Response {
    SmpHeader header;
    std::map<std::string, uint32_t> values;
    std::map<std::string, Bytes> bytes;
    std::vector<Entry> entries;
    std::optional<uint32_t> error_group;
    std::optional<uint32_t> error;

    [[nodiscard]] uint32_t Value(const std::string& key) const {
        auto it = values.find(key);
        zassert_true(it != values.end(), "Response has no \"%s\"", key.c_str());
        return it->second;
    }

    [[nodiscard]] bool Has(const std::string& key) const { return values.contains(key) || bytes.contains(key); }
    [[nodiscard]] bool IsOk() const { return !error.has_value() && !values.contains("rc"); }
};

namespace detail {

inline uint8_t MajorType(const zcbor_state_t* state) {
    return static_cast<uint8_t>(*state->payload >> 5);
}

inline bool DecodeEntry(zcbor_state_t* zsd, Entry& entry) {
    if(!zcbor_map_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        zcbor_string key{};
        uint32_t value = 0;
        if(!zcbor_tstr_decode(zsd, &key) || !zcbor_uint32_decode(zsd, &value))
            return false;

        entry[std::string(reinterpret_cast<const char*>(key.value), key.len)] = value;
    }

    return zcbor_map_end_decode(zsd);
}

inline bool DecodeBody(std::span<const uint8_t> body, Response& response) {
    ZCBOR_STATE_D(zsd, 4, body.data(), body.size(), 1, 0);

    if(!zcbor_map_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        zcbor_string key{};
        if(!zcbor_tstr_decode(zsd, &key))
            return false;

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
                uint32_t value = 0;
                if(!zcbor_uint32_decode(zsd, &value))
                    return false;
                response.values[name] = value;
                break;
            }
            case 2: {
                zcbor_string value{};
                if(!zcbor_bstr_decode(zsd, &value))
                    return false;
                response.bytes[name] = Bytes(value.value, value.value + value.len);
                break;
            }
            case 4: {
                if(!zcbor_list_start_decode(zsd))
                    return false;
                while(!zcbor_array_at_end(zsd)) {
                    Entry entry;
                    if(!DecodeEntry(zsd, entry))
                        return false;
                    response.entries.push_back(entry);
                }
                if(!zcbor_list_end_decode(zsd))
                    return false;
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
    static inline SmpTestClient* instance_ = nullptr;

    smp_transport transport_{};
    k_sem received_{};
    k_mutex lock_{};
    Bytes response_;
    uint8_t sequence_ = 0;

    static int Output(net_buf* packet) {
        SmpTestClient* client = instance_;
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
        instance_ = this;
    }

    ~SmpTestClient() {
        k_work_sync sync;
        k_work_cancel_sync(&transport_.work, &sync);
        instance_ = nullptr;
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

        Response response{.header = *response_header};
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

// A configuration manager that stores the raw CBOR it is given.
class FakeConfigurationManager : public ICborConfigurationManager {
public:
    Bytes stored;
    int apply_count = 0;
    bool reject = false;

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override {
        apply_count++;
        if(reject)
            return false;

        stored.assign(cbor_data.begin(), cbor_data.end());
        return true;
    }

    std::pmr::vector<uint8_t> GetCborConfiguration() override {
        return std::pmr::vector<uint8_t>(stored.begin(), stored.end());
    }

    eerie_leap::configuration::services::StoredCborInfo GetCborConfigurationInfo() override {
        return {.size = stored.size(), .crc = crc32_ieee(stored.data(), stored.size())};
    }
};

class FakeNetworkInfo : public ICdmpNetworkInfo {
public:
    CdmpDeviceInfo self{};
    std::vector<CdmpDeviceInfo> devices;

    [[nodiscard]] CdmpDeviceInfo GetDeviceInfo() const override { return self; }

    size_t GetNetworkDevices(std::span<CdmpDeviceInfo> out) const override {
        const size_t count = std::min(out.size(), devices.size());
        std::copy_n(devices.begin(), count, out.begin());
        return count;
    }

    [[nodiscard]] size_t GetNetworkDeviceCount() const override { return devices.size(); }
};

} // namespace smp_groups_test
