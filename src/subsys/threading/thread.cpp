#include "thread.h"

namespace eerie_leap::subsys::threading {

Thread::Thread(
    std::string name,
    IThread* instance,
    int stack_size,
    int priority,
    bool is_cooperative,
    std::pmr::memory_resource* resource)
        : ThreadBase(std::move(name), stack_size, priority, is_cooperative, resource),
        instance_(instance),
        is_running_(ATOMIC_INIT(0)),
        is_initialized_(false) {}

void Thread::Initialize() {
    InitializeStack();
    is_initialized_ = true;
}

void Thread::Start() {
    if(!is_initialized_)
        return;

    if(atomic_get(&is_running_) != 0)
        return;

    k_thread_create(
        &thread_,
        stack_area_,
        k_stack_size_,
        [](void* instance, void* p2, void* p3) {
            static_cast<IThread*>(instance)->ThreadEntry(); },
        instance_, nullptr, nullptr,
        k_priority_, 0, K_NO_WAIT);

    k_thread_name_set(&thread_, name_.c_str());
    atomic_set(&is_running_, 1);
}

[[nodiscard]] k_thread* Thread::GetThread() {
    if(atomic_get(&is_running_) == 0)
        return nullptr;

    return &thread_;
}

void Thread::Join() {
    if(!is_initialized_)
        return;

    if(atomic_get(&is_running_) == 0)
        return;

    atomic_set(&is_running_, 0);
    k_thread_join(&thread_, K_FOREVER);
}

bool Thread::IsRunning() const noexcept {
    return atomic_get(&is_running_) != 0;
}

} // namespace eerie_leap::subsys::threading
