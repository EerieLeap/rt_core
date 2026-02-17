#include <stdexcept>
#include <span>
#include <algorithm>
#include <memory>

#include <zephyr/logging/log.h>

#include "canbus.h"

LOG_MODULE_REGISTER(canbus_logger);

namespace eerie_leap::subsys::canbus {

Canbus::Canbus(
    const device *canbus_dev,
    CanbusType type,
    uint32_t bitrate,
    uint32_t data_bitrate,
    bool is_extended_id)
        : canbus_dev_(canbus_dev),
        type_(type),
        is_extended_id_(is_extended_id),
        bitrate_(bitrate),
        data_bitrate_(data_bitrate),
        auto_detect_running_(ATOMIC_INIT(0)) {

    k_msgq_init(
        &frame_msgq_,
        frame_msgq_buffer_,
        sizeof(IsrCanFrameWrapper),
        FRAME_MSGQ_SIZE);

    if(type_ == CanbusType::CANFD && data_bitrate_ == 0)
        data_bitrate_ = bitrate_;

    thread_ = std::make_unique<Thread>(
        "can_" + std::string(canbus_dev_->name),
        this,
        thread_stack_size_,
        thread_priority_,
        thread_is_cooperative_);
}

Canbus::~Canbus() {
    StopActivityMonitoring();
    if(canbus_dev_ != nullptr && is_initialized_)
        can_stop(canbus_dev_);

    if(thread_)
        thread_->Join();
}

bool Canbus::Initialize() {
    if(!device_is_ready(canbus_dev_)) {
		LOG_ERR("Device driver not ready.");
		return false;
	}

    can_mode_t capabilities;
    int ret = can_get_capabilities(canbus_dev_, &capabilities);
    if(ret != 0) {
        LOG_ERR("Failed to get capabilities.");
        return false;
    }

    thread_->Initialize();

    can_mode_t can_mode = CAN_MODE_NORMAL;
    if(type_ == CanbusType::CANFD && (capabilities & CAN_MODE_FD))
        can_mode = CAN_MODE_FD;
    else
        type_ = CanbusType::CLASSICAL_CAN;

    ret = can_set_mode(canbus_dev_, can_mode);
	if(ret != 0) {
		LOG_ERR("Failed to set mode [%d].", ret);
		return false;
	}
    LOG_INF("CAN mode set to %d.", can_mode);

    if(bitrate_ == 0) {
        LOG_INF("Auto-bitrate mode enabled - will detect on bus activity");

        if(!StartActivityMonitoring()) {
            LOG_ERR("Failed to start activity monitoring.");
            return false;
        }
    } else {
        if(!SetTiming(bitrate_)) {
            PrintCanLimits();
            return false;
        }

        if(type_ == CanbusType::CANFD && !SetDataTiming(data_bitrate_)) {
            PrintCanFdLimits();
            return false;
        }

        ret = can_start(canbus_dev_);
        if(ret != 0) {
            LOG_ERR("Failed to start device [%d].", ret);
            return false;
        }

        bitrate_detected_ = true;

        LOG_INF("Bitrate set to: %d.", bitrate_);
    }

    LOG_INF("CANBus initialized successfully.");
    is_initialized_ = true;

    thread_->Start();

    return true;
}

bool Canbus::SetTiming(uint32_t bitrate) {
    if(bitrate == 0)
        return false;

    int ret = can_set_bitrate(canbus_dev_, bitrate);
    if(ret != 0) {
        LOG_ERR("Failed to set bitrate [%d].", ret);
        return false;
    }

    return true;
}

bool Canbus::SetDataTiming(uint32_t bitrate) {
    if(bitrate == 0)
        return false;

    if(type_ != CanbusType::CANFD)
        return false;

    int ret = can_set_bitrate_data(canbus_dev_, bitrate);
    if(ret != 0) {
        LOG_ERR("Failed to set CANFD bitrate [%d].", ret);
        return false;
    }

    return true;
}

void Canbus::SendFrame(uint32_t frame_id, std::span<const uint8_t> frame_data) {
    if(!is_initialized_ || !bitrate_detected_)
        return;

    uint8_t flags = 0;

    if(type_ == CanbusType::CANFD)
        flags |= CAN_FRAME_FDF | CAN_FRAME_BRS;

    if(is_extended_id_)
        flags |= CAN_FRAME_IDE;

    struct can_frame can_frame = {
        .id = frame_id,
        .dlc = can_bytes_to_dlc(frame_data.size()),
        .flags = flags,
    };
    memcpy(can_frame.data, frame_data.data(), frame_data.size());

    int res = can_send(
        canbus_dev_,
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

        int filter_id = can_add_rx_filter(canbus_dev_, CanFrameReceivedCallback, this, &filter);
        if(filter_id < 0) {
            LOG_ERR("Unable to add rx filter [%d].", filter_id);
            return false;
        }

        can_filters_.insert({can_id, filter});
    }

    return true;
}

bool Canbus::RemoveFrameReceivedHandler(uint32_t can_id, int handler_id) {
    if(!is_initialized_) {
        LOG_ERR("CANBus is not initialized.");
        return false;
    }

    if(!handlers_.contains(can_id))
        return false;

    auto& handler_list = handlers_.at(can_id);

    if(!handler_list.contains(handler_id))
        return false;

    handler_list.erase(handler_id);
    if(handler_list.empty()) {
        // Remove filter
        if(can_filters_.contains(can_id)) {
            can_remove_rx_filter(canbus_dev_, can_filters_.at(can_id).id);
            can_filters_.erase(can_id);
        }

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

    while(true) {
        if(atomic_get(&auto_detect_running_) && !bitrate_detected_)
            BitrateAutodetectTask();

        ProcessFramesTask();
    }
}

void Canbus::BitrateAutodetectTask() {
    LOG_INF("CANBus auto-detection started.");

    while(atomic_get(&auto_detect_running_) && !bitrate_detected_) {
        if(AutoDetectBitrate()) {
            LOG_INF("Bitrate successfully detected: %u bps", bitrate_);

            if(bitrate_detected_fn_)
                bitrate_detected_fn_(bitrate_);

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
    if(k_msgq_get(&frame_msgq_, &frame_wrapper, K_FOREVER) != 0)
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
    if(type_ == CanbusType::CANFD)
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
            bitrate_ = supported_bitrates[i];

            if(type_ == CanbusType::CANFD && data_bitrate_ == 0)
                data_bitrate_ = supported_bitrates[i];

            return true;
        }

        can_stop(canbus_dev_);
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

bool Canbus::TestBitrate(uint32_t bitrate, uint32_t &frame_count) {
    if(!SetTiming(bitrate))
        return false;

    if(type_ == CanbusType::CANFD) {
        uint32_t data_bitrate = data_bitrate_ == 0 ? bitrate : data_bitrate_;
        if(!SetDataTiming(data_bitrate))
            return false;
    }

    int ret = can_start(canbus_dev_);
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

    int filter_id = can_add_rx_filter(canbus_dev_,
        [](const device *dev, can_frame *frame, void *user_data) {
            volatile uint32_t *counter = static_cast<volatile uint32_t*>(user_data);
            (*counter)++;
        },
        (void*)&received_frames,
        &filter);

    if(filter_id < 0) {
        LOG_WRN("Failed to add test filter [%d]", filter_id);
        can_stop(canbus_dev_);

        return false;
    }

    k_sleep(K_MSEC(AUTO_DETECT_TIMEOUT_MS));
    can_remove_rx_filter(canbus_dev_, filter_id);
    frame_count = received_frames;

    if(received_frames >= MIN_FRAMES_FOR_DETECTION) {
        enum can_state state;
        struct can_bus_err_cnt err_cnt;

        int ret = can_get_state(canbus_dev_, &state, &err_cnt);
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

    uint32_t min_bitrate = can_get_bitrate_min(canbus_dev_);
    uint32_t max_bitrate = can_get_bitrate_max(canbus_dev_);
    LOG_INF("CAN bitrate range: %u - %u bps.", min_bitrate, max_bitrate);

    auto bitrates = classical_can_supported_bitrates_;
    std::sort(bitrates.begin(), bitrates.end());

    for(auto bitrate : bitrates) {
        struct can_timing timing_data = {0};
        uint16_t sample_pnt = sample_point_for_bitrate(bitrate);
        int ret = can_calc_timing(canbus_dev_, &timing_data, bitrate, sample_pnt);

        PrintCanLimitsDetails(bitrate, ret);
    }
}

void Canbus::PrintCanFdLimits() {
    LOG_INF("Hardware CAN FD data bitrate capabilities:");

    uint32_t min_bitrate = can_get_bitrate_min(canbus_dev_);
    uint32_t max_bitrate = can_get_bitrate_max(canbus_dev_);
    LOG_INF("CAN FD data bitrate range: %u - %u bps.", min_bitrate, max_bitrate);

    auto bitrates = canfd_supported_bitrates_;
    std::sort(bitrates.begin(), bitrates.end());

    for(auto bitrate : bitrates) {
        struct can_timing timing_data = {0};
        uint16_t sample_pnt = sample_point_for_bitrate(bitrate);
        int ret = can_calc_timing_data(canbus_dev_, &timing_data, bitrate, sample_pnt);

        PrintCanLimitsDetails(bitrate, ret);
    }
}

}  // namespace eerie_leap::subsys::canbus
