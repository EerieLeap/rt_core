#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

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
        return 750; // 75.0%

    if(bitrate > 500000)
        return 800; // 80.0%

    return 875; // 87.5%
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

// Classical frames never carry more than 8 bytes even though DLC 9..15 is legal on the wire, and
// remote frames carry none at all.
CanFrame ToCanFrame(const can_frame& raw_frame, uint64_t time_us, bool is_transmit) {
    const bool is_can_fd = (raw_frame.flags & CAN_FRAME_FDF) != 0;
    const bool is_remote_request = (raw_frame.flags & CAN_FRAME_RTR) != 0;

    CanFrame can_frame = { .id = raw_frame.id,
        .is_extended = (raw_frame.flags & CAN_FRAME_IDE) != 0,
        .is_transmit = is_transmit,
        .is_can_fd = is_can_fd,
        .is_bitrate_switch = (raw_frame.flags & CAN_FRAME_BRS) != 0,
        .is_remote_request = is_remote_request,
        .is_error_state_indicator = (raw_frame.flags & CAN_FRAME_ESI) != 0,
        .dlc = raw_frame.dlc,
        .time_us = time_us };

    size_t frame_size = 0;
    if(!is_remote_request) {
        frame_size = can_dlc_to_bytes(raw_frame.dlc);
        // NOLINTNEXTLINE(bugprone-branch-clone): CAN_MAX_DLEN is 8 too without CONFIG_CAN_FD_MODE
        frame_size = std::min<size_t>(frame_size, is_can_fd ? CAN_MAX_DLEN : 8U);
        frame_size = std::min<size_t>(frame_size, sizeof(raw_frame.data));
    }

    can_frame.data.Assign(std::span<const uint8_t>(raw_frame.data, frame_size));

    return can_frame;
}

} // namespace

Canbus::Canbus(const CanbusConfig& config) :
    config_(config) {

    if(config_.canbus_dev == nullptr)
        throw std::invalid_argument("CAN device must not be null.");

    if(config_.type == CanbusType::CANFD && config_.data_bitrate == 0)
        config_.data_bitrate = config_.bitrate;

    k_mutex_init(&lock_);
    k_msgq_init(&frame_msgq_, frame_msgq_buffer_, sizeof(CanbusRxEntry), FRAME_MSGQ_SIZE);
    state_change_node_.canbus = this;

    thread_ = std::make_unique<Thread>(
        "can_" + std::string(config_.canbus_dev->name), this, THREAD_STACK_SIZE, THREAD_PRIORITY, THREAD_IS_COOPERATIVE
    );
}

Canbus::~Canbus() {
    // A destructor is noexcept: an exception from Stop() or the mutex would end in std::terminate.
    try {
        if(GetState() != ServiceState::STOPPED) {
            Stop();
            UnregisterStateChangeCallback();
            return;
        }

        // Filters outlive a failed Configure(); the driver would otherwise keep calling back into a destroyed object.
        StopActivityMonitoring();

        if(thread_ && thread_->IsRunning())
            thread_->Join();

        // Same for the state change callback node: the driver's list must not point into this object.
        UnregisterStateChangeCallback();

        ScopedMutex guard(lock_);
        ClearTap();
        RemoveAllFilters();
    } catch(const std::exception& e) {
        LOG_ERR("CANBus teardown failed: %s", e.what());
    } catch(...) {
        LOG_ERR("CANBus teardown failed.");
    }
}

