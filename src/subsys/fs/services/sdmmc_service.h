#pragma once

#include <memory>
#include <functional>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/atomic.h>

#include "subsys/threading/thread.h"

#include "fs_service.h"

namespace eerie_leap::subsys::fs::services {

using eerie_leap::subsys::threading::IThread;
using eerie_leap::subsys::threading::Thread;

class SdmmcService : public FsService, public IThread {
private:
    static constexpr int k_stack_size_ = CONFIG_EERIE_LEAP_FS_SD_THREAD_STACK_SIZE;
    static constexpr int k_priority_ = 2;
    std::unique_ptr<Thread> thread_;

    const char* disk_name_;
    bool sd_card_present_ = false;
    k_sem sd_monitor_stop_sem_;
    atomic_t monitor_running_;

    std::function<bool()> _is_sd_card_present_handler;

    void ThreadEntry() override;

    static bool IsSdCardAttached(const char* disk_name);
    void SdMonitorHandler();
    int PrintInfo() const;

public:
    SdmmcService(fs_mount_t mountpoint, const char* disk_name);
    ~SdmmcService();

    bool Initialize() override;
    void RegisterIsSdCardPresentHandler(std::function<bool()> handler);
    bool IsAvailable() const override;

    int SdMonitorStart();
    int SdMonitorStop();
};

} // namespace eerie_leap::subsys::fs::services
