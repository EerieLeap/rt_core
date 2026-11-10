#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <zcbor_encode.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "domain/can_monitor_domain/services/can_monitor_service.h"
#include "domain/can_monitor_domain/utilities/can_record_reader.h"
#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"
#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "subsys/bluetooth/i_ble_notifier.h"
#include "subsys/canbus/canbus_time.h"
#include "subsys/device_tree/dt_canbus.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "utilities/memory/memory_resource_manager.h"

namespace can_monitor_test {

using namespace eerie_leap::domain::can_monitor_domain::models;
using namespace eerie_leap::domain::can_monitor_domain::services;
using namespace eerie_leap::domain::can_monitor_domain::utilities;

using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::domain::canbus_domain::models::CanChannelConfiguration;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::subsys::bluetooth::IBleNotifier;
using eerie_leap::subsys::canbus::NowUs;
using eerie_leap::subsys::device_tree::DtCanbus;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::utilities::memory::Mrm;

using Bytes = std::vector<uint8_t>;

inline constexpr size_t NOTIFY_MAX = 495;
inline constexpr uint8_t CONFIGURED_PORT = 0;
inline constexpr uint8_t UNCONFIGURED_PORT = 1;
inline constexpr uint32_t FLUSH_MS = CONFIG_EERIE_LEAP_CAN_MONITOR_FLUSH_MS;

inline const device* ConfiguredDevice() {
    return DEVICE_DT_GET(DT_ALIAS(can0));
}

/** @brief can1, which the stored configuration does not run. */
inline const device* UnconfiguredDevice() {
    return DEVICE_DT_GET(DT_ALIAS(can1));
}

/** @brief The loopback controller only hands frames back in loopback mode; Canbus starts it in normal mode. */
inline void SwitchToLoopbackMode(const device* controller) {
    zassert_ok(can_stop(controller));
    zassert_ok(can_set_mode(controller, CAN_MODE_LOOPBACK));
    zassert_ok(can_start(controller));
}

// A failing assertion skips the harness, so the controllers are stopped separately.
inline void ResetControllers(void*) {
    can_stop(ConfiguredDevice());
    can_stop(UnconfiguredDevice());
}

/** @brief The controller's counts, set by the test. */
class StubCounters : public ICanErrorCounters {
private:
    std::array<std::atomic<uint32_t>, 5> errors_{};
    std::atomic<uint32_t> rx_overruns_{ 0 };

public:
    [[nodiscard]] CanErrorCounts Read() const override {
        CanErrorCounts counts;
        for(size_t i = 0; i < errors_.size(); ++i)
            counts.errors[i] = errors_[i].load();
        counts.rx_overruns = rx_overruns_.load();

        return counts;
    }

    void AddError(size_t index, uint32_t count) {
        errors_[index] += count;
    }
    void AddOverruns(uint32_t count) {
        rx_overruns_ += count;
    }
};

/** @brief Sleeps a few ms at a time until @p predicate holds; the clock of native_sim only moves while a thread sleeps. */
template<typename Predicate>
bool WaitUntil(Predicate predicate, int timeout_ms = 3000) {
    for(int waited = 0; waited <= timeout_ms; waited += 5) {
        if(predicate())
            return true;

        k_sleep(K_MSEC(5));
    }

    return predicate();
}

struct SentNotification {
    Bytes data;
    int64_t time_ms = 0;
};

/**
 * @brief The `bus` characteristic of a central: keeps every notification, and refuses on demand.
 *
 * Completions are the test's to report (CanMonitorService::OnNotificationSent()), or on_notified's.
 */
class FakeBusNotifier : public IBleNotifier {
private:
    mutable k_mutex lock_{};
    size_t max_size_ = NOTIFY_MAX;
    int result_ = 0;
    int fail_next_result_ = 0;
    int fail_next_count_ = 0;
    std::vector<SentNotification> notifications_;
    uint32_t refused_ = 0;

public:
    /// Called after each accepted notification, outside the lock.
    std::function<void()> on_notified;

    FakeBusNotifier() {
        k_mutex_init(&lock_);
        notifications_.reserve(1024);
    }

    void SetMaxSize(size_t size) {
        k_mutex_lock(&lock_, K_FOREVER);
        max_size_ = size;
        k_mutex_unlock(&lock_);
    }

    /** @brief Every notify answers @p result (0 accepts). */
    void SetResult(int result) {
        k_mutex_lock(&lock_, K_FOREVER);
        result_ = result;
        k_mutex_unlock(&lock_);
    }