bool Canbus::DoInitialize() {
    if(!thread_->Initialize()) {
        LOG_ERR("Failed to allocate CAN bus thread stack.");
        return false;
    }

    k_msgq_purge(&frame_msgq_);

    {
        ScopedMutex guard(lock_);

        ClearTap();
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
        LOG_WRN(
            "Requested modes 0x%08x are not supported (capabilities 0x%08x).", extra_modes & ~capabilities, capabilities
        );
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
        LOG_ERR("Bitrate %u is not supported for CAN type %u.", config.bitrate, static_cast<unsigned>(config.type));
        return false;
    }

    if(config.type == CanbusType::CANFD
        && config.data_bitrate > 0
        && !IsBitrateSupported(config.type, config.data_bitrate)) {
        LOG_ERR("Data bitrate %u is not supported for CAN FD.", config.data_bitrate);
        return false;
    }

    ScopedMutex guard(lock_);

    // A previous run may still hold filters bound to the old device. A tap does not survive a
    // reconfiguration: its accept-all filters would have to follow the device and the filter swap.
    ClearTap();
    RemoveAllFilters();

    CanbusConfig previous_config = config_;
    config_ = config;

    if(config_.type == CanbusType::CANFD && config_.data_bitrate == 0)
        config_.data_bitrate = config_.bitrate;

    const auto rollback = [&] {
        config_ = previous_config;
        RegisterStateChangeCallback();
        return false;
    };

    if(!ApplyMode(config_.extra_modes))
        return rollback();

    RegisterStateChangeCallback();

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

        const int ret = can_start(config_.canbus_dev);
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

    const int ret = can_set_bitrate(config_.canbus_dev, bitrate);
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

    const int ret = can_set_bitrate_data(config_.canbus_dev, bitrate);
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

    const CanbusType type = GetType();
    const bool is_can_fd = type == CanbusType::CANFD;

    // CanFramePayload truncates what it cannot hold, which the caller must hear of instead.
    const uint32_t max_data_length = std::min<uint32_t>(GetMaxDataLength(type), CAN_MAX_DLEN);
    if(frame_data.size() > max_data_length) {
        LOG_ERR("Frame payload of %zu bytes exceeds the %u byte limit.", frame_data.size(), max_data_length);
        return -EINVAL;
    }

    CanFrame frame = {
        .id = frame_id.id,
        .is_extended = frame_id.is_extended,
        .is_can_fd = is_can_fd,
        .is_bitrate_switch = is_can_fd,
    };
    frame.data.Assign(frame_data);

    // The DLC table rounds FD lengths up; the padding bytes are zero.
    if(is_can_fd)
        frame.data.Resize(can_dlc_to_bytes(can_bytes_to_dlc(static_cast<uint8_t>(frame_data.size()))));

    return SendFrame(frame, timeout);
}

int Canbus::SendFrame(const CanFrame& frame, k_timeout_t timeout) const {
    if(atomic_get(&is_initialized_) == 0 || !IsBitrateDetected() || GetState() != ServiceState::RUNNING)
        return -ENETDOWN;

    // can_send() may wait for a TX slot, so the lock RX dispatch needs is not held across it.
    const device* canbus_dev = nullptr;
    CanbusType type = CanbusType::CLASSICAL_CAN;
    const TxSlot* slots = nullptr;
    {
        ScopedMutex guard(lock_);

        canbus_dev = config_.canbus_dev;
        type = config_.type;

        if(tap_ != nullptr)
            slots = tx_slots_.get();
    }

    const CanId frame_id = { frame.id, frame.is_extended };
    if(!frame_id.IsValid()) {
        LOG_DBG("Frame ID 0x%08X does not fit a %s identifier.", frame.id, frame.is_extended ? "29-bit" : "11-bit");
        return -EINVAL;
    }

    if(frame.is_can_fd && type != CanbusType::CANFD) {
        LOG_DBG("An FD frame does not go on a classical channel.");
        return -EINVAL;
    }

    if(frame.is_bitrate_switch && !frame.is_can_fd) {
        LOG_DBG("A bit rate switch needs an FD frame.");
        return -EINVAL;
    }

    const size_t size = frame.data.size();
    uint8_t dlc = 0;

    if(frame.is_remote_request) {
        // CAN FD has no remote frames, and a remote frame carries no data.
        if(frame.is_can_fd || size != 0 || frame.dlc > CAN_MAX_DLC) {
            LOG_DBG("Malformed remote frame (FD %d, %zu bytes, DLC %u).", frame.is_can_fd, size, frame.dlc);
            return -EINVAL;
        }

        dlc = frame.dlc;
    } else {
        const size_t max_data_length = frame.is_can_fd ? std::min(CAN_FRAME_MAX_DATA_LENGTH, sizeof(can_frame::data))
                                                       : static_cast<size_t>(CAN_MAX_DLC);
        if(size > max_data_length) {
            LOG_DBG("Frame payload of %zu bytes exceeds the %zu byte limit.", size, max_data_length);
            return -EINVAL;
        }

        dlc = can_bytes_to_dlc(static_cast<uint8_t>(size));
        if(can_dlc_to_bytes(dlc) != size) {
            LOG_DBG("No DLC gives %zu bytes.", size);
            return -EINVAL;
        }
    }

    uint8_t flags = 0;

    if(frame.is_can_fd)
        flags |= CAN_FRAME_FDF;

    if(frame.is_bitrate_switch)
        flags |= CAN_FRAME_BRS;

    if(frame.is_remote_request)
        flags |= CAN_FRAME_RTR;

    if(frame.is_extended)
        flags |= CAN_FRAME_IDE;

    struct can_frame can_frame = {
        .id = frame.id,
        .dlc = dlc,
        .flags = flags,
    };
    std::ranges::copy(frame.data, std::begin(can_frame.data));

    // The completion callback gets no frame, so a tapped bus keeps the frame in a slot until then.
    TxSlot* slot = nullptr;
    if(slots != nullptr) {
        slot = TakeTxSlot();
        if(slot == nullptr) {
            atomic_inc(&tx_echo_lost_);
        } else {
            slot->owner = this;
            slot->frame = can_frame;
        }
    }

    const int res = can_send(canbus_dev, &can_frame, timeout, SendFrameCallback, slot);
    if(res != 0) {
        // The callback does not run for a frame that was not accepted.
        if(slot != nullptr)
            ReleaseTxSlot(slot);

        LOG_DBG("Failed to send frame [%d].", res);
        return res;
    }

    LOG_DBG("Frame sent: ID=0x%08X, DLC=%d", frame.id, dlc);
    return 0;
}

