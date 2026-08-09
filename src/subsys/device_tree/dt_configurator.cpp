#include "dt_fs.h"
#include "dt_gpio.h"
#include "dt_adc.h"
#include "dt_display.h"
#include "dt_canbus.h"

#include "dt_configurator.h"

namespace eerie_leap::subsys::device_tree {

void DtConfigurator::Initialize(DtFeature features) {
    if(HasDtFeature(features, DtFeature::INTERNAL_FS))
        DtFs::InitInternalFs();

    if(HasDtFeature(features, DtFeature::SD_FS))
        DtFs::InitSdFs();

    if(HasDtFeature(features, DtFeature::GPIO))
        DtGpio::Initialize();

    if(HasDtFeature(features, DtFeature::ADC))
        DtAdc::Initialize();

    if(HasDtFeature(features, DtFeature::DISPLAY))
        DtDisplay::Initialize();

    if(HasDtFeature(features, DtFeature::CANBUS))
        DtCanbus::Initialize();
}

} // namespace eerie_leap::subsys::device_tree