    /** @brief The next @p count notifies answer @p result, then it is back to SetResult()'s. */
    void FailNext(int result, int count) {
        k_mutex_lock(&lock_, K_FOREVER);
        fail_next_result_ = result;
        fail_next_count_ = count;
        k_mutex_unlock(&lock_);
    }

    [[nodiscard]] size_t GetMaxNotificationSize() const override {
        k_mutex_lock(&lock_, K_FOREVER);
        const size_t size = max_size_;
        k_mutex_unlock(&lock_);

        return size;
    }

    int Notify(std::span<const uint8_t> data) override {
        k_mutex_lock(&lock_, K_FOREVER);
        int result = result_;
        if(fail_next_count_ > 0) {
            --fail_next_count_;
            result = fail_next_result_;
        }
        if(result == 0)
            notifications_.push_back({ Bytes(data.begin(), data.end()), k_uptime_get() });
        else
            ++refused_;
        k_mutex_unlock(&lock_);

        if(result == 0 && on_notified)
            on_notified();

        return result;
    }

    [[nodiscard]] std::vector<SentNotification> Notifications() const {
        k_mutex_lock(&lock_, K_FOREVER);
        auto notifications = notifications_;
        k_mutex_unlock(&lock_);

        return notifications;
    }

    [[nodiscard]] std::optional<SentNotification> Last() const {
        k_mutex_lock(&lock_, K_FOREVER);
        std::optional<SentNotification> last;
        if(!notifications_.empty())
            last = notifications_.back();
        k_mutex_unlock(&lock_);

        return last;
    }

    [[nodiscard]] size_t Count() const {
        k_mutex_lock(&lock_, K_FOREVER);
        const size_t count = notifications_.size();
        k_mutex_unlock(&lock_);

        return count;
    }

    [[nodiscard]] uint32_t GetRefused() const {
        k_mutex_lock(&lock_, K_FOREVER);
        const uint32_t refused = refused_;
        k_mutex_unlock(&lock_);

        return refused;
    }
};

/** @brief A record read back from a notification, with no pointer into it. */
struct DecodedRecord {
    CanRecord record;
    /// The stream's time at this record.
    uint64_t time_us = 0;
    Bytes payload;
};

struct DecodedNotification {
    uint8_t version = 0;
    uint8_t port = 0;
    uint16_t sequence = 0;
    uint64_t base_us = 0;
    size_t size = 0;
    int64_t time_ms = 0;
    std::vector<DecodedRecord> records;