void Canbus::SendFrameCallback(const device* dev, int error, void* user_data) {
    ARG_UNUSED(dev);

    if(error != 0)
        LOG_ERR("SendFrameCallback error: %d", error);

    // Driver context, like the receive callback: only the message queue may be touched here.
    auto* slot = static_cast<TxSlot*>(user_data);
    if(slot == nullptr)
        return;

    const Canbus* canbus = slot->owner;
    const CanbusRxEntry entry = CanbusRxEntry::TxEcho(slot->frame, NowUs(), error);

    if(k_msgq_put(&canbus->frame_msgq_, &entry, K_NO_WAIT) != 0)
        atomic_inc(&canbus->tx_echo_lost_);

    canbus->ReleaseTxSlot(slot);
}

Canbus::TxSlot* Canbus::TakeTxSlot() const {
    TxSlot* slots = tx_slots_.get();
    if(slots == nullptr)
        return nullptr;

    for(size_t i = 0; i < TX_SLOTS; i++) {
        if(!atomic_test_and_set_bit(tx_slots_used_, static_cast<int>(i)))
            return &slots[i];
    }

    return nullptr;
}

void Canbus::ReleaseTxSlot(TxSlot* slot) const {
    atomic_clear_bit(tx_slots_used_, static_cast<int>(slot - tx_slots_.get()));
}

void Canbus::ResetTxSlots() const {
    for(auto& word : tx_slots_used_)
        atomic_clear(&word);
}

void Canbus::CanFrameReceivedCallback(const device* dev, can_frame* frame, void* user_data) {
    ARG_UNUSED(dev);

    auto* canbus = static_cast<Canbus*>(user_data);

    // Runs in ISR context: only the message queue may be touched here, the
    // handler maps are owned by the bottom half thread and its mutex.
    const CanbusRxEntry entry = CanbusRxEntry::Received(*frame, NowUs());
    if(k_msgq_put(&canbus->frame_msgq_, &entry, K_NO_WAIT) != 0)
        atomic_inc(&canbus->rx_dropped_);
}

void Canbus::RegisterStateChangeCallback() {
    if(state_change_callback_dev_ == config_.canbus_dev)
        return;

    UnregisterStateChangeCallback();

    can_init_state_change_callback(&state_change_node_.callback, BusStateChangedCallback);
    const int err = can_add_state_change_callback(config_.canbus_dev, &state_change_node_.callback);
    if(err != 0) {
        LOG_WRN("Failed to register the CAN state change callback [%d].", err);
        return;
    }

    state_change_callback_dev_ = config_.canbus_dev;
}

