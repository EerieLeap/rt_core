#pragma once

#include "memory_resource.h"

namespace eerie_leap::utilities::memory {

class Mrm {
private:
    static ExtMemoryResource ext_memory_resource_;

public:
    static std::pmr::memory_resource* GetDefaultPmr();
    static std::pmr::memory_resource* GetExtPmr();
};

} // namespace eerie_leap::utilities::memory