    /// The time of the last record, which the next notification's `base_us` must be.
    [[nodiscard]] uint64_t EndTimeUs() const {
        return records.empty() ? base_us : records.back().time_us;
    }
};

/** @brief Parses a whole notification; nothing when it isn't one: a wrong version, count or leftover bytes. */
inline std::optional<DecodedNotification> DecodeNotification(const SentNotification& sent) {
    const Bytes& raw = sent.data;
    if(raw.size() < CAN_NOTIFICATION_HEADER_SIZE || raw[0] != CAN_RECORD_VERSION)
        return std::nullopt;

    DecodedNotification notification;
    notification.version = raw[0];
    notification.port = raw[1];
    notification.sequence = sys_get_le16(&raw[2]);
    notification.base_us = sys_get_le64(&raw[4]);
    notification.size = raw.size();
    notification.time_ms = sent.time_ms;

    uint64_t time_us = notification.base_us;
    size_t offset = CAN_NOTIFICATION_HEADER_SIZE;
    for(size_t i = 0; i < raw[12]; ++i) {
        auto parsed = ParseRecord(std::span<const uint8_t>(raw).subspan(offset));
        if(!parsed)
            return std::nullopt;

        time_us += parsed->dt_us;
        offset += parsed->size;

        DecodedRecord decoded;
        decoded.payload.assign(parsed->payload.begin(), parsed->payload.end());
        decoded.record = *parsed;
        decoded.record.payload = {};
        decoded.record.masks = {};
        decoded.time_us = time_us;
        notification.records.push_back(std::move(decoded));
    }

    if(offset != raw.size())
        return std::nullopt;

    return notification;
}

/** @brief Every notification decoded; a malformed one fails the test. */
inline std::vector<DecodedNotification> DecodeAll(const std::vector<SentNotification>& sent) {
    std::vector<DecodedNotification> all;
    for(const SentNotification& notification : sent) {
        auto decoded = DecodeNotification(notification);
        zassert_true(decoded.has_value(), "A notification of %zu bytes does not decode", notification.data.size());
        all.push_back(std::move(*decoded));
    }

    return all;
}

/**
 * @brief Checks the chain a client relies on: `seq` from @p first_sequence in steps of one, and each
 * `base_us` the time of the last record of the notification before it.
 */
inline void ExpectChain(const std::vector<DecodedNotification>& notifications, uint16_t first_sequence = 0) {
    for(size_t i = 0; i < notifications.size(); ++i) {
        zassert_equal(
            notifications[i].sequence, static_cast<uint16_t>(first_sequence + i), "seq of notification %zu", i
        );

        if(i > 0)
            zassert_equal(notifications[i].base_us, notifications[i - 1].EndTimeUs(), "base_us of notification %zu", i);
    }
}

/** @brief The records of one kind, in order. */
inline std::vector<const DecodedRecord*>
RecordsOf(const std::vector<DecodedNotification>& notifications, CanRecordKind kind) {
    std::vector<const DecodedRecord*> records;
    for(const DecodedNotification& notification : notifications) {
        for(const DecodedRecord& record : notification.records) {
            if(record.record.kind == kind)
                records.push_back(&record);
        }
    }

    return records;
}

/** @brief The sum of the statistics records' values. */
inline CanStatistics SumStatistics(const std::vector<DecodedNotification>& notifications) {
    CanStatistics sum;
    for(const DecodedRecord* record : RecordsOf(notifications, CanRecordKind::STATISTICS)) {
        const CanStatistics& s = record->record.statistics;
        sum.frames_received += s.frames_received;
        sum.frames_sent += s.frames_sent;
        sum.busy_us += s.busy_us;
        sum.filtered += s.filtered;
        for(size_t i = 0; i < sum.errors.size(); ++i)
            sum.errors[i] += s.errors[i];
        for(size_t i = 0; i < sum.lost.size(); ++i)
            sum.lost[i] += s.lost[i];
    }

    return sum;
}

/**
 * @brief Walks a long stream without keeping it: calls @p visit(record, time_us) for every record in
 * order, and fails the test unless the stream is whole: `seq` from @p first_sequence in steps of one, each
 * `base_us` the time of the record before the notification's first, no leftover bytes.
 *
 * The payload span of the record points into the notification and is valid during the call.
 */
template<typename Visit>
void WalkStream(const std::vector<SentNotification>& sent, Visit&& visit, uint16_t first_sequence = 0) {
    uint64_t previous_end_us = 0;
    for(size_t i = 0; i < sent.size(); ++i) {
        const Bytes& raw = sent[i].data;
        zassert_true(raw.size() >= CAN_NOTIFICATION_HEADER_SIZE && raw[0] == CAN_RECORD_VERSION, "Notification %zu", i);
        zassert_equal(sys_get_le16(&raw[2]), static_cast<uint16_t>(first_sequence + i), "seq of notification %zu", i);

        uint64_t time_us = sys_get_le64(&raw[4]);
        if(i > 0)
            zassert_equal(time_us, previous_end_us, "base_us of notification %zu", i);

        size_t offset = CAN_NOTIFICATION_HEADER_SIZE;
        for(size_t n = 0; n < raw[12]; ++n) {
            const auto record = ParseRecord(std::span<const uint8_t>(raw).subspan(offset));
            zassert_true(record.has_value(), "Record %zu of notification %zu", n, i);

            time_us += record->dt_us;
            offset += record->size;
            visit(*record, time_us);
        }

        zassert_equal(offset, raw.size(), "Notification %zu has leftover bytes", i);
        previous_end_us = time_us;
    }
}

/** @brief The notifications of one port: each session numbers and chains its own, so a stream of two is two. */
inline std::vector<SentNotification> OnPort(const std::vector<SentNotification>& sent, uint8_t port) {
    std::vector<SentNotification> notifications;
    for(const SentNotification& notification : sent) {
        if(notification.data.size() > 1 && notification.data[1] == port)
            notifications.push_back(notification);
    }

    return notifications;
}

/** @brief What a stream holds, counted as it was walked. */
struct StreamSummary {
    size_t notifications = 0;
    size_t frames = 0;
    size_t gaps = 0;
    uint64_t gap_frames = 0;
    bool are_gaps_from_the_ring = true;
    CanStatistics statistics;
    std::vector<CanEndReason> ends;
    CanRecordKind last_kind = CanRecordKind::FRAME;
    size_t largest_notification = 0;
};

inline StreamSummary Summarize(const std::vector<SentNotification>& sent, uint16_t first_sequence = 0) {
    StreamSummary summary;
    summary.notifications = sent.size();
    for(const SentNotification& notification : sent)
        summary.largest_notification = std::max(summary.largest_notification, notification.data.size());

    WalkStream(
        sent,
        [&summary](const CanRecord& record, uint64_t) {
            summary.last_kind = record.kind;
            switch(record.kind) {
                case CanRecordKind::FRAME:
                    ++summary.frames;
                    break;
                case CanRecordKind::GAP:
                    ++summary.gaps;
                    summary.gap_frames += record.frames_lost;
                    summary.are_gaps_from_the_ring = summary.are_gaps_from_the_ring
                        && record.gap_cause == CanGapCause::RING;
                    break;
                case CanRecordKind::STATISTICS: {
                    const CanStatistics& s = record.statistics;
                    summary.statistics.frames_received += s.frames_received;
                    summary.statistics.frames_sent += s.frames_sent;
                    summary.statistics.busy_us += s.busy_us;
                    summary.statistics.filtered += s.filtered;
                    for(size_t i = 0; i < s.errors.size(); ++i)
                        summary.statistics.errors[i] += s.errors[i];
                    for(size_t i = 0; i < s.lost.size(); ++i)
                        summary.statistics.lost[i] += s.lost[i];
                    break;
                }
                case CanRecordKind::END:
                    summary.ends.push_back(record.end_reason);
                    break;
                default:
                    break;
            }
        },
        first_sequence
    );

    return summary;
}

/**
 * @brief The unit with the CAN monitor on top: the canbus service of a stored configuration that runs
 * can0 (can1 is not run), a fake link and the monitor service, running.
 */
class Harness {
public:
    std::shared_ptr<FakeBusNotifier> notifier = std::make_shared<FakeBusNotifier>();
    std::atomic<bool> is_smp_busy{ false };
    std::atomic<bool> is_auto_complete{ true };
    std::atomic<int> holds{ 0 };
    std::atomic<int> releases{ 0 };
    /// The error counts every session reads.
    std::shared_ptr<StubCounters> counters = std::make_shared<StubCounters>();
    std::shared_ptr<CanbusService> canbus_service;
    std::shared_ptr<CanMonitorService> service;

