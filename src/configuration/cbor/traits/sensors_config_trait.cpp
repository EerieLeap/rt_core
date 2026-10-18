#include <configuration/cbor/cbor_sensors_config/cbor_sensors_config.h>
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config_cbor_decode.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config_cbor_encode.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config_size.h"
#include "configuration/cbor/cbor_trait.h"

namespace eerie_leap::configuration::cbor::traits {

namespace {

struct CborSensorsConfigTraitRegistrar {
    CborSensorsConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborSensorsConfig>(
            cbor_encode_CborSensorsConfig, cbor_decode_CborSensorsConfig, cbor_get_size_CborSensorsConfig
        );
    }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the registry insert can only throw std::bad_alloc, fatal before main
const CborSensorsConfigTraitRegistrar CBOR_SENSORS_CONFIG_TRAIT_REGISTRAR;

} // namespace

} // namespace eerie_leap::configuration::cbor::traits
