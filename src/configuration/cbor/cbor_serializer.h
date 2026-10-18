#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <eerie_memory.hpp>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_instance.h>

#include "configuration/cbor/cbor_trait.h"
#include "utilities/memory/memory_resource_manager.h"

namespace eerie_leap::configuration::cbor {

using eerie_leap::utilities::memory::Mrm;

template<typename T>
class CborSerializer {
public:
    CborSerializer() {
        const auto funcs = CborTraitRegistry::Get<T>();
        encode_fn_ = funcs.encode;
        decode_fn_ = funcs.decode;
        get_serializing_size_fn_ = funcs.get_size;
    }

    std::pmr::vector<uint8_t> Serialize(const T& obj, size_t* payload_len_out = nullptr) {
        // NOLINTNEXTLINE(misc-const-correctness): the log_source_dynamic_data pointer is declared by the Zephyr macro
        LOG_MODULE_DECLARE(cbor_serializer_logger);

        std::pmr::vector<uint8_t> buffer(get_serializing_size_fn_(obj), Mrm::GetExtPmr());

        size_t obj_size = 0;
        if(encode_fn_(buffer.data(), buffer.size(), &obj, &obj_size)) {
            LOG_ERR("Failed to encode object.");
            return {};
        }

        buffer.resize(obj_size);

        if(payload_len_out != nullptr)
            *payload_len_out = obj_size;

        return buffer;
    }

    eerie_memory::pmr_unique_ptr<T> Deserialize(std::span<const uint8_t> input) {
        // NOLINTNEXTLINE(misc-const-correctness): the log_source_dynamic_data pointer is declared by the Zephyr macro
        LOG_MODULE_DECLARE(cbor_serializer_logger);

        auto obj = eerie_memory::make_unique_pmr<T>(Mrm::GetExtPmr());
        size_t decoded_size = 0;
        // Trailing bytes are rejected, since validated input may be stored as it is.
        if(decode_fn_(input.data(), input.size(), obj.get(), &decoded_size) || decoded_size != input.size()) {
            LOG_ERR("Failed to decode object.");
            return nullptr;
        }

        return obj;
    }

private:
    CborTraitRegistry::CborEncodeFn<T> encode_fn_;
    CborTraitRegistry::CborDecodeFn<T> decode_fn_;
    CborTraitRegistry::CborGetSerializingSizeFn<T> get_serializing_size_fn_;
};

} // namespace eerie_leap::configuration::cbor