void Canbus::UnregisterStateChangeCallback() {
    if(state_change_callback_dev_ == nullptr)
        return;

    can_remove_state_change_callback(state_change_callback_dev_, &state_change_node_.callback);
    state_change_callback_dev_ = nullptr;
}

void Canbus::BusStateChangedCallback(
    const device* dev,
    can_state_change_callback* callback,
    can_state state,
    can_bus_err_cnt err_cnt
) {
    ARG_UNUSED(dev);

    // The node is the first member of a StateChangeNode, which is all that was registered.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the callback carries no user data, see StateChangeNode
    const auto* node = reinterpret_cast<const StateChangeNode*>(callback);
    node->canbus->OnBusStateChanged(state, err_cnt);
}

void Canbus::OnBusStateChanged(can_state state, can_bus_err_cnt err_cnt) {
    LOG_WRN(
        "CAN bus state changed to %d (tx errors: %u, rx errors: %u).", static_cast<int>(state), err_cnt.tx_err_cnt,
        err_cnt.rx_err_cnt
    );

    // Driver context. A full queue drops the entry; the next change or the statistics catch up.
    const CanbusRxEntry entry = CanbusRxEntry::StateChange(state, err_cnt, NowUs());
    k_msgq_put(&frame_msgq_, &entry, K_NO_WAIT);
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
        LOG_ERR(
            "CAN filter 0x%08X/0x%08X does not fit a %s identifier.", requested_filter.id, requested_filter.mask,
            requested_filter.is_extended ? "29-bit" : "11-bit"
        );
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
    if(IsDetectingBitrate()) {
        LOG_DBG("Frame received handler deferred for 0x%08X/0x%08X pending bitrate detection.", filter.id, filter.mask);
        return handler_id;
    }

    // The tap's accept-all filters feed this handler through the software dispatch until the tap goes.
    if(tap_ != nullptr) {
        LOG_DBG("Frame received handler for 0x%08X/0x%08X served by the tap's filters.", filter.id, filter.mask);
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
        const auto it = exact_entries_.find(CanId{ filter.id, filter.is_extended }.Key());
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
    const auto it = exact_entries_.find(can_id.Key());
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
        auto [it, _] = exact_entries_.try_emplace(CanId{ filter.id, filter.is_extended }.Key(), FilterEntry{ filter });
        return &it->second;
    }

    for(auto& entry : mask_entries_) {
        if(!entry.has_value())
            return &entry.emplace(FilterEntry{ filter });
    }

    return nullptr;
}

void Canbus::EraseEntry(const CanFilter& filter) {
    if(filter.IsExact()) {
        exact_entries_.erase(CanId{ filter.id, filter.is_extended }.Key());
        return;
    }

    for(auto& entry : mask_entries_) {
        if(entry.has_value() && entry->filter == filter)
            entry.reset();
    }
}

bool Canbus::HasOverlap(const CanFilter& filter) const {
    if(std::ranges::any_of(exact_entries_, [&filter](const auto& item) {
           return item.second.filter.Overlaps(filter);
       }))
        return true;

    return std::ranges::any_of(mask_entries_, [&filter](const auto& entry) {
        return entry.has_value() && entry->filter.Overlaps(filter);
    });
}

bool Canbus::InstallFilter(FilterEntry& entry) {
    if(entry.filter_id >= 0)
        return true;

    const can_filter filter = { .id = entry.filter.id,
        .mask = entry.filter.mask,
        .flags = static_cast<uint8_t>(entry.filter.is_extended ? CAN_FILTER_IDE : 0) };

    const int filter_id = can_add_rx_filter(config_.canbus_dev, CanFrameReceivedCallback, this, &filter);
    if(filter_id < 0) {
        LOG_ERR("Unable to add rx filter [%d].", filter_id);
        return false;
    }

    entry.filter_id = filter_id;

    return true;
}

void Canbus::UninstallFilter(FilterEntry& entry) const {
    if(entry.filter_id < 0)
        return;

    can_remove_rx_filter(config_.canbus_dev, entry.filter_id);
    entry.filter_id = -1;
}

bool Canbus::InstallDeferredFilters() {
    bool all_installed = true;

    for(auto& [_, entry] : exact_entries_)
        all_installed &= InstallFilter(entry);

    for(auto& entry : mask_entries_) {
        if(entry.has_value())
            all_installed &= InstallFilter(entry.value());
    }

    return all_installed;
}

