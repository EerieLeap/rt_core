#pragma once

#include <memory_resource>
#include <string>

#include <zephyr/kernel.h>

namespace eerie_leap::subsys::threading {

class ThreadBase {
protected:
    std::pmr::memory_resource* mr_;
    int k_stack_size_;
    int k_priority_;
    k_thread_stack_t* stack_area_{ nullptr };
    std::string name_;

public:
    ThreadBase(
        std::string name,
        int stack_size,
        int priority,
        bool is_cooperative = false,
        std::pmr::memory_resource* mr = nullptr
    );
    virtual ~ThreadBase();

    ThreadBase(const ThreadBase&) = delete;
    ThreadBase& operator=(const ThreadBase&) = delete;
    ThreadBase(ThreadBase&&) = delete;
    ThreadBase& operator=(ThreadBase&&) = delete;

    bool InitializeStack();
    [[nodiscard]] const k_thread_stack_t* GetStack() const;
};

} // namespace eerie_leap::subsys::threading
