#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config_cbor_decode.h"
#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config_cbor_encode.h"
#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config_size.h"
#include "configuration/cbor/cbor_trait.h"

namespace eerie_leap::configuration::cbor::traits {

static struct CborLoggingControlConfigTraitRegistrar {
    CborLoggingControlConfigTraitRegistrar() {
        CborTraitRegistry::Register<CborLoggingControlConfig>(
            cbor_encode_CborLoggingControlConfig, cbor_decode_CborLoggingControlConfig,
            cbor_get_size_CborLoggingControlConfig
        );
    }
} CborLoggingControlConfigTraitRegistrar;

} // namespace eerie_leap::configuration::cbor::traits
