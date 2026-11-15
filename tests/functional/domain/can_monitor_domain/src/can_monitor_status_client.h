#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mgmt/mcumgr/transport/smp_internal.h>
#include <zcbor_decode.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include "subsys/smp/smp_group_id.h"
#include "subsys/smp/smp_header.h"

// STATUS answers a list of maps that hold a list of numbers, which the common SMP test client does not decode.
// This client sends a request as it does and hands back the raw answer, which StatusView reads.
namespace can_monitor_test {

using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpHeader;
using eerie_leap::subsys::smp::SmpOperation;

struct StatusPort {
    uint32_t ch = 0;
    std::vector<uint32_t> types;
    bool is_configured = false;
    uint32_t state = 0;
    uint32_t bitrate = 0;
    uint32_t data_bitrate = 0;
    bool has_session = false;
};

/** @brief STATUS's answer, decoded. */
struct StatusView {
    uint32_t version = 0;
    std::vector<StatusPort> ports;
    uint32_t filters_max = 0;
    uint32_t ring_bytes = 0;
    uint32_t notify_max = 0;
    bool has_tx = false;
    bool has_synthetic = false;
    bool has_delta = false;

    [[nodiscard]] const StatusPort* Port(uint32_t ch) const {
        for(const StatusPort& port : ports) {
            if(port.ch == ch)
                return &port;
        }

        return nullptr;
    }
};

inline bool KeyIs(const zcbor_string& key, std::string_view name) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out text as bytes
    return std::string_view(reinterpret_cast<const char*>(key.value), key.len) == name;
}

inline bool DecodePort(zcbor_state_t* zsd, StatusPort& port) {
    if(!zcbor_map_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        zcbor_string key{};
        if(!zcbor_tstr_decode(zsd, &key))
            return false;

        bool ok = false;
        if(KeyIs(key, "ch")) {
            ok = zcbor_uint32_decode(zsd, &port.ch);
        } else if(KeyIs(key, "types")) {
            ok = zcbor_list_start_decode(zsd);
            while(ok && !zcbor_array_at_end(zsd)) {
                uint32_t type = 0;
                ok = zcbor_uint32_decode(zsd, &type);
                port.types.push_back(type);
            }
            ok = ok && zcbor_list_end_decode(zsd);
        } else if(KeyIs(key, "configured")) {
            ok = zcbor_bool_decode(zsd, &port.is_configured);
        } else if(KeyIs(key, "state")) {
            ok = zcbor_uint32_decode(zsd, &port.state);
        } else if(KeyIs(key, "bitrate")) {
            ok = zcbor_uint32_decode(zsd, &port.bitrate);
        } else if(KeyIs(key, "data_bitrate")) {
            ok = zcbor_uint32_decode(zsd, &port.data_bitrate);
        } else if(KeyIs(key, "session")) {
            ok = zcbor_bool_decode(zsd, &port.has_session);
        } else {
            ok = zcbor_any_skip(zsd, nullptr);
        }

        if(!ok)
            return false;
    }

    return zcbor_map_end_decode(zsd);
}

inline std::optional<StatusView> DecodeStatus(std::span<const uint8_t> body) {
    ZCBOR_STATE_D(zsd, 6, body.data(), body.size(), 1, 0);

    StatusView view;
    if(!zcbor_map_start_decode(zsd))
        return std::nullopt;

    while(!zcbor_array_at_end(zsd)) {
        zcbor_string key{};
        if(!zcbor_tstr_decode(zsd, &key))
            return std::nullopt;

        bool ok = false;
        if(KeyIs(key, "version")) {
            ok = zcbor_uint32_decode(zsd, &view.version);
        } else if(KeyIs(key, "ports")) {
            ok = zcbor_list_start_decode(zsd);
            while(ok && !zcbor_array_at_end(zsd)) {
                StatusPort port;
                ok = DecodePort(zsd, port);
                view.ports.push_back(std::move(port));
            }
            ok = ok && zcbor_list_end_decode(zsd);
        } else if(KeyIs(key, "filters_max")) {
            ok = zcbor_uint32_decode(zsd, &view.filters_max);
        } else if(KeyIs(key, "ring_bytes")) {
            ok = zcbor_uint32_decode(zsd, &view.ring_bytes);
        } else if(KeyIs(key, "notify_max")) {
            ok = zcbor_uint32_decode(zsd, &view.notify_max);
        } else if(KeyIs(key, "tx")) {
            ok = zcbor_bool_decode(zsd, &view.has_tx);
        } else if(KeyIs(key, "synthetic")) {
            ok = zcbor_bool_decode(zsd, &view.has_synthetic);
        } else if(KeyIs(key, "delta")) {
            ok = zcbor_bool_decode(zsd, &view.has_delta);
        } else {
            ok = zcbor_any_skip(zsd, nullptr);
        }

        if(!ok)
            return std::nullopt;
    }

    if(!zcbor_map_end_decode(zsd))
        return std::nullopt;

    return view;
}

/** @brief Sends a read to MCUmgr as a client on a test transport and returns the body of the answer. */
class RawSmpClient {
private:
    static inline RawSmpClient* instance = nullptr;

    smp_transport transport_{};
    k_sem received_{};
    k_mutex lock_{};
    std::vector<uint8_t> response_;
    uint8_t sequence_ = 0;

    static int Output(net_buf* packet) {
        RawSmpClient* client = instance;
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
    RawSmpClient() {
        k_sem_init(&received_, 0, 1);
        k_mutex_init(&lock_);
        transport_.functions.output = Output;
        zassert_equal(smp_transport_init(&transport_), 0);
        instance = this;
    }

    RawSmpClient(const RawSmpClient&) = delete;
    RawSmpClient& operator=(const RawSmpClient&) = delete;
    RawSmpClient(RawSmpClient&&) = delete;
    RawSmpClient& operator=(RawSmpClient&&) = delete;

    ~RawSmpClient() {
        k_work_sync sync{};
        k_work_cancel_sync(&transport_.work, &sync);
        instance = nullptr;
    }

    /** @return The CBOR body of the answer; nothing when none came or it is not the answer to this request. */
    std::optional<std::vector<uint8_t>>
    Read(SmpGroupId group, uint8_t command, const std::vector<uint8_t>& body = { 0xA0 }) {
        net_buf* packet = smp_packet_alloc();
        zassert_not_null(packet, "SMP pool exhausted");

        const SmpHeader header{
            .operation = SmpOperation::READ,
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
        const std::vector<uint8_t> raw = response_;
        k_mutex_unlock(&lock_);

        const auto response_header = SmpHeader::Parse(raw);
        if(!response_header.has_value() || response_header->sequence != header.sequence)
            return std::nullopt;

        return std::vector<uint8_t>(raw.begin() + SmpHeader::SIZE, raw.end());
    }
};

} // namespace can_monitor_test
