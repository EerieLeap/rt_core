#include <zephyr/devicetree.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

#include "cfb.h"
#include "cfb_fonts.h"

LOG_MODULE_REGISTER(cfb_logger);

namespace eerie_leap::subsys::cfb {

namespace {

// Coordinate holds int, the CFB driver int16_t / uint16_t; clamp instead of wrapping.
int16_t ToCfbCoordinate(int value) {
    return static_cast<int16_t>(
        std::clamp<int>(value, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max())
    );
}

uint16_t ToCfbExtent(int value) {
    return static_cast<uint16_t>(std::clamp<int>(value, 0, std::numeric_limits<uint16_t>::max()));
}

cfb_position ToCfbPosition(const Coordinate& coordinate) {
    return cfb_position{ ToCfbCoordinate(coordinate.x), ToCfbCoordinate(coordinate.y) };
}

} // namespace

Cfb::Cfb(const device* display_dev) :
    display_dev_(display_dev),
    work_queue_thread_(nullptr),
    work_queue_task_(std::nullopt) {}

bool Cfb::Initialize() {
    if(initialized_) {
        LOG_INF("Cfb already initialized.");
        return true;
    }

    LOG_INF("Cfb initialization started.");

    if(!display_dev_) {
        LOG_ERR("Display device not found.");
        return false;
    }

    if(display_set_pixel_format(display_dev_, PIXEL_FORMAT_MONO10) != 0) {
        LOG_ERR("Failed to set pixel format");
        return false;
    }

    if(cfb_framebuffer_init(display_dev_) != 0) {
        LOG_ERR("Framebuffer initialization failed!");
        return false;
    }

    cfb_framebuffer_clear(display_dev_, true);

    display_blanking_off(display_dev_);
    cfb_framebuffer_finalize(display_dev_);

    SetFont(0);
    cfb_set_kerning(display_dev_, 0);

    LOG_INF("Cfb initialized successfully.");

    x_res_ = cfb_get_display_parameter(display_dev_, CFB_DISPLAY_WIDTH);
    y_res_ = cfb_get_display_parameter(display_dev_, CFB_DISPLAY_HEIGHT);

    PrintScreenInfo();

    InitializeThread();

    initialized_ = true;

    return true;
}

void Cfb::InitializeThread() {
    work_queue_thread_ = std::make_unique<WorkQueueThread>("display_service", THREAD_STACK_SIZE, THREAD_PRIORITY);
    work_queue_thread_->Initialize();

    auto task = std::make_unique<CfbTask>();
    task->display_dev = display_dev_;

    work_queue_task_ = work_queue_thread_->CreateTask(ProcessWorkTask, std::move(task));
    GetTask().Schedule();

    LOG_INF("Display service initialized.");
}

WorkQueueTaskResult Cfb::ProcessWorkTask(CfbTask* task) {
    const bool is_animation_running = atomic_get(&task->is_animation_running) != 0
        && task->frame_rate > 0
        && task->animation_handler;

    if(is_animation_running)
        task->animation_handler();

    cfb_framebuffer_finalize(task->display_dev);

    // frame_rate is 0 until SetAnimationHandler(); the delay only matters while an animation runs.
    const uint32_t frame_period_ms = task->frame_rate > 0 ? 1000 / task->frame_rate : 0;

    return { .reschedule = is_animation_running, .delay = K_MSEC(frame_period_ms) };
}

threading::WorkQueueTask<CfbTask>& Cfb::GetTask() {
    if(!work_queue_task_.has_value())
        throw std::logic_error("Cfb is not initialized.");

    return *work_queue_task_;
}

bool Cfb::Flush() {
    GetTask().Schedule();
    GetTask().Flush();

    return true;
}

bool Cfb::SetFont(uint8_t font_idx) {
    if(cfb_framebuffer_set_font(display_dev_, font_idx) != 0) {
        LOG_ERR("Failed to set font");
        return false;
    }

    if(cfb_get_font_size(display_dev_, font_idx, &font_width_, &font_height_) != 0) {
        LOG_ERR("Failed to get font size");
        return false;
    }

    return true;
}

int Cfb::GetFontHeight() const {
    return font_height_;
}

int Cfb::GetFontWidth() const {
    return font_width_;
}

bool Cfb::PrintString(const char* str, const Coordinate& coordinate) {
    if(cfb_print(display_dev_, str, ToCfbCoordinate(coordinate.x), ToCfbCoordinate(coordinate.y)) != 0) {
        LOG_ERR("Failed to print a string");
        return false;
    }

    return true;
}

bool Cfb::PrintStringLine(const char* str, const Coordinate& coordinate) {
    if(cfb_draw_text(display_dev_, str, ToCfbCoordinate(coordinate.x), ToCfbCoordinate(coordinate.y)) != 0) {
        LOG_ERR("Failed to print a string");
        return false;
    }

    return true;
}

bool Cfb::DrawPoint(const Coordinate& coordinate) {
    const cfb_position cbf_coordinate = ToCfbPosition(coordinate);

    if(cfb_draw_point(display_dev_, &cbf_coordinate) != 0) {
        LOG_ERR("Failed to draw a point");
        return false;
    }

    return true;
}

bool Cfb::DrawLine(const Coordinate& start, const Coordinate& end) {
    const cfb_position cbf_start = ToCfbPosition(start);
    const cfb_position cbf_end = ToCfbPosition(end);

    if(cfb_draw_line(display_dev_, &cbf_start, &cbf_end) != 0) {
        LOG_ERR("Failed to draw a line");
        return false;
    }

    return true;
}

bool Cfb::DrawRectangle(const Coordinate& start, const Coordinate& end) {
    const cfb_position cbf_start = ToCfbPosition(start);
    const cfb_position cbf_end = ToCfbPosition(end);

    if(cfb_draw_rect(display_dev_, &cbf_start, &cbf_end) != 0) {
        LOG_ERR("Failed to draw a rectangle");
        return false;
    }

    return true;
}

bool Cfb::DrawCircle(const Coordinate& center, uint16_t radius) {
    const cfb_position cbf_center = ToCfbPosition(center);

    if(cfb_draw_circle(display_dev_, &cbf_center, radius) != 0) {
        LOG_ERR("Failed to draw a circle");
        return false;
    }

    return true;
}

bool Cfb::InvertArea(const Coordinate& start, const Coordinate& end) {
    const uint16_t height = ToCfbExtent(std::abs(end.y - start.y));
    const uint16_t width = ToCfbExtent(std::abs(end.x - start.x));

    if(cfb_invert_area(
           display_dev_, ToCfbCoordinate(std::min(start.x, end.x)), ToCfbCoordinate(std::min(start.y, end.y)), width,
           height
       ) != 0) {

        LOG_ERR("Failed to invert an area");
        return false;
    }

    return true;
}

bool Cfb::Clear(bool clear_display) {
    if(cfb_framebuffer_clear(display_dev_, clear_display) != 0) {
        LOG_ERR("Failed to clear the screen");
        return false;
    }

    return true;
}

void Cfb::SetAnimationHandler(std::function<void()> handler, uint32_t frame_rate) {
    GetTask().GetUserdata()->animation_handler = std::move(handler);
    GetTask().GetUserdata()->frame_rate = frame_rate;
}

void Cfb::StartAnimation() {
    atomic_set(&GetTask().GetUserdata()->is_animation_running, 1);
    GetTask().Schedule();
}

void Cfb::StopAnimation() {
    GetTask().Cancel();
    atomic_set(&GetTask().GetUserdata()->is_animation_running, 0);
}

void Cfb::PrintScreenInfo() {
    const uint16_t rows = cfb_get_display_parameter(display_dev_, CFB_DISPLAY_ROWS);
    const uint8_t ppt = cfb_get_display_parameter(display_dev_, CFB_DISPLAY_PPT);
    const uint16_t cols = cfb_get_display_parameter(display_dev_, CFB_DISPLAY_COLS);

    LOG_INF("x_res %d, y_res %d, ppt %d, rows %d, cols %d", x_res_, y_res_, ppt, rows, cols);

    uint8_t font_width = 0;
    uint8_t font_height = 0;

    for(int idx = 0; idx < 42; idx++) {
        if(cfb_get_font_size(display_dev_, idx, &font_width, &font_height) != 0)
            break;

        LOG_INF("font %d: width %d, height %d", idx, font_width, font_height);
    }
}

} // namespace eerie_leap::subsys::cfb
