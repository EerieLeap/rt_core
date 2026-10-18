#include "memory_resource_manager.h"

namespace eerie_leap::utilities::memory {

ExtMemoryResource Mrm::ext_memory_resource;

std::pmr::memory_resource* Mrm::GetDefaultPmr() {
    return std::pmr::get_default_resource();
}

std::pmr::memory_resource* Mrm::GetExtPmr() {
    return &ext_memory_resource;
}

} // namespace eerie_leap::utilities::memory
