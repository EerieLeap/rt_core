#pragma once

#include <string>

namespace eerie_leap::subsys::mdf {

// One <e name="...">value</e> entry of a <common_properties> metadata section.
struct MdfProperty {
    std::string name;
    std::string value;
};

} // namespace eerie_leap::subsys::mdf