    Harness() {
        DtCanbus::Initialize();
        DtFs::InitInternalFs();
        const auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        zassert_true(fs_service->Format());

        const auto configuration_manager = std::make_shared<CanbusConfigurationManager>(
            std::make_unique<CborConfigurationService<CborCanbusConfig>>("canbus_config", fs_service), nullptr
        );

        CanbusConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
        CanChannelConfiguration channel(std::allocator_arg, Mrm::GetDefaultPmr());
        channel.type = eerie_leap::subsys::canbus::CanbusType::CLASSICAL_CAN;
        channel.bus_channel = CONFIGURED_PORT;
        channel.bitrate = 500000;
        configuration.channel_configurations.emplace(CONFIGURED_PORT, std::move(channel));
        zassert_true(configuration_manager->Update(configuration));

        canbus_service = std::make_shared<CanbusService>(DtCanbus::Get, configuration_manager);
        zassert_not_null(canbus_service->GetCanbus(CONFIGURED_PORT).get(), "can0 is run");
        zassert_is_null(canbus_service->GetCanbus(UNCONFIGURED_PORT).get(), "can1 is not");

        // The loopback controller hands no frame back in normal mode.
        const device* controller = ConfiguredDevice();
        zassert_ok(can_stop(controller));
        zassert_ok(can_set_mode(controller, CAN_MODE_LOOPBACK));
        zassert_ok(can_start(controller));

        CanMonitorLink link;
        link.notifier = notifier;
        link.hold_fast = [this] {
            holds++;
        };
        link.release_fast = [this] {
            releases++;
        };
        link.is_smp_busy = [this] {
            return is_smp_busy.load();
        };

        service = std::make_shared<CanMonitorService>(
            canbus_service, DtCanbus::Get, link, [this](const device*) -> std::shared_ptr<ICanErrorCounters> {
                return counters;
            }
        );
        zassert_true(service->Initialize());
        zassert_true(service->Start());

        notifier->on_notified = [this] {
            if(is_auto_complete)
                service->OnNotificationSent();
        };
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    ~Harness() {
        notifier->on_notified = nullptr;
        service.reset();
        canbus_service.reset();
        can_stop(ConfiguredDevice());
        can_stop(UnconfiguredDevice());
    }

    /** @brief Sleeps @p duration_ms, renewing the session's lease every half second. */
    void Sleep(uint32_t token, int duration_ms) {
        for(int slept = 0; slept < duration_ms; slept += 500) {
            service->GetStats(token);
            k_msleep(std::min(500, duration_ms - slept));
        }
    }

    [[nodiscard]] eerie_leap::subsys::canbus::Canbus* Bus() const {
        return canbus_service->GetCanbus(CONFIGURED_PORT)->Get();
    }

    /** @brief Opens a synthetic session; fails the test if it isn't opened. */
    CanMonitorOpenAnswer
    OpenSynthetic(uint32_t fps, uint32_t ids = 4, uint8_t dlc = 8, bool ext = false, bool fd = false) {
        CanMonitorOpenRequest request;
        request.synthetic = CanSyntheticParameters{ .fps = fps, .ids = ids, .dlc = dlc, .ext = ext, .fd = fd };

        const auto answer = service->Open(request);
        zassert_true(answer.has_value(), "Synthetic OPEN failed (%d)", answer ? 0 : static_cast<int>(answer.error()));

        return *answer;
    }

    /** @brief True once the stream has its end record, which is the last record there is. */
    [[nodiscard]] bool HasEnded() const {
        const auto last = notifier->Last();
        if(!last)
            return false;

        const auto decoded = DecodeNotification(*last);
        return decoded && !decoded->records.empty() && decoded->records.back().record.kind == CanRecordKind::END;
    }

    /** @brief Closes @p token and waits for the end record. */
    void CloseAndDrain(uint32_t token) {
        // The fake reports completions itself, unless a test turned that off.
        is_auto_complete = true;
        notifier->SetResult(0);

        zassert_true(service->Close(token).has_value());
        zassert_true(
            WaitUntil(
                [&] {
                    service->OnNotificationSent();
                    return HasEnded();
                },
                8000
            ),
            "The end record never came"
        );
    }
};

/** @brief A request body for OPEN: the keys the SMP test client's map builder can't write. */
class OpenBody {
private:
    std::array<uint8_t, 256> buffer_{};
    zcbor_state_t states_[5]{};

public:
    struct FilterSpec {
        uint32_t id;
        uint32_t mask;
        bool ext;
    };