bool Canbus::IsDetectingBitrate() const {
    return atomic_get(&auto_detect_running_) != 0 && !IsBitrateDetected();
}

bool Canbus::HasTapFilters() const {
    return tap_filter_ids_[0] >= 0 || tap_filter_ids_[1] >= 0;
}

// Accept-all filters (mask 0), one for standard and one for extended identifiers, to the same callback
// the handlers' filters use. All or none.
bool Canbus::AddTapFilters() {
    if(HasTapFilters())
        return true;

    for(size_t i = 0; i < tap_filter_ids_.size(); i++) {
        const can_filter filter = { .id = 0, .mask = 0, .flags = static_cast<uint8_t>(i == 1 ? CAN_FILTER_IDE : 0) };

        const int filter_id = can_add_rx_filter(config_.canbus_dev, CanFrameReceivedCallback, this, &filter);
        if(filter_id < 0) {
            LOG_WRN("Unable to add the tap's rx filter [%d].", filter_id);
            RemoveTapFilters();

            return false;
        }

        tap_filter_ids_[i] = filter_id;
    }

    return true;
}

void Canbus::RemoveTapFilters() {
    for(int& filter_id : tap_filter_ids_) {
        if(filter_id >= 0)
            can_remove_rx_filter(config_.canbus_dev, filter_id);

        filter_id = -1;
    }
}

// Requires lock_. For a bus that goes away or is reconfigured: the driver must not call back afterwards,
// and the handlers' filters are the caller's to remove or keep.
void Canbus::ClearTap() {
    tap_ = nullptr;
    atomic_clear(&tap_active_);
    atomic_set(&filter_swap_, FILTER_SWAP_NONE);

    RemoveTapFilters();
}

// Receive thread. The tap is called under lock_, as the handlers are, so SetTap(nullptr) waits for a call
// in progress and none follows it. A swap SetTap() finished is announced first, so the tap hears of it
// before the first entry it sees after the swap; SetTap() never calls the tap itself (R-11: one producer).
template<typename Call>
void Canbus::WithTap(Call&& call) {
    if(atomic_get(&tap_active_) == 0)
        return;

    ScopedMutex guard(lock_);

    if(tap_ == nullptr)
        return;

    const atomic_val_t swap = atomic_set(&filter_swap_, FILTER_SWAP_NONE);
    if(swap != FILTER_SWAP_NONE)
        tap_->OnFilterSwap(swap == FILTER_SWAP_MAY_HAVE_MISSED, filter_swap_time_us_);

    std::forward<Call>(call)(*tap_);
}

