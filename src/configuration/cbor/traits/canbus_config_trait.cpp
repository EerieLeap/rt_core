#include <configuration/cbor/cbor_canbus_config/cbor_canbus_config.h>
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config_cbor_decode.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config_cbor_encode.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config_size.h"
#include "configuration/cbor/cbor_trait.h"

namespace eerie_leap::configuration::cbor::traits {

namespace {

struct CborCanbusConfigTraitRegistrar {
    CborCanbusConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborCanbusConfig>(
            cbor_encode_CborCanbusConfig, cbor_decode_CborCanbusConfig, cbor_get_size_CborCanbusConfig
        );
    }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the registry insert can only throw std::bad_alloc, fatal before main
const CborCanbusConfigTraitRegistrar CBOR_CANBUS_CONFIG_TRAIT_REGISTRAR;

} // namespace

} // namespace eerie_leap::configuration::cbor::traits
