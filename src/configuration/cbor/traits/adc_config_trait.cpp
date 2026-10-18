#include <configuration/cbor/cbor_adc_config/cbor_adc_config.h>
#include "configuration/cbor/cbor_adc_config/cbor_adc_config_cbor_decode.h"
#include "configuration/cbor/cbor_adc_config/cbor_adc_config_cbor_encode.h"
#include "configuration/cbor/cbor_adc_config/cbor_adc_config_size.h"
#include "configuration/cbor/cbor_trait.h"

namespace eerie_leap::configuration::cbor::traits {

namespace {

struct CborAdcConfigTraitRegistrar {
    CborAdcConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborAdcConfig>(
            cbor_encode_CborAdcConfig, cbor_decode_CborAdcConfig, cbor_get_size_CborAdcConfig
        );
    }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the registry insert can only throw std::bad_alloc, fatal before main
const CborAdcConfigTraitRegistrar CBOR_ADC_CONFIG_TRAIT_REGISTRAR;

} // namespace

} // namespace eerie_leap::configuration::cbor::traits
