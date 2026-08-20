#include "thread.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(thread);

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
        is_initialized_(false),
        is_created_(false),
        is_running_(ATOMIC_INIT(0)) {}

Thread::~Thread() {
    Join();
}

bool Thread::Initialize() {
    is_initialized_ = InitializeStack();

    return is_initialized_;
}

bool Thread::Start() {
    if(!is_initialized_)
        return false;

    if(!atomic_cas(&is_running_, 0, 1))
        return false;

    // A previous run has to be reaped before its thread object can be handed out again.
    if(is_created_)
        k_thread_join(&thread_, K_FOREVER);

    k_thread_create(
        &thread_,
        stack_area_,
        k_stack_size_,
        [](void* instance, void* is_running, void* name) {
            try {
                static_cast<IThread*>(instance)->ThreadEntry();
            } catch(const std::exception& e) {
                LOG_ERR("Thread %s threw: %s", static_cast<const char*>(name), e.what());
            } catch(...) {
                LOG_ERR("Thread %s threw an unknown exception", static_cast<const char*>(name));
            }
            atomic_clear(static_cast<atomic_t*>(is_running));
        },
        instance_, &is_running_, const_cast<char*>(name_.c_str()),
        k_priority_, 0, K_NO_WAIT);

    is_created_ = true;

    k_thread_name_set(&thread_, name_.c_str());

    return true;
}

[[nodiscard]] k_thread* Thread::GetThread() {
    if(!is_created_)
        return nullptr;

    return &thread_;
}

void Thread::Join() {
    if(!is_created_)
        return;

    atomic_clear(&is_running_);
    k_thread_join(&thread_, K_FOREVER);
}

bool Thread::IsRunning() const noexcept {
    return atomic_get(&is_running_) != 0;
}

} // namespace eerie_leap::subsys::threading