bool Canbus::SetTap(ICanFrameTap* tap) {
    ScopedMutex guard(lock_);

    if(tap == tap_)
        return true;

    if(tap == nullptr) {
        // The receive thread waits on lock_ from here on, so no call to the old tap is left.
        tap_ = nullptr;
        atomic_clear(&tap_active_);
        atomic_set(&filter_swap_, FILTER_SWAP_NONE);

        // While the bitrate is detected no filter is installed, the handlers' ones wait for it.
        if(IsDetectingBitrate())
            return true;

        // The tap's filters never went in (the driver refused them after detection): the handlers'
        // ones are installed already, or go in now.
        if(!HasTapFilters()) {
            if(!InstallDeferredFilters())
                LOG_ERR("Unable to restore every handler filter after the tap.");

            return true;
        }

        // The handlers' filters go in before the accept-all ones leave; where the driver has no room for
        // both, the rest follow once the room is there.
        const bool all_installed = InstallDeferredFilters();
        RemoveTapFilters();
        if(!all_installed && !InstallDeferredFilters())
            LOG_ERR("Unable to restore every handler filter after the tap.");

        return true;
    }

    if(tap_ != nullptr) {
        LOG_ERR("A tap is already set.");
        return false;
    }

    if(atomic_get(&is_initialized_) == 0) {
        LOG_ERR("CANBus is not initialized.");
        return false;
    }

    // Allocated once and kept for the bus's life, so a completion can never find the pool gone.
    if(!tx_slots_) {
        tx_slots_.reset(new(std::nothrow) TxSlot[TX_SLOTS]);
        if(!tx_slots_) {
            LOG_ERR("Unable to allocate the tap's TX slots.");
            return false;
        }
    }

    if(IsDetectingBitrate()) {
        // Nothing is installed yet: BitrateAutodetectTask() installs the accept-all filters and reports
        // the state when detection succeeds.
        tap_ = tap;
        atomic_set(&tap_active_, 1);

        return true;
    }

    bool may_have_missed = false;

    if(AddTapFilters()) {
        // Both sets are installed for a moment: a frame may reach a handler twice.
        RemoveAllFilters();
    } else {
        // No room for both: a frame may go missing between the two steps.
        may_have_missed = true;
        RemoveAllFilters();

        if(!AddTapFilters()) {
            InstallDeferredFilters();
            LOG_ERR("The driver rejected the tap's rx filters.");

            return false;
        }
    }

    tap_ = tap;
    atomic_set(&tap_active_, 1);

    filter_swap_time_us_ = NowUs();
    atomic_set(&filter_swap_, may_have_missed ? FILTER_SWAP_MAY_HAVE_MISSED : FILTER_SWAP_REPEATED);

    return true;
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

                if(tap_ == nullptr) {
                    InstallDeferredFilters();
                } else {
                    // The tap's filters stand in for the handlers' ones, which the software dispatch serves.
                    // Without room for them the handlers keep their own, and the tap sees only their frames.
                    if(!AddTapFilters()) {
                        LOG_ERR("Unable to add the tap's rx filters after bitrate detection.");
                        InstallDeferredFilters();
                    }

                    can_state state = CAN_STATE_ERROR_ACTIVE;
                    can_bus_err_cnt err_cnt = {};

                    // Tells the session the bus is up; the receive thread is the one running this.
                    if(can_get_state(config_.canbus_dev, &state, &err_cnt) == 0)
                        WithTap([&](ICanFrameTap& tap) {
                            tap.OnState(state, err_cnt, NowUs());
                        });
                }
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
    CanbusRxEntry entry;
    if(k_msgq_get(&frame_msgq_, &entry, K_MSEC(MSGQ_GET_TIMEOUT_MS)) != 0) {
        // A swap that ended while the queue was idle is announced now, not with the next frame.
        if(atomic_get(&filter_swap_) != FILTER_SWAP_NONE)
            WithTap([](ICanFrameTap&) {});

        return;
    }

    switch(entry.kind) {
        case CanbusRxKind::RECEIVED:
            ProcessReceivedEntry(entry);
            break;
        case CanbusRxKind::TX_ECHO:
            ProcessTxEchoEntry(entry);
            break;
        case CanbusRxKind::STATE_CHANGE:
            WithTap([&](ICanFrameTap& tap) {
                tap.OnState(entry.state, entry.err_cnt, entry.time_us);
            });
            break;
    }
}

void Canbus::ProcessReceivedEntry(const CanbusRxEntry& entry) {
    const CanFrame can_frame = ToCanFrame(entry.frame, entry.time_us, false);

    WithTap([&](ICanFrameTap& tap) {
        tap.OnFrame(can_frame, entry.time_us, false, 0);
    });

    // Remote frames belong to the tap alone: the handlers have never been given a frame without a payload.
    if(can_frame.is_remote_request)
        return;

    DispatchFrame(can_frame);
}

// The unit's own frame, completed by the controller; only a tap wants it.
void Canbus::ProcessTxEchoEntry(const CanbusRxEntry& entry) {
    const CanFrame can_frame = ToCanFrame(entry.frame, entry.time_us, true);

    WithTap([&](ICanFrameTap& tap) {
        tap.OnFrame(can_frame, entry.time_us, true, entry.tx_error);
    });
}

void Canbus::DispatchFrame(const CanFrame& frame) {
    const CanId can_id{ frame.id, frame.is_extended };

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

        const auto handler_it = current->handlers.find(handler_ids[i]);
        if(handler_it == current->handlers.end())
            continue;

        handler_it->second(frame);
    }
}

void Canbus::AutoDetectFrameCallback(const device* dev, can_frame* frame, void* user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(frame);

    atomic_inc(static_cast<atomic_t*>(user_data));
}

