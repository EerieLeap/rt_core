#pragma once

#include <memory>
#include <unordered_map>
#include <functional>

#include "canbus.h"

namespace eerie_leap::subsys::canbus {

class CanbusProxy {
public:
    using CanbusProxyUpdatedHandler = std::function<void(Canbus*)>;

private:
    std::unique_ptr<Canbus> canbus_;

    int next_handler_id_ = 1;
    std::unordered_map<int, CanbusProxyUpdatedHandler> updated_handlers_;

public:
    explicit CanbusProxy(std::unique_ptr<Canbus> canbus)
        : canbus_(std::move(canbus)) {}

    Canbus* operator->() const {
        return canbus_.get();
    }

    std::unique_ptr<Canbus> Release() {
        auto canbus = std::move(canbus_);
        for(const auto& [_, handler] : updated_handlers_)
            handler(canbus.get());

        return canbus;
    }

    void Update(std::unique_ptr<Canbus> canbus) {
        canbus_ = std::move(canbus);

        for(const auto& [_, handler] : updated_handlers_)
            handler(canbus_.get());
    }

    [[nodiscard]] bool IsValid() const {
        return canbus_ != nullptr;
    }

    int RegisterUpdatedHandler(CanbusProxyUpdatedHandler handler) {
        int handler_id = next_handler_id_++;
        updated_handlers_.try_emplace(handler_id, std::move(handler));

        return handler_id;
    }

    bool UnregisterUpdatedHandler(int handler_id) {
        return updated_handlers_.erase(handler_id) > 0;
    }
};

}
