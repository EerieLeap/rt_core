#include <configuration/cbor/cbor_system_config/cbor_system_config.h>
#include "configuration/cbor/cbor_system_config/cbor_system_config_cbor_decode.h"
#include "configuration/cbor/cbor_system_config/cbor_system_config_cbor_encode.h"
#include "configuration/cbor/cbor_system_config/cbor_system_config_size.h"
#include "configuration/cbor/cbor_trait.h"

namespace eerie_leap::configuration::cbor::traits {

namespace {

struct CborSystemConfigTraitRegistrar {
    CborSystemConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborSystemConfig>(
            cbor_encode_CborSystemConfig, cbor_decode_CborSystemConfig, cbor_get_size_CborSystemConfig
        );
    }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the registry insert can only throw std::bad_alloc, fatal before main
const CborSystemConfigTraitRegistrar CBOR_SYSTEM_CONFIG_TRAIT_REGISTRAR;

} // namespace

} // namespace eerie_leap::configuration::cbor::traits
