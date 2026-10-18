#pragma once

#include <eerie_memory.hpp>
#include <memory>

namespace eerie_leap::configuration::services {

template<typename T>
struct LoadedConfig {
public:
    std::pmr::vector<uint8_t> config_raw;
    eerie_memory::PmrUniquePtr<T> config;
    uint32_t checksum{ 0 };
};

} // namespace eerie_leap::configuration::services
