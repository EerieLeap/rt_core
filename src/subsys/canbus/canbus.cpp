#include <stdexcept>
#include <span>
#include <algorithm>
#include <memory>
#include <string>
#include <numeric>

#include <zephyr/logging/log.h>

#include "canbus.h"

LOG_MODULE_REGISTER(canbus_logger, CONFIG_CAN_LOG_LEVEL);

namespace eerie_leap::subsys::canbus {

Canbus::Canbus(const CanbusConfig& config)
        : config_(config) {

    if(config_.type == CanbusType::CANFD && config_.data_bitrate == 0)
        config_.data_bitrate = config_.bitrate;

    thread_ = std::make_unique<Thread>(
        "can_" + std::string(config_.canbus_dev->name),
        this,
        thread_stack_size_,
        thread_priority_,
        thread_is_cooperative_);
}

Canbus::~Canbus() {
    StopActivityMonitoring();
    if(config_.canbus_dev != nullptr && is_initialized_)
        can_stop(config_.canbus_dev);

    if(thread_ && thread_->IsRunning())
        thread_->Join();
}

bool Canbus::Initialize() {
    if(state_ != CanbusState::STOPPED) {
        LOG_ERR("Cannot initialize CAN bus when not in STOPPED state.");
        return false;
    }

    // Initialize thread only
    thread_->Initialize();

    // Initialize message queue
    k_msgq_init(
        &frame_msgq_,
        frame_msgq_buffer_,
        sizeof(IsrCanFrameWrapper),
        FRAME_MSGQ_SIZE);

    // Clear any existing state
    can_filter_ids_.clear();
    can_filters_.clear();
    handlers_.clear();
    bitrate_detected_ = false;

    // Configure with current config
    if(!Configure(config_)) {
        LOG_ERR("Failed to configure CAN bus during initialization.");
        return false;
    }

    LOG_INF("CANBus initialized and configured successfully.");
    return true;
}

bool Canbus::Configure(const CanbusConfig& config) {
    if(state_ != CanbusState::STOPPED) {
        LOG_ERR("Cannot configure CAN bus when not in STOPPED state.");
        return false;
    }

    // Validate new configuration
    if(!device_is_ready(config.canbus_dev)) {
        LOG_ERR("CAN device is not ready.");
        return false;
    }

    if(!IsBitrateSupported(config.type, config.bitrate)) {
        LOG_ERR("Bitrate %u is not supported for CAN type %d.", config.bitrate, static_cast<int>(config.type));
        return false;
    }

    if(config.type == CanbusType::CANFD && config.data_bitrate > 0 &&
       !IsBitrateSupported(config.type, config.data_bitrate)) {
        LOG_ERR("Data bitrate %u is not supported for CAN FD.", config.data_bitrate);
        return false;
    }

    // Update configuration
    config_ = config;

    can_mode_t capabilities;
    int ret = can_get_capabilities(config_.canbus_dev, &capabilities);
    if(ret != 0) {
        LOG_ERR("Failed to get capabilities.");
        return false;
    }

    can_mode_t can_mode = CAN_MODE_NORMAL;
    if(config_.type == CanbusType::CANFD && (capabilities & CAN_MODE_FD))
        can_mode = CAN_MODE_FD;
    else
        config_.type = CanbusType::CLASSICAL_CAN;

    ret = can_set_mode(config_.canbus_dev, can_mode);
    if(ret != 0) {
        LOG_ERR("Failed to set mode [%d].", ret);
        return false;
    }
    LOG_INF("CAN mode set to %d.", can_mode);

    if(config_.bitrate == 0) {
        LOG_INF("Auto-bitrate mode enabled - will detect on bus activity");
        if(!StartActivityMonitoring()) {
            LOG_ERR("Failed to start activity monitoring.");
            return false;
        }
    } else {
        if(!SetTiming(config_.bitrate)) {
            PrintCanLimits();
            return false;
        }

        if(config_.type == CanbusType::CANFD && !SetDataTiming(config_.data_bitrate)) {
            PrintCanFdLimits();
            return false;
        }

        ret = can_start(config_.canbus_dev);
        if(ret != 0) {
            LOG_ERR("Failed to start device [%d].", ret);
            return false;
        }

        bitrate_detected_ = true;
        LOG_INF("Bitrate set to: %d.", config_.bitrate);
    }

    is_initialized_ = true;
    LOG_INF("CANBus configured successfully.");
    return true;
}

bool Canbus::SetTiming(uint32_t bitrate) const {
    if(bitrate == 0)
        return false;

    int ret = can_set_bitrate(config_.canbus_dev, bitrate);
    if(ret != 0) {
        LOG_ERR("Failed to set bitrate [%d].", ret);
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
        LOG_ERR("Failed to set CANFD bitrate [%d].", ret);
        return false;
    }

    return true;
}

void Canbus::SendFrame(uint32_t frame_id, std::span<const uint8_t> frame_data) const {
    if(!is_initialized_ || !bitrate_detected_ || state_ != CanbusState::RUNNING)
        return;

    uint8_t flags = 0;

    if(config_.type == CanbusType::CANFD)
        flags |= CAN_FRAME_FDF | CAN_FRAME_BRS;

    if(config_.is_extended_id)
        flags |= CAN_FRAME_IDE;

    struct can_frame can_frame = {
        .id = frame_id,
        .dlc = can_bytes_to_dlc(frame_data.size()),
        .flags = flags,
    };
    memcpy(can_frame.data, frame_data.data(), frame_data.size());

    int res = can_send(
        config_.canbus_dev,
        &can_frame,
        FRAME_SEND_TIMEOUT_MS,
        SendFrameCallback,
        nullptr);

    if(res != 0) {
        LOG_DBG("Failed to send frame [%d].", res);
        return;
    }

    LOG_DBG("Frame sent: ID=0x%08X, DLC=%d", frame_id, can_bytes_to_dlc(frame_data.size()));
}

void Canbus::SendFrameCallback(const device* dev, int error, void* user_data) {
    if(error != 0)
        LOG_ERR("SendFrameCallback error: %d", error);
}

void Canbus::CanFrameReceivedCallback(const device *dev, can_frame *frame, void *user_data) {
    LOG_DBG("Frame received: ID=0x%08X, DLC=%d", frame->id, can_bytes_to_dlc(frame->dlc));

    auto* canbus = static_cast<Canbus*>(user_data);

    if(!canbus->handlers_.contains(frame->id))
        return;

    IsrCanFrameWrapper wrapper = {
        .canbus = canbus,
        .frame = *frame
    };

    k_msgq_put(&canbus->frame_msgq_, &wrapper, K_NO_WAIT);
}

int Canbus::RegisterFrameReceivedHandler(uint32_t can_id, CanFrameHandler handler) {
    if(!is_initialized_) {
        LOG_ERR("CANBus is not initialized.");
        return false;
    }

    if(!handlers_.contains(can_id))
        handlers_.insert({can_id, {}});

    int max_handler_id = 0;
    for(const auto& [handler_id, _] : handlers_.at(can_id))
        max_handler_id = std::max(max_handler_id, handler_id);

    int handler_id = max_handler_id + 1;

    handlers_.at(can_id).emplace(handler_id, std::move(handler));

    if(atomic_get(&auto_detect_running_) && !bitrate_detected_)
        return false;

    if(!RegisterFilter(can_id)) {
        handlers_.at(can_id).erase(handler_id);
        return -1;
    }

    LOG_DBG("Frame received handler registered for ID=0x%08X", can_id);

    return handler_id;
}

bool Canbus::RegisterFilter(uint32_t can_id) {
    if(!is_initialized_) {
        LOG_ERR("CANBus is not initialized.");
        return false;
    }

    if(!handlers_.contains(can_id))
        throw std::runtime_error("Handler not found for ID " + std::to_string(can_id));

    if(!can_filters_.contains(can_id)) {
        can_filter filter = {
            .id = can_id,
            .mask = CAN_STD_ID_MASK,
            .flags = 0
        };

        int filter_id = can_add_rx_filter(config_.canbus_dev, CanFrameReceivedCallback, this, &filter);
        if(filter_id < 0) {
            LOG_ERR("Unable to add rx filter [%d].", filter_id);
            return false;
        }

        can_filter_ids_.insert({ can_id, filter_id });
        can_filters_.insert({ can_id, filter });
    }

    return true;
}

bool Canbus::RemoveFrameReceivedHandler(uint32_t can_id, int handler_id) {
    if(!is_initialized_) {
        LOG_ERR("CANBus is not initialized.");
        return false;
    }

    if(!can_filter_ids_.contains(can_id))
        return false;

    if(!can_filters_.contains(can_id))
        return false;

    if(!handlers_.contains(can_id))
        return false;

    auto& handler_list = handlers_.at(can_id);

    if(!handler_list.contains(handler_id))
        return false;

    handler_list.erase(handler_id);

    if(handler_list.empty()) {
        // Remove filter
        can_remove_rx_filter(config_.canbus_dev, can_filter_ids_.at(can_id));

        can_filters_.erase(can_id);
        can_filter_ids_.erase(can_id);
        handlers_.erase(can_id);
    }

    return true;
}

bool Canbus::StartActivityMonitoring() {
    atomic_set(&auto_detect_running_, 1);

    return true;
}

void Canbus::StopActivityMonitoring() {
    if(atomic_get(&auto_detect_running_))
        atomic_set(&auto_detect_running_, 0);
}

void Canbus::ThreadEntry() {
    LOG_INF("CANBus thread started.");

    while(thread_->IsRunning()) {
        if(atomic_get(&auto_detect_running_) && !bitrate_detected_)
            BitrateAutodetectTask();

        ProcessFramesTask();
    }
}

void Canbus::BitrateAutodetectTask() {
    LOG_INF("CANBus auto-detection started.");

    while(atomic_get(&auto_detect_running_) && !bitrate_detected_) {
        if(AutoDetectBitrate()) {
            LOG_INF("Bitrate successfully detected: %u bps", config_.bitrate);

            if(bitrate_detected_fn_)
                bitrate_detected_fn_(config_.bitrate);

            for(const auto& [can_id, _] : handlers_)
                RegisterFilter(can_id);

            break;
        }

        k_sleep(K_MSEC(CONFIG_EERIE_LEAP_CANBUS_AUTO_DETECT_INTERVAL_MS));
    }

    LOG_INF("CANBus auto-detection stopped.");
}

void Canbus::ProcessFramesTask() {
    IsrCanFrameWrapper frame_wrapper;
    if(k_msgq_get(&frame_msgq_, &frame_wrapper, K_MSEC(MSGQ_GET_TIMEOUT_MS)) != 0)
        return;

    auto* canbus = frame_wrapper.canbus;
    can_frame* frame = &frame_wrapper.frame;

    CanFrame can_frame = {
        .id = frame->id,
        .is_transmit = false,
        .is_can_fd = (frame->flags & CAN_FRAME_FDF) != 0
    };

    int frame_size = can_dlc_to_bytes(frame->dlc);
    can_frame.data.resize(frame_size);
    std::copy(frame->data, frame->data + frame_size, can_frame.data.begin());

    if(canbus->handlers_.contains(frame->id)) {
        for(const auto& [_, handler] : canbus->handlers_.at(frame->id))
            handler(can_frame);
    }
}

bool Canbus::AutoDetectBitrate() {
    std::span<const uint32_t> supported_bitrates;
    if(config_.type == CanbusType::CANFD)
        supported_bitrates = canfd_supported_bitrates_;
    else
        supported_bitrates = classical_can_supported_bitrates_;

    for(int i = 0; i < supported_bitrates.size(); i++) {
        if(!atomic_get(&auto_detect_running_)) {
            LOG_WRN("Bitrate detection stopped by user");
            return false;
        }

        uint32_t frame_count = 0;
        if(TestBitrate(supported_bitrates[i], frame_count)) {
            bitrate_detected_ = true;
            config_.bitrate = supported_bitrates[i];

            if(config_.type == CanbusType::CANFD && config_.data_bitrate == 0)
                config_.data_bitrate = supported_bitrates[i];

            return true;
        }

        can_stop(config_.canbus_dev);
        k_sleep(K_MSEC(50));
    }

    return false;
}

bool Canbus::IsBitrateSupported(CanbusType type, uint32_t bitrate) {
    if(bitrate == 0)
        return true;

    if(type == CanbusType::CANFD)
        return std::ranges::find(canfd_supported_bitrates_, bitrate) != canfd_supported_bitrates_.end();
    else
        return std::ranges::find(classical_can_supported_bitrates_, bitrate) != classical_can_supported_bitrates_.end();
}

bool Canbus::TestBitrate(uint32_t bitrate, uint32_t &frame_count) const {
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

    volatile uint32_t received_frames = 0;

    int filter_id = can_add_rx_filter(config_.canbus_dev,
        [](const device *dev, can_frame *frame, void *user_data) {
            volatile uint32_t *counter = static_cast<volatile uint32_t*>(user_data);
            (*counter)++;
        },
        (void*)&received_frames,
        &filter);

    if(filter_id < 0) {
        LOG_WRN("Failed to add test filter [%d]", filter_id);
        can_stop(config_.canbus_dev);

        return false;
    }

    k_sleep(K_MSEC(AUTO_DETECT_TIMEOUT_MS));
    can_remove_rx_filter(config_.canbus_dev, filter_id);
    frame_count = received_frames;

    if(received_frames >= MIN_FRAMES_FOR_DETECTION) {
        enum can_state state;
        struct can_bus_err_cnt err_cnt;

        ret = can_get_state(config_.canbus_dev, &state, &err_cnt);
        if(ret != 0)
            return false;

        // Valid activity means error-active state with reasonable error counts
        return (state == CAN_STATE_ERROR_ACTIVE
            && err_cnt.tx_err_cnt < 128
            && err_cnt.rx_err_cnt < 128);
    }

    return false;
}

void Canbus::RegisterBitrateDetectedCallback(const BitrateDetectedCallback& callback) {
    bitrate_detected_fn_ = callback;
}

// NOTE: Borrowed from zephyr/drivers/can/can_common.c
uint16_t sample_point_for_bitrate(uint32_t bitrate) {
	uint16_t sample_pnt;

	if (bitrate > 800000) {
		/* 75.0% */
		sample_pnt = 750;
	} else if (bitrate > 500000) {
		/* 80.0% */
		sample_pnt = 800;
	} else {
		/* 87.5% */
		sample_pnt = 875;
	}

	return sample_pnt;
}

static void PrintCanLimitsDetails(uint32_t bitrate, int ret) {
    if(ret >= 0 && ret <= CONFIG_CAN_SAMPLE_POINT_MARGIN) {
        LOG_INF("  %u bps: OK (sample point error: %d).", bitrate, ret);
    } else if(ret == -EINVAL) {
        LOG_ERR("  %u bps: INVALID.", bitrate);
    } else if(ret == -ENOTSUP) {
        LOG_ERR("  %u bps: NOT SUPPORTED.", bitrate);
    } else {
        LOG_ERR("  %u bps: SAMPLE POINT OUT OF RANGE (error: %d).", bitrate, ret);
    }
}

void Canbus::PrintCanLimits() {
    LOG_INF("Hardware CAN bitrate capabilities:");

    uint32_t min_bitrate = can_get_bitrate_min(config_.canbus_dev);
    uint32_t max_bitrate = can_get_bitrate_max(config_.canbus_dev);
    LOG_INF("CAN bitrate range: %u - %u bps.", min_bitrate, max_bitrate);

    auto bitrates = classical_can_supported_bitrates_;
    std::sort(bitrates.begin(), bitrates.end());

    for(auto bitrate : bitrates) {
        struct can_timing timing_data = {0};
        uint16_t sample_pnt = sample_point_for_bitrate(bitrate);
        int ret = can_calc_timing(config_.canbus_dev, &timing_data, bitrate, sample_pnt);

        PrintCanLimitsDetails(bitrate, ret);
    }
}

void Canbus::PrintCanFdLimits() {
    LOG_INF("Hardware CAN FD data bitrate capabilities:");

    uint32_t min_bitrate = can_get_bitrate_min(config_.canbus_dev);
    uint32_t max_bitrate = can_get_bitrate_max(config_.canbus_dev);
    LOG_INF("CAN FD data bitrate range: %u - %u bps.", min_bitrate, max_bitrate);

    auto bitrates = canfd_supported_bitrates_;
    std::sort(bitrates.begin(), bitrates.end());

    for(auto bitrate : bitrates) {
        struct can_timing timing_data = {0};
        uint16_t sample_pnt = sample_point_for_bitrate(bitrate);
        int ret = can_calc_timing_data(config_.canbus_dev, &timing_data, bitrate, sample_pnt);

        PrintCanLimitsDetails(bitrate, ret);
    }
}

bool Canbus::Stop() {
    if(state_ == CanbusState::STOPPED) {
        LOG_WRN("CAN bus is already stopped.");
        return true;
    }

    if(state_ == CanbusState::STOPPING) {
        LOG_WRN("CAN bus is already stopping.");
        return false;
    }

    state_ = CanbusState::STOPPING;

    // Stop activity monitoring
    StopActivityMonitoring();

    // Stop the CAN hardware
    if(config_.canbus_dev != nullptr && is_initialized_) {
        int ret = can_stop(config_.canbus_dev);
        if(ret != 0) {
            LOG_ERR("Failed to stop CAN device [%d].", ret);
            state_ = CanbusState::RUNNING; // Restore state on failure
            return false;
        }
    }

    // Stop the thread
    if(thread_ && thread_->IsRunning())
        thread_->Join();

    // Clear filters and handlers
    for(const auto& [can_id, filter_id] : can_filter_ids_)
        can_remove_rx_filter(config_.canbus_dev, filter_id);

    can_filter_ids_.clear();
    can_filters_.clear();
    handlers_.clear();

    // Reset state
    is_initialized_ = false;
    bitrate_detected_ = false;
    state_ = CanbusState::STOPPED;

    LOG_INF("CAN bus stopped successfully.");
    return true;
}

bool Canbus::Start() {
    if(state_ == CanbusState::RUNNING) {
        LOG_WRN("CAN bus is already running.");
        return true;
    }

    if(state_ == CanbusState::STARTING) {
        LOG_WRN("CAN bus is already starting.");
        return false;
    }

    if(!is_initialized_) {
        LOG_ERR("CAN bus must be initialized before starting. Call Initialize() first.");
        return false;
    }

    state_ = CanbusState::STARTING;

    // Start the processing thread
    thread_->Start();

    state_ = CanbusState::RUNNING;
    LOG_INF("CAN bus started successfully.");
    return true;
}


}  // namespace eerie_leap::subsys::canbus
