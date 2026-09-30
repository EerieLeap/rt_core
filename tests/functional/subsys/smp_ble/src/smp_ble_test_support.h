#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zcbor_decode.h>

#include "subsys/smp/smp_header.h"
#include "subsys/smp/smp_packet.h"
#include "subsys/smp/i_smp_forwarder.h"
#include "subsys/smp/ble/i_smp_ble_link.h"
#include "subsys/smp/ble/smp_ble_router.h"

namespace smp_ble_test {

using namespace eerie_leap::subsys::smp;
using namespace eerie_leap::subsys::smp::ble;

using Bytes = std::vector<uint8_t>;

inline constexpr int RESPONSE_TIMEOUT_MS = 2000;

inline Bytes MakePacket(SmpOperation operation, uint16_t group, uint8_t command_id, uint8_t sequence, const Bytes& body) {
    const SmpHeader header{
        .operation = operation,
        .version = 1,
        .length = static_cast<uint16_t>(body.size()),
        .group = group,
        .sequence = sequence,
        .command_id = command_id,
    };

    Bytes packet(SmpHeader::SIZE);
    header.Encode(std::span<uint8_t, SmpHeader::SIZE>(packet.data(), SmpHeader::SIZE));
    packet.insert(packet.end(), body.begin(), body.end());

    return packet;
}

inline Bytes CborText(std::string_view text) {
    Bytes bytes;
    if(text.size() < 24) {
        bytes.push_back(static_cast<uint8_t>(0x60 | text.size()));
    } else {
        bytes.push_back(0x78);
        bytes.push_back(static_cast<uint8_t>(text.size()));
    }
    bytes.insert(bytes.end(), text.begin(), text.end());

    return bytes;
}

// os group (0), echo (0): {"d": text}
inline Bytes MakeEchoRequest(std::string_view text, uint8_t sequence, SmpOperation operation = SmpOperation::WRITE) {
    Bytes body = {0xA1, 0x61, 'd'};
    auto value = CborText(text);
    body.insert(body.end(), value.begin(), value.end());

    return MakePacket(operation, 0, 0, sequence, body);
}

inline Bytes Routed(uint8_t route, const Bytes& packet) {
    Bytes routed = {route};
    routed.insert(routed.end(), packet.begin(), packet.end());

    return routed;
}

inline std::string LongText(size_t size) {
    std::string text(size, ' ');
    for(size_t i = 0; i < size; i++)
        text[i] = static_cast<char>('a' + i % 26);

    return text;
}

inline SmpPacket ToSmpPacket(const Bytes& bytes) {
    SmpPacket packet = AllocateSmpPacket();
    zassert_not_null(packet.get(), "SMP pool exhausted");
    net_buf_add_mem(packet.get(), bytes.data(), bytes.size());

    return packet;
}

struct RoutedPacket {
    uint8_t route;
    SmpHeader header;
    Bytes packet;

    [[nodiscard]] std::span<const uint8_t> Body() const {
        return std::span(packet).subspan(SmpHeader::SIZE);
    }
};

// Cuts a notification stream into its [route][SMP packet] units.
inline std::vector<RoutedPacket> SplitStream(const Bytes& stream) {
    std::vector<RoutedPacket> packets;

    size_t offset = 0;
    while(stream.size() - offset >= 1 + SmpHeader::SIZE) {
        const auto header = SmpHeader::Parse(std::span(stream).subspan(offset + 1));
        const size_t size = header->GetPacketSize();
        if(stream.size() - offset < 1 + size)
            break;

        packets.push_back({
            .route = stream[offset],
            .header = *header,
            .packet = Bytes(stream.begin() + offset + 1, stream.begin() + offset + 1 + size),
        });
        offset += 1 + size;
    }

    return packets;
}

// Reads a text or unsigned value of a flat CBOR map.
inline std::optional<std::string> FindText(std::span<const uint8_t> body, std::string_view key) {
    ZCBOR_STATE_D(zsd, 2, body.data(), body.size(), 1, 0);
    if(!zcbor_map_start_decode(zsd))
        return std::nullopt;

    while(!zcbor_array_at_end(zsd)) {
        zcbor_string name{};
        if(!zcbor_tstr_decode(zsd, &name))
            return std::nullopt;

        zcbor_string value{};
        if(std::string_view(reinterpret_cast<const char*>(name.value), name.len) == key && zcbor_tstr_decode(zsd, &value))
            return std::string(reinterpret_cast<const char*>(value.value), value.len);

        if(!zcbor_any_skip(zsd, nullptr))
            return std::nullopt;
    }

    return std::nullopt;
}

inline std::optional<int32_t> FindInt(std::span<const uint8_t> body, std::string_view key) {
    ZCBOR_STATE_D(zsd, 2, body.data(), body.size(), 1, 0);
    if(!zcbor_map_start_decode(zsd))
        return std::nullopt;

    while(!zcbor_array_at_end(zsd)) {
        zcbor_string name{};
        if(!zcbor_tstr_decode(zsd, &name))
            return std::nullopt;

        int32_t value = 0;
        if(std::string_view(reinterpret_cast<const char*>(name.value), name.len) == key && zcbor_int32_decode(zsd, &value))
            return value;

        if(!zcbor_any_skip(zsd, nullptr))
            return std::nullopt;
    }

    return std::nullopt;
}

inline void ExpectEchoResponse(const RoutedPacket& response, uint8_t route, std::string_view text, uint8_t sequence) {
    zassert_equal(response.route, route);
    zassert_equal(response.header.operation, SmpOperation::WRITE_RESPONSE);
    zassert_equal(response.header.group, 0);
    zassert_equal(response.header.command_id, 0);
    zassert_equal(response.header.sequence, sequence);
    zassert_true(FindText(response.Body(), "r") == text, "Echo text differs");
}

// The central: records notifications, and runs out of buffers on demand.
class FakeLink : public ISmpBleLink {
private:
    mutable k_mutex lock_{};
    size_t max_size_ = 20;
    int budget_ = -1;
    Bytes stream_;
    std::vector<size_t> sizes_;

public:
    FakeLink() { k_mutex_init(&lock_); }

