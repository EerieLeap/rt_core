#pragma once

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <functional>

#include <zephyr/kernel.h>

#include "subsys/threading/scoped_mutex.h"

#include "canbus.h"

namespace eerie_leap::subsys::canbus {

using eerie_leap::subsys::threading::ScopedMutex;

// Hands out the active Canbus instance to consumers that outlive a
// reconfiguration. Access is serialised so a Release()/Update() from the
// configuration thread cannot be observed half applied.
class CanbusProxy {
public:
    using CanbusProxyUpdatedHandler = std::function<void(Canbus*)>;

private:
    mutable k_mutex lock_;
    std::unique_ptr<Canbus> canbus_;

    int next_handler_id_ = 1;
    std::unordered_map<int, CanbusProxyUpdatedHandler> updated_handlers_;

    void NotifyUpdated(Canbus* canbus) const {
        for(const auto& [_, handler] : updated_handlers_)
            handler(canbus);
    }

public:
    explicit CanbusProxy(std::unique_ptr<Canbus> canbus)
        : canbus_(std::move(canbus)) { k_mutex_init(&lock_); }

    CanbusProxy(const CanbusProxy&) = delete;
    CanbusProxy& operator=(const CanbusProxy&) = delete;
    CanbusProxy(CanbusProxy&&) = delete;
    CanbusProxy& operator=(CanbusProxy&&) = delete;

    Canbus* operator->() const {
        ScopedMutex guard(lock_);

        if(canbus_ == nullptr)
            throw std::runtime_error("CANBus proxy holds no instance.");

        return canbus_.get();
    }

    [[nodiscard]] Canbus* Get() const {
        ScopedMutex guard(lock_);

        return canbus_.get();
    }

    std::unique_ptr<Canbus> Release() {
        ScopedMutex guard(lock_);

        auto canbus = std::move(canbus_);
        canbus_ = nullptr;
        NotifyUpdated(nullptr);

        return canbus;
    }

    void Update(std::unique_ptr<Canbus> canbus) {
        ScopedMutex guard(lock_);

        canbus_ = std::move(canbus);
        NotifyUpdated(canbus_.get());
    }

    [[nodiscard]] bool IsValid() const {
        ScopedMutex guard(lock_);

        return canbus_ != nullptr;
    }

    int RegisterUpdatedHandler(CanbusProxyUpdatedHandler handler) {
        ScopedMutex guard(lock_);

        int handler_id = next_handler_id_++;
        updated_handlers_.try_emplace(handler_id, std::move(handler));

        return handler_id;
    }

    bool UnregisterUpdatedHandler(int handler_id) {
        ScopedMutex guard(lock_);

        return updated_handlers_.erase(handler_id) > 0;
    }
};

}
