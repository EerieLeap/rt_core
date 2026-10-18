#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>

#include "can_signal_configuration.h"
#include "subsys/lua_script/lua_script.h"

namespace eerie_leap::domain::canbus_domain::models {

using eerie_leap::subsys::lua_script::LuaScript;

struct CanMessageConfiguration {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    uint32_t frame_id = 0;
    bool is_enabled = true;
    std::optional<int> send_interval_ms = std::nullopt;
    std::pmr::string script_path;

    std::pmr::string name;
    uint32_t message_size = 0;
    std::pmr::vector<CanSignalConfiguration> signal_configurations;

    std::shared_ptr<LuaScript> lua_script = nullptr;

    CanMessageConfiguration(std::allocator_arg_t, allocator_type alloc) :
        script_path(alloc),
        name(alloc),
        signal_configurations(alloc) {}

    CanMessageConfiguration(const CanMessageConfiguration&) = delete;
    CanMessageConfiguration& operator=(const CanMessageConfiguration&) noexcept = default;
    CanMessageConfiguration& operator=(CanMessageConfiguration&&) noexcept = default;
    CanMessageConfiguration(CanMessageConfiguration&&) noexcept = default;
    ~CanMessageConfiguration() = default;

    CanMessageConfiguration(CanMessageConfiguration&& other, allocator_type alloc) :
        frame_id(other.frame_id),
        is_enabled(other.is_enabled),
        send_interval_ms(other.send_interval_ms),
        script_path(std::move(other.script_path), alloc),
        name(std::move(other.name), alloc),
        message_size(other.message_size),
        signal_configurations(std::move(other.signal_configurations), alloc),
        lua_script(std::move(other.lua_script)) {}

    const CanSignalConfiguration* TryGetSignal(uint32_t signal_name_hash) const {
        for(const auto& signal_configuration : signal_configurations) {
            if(signal_configuration.name_hash == signal_name_hash)
                return &signal_configuration;
        }

        return nullptr;
    }
};

} // namespace eerie_leap::domain::canbus_domain::models