    void SetMaxSize(size_t size) {
        k_mutex_lock(&lock_, K_FOREVER);
        max_size_ = size;
        k_mutex_unlock(&lock_);
    }

    // Notifications accepted before -ENOMEM; negative for no limit.
    void SetBudget(int budget) {
        k_mutex_lock(&lock_, K_FOREVER);
        budget_ = budget;
        k_mutex_unlock(&lock_);
    }

    [[nodiscard]] size_t GetMaxNotificationSize() const override {
        k_mutex_lock(&lock_, K_FOREVER);
        const size_t size = max_size_;
        k_mutex_unlock(&lock_);

        return size;
    }

    int Notify(std::span<const uint8_t> fragment) override {
        k_mutex_lock(&lock_, K_FOREVER);

        if(budget_ == 0) {
            k_mutex_unlock(&lock_);
            return -ENOMEM;
        }
        if(budget_ > 0)
            budget_--;

        stream_.insert(stream_.end(), fragment.begin(), fragment.end());
        sizes_.push_back(fragment.size());
        k_mutex_unlock(&lock_);

        return 0;
    }

    [[nodiscard]] Bytes Stream() const {
        k_mutex_lock(&lock_, K_FOREVER);
        Bytes stream = stream_;
        k_mutex_unlock(&lock_);

        return stream;
    }

    [[nodiscard]] std::vector<size_t> Sizes() const {
        k_mutex_lock(&lock_, K_FOREVER);
        auto sizes = sizes_;
        k_mutex_unlock(&lock_);

        return sizes;
    }

    std::vector<RoutedPacket> WaitForPackets(size_t count) const {
        for(int waited_ms = 0; waited_ms < RESPONSE_TIMEOUT_MS; waited_ms += 10) {
            auto packets = SplitStream(Stream());
            if(packets.size() >= count)
                return packets;

            k_msleep(10);
        }

        return SplitStream(Stream());
    }
};

class FakeForwarder : public ISmpForwarder {
public:
    uint8_t address = 0;
    std::set<uint8_t> reachable;
    bool accept = true;
    std::vector<std::pair<uint8_t, Bytes>> forwarded;
    std::shared_ptr<ISmpResponseSink> response_sink;

    [[nodiscard]] uint8_t GetAddress() const override { return address; }
    [[nodiscard]] bool IsReachable(uint8_t target) const override { return reachable.contains(target); }

    bool Forward(uint8_t target, SmpPacket packet) override {
        if(!accept)
            return false;

        forwarded.emplace_back(target, Bytes(packet->data, packet->data + packet->len));
        return true;
    }

    void SetResponseSink(std::shared_ptr<ISmpResponseSink> sink) override { response_sink = std::move(sink); }
};

struct RouterHarness {
    std::shared_ptr<FakeLink> link = std::make_shared<FakeLink>();
    std::shared_ptr<FakeForwarder> forwarder;
    std::shared_ptr<SmpBleRouter> router;

    explicit RouterHarness(bool has_forwarder = true) {
        if(has_forwarder)
            forwarder = std::make_shared<FakeForwarder>();

        router = std::make_shared<SmpBleRouter>(link, forwarder);
        zassert_true(router->Initialize());
    }
};

// All pool buffers are back once nothing holds a packet any more.
inline size_t CountFreePackets() {
    std::vector<SmpPacket> packets;
    while(auto packet = AllocateSmpPacket())
        packets.push_back(std::move(packet));

    return packets.size();
}

} // namespace smp_ble_test
