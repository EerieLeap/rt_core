#include <cstring>
#include <stdexcept>
#include <span>
#include <algorithm>
#include <memory>
#include <string>

#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"

#include "canbus.h"

LOG_MODULE_REGISTER(canbus_logger, CONFIG_EERIE_LEAP_CANBUS_LOG_LEVEL);

namespace eerie_leap::subsys::canbus {

using eerie_leap::subsys::threading::ScopedMutex;

namespace {

// NOTE: Borrowed from zephyr/drivers/can/can_common.c
uint16_t SamplePointForBitrate(uint32_t bitrate) {
    if(bitrate > 800000)
        return 750;  // 75.0%

    if(bitrate > 500000)
        return 800;  // 80.0%

    return 875;  // 87.5%
}

void PrintCanLimitsDetails(uint32_t bitrate, int ret) {
    if(ret >= 0 && ret <= CONFIG_CAN_SAMPLE_POINT_MARGIN)
        LOG_INF("  %u bps: OK (sample point error: %d).", bitrate, ret);
    else if(ret == -EINVAL)
        LOG_ERR("  %u bps: INVALID.", bitrate);
    else if(ret == -ENOTSUP)
        LOG_ERR("  %u bps: NOT SUPPORTED.", bitrate);
    else
        LOG_ERR("  %u bps: SAMPLE POINT OUT OF RANGE (error: %d).", bitrate, ret);
}

} // namespace

Canbus::Canbus(const CanbusConfig& config)
        : config_(config) {

    if(config_.canbus_dev == nullptr)
        throw std::invalid_argument("CAN device must not be null.");

    if(config_.type == CanbusType::CANFD && config_.data_bitrate == 0)
        config_.data_bitrate = config_.bitrate;

    k_mutex_init(&lock_);
    k_msgq_init(
        &frame_msgq_,
        frame_msgq_buffer_,
        sizeof(can_frame),
        FRAME_MSGQ_SIZE);

    thread_ = std::make_unique<Thread>(
        "can_" + std::string(config_.canbus_dev->name),
        this,
        thread_stack_size_,
        thread_priority_,
        thread_is_cooperative_);
}

Canbus::~Canbus() {
    if(GetState() != ServiceState::STOPPED) {
        Stop();
        return;
    }

    // Filters outlive a failed Configure(); the driver would otherwise keep calling back into a destroyed object.
    StopActivityMonitoring();

    if(thread_ && thread_->IsRunning())
        thread_->Join();

    ScopedMutex guard(lock_);
    RemoveAllFilters();
}

bool Canbus::DoInitialize() {
    if(!thread_->Initialize()) {
        LOG_ERR("Failed to allocate CAN bus thread stack.");
        return false;
    }

    k_msgq_purge(&frame_msgq_);

    {
        ScopedMutex guard(lock_);

        RemoveAllFilters();
        ClearEntries();
    }

    atomic_clear(&bitrate_detected_);
    atomic_clear(&rx_dropped_);

    if(!Configure(config_)) {
        LOG_ERR("Failed to configure CAN bus during initialization.");
        return false;
    }

    LOG_INF("CANBus initialized and configured successfully.");
    return true;
}

bool Canbus::ApplyMode(can_mode_t extra_modes) {
    can_mode_t capabilities = 0;
    int ret = can_get_capabilities(config_.canbus_dev, &capabilities);
    if(ret != 0) {
        LOG_ERR("Failed to get capabilities [%d].", ret);
        return false;
    }

    can_mode_t can_mode = CAN_MODE_NORMAL;
    if(config_.type == CanbusType::CANFD) {
        if((capabilities & CAN_MODE_FD) != 0) {
            can_mode |= CAN_MODE_FD;
        } else {
            LOG_WRN("Controller does not support CAN FD, falling back to classical CAN.");
            config_.type = CanbusType::CLASSICAL_CAN;
            config_.data_bitrate = 0;
        }
    }

    if((extra_modes & ~capabilities) != 0) {
        LOG_WRN("Requested modes 0x%08x are not supported (capabilities 0x%08x).",
            extra_modes & ~capabilities, capabilities);
        extra_modes &= capabilities;
    }

    can_mode |= extra_modes;

    ret = can_set_mode(config_.canbus_dev, can_mode);
    if(ret != 0) {
        LOG_ERR("Failed to set mode 0x%08x [%d].", can_mode, ret);
        return false;
    }

    LOG_INF("CAN mode set to 0x%08x.", can_mode);
    return true;
}

bool Canbus::Configure(const CanbusConfig& config) {
    if(GetState() != ServiceState::STOPPED) {
        LOG_ERR("Cannot configure CAN bus when not in STOPPED state.");
        return false;
    }

    if(!device_is_ready(config.canbus_dev)) {
        LOG_ERR("CAN device is not ready.");
        return false;
    }

    if(!IsBitrateSupported(config.type, config.bitrate)) {
        LOG_ERR("Bitrate %u is not supported for CAN type %u.",
            config.bitrate, static_cast<unsigned>(config.type));
        return false;
    }

    if(config.type == CanbusType::CANFD && config.data_bitrate > 0 &&
       !IsBitrateSupported(config.type, config.data_bitrate)) {
        LOG_ERR("Data bitrate %u is not supported for CAN FD.", config.data_bitrate);
        return false;
    }

    ScopedMutex guard(lock_);

    // A previous run may still hold filters bound to the old device.
    RemoveAllFilters();

    CanbusConfig previous_config = config_;
    config_ = config;

    if(config_.type == CanbusType::CANFD && config_.data_bitrate == 0)
        config_.data_bitrate = config_.bitrate;

    auto rollback = [&]() {
        config_ = previous_config;
        return false;
    };

    if(!ApplyMode(config_.extra_modes))
        return rollback();

    can_set_state_change_callback(config_.canbus_dev, BusStateChangedCallback, this);

    atomic_clear(&bitrate_detected_);

    if(config_.bitrate == 0) {
        LOG_INF("Auto-bitrate mode enabled - will detect on bus activity.");
        if(!StartActivityMonitoring()) {
            LOG_ERR("Failed to start activity monitoring.");
            return rollback();
        }
    } else {
        if(!SetTiming(config_.bitrate)) {
            PrintCanLimits();
            return rollback();
        }

        if(config_.type == CanbusType::CANFD && !SetDataTiming(config_.data_bitrate)) {
            PrintCanFdLimits();
            return rollback();
        }

        int ret = can_start(config_.canbus_dev);
        if(ret != 0) {
            LOG_ERR("Failed to start device [%d].", ret);
            return rollback();
        }

        atomic_set(&bitrate_detected_, 1);
        LOG_INF("Bitrate set to: %u.", config_.bitrate);
    }

    atomic_set(&is_initialized_, 1);
    LOG_INF("CANBus configured successfully.");
    return true;
}

bool Canbus::SetTiming(uint32_t bitrate) const {
    if(bitrate == 0)
        return false;

    int ret = can_set_bitrate(config_.canbus_dev, bitrate);
    if(ret != 0) {
        LOG_ERR("Failed to set bitrate %u [%d].", bitrate, ret);
        return false;
    }

    return true;
}

bool Canbus::SetDataTiming(uint32_t bitrate) const {
    if(bitrate == 0)
        return false;

    if(config_.type != CanbusType::CANFD)
        return false;

    int ret = can_set_bitrate_data(config_.canbus_dev, bitrate);
    if(ret != 0) {
        LOG_ERR("Failed to set CANFD bitrate %u [%d].", bitrate, ret);
        return false;
    }

    return true;
}

uint32_t Canbus::GetMaxDataLength(CanbusType type) {
    return type == CanbusType::CANFD ? CAN_FRAME_MAX_DATA_LENGTH : 8U;
}

int Canbus::SendFrame(const CanId& frame_id, std::span<const uint8_t> frame_data, k_timeout_t timeout) const {
    if(atomic_get(&is_initialized_) == 0 || !IsBitrateDetected() || GetState() != ServiceState::RUNNING)
        return -ENETDOWN;

    if(!frame_id.IsValid()) {
        LOG_ERR("Frame ID 0x%08X does not fit a %s identifier.",
            frame_id.id, frame_id.is_extended ? "29-bit" : "11-bit");
        return -EINVAL;
    }

    // can_send() may wait for a TX slot, so the lock RX dispatch needs is not held across it.
    const device* canbus_dev = nullptr;
    CanbusType type = CanbusType::CLASSICAL_CAN;
    {
        ScopedMutex guard(lock_);

        canbus_dev = config_.canbus_dev;
        type = config_.type;
    }

    const uint32_t max_data_length = std::min<uint32_t>(GetMaxDataLength(type), CAN_MAX_DLEN);
    if(frame_data.size() > max_data_length) {
        LOG_ERR("Frame payload of %zu bytes exceeds the %u byte limit.", frame_data.size(), max_data_length);
        return -EINVAL;
    }

    uint8_t flags = 0;

    if(type == CanbusType::CANFD)
        flags |= CAN_FRAME_FDF | CAN_FRAME_BRS;

    if(frame_id.is_extended)
        flags |= CAN_FRAME_IDE;

    const uint8_t dlc = can_bytes_to_dlc(static_cast<uint8_t>(frame_data.size()));

    struct can_frame can_frame = {
        .id = frame_id.id,
        .dlc = dlc,
        .flags = flags,
    };
    // The DLC table rounds FD lengths up, so the padding bytes stay zeroed.
    std::copy(frame_data.begin(), frame_data.end(), std::begin(can_frame.data));

    int res = can_send(canbus_dev, &can_frame, timeout, SendFrameCallback, nullptr);
    if(res != 0) {
        LOG_DBG("Failed to send frame [%d].", res);
        return res;
    }

    LOG_DBG("Frame sent: ID=0x%08X, DLC=%d", frame_id.id, dlc);
    return 0;
}

void Canbus::SendFrameCallback(const device* dev, int error, void* user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    if(error != 0)
        LOG_ERR("SendFrameCallback error: %d", error);
}

void Canbus::CanFrameReceivedCallback(const device *dev, can_frame *frame, void *user_data) {
    ARG_UNUSED(dev);

    auto* canbus = static_cast<Canbus*>(user_data);

    // Runs in ISR context: only the message queue may be touched here, the
    // handler maps are owned by the bottom half thread and its mutex.
    if(k_msgq_put(&canbus->frame_msgq_, frame, K_NO_WAIT) != 0)
        atomic_inc(&canbus->rx_dropped_);
}

void Canbus::BusStateChangedCallback(const device *dev, can_state state, can_bus_err_cnt err_cnt, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    LOG_WRN("CAN bus state changed to %d (tx errors: %u, rx errors: %u).",
        static_cast<int>(state), err_cnt.tx_err_cnt, err_cnt.rx_err_cnt);
}

int Canbus::RegisterFrameReceivedHandler(const CanFilter& requested_filter, CanFrameHandler handler) {
    if(atomic_get(&is_initialized_) == 0) {
        LOG_ERR("CANBus is not initialized.");
        return ERR_NOT_INITIALIZED;
    }

    if(!handler) {
        LOG_ERR("CAN frame handler must not be empty.");
        return ERR_INVALID_ARGUMENT;
    }

    if(!requested_filter.IsValid()) {
        LOG_ERR("CAN filter 0x%08X/0x%08X does not fit a %s identifier.",
            requested_filter.id, requested_filter.mask, requested_filter.is_extended ? "29-bit" : "11-bit");
        return ERR_INVALID_ARGUMENT;
    }

    const CanFilter filter = requested_filter.Normalized();

    ScopedMutex guard(lock_);

    FilterEntry* entry = FindEntry(filter);
    if(entry == nullptr) {
        if(HasOverlap(filter)) {
            LOG_ERR("CAN filter 0x%08X/0x%08X overlaps a registered filter.", filter.id, filter.mask);
            return ERR_FILTER_OVERLAP;
        }

        entry = CreateEntry(filter);
        if(entry == nullptr) {
            LOG_ERR("All %zu mask filters are in use.", MAX_MASK_FILTERS);
            return ERR_TOO_MANY_FILTERS;
        }
    }

    if(entry->handlers.size() >= MAX_HANDLERS_PER_FILTER) {
        LOG_ERR("CAN filter 0x%08X/0x%08X already has the maximum number of handlers.", filter.id, filter.mask);
        return ERR_TOO_MANY_HANDLERS;
    }

    const int handler_id = next_handler_id_++;
    entry->handlers.emplace(handler_id, std::move(handler));

    // Hardware filters are installed once the bitrate is known.
    if(atomic_get(&auto_detect_running_) != 0 && !IsBitrateDetected()) {
        LOG_DBG("Frame received handler deferred for 0x%08X/0x%08X pending bitrate detection.",
            filter.id, filter.mask);
        return handler_id;
    }

    if(!InstallFilter(*entry)) {
        entry->handlers.erase(handler_id);
        if(entry->handlers.empty())
            EraseEntry(filter);

        return ERR_FILTER_REJECTED;
    }

    LOG_DBG("Frame received handler registered for 0x%08X/0x%08X", filter.id, filter.mask);

    return handler_id;
}

Canbus::FilterEntry* Canbus::FindEntry(const CanFilter& filter) {
    if(filter.IsExact()) {
        auto it = exact_entries_.find(CanId{filter.id, filter.is_extended}.Key());
        return it != exact_entries_.end() ? &it->second : nullptr;
    }

    for(auto& entry : mask_entries_) {
        if(entry.has_value() && entry->filter == filter)
            return &entry.value();
    }

    return nullptr;
}

Canbus::FilterEntry* Canbus::FindEntryByHandler(int handler_id) {
    for(auto& [_, entry] : exact_entries_) {
        if(entry.handlers.contains(handler_id))
            return &entry;
    }

    for(auto& entry : mask_entries_) {
        if(entry.has_value() && entry->handlers.contains(handler_id))
            return &entry.value();
    }

    return nullptr;
}

Canbus::FilterEntry* Canbus::FindMatchingEntry(const CanId& can_id) {
    auto it = exact_entries_.find(can_id.Key());
    if(it != exact_entries_.end())
        return &it->second;

    for(auto& entry : mask_entries_) {
        if(entry.has_value() && entry->filter.Matches(can_id))
            return &entry.value();
    }

    return nullptr;
}

Canbus::FilterEntry* Canbus::CreateEntry(const CanFilter& filter) {
    if(filter.IsExact()) {
        auto [it, _] = exact_entries_.try_emplace(CanId{filter.id, filter.is_extended}.Key(), FilterEntry{filter});
        return &it->second;
    }

    for(auto& entry : mask_entries_) {
        if(!entry.has_value())
            return &entry.emplace(FilterEntry{filter});
    }

    return nullptr;
}

void Canbus::EraseEntry(const CanFilter& filter) {
    if(filter.IsExact()) {
        exact_entries_.erase(CanId{filter.id, filter.is_extended}.Key());
        return;
    }

    for(auto& entry : mask_entries_) {
        if(entry.has_value() && entry->filter == filter)
            entry.reset();
    }
}

bool Canbus::HasOverlap(const CanFilter& filter) const {
    for(const auto& [_, entry] : exact_entries_) {
        if(entry.filter.Overlaps(filter))
            return true;
    }

    for(const auto& entry : mask_entries_) {
        if(entry.has_value() && entry->filter.Overlaps(filter))
            return true;
    }

    return false;
}

bool Canbus::InstallFilter(FilterEntry& entry) {
    if(entry.filter_id >= 0)
        return true;

    const can_filter filter = {
        .id = entry.filter.id,
        .mask = entry.filter.mask,
        .flags = static_cast<uint8_t>(entry.filter.is_extended ? CAN_FILTER_IDE : 0)
    };

    int filter_id = can_add_rx_filter(config_.canbus_dev, CanFrameReceivedCallback, this, &filter);
    if(filter_id < 0) {
        LOG_ERR("Unable to add rx filter [%d].", filter_id);
        return false;
    }

    entry.filter_id = filter_id;

    return true;
}

void Canbus::UninstallFilter(FilterEntry& entry) {
    if(entry.filter_id < 0)
        return;

    can_remove_rx_filter(config_.canbus_dev, entry.filter_id);
    entry.filter_id = -1;
}

void Canbus::InstallDeferredFilters() {
    for(auto& [_, entry] : exact_entries_)
        InstallFilter(entry);

    for(auto& entry : mask_entries_) {
        if(entry.has_value())
            InstallFilter(entry.value());
    }
}

void Canbus::RemoveAllFilters() {
    for(auto& [_, entry] : exact_entries_)
        UninstallFilter(entry);

    for(auto& entry : mask_entries_) {
        if(entry.has_value())
            UninstallFilter(entry.value());
    }
}

void Canbus::ClearEntries() {
    exact_entries_.clear();

    for(auto& entry : mask_entries_)
        entry.reset();
}

bool Canbus::RemoveFrameReceivedHandler(int handler_id) {
    if(atomic_get(&is_initialized_) == 0) {
        LOG_ERR("CANBus is not initialized.");
        return false;
    }

    ScopedMutex guard(lock_);

    FilterEntry* entry = FindEntryByHandler(handler_id);
    if(entry == nullptr)
        return false;

    entry->handlers.erase(handler_id);
    if(!entry->handlers.empty())
        return true;

    // Copied because erasing the entry destroys its filter.
    const CanFilter filter = entry->filter;
    UninstallFilter(*entry);
    EraseEntry(filter);

    return true;
}

bool Canbus::StartActivityMonitoring() {
    atomic_set(&auto_detect_running_, 1);

    return true;
}

void Canbus::StopActivityMonitoring() {
    atomic_clear(&auto_detect_running_);
}

void Canbus::ThreadEntry() {
    LOG_INF("CANBus thread started.");

    while(thread_->IsRunning()) {
        if(atomic_get(&auto_detect_running_) != 0 && !IsBitrateDetected())
            BitrateAutodetectTask();

        ProcessFramesTask();
    }

    LOG_INF("CANBus thread stopped.");
}

void Canbus::BitrateAutodetectTask() {
    LOG_INF("CANBus auto-detection started.");

    while(thread_->IsRunning() && atomic_get(&auto_detect_running_) != 0 && !IsBitrateDetected()) {
        if(AutoDetectBitrate()) {
            BitrateDetectedCallback callback;
            uint32_t bitrate = 0;

            {
                ScopedMutex guard(lock_);

                bitrate = config_.bitrate;
                callback = bitrate_detected_fn_;

                InstallDeferredFilters();
            }

            LOG_INF("Bitrate successfully detected: %u bps", bitrate);

            if(callback)
                callback(bitrate);

            break;
        }

        k_sleep(K_MSEC(CONFIG_EERIE_LEAP_CANBUS_AUTO_DETECT_INTERVAL_MS));
    }

    StopActivityMonitoring();
    LOG_INF("CANBus auto-detection stopped.");
}

void Canbus::ProcessFramesTask() {
    can_frame raw_frame;
    if(k_msgq_get(&frame_msgq_, &raw_frame, K_MSEC(MSGQ_GET_TIMEOUT_MS)) != 0)
        return;

    const bool is_can_fd = (raw_frame.flags & CAN_FRAME_FDF) != 0;
    const bool is_remote_request = (raw_frame.flags & CAN_FRAME_RTR) != 0;

    CanFrame can_frame = {
        .id = raw_frame.id,
        .is_extended = (raw_frame.flags & CAN_FRAME_IDE) != 0,
        .is_transmit = false,
        .is_can_fd = is_can_fd,
        .is_bitrate_switch = (raw_frame.flags & CAN_FRAME_BRS) != 0,
        .is_remote_request = is_remote_request
    };

    // Classical frames never carry more than 8 bytes even though DLC 9..15 is
    // legal on the wire, and remote frames carry none at all.
    size_t frame_size = 0;
    if(!is_remote_request) {
        frame_size = can_dlc_to_bytes(raw_frame.dlc);
        frame_size = std::min<size_t>(frame_size, is_can_fd ? CAN_MAX_DLEN : 8U);
        frame_size = std::min<size_t>(frame_size, sizeof(raw_frame.data));
    }

    can_frame.data.Assign(std::span<const uint8_t>(raw_frame.data, frame_size));

    DispatchFrame(can_frame);
}

void Canbus::DispatchFrame(const CanFrame& frame) {
    const CanId can_id{frame.id, frame.is_extended};

    // Snapshot the ids so a handler may unregister itself while being invoked.
    std::array<int, MAX_HANDLERS_PER_FILTER> handler_ids{};
    size_t handler_count = 0;

    ScopedMutex guard(lock_);

    const FilterEntry* entry = FindMatchingEntry(can_id);
    if(entry == nullptr)
        return;

    for(const auto& [handler_id, _] : entry->handlers) {
        if(handler_count == handler_ids.size())
            break;

        handler_ids[handler_count++] = handler_id;
    }

    for(size_t i = 0; i < handler_count; i++) {
        FilterEntry* current = FindMatchingEntry(can_id);
        if(current == nullptr)
            return;

        auto handler_it = current->handlers.find(handler_ids[i]);
        if(handler_it == current->handlers.end())
            continue;

        handler_it->second(frame);
    }
}

void Canbus::AutoDetectFrameCallback(const device *dev, can_frame *frame, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(frame);

    atomic_inc(static_cast<atomic_t*>(user_data));
}

bool Canbus::AutoDetectBitrate() {
    std::span<const uint32_t> supported_bitrates = config_.type == CanbusType::CANFD
        ? std::span<const uint32_t>(canfd_supported_bitrates_)
        : std::span<const uint32_t>(classical_can_supported_bitrates_);

    for(uint32_t bitrate : supported_bitrates) {
        if(atomic_get(&auto_detect_running_) == 0 || !thread_->IsRunning()) {
            LOG_WRN("Bitrate detection stopped by user");
            return false;
        }

        if(TestBitrate(bitrate)) {
            ScopedMutex guard(lock_);

            atomic_set(&bitrate_detected_, 1);
            config_.bitrate = bitrate;

            if(config_.type == CanbusType::CANFD && config_.data_bitrate == 0)
                config_.data_bitrate = bitrate;

            return true;
        }

        k_sleep(K_MSEC(50));
    }

    return false;
}

bool Canbus::IsBitrateSupported(CanbusType type, uint32_t bitrate) {
    if(bitrate == 0)
        return true;

    if(type == CanbusType::CANFD)
        return std::ranges::find(canfd_supported_bitrates_, bitrate) != canfd_supported_bitrates_.end();

    return std::ranges::find(classical_can_supported_bitrates_, bitrate) != classical_can_supported_bitrates_.end();
}

bool Canbus::TestBitrate(uint32_t bitrate) {
    // Probing has to stay off the wire: a wrong bitrate in normal mode makes the
    // controller ACK at the wrong bit times and corrupt a live bus.
    if(!ApplyMode(config_.extra_modes | CAN_MODE_LISTENONLY))
        return false;

    if(!SetTiming(bitrate))
        return false;

    if(config_.type == CanbusType::CANFD) {
        uint32_t data_bitrate = config_.data_bitrate == 0 ? bitrate : config_.data_bitrate;
        if(!SetDataTiming(data_bitrate))
            return false;
    }

    int ret = can_start(config_.canbus_dev);
    if(ret != 0) {
        LOG_WRN("Failed to start CAN for bitrate %u [%d]", bitrate, ret);
        return false;
    }

    can_filter filter = {
        .id = 0,
        .mask = 0,  // Accept all IDs
        .flags = 0
    };

    atomic_clear(&auto_detect_frame_count_);

    int filter_id = can_add_rx_filter(config_.canbus_dev,
        AutoDetectFrameCallback,
        &auto_detect_frame_count_,
        &filter);

    if(filter_id < 0) {
        LOG_WRN("Failed to add test filter [%d]", filter_id);
        can_stop(config_.canbus_dev);

        return false;
    }

    k_sleep(K_MSEC(AUTO_DETECT_SAMPLE_MS));
    can_remove_rx_filter(config_.canbus_dev, filter_id);

    const auto received_frames = static_cast<uint32_t>(atomic_get(&auto_detect_frame_count_));

    bool detected = false;
    if(received_frames >= MIN_FRAMES_FOR_DETECTION) {
        can_state state = CAN_STATE_STOPPED;
        can_bus_err_cnt err_cnt = {};

        if(can_get_state(config_.canbus_dev, &state, &err_cnt) == 0) {
            // Valid activity means error-active state with reasonable error counts
            detected = state == CAN_STATE_ERROR_ACTIVE
                && err_cnt.tx_err_cnt < 128
                && err_cnt.rx_err_cnt < 128;
        }
    }

    ret = can_stop(config_.canbus_dev);
    if(ret != 0 && ret != -EALREADY)
        LOG_WRN("Failed to stop CAN after probing bitrate %u [%d]", bitrate, ret);

    if(!detected)
        return false;

    // Re-apply the requested mode and timing now that probing is done.
    if(!ApplyMode(config_.extra_modes) || !SetTiming(bitrate))
        return false;

    if(config_.type == CanbusType::CANFD && !SetDataTiming(config_.data_bitrate == 0 ? bitrate : config_.data_bitrate))
        return false;

    ret = can_start(config_.canbus_dev);
    if(ret != 0) {
        LOG_ERR("Failed to start CAN at detected bitrate %u [%d]", bitrate, ret);
        return false;
    }

    return true;
}

void Canbus::RegisterBitrateDetectedCallback(const BitrateDetectedCallback& callback) {
    ScopedMutex guard(lock_);

    bitrate_detected_fn_ = callback;
}

CanbusType Canbus::GetType() const {
    ScopedMutex guard(lock_);

    return config_.type;
}

CanbusConfig Canbus::GetConfig() const {
    ScopedMutex guard(lock_);

    return config_;
}

uint32_t Canbus::GetDetectedBitrate() const {
    ScopedMutex guard(lock_);

    return config_.bitrate;
}

void Canbus::PrintCanLimits() const {
    LOG_INF("Hardware CAN bitrate capabilities:");

    LOG_INF("CAN bitrate range: %u - %u bps.",
        can_get_bitrate_min(config_.canbus_dev),
        can_get_bitrate_max(config_.canbus_dev));

    auto bitrates = classical_can_supported_bitrates_;
    std::sort(bitrates.begin(), bitrates.end());

    for(auto bitrate : bitrates) {
        struct can_timing timing_data = {0};
        int ret = can_calc_timing(config_.canbus_dev, &timing_data, bitrate, SamplePointForBitrate(bitrate));

        PrintCanLimitsDetails(bitrate, ret);
    }
}

void Canbus::PrintCanFdLimits() const {
    LOG_INF("Hardware CAN FD data bitrate capabilities:");

    LOG_INF("CAN FD data bitrate range: %u - %u bps.",
        can_get_bitrate_min(config_.canbus_dev),
        can_get_bitrate_max(config_.canbus_dev));

    auto bitrates = canfd_supported_bitrates_;
    std::sort(bitrates.begin(), bitrates.end());

    for(auto bitrate : bitrates) {
        struct can_timing timing_data = {0};
        int ret = can_calc_timing_data(config_.canbus_dev, &timing_data, bitrate, SamplePointForBitrate(bitrate));

        PrintCanLimitsDetails(bitrate, ret);
    }
}

bool Canbus::DoStop() {
    StopActivityMonitoring();

   if(thread_ && thread_->IsRunning())
        thread_->Join();

    if(atomic_get(&is_initialized_) != 0) {
        int ret = can_stop(config_.canbus_dev);
        if(ret != 0 && ret != -EALREADY) {
            LOG_ERR("Failed to stop CAN device [%d].", ret);
            return false;
        }
    }

    {
        ScopedMutex guard(lock_);

        RemoveAllFilters();
        ClearEntries();
    }

    k_msgq_purge(&frame_msgq_);

    atomic_clear(&is_initialized_);
    atomic_clear(&bitrate_detected_);

    LOG_INF("CAN bus stopped successfully.");
    return true;
}

bool Canbus::DoStart() {
    if(atomic_get(&is_initialized_) == 0) {
        LOG_ERR("CAN bus must be initialized before starting. Call Initialize() first.");
        return false;
    }

    if(!thread_->Start()) {
        LOG_ERR("Failed to start CAN bus thread.");
        return false;
    }

    LOG_INF("CAN bus started successfully.");
    return true;
}

}  // namespace eerie_leap::subsys::canbus
