#pragma once

#include <zephyr/kernel.h>

namespace eerie_leap::subsys::threading {

class ScopedMutex {
private:
    k_mutex* mutex_;

public:
    explicit ScopedMutex(k_mutex& mutex) : mutex_(&mutex) { k_mutex_lock(mutex_, K_FOREVER); }
    ~ScopedMutex() { k_mutex_unlock(mutex_); }

    ScopedMutex(const ScopedMutex&) = delete;
    ScopedMutex& operator=(const ScopedMutex&) = delete;
    ScopedMutex(ScopedMutex&&) = delete;
    ScopedMutex& operator=(ScopedMutex&&) = delete;
};

} // namespace eerie_leap::subsys::threading