bool Canbus::AutoDetectBitrate() {
    const std::span<const uint32_t> supported_bitrates = config_.type == CanbusType::CANFD
        ? std::span<const uint32_t>(CANFD_SUPPORTED_BITRATES)
        : std::span<const uint32_t>(CLASSICAL_CAN_SUPPORTED_BITRATES);

    for(const uint32_t bitrate : supported_bitrates) {
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
        return std::ranges::find(CANFD_SUPPORTED_BITRATES, bitrate) != CANFD_SUPPORTED_BITRATES.end();

    return std::ranges::find(CLASSICAL_CAN_SUPPORTED_BITRATES, bitrate) != CLASSICAL_CAN_SUPPORTED_BITRATES.end();
}

bool Canbus::TestBitrate(uint32_t bitrate) {
    // Probing has to stay off the wire: a wrong bitrate in normal mode makes the
    // controller ACK at the wrong bit times and corrupt a live bus.
    if(!ApplyMode(config_.extra_modes | CAN_MODE_LISTENONLY))
        return false;

    if(!SetTiming(bitrate))
        return false;

    if(config_.type == CanbusType::CANFD) {
        const uint32_t data_bitrate = config_.data_bitrate == 0 ? bitrate : config_.data_bitrate;
        if(!SetDataTiming(data_bitrate))
            return false;
    }

    int ret = can_start(config_.canbus_dev);
    if(ret != 0) {
        LOG_WRN("Failed to start CAN for bitrate %u [%d]", bitrate, ret);
        return false;
    }

    const can_filter filter = { .id = 0,
        .mask = 0, // Accept all IDs
        .flags = 0 };

    atomic_clear(&auto_detect_frame_count_);

    const int filter_id = can_add_rx_filter(
        config_.canbus_dev, AutoDetectFrameCallback, &auto_detect_frame_count_, &filter
    );

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
            detected = state == CAN_STATE_ERROR_ACTIVE && err_cnt.tx_err_cnt < 128 && err_cnt.rx_err_cnt < 128;
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

    LOG_INF(
        "CAN bitrate range: %u - %u bps.", can_get_bitrate_min(config_.canbus_dev),
        can_get_bitrate_max(config_.canbus_dev)
    );

    auto bitrates = CLASSICAL_CAN_SUPPORTED_BITRATES;
    std::ranges::sort(bitrates);

    for(const auto bitrate : bitrates) {
        struct can_timing timing_data = { 0 };
        const int ret = can_calc_timing(config_.canbus_dev, &timing_data, bitrate, SamplePointForBitrate(bitrate));

        PrintCanLimitsDetails(bitrate, ret);
    }
}

void Canbus::PrintCanFdLimits() const {
    LOG_INF("Hardware CAN FD data bitrate capabilities:");

    LOG_INF(
        "CAN FD data bitrate range: %u - %u bps.", can_get_bitrate_min(config_.canbus_dev),
        can_get_bitrate_max(config_.canbus_dev)
    );

    auto bitrates = CANFD_SUPPORTED_BITRATES;
    std::ranges::sort(bitrates);

    for(const auto bitrate : bitrates) {
        struct can_timing timing_data = { 0 };
        const int ret = can_calc_timing_data(config_.canbus_dev, &timing_data, bitrate, SamplePointForBitrate(bitrate));

        PrintCanLimitsDetails(bitrate, ret);
    }
}

bool Canbus::DoStop() {
    StopActivityMonitoring();

    if(thread_ && thread_->IsRunning())
        thread_->Join();

    if(atomic_get(&is_initialized_) != 0) {
        const int ret = can_stop(config_.canbus_dev);
        if(ret != 0 && ret != -EALREADY) {
            LOG_ERR("Failed to stop CAN device [%d].", ret);
            return false;
        }
    }

    {
        ScopedMutex guard(lock_);

        ClearTap();
        RemoveAllFilters();
        ClearEntries();
    }

    k_msgq_purge(&frame_msgq_);

    // can_stop() aborted the frames in flight, and some drivers drop them without a completion (the
    // loopback does), so their slots would stay taken. The pool itself stays until the destructor.
    ResetTxSlots();

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

} // namespace eerie_leap::subsys::canbus
