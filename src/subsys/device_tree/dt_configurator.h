#pragma once

#include "subsys/device_tree/dt_feature.h"

namespace eerie_leap::subsys::device_tree {

class DtConfigurator {
private:
    DtConfigurator() = default;

public:
    static void Initialize(DtFeature features);
};

} // namespace eerie_leap::subsys::device_tree