    OpenBody() {
        zcbor_new_encode_state(states_, ZCBOR_ARRAY_SIZE(states_), buffer_.data(), buffer_.size(), 0);
        zcbor_map_start_encode(states_, 16);
    }

    OpenBody& Put(std::string_view key, uint32_t value) {
        zassert_true(zcbor_tstr_encode_ptr(states_, key.data(), key.size()) && zcbor_uint32_put(states_, value));
        return *this;
    }

    OpenBody& PutBool(std::string_view key, bool value) {
        zassert_true(zcbor_tstr_encode_ptr(states_, key.data(), key.size()) && zcbor_bool_put(states_, value));
        return *this;
    }

    OpenBody& PutFilters(std::span<const FilterSpec> filters) {
        bool ok = zcbor_tstr_put_lit(states_, "filters") && zcbor_list_start_encode(states_, filters.size());
        for(const FilterSpec& filter : filters) {
            ok = ok
                && zcbor_list_start_encode(states_, 3)
                && zcbor_uint32_put(states_, filter.id)
                && zcbor_uint32_put(states_, filter.mask)
                && zcbor_bool_put(states_, filter.ext)
                && zcbor_list_end_encode(states_, 3);
        }

        zassert_true(ok && zcbor_list_end_encode(states_, filters.size()));
        return *this;
    }

    OpenBody& PutSynthetic(uint32_t fps, uint32_t ids, uint32_t dlc, bool ext, bool fd) {
        zassert_true(
            zcbor_tstr_put_lit(states_, "synthetic")
            && zcbor_map_start_encode(states_, 5)
            && zcbor_tstr_put_lit(states_, "fps")
            && zcbor_uint32_put(states_, fps)
            && zcbor_tstr_put_lit(states_, "ids")
            && zcbor_uint32_put(states_, ids)
            && zcbor_tstr_put_lit(states_, "dlc")
            && zcbor_uint32_put(states_, dlc)
            && zcbor_tstr_put_lit(states_, "ext")
            && zcbor_bool_put(states_, ext)
            && zcbor_tstr_put_lit(states_, "fd")
            && zcbor_bool_put(states_, fd)
            && zcbor_map_end_encode(states_, 5)
        );
        return *this;
    }

    Bytes Build() {
        zassert_true(zcbor_map_end_encode(states_, 16));
        const auto size = static_cast<size_t>(states_[0].payload - buffer_.data());
        return { buffer_.begin(), buffer_.begin() + size };
    }
};

} // namespace can_monitor_test
