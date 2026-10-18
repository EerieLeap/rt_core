#include <configuration/cbor/cbor_logging_config/cbor_logging_config.h>
#include "configuration/cbor/cbor_logging_config/cbor_logging_config_cbor_decode.h"
#include "configuration/cbor/cbor_logging_config/cbor_logging_config_cbor_encode.h"
#include "configuration/cbor/cbor_logging_config/cbor_logging_config_size.h"
#include "configuration/cbor/cbor_trait.h"

namespace eerie_leap::configuration::cbor::traits {

namespace {

struct CborLoggingConfigTraitRegistrar {
    CborLoggingConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborLoggingConfig>(
            cbor_encode_CborLoggingConfig, cbor_decode_CborLoggingConfig, cbor_get_size_CborLoggingConfig
        );
    }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the registry insert can only throw std::bad_alloc, fatal before main
const CborLoggingConfigTraitRegistrar CBOR_LOGGING_CONFIG_TRAIT_REGISTRAR;

} // namespace

} // namespace eerie_leap::configuration::cbor::traits
