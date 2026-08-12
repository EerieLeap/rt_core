#pragma once

#include <string>

#include <zephyr/sys/atomic.h>

#include "thread_base.h"
#include "i_thread.h"

namespace eerie_leap::subsys::threading {

class Thread : public ThreadBase {
private:
    IThread* instance_;
    k_thread thread_;
    bool is_initialized_;
    bool is_created_;
    atomic_t is_running_;

public:
    Thread(
        std::string name,
        IThread* instance,
        int stack_size,
        int priority,
        bool is_cooperative = false,
        std::pmr::memory_resource* resource = nullptr);
    ~Thread() override;

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    bool Initialize();
    bool Start();

    [[nodiscard]] k_thread* GetThread();
    void Join();

    [[nodiscard]] bool IsRunning() const noexcept;
};

} // namespace eerie_leap::subsys::threading
