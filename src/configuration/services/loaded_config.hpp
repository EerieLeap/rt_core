#pragma once

#include <memory>
#include <eerie_memory.hpp>

namespace eerie_leap::configuration::services {

template <typename T>
struct LoadedConfig {
public:
    std::pmr::vector<uint8_t> config_raw;
    eerie_memory::pmr_unique_ptr<T> config;
    uint32_t checksum;
};

}
