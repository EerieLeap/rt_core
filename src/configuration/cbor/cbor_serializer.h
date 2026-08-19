#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_instance.h>
#include <zcbor_common.h>
#include <zcbor_encode.h>
#include <zcbor_decode.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_trait.h"

namespace eerie_leap::configuration::cbor {

using eerie_leap::utilities::memory::Mrm;

template <typename T>
class CborSerializer {
public:
    CborSerializer() {
        auto funcs = CborTraitRegistry::Get<T>();
        encodeFn_ = funcs.encode;
        decodeFn_ = funcs.decode;
        getSerializingSizeFn_ = funcs.get_size;
    }

    std::pmr::vector<uint8_t> Serialize(const T& obj, size_t *payload_len_out = nullptr) {
        LOG_MODULE_DECLARE(cbor_serializer_logger);

        std::pmr::vector<uint8_t> buffer(getSerializingSizeFn_(obj), Mrm::GetExtPmr());

        size_t obj_size = 0;
        if(encodeFn_(buffer.data(), buffer.size(), &obj, &obj_size)) {
            LOG_ERR("Failed to encode object.");
            return {};
        }

        buffer.resize(obj_size);

        if (payload_len_out != nullptr)
            *payload_len_out = obj_size;

        return buffer;
    }

    eerie_memory::pmr_unique_ptr<T> Deserialize(std::span<const uint8_t> input) {
        LOG_MODULE_DECLARE(cbor_serializer_logger);

        auto obj = eerie_memory::make_unique_pmr<T>(Mrm::GetExtPmr());
        if(decodeFn_(input.data(), input.size(), obj.get(), nullptr)) {
            LOG_ERR("Failed to decode object.");
            return nullptr;
        }

        return obj;
    }

private:
    CborTraitRegistry::CborEncodeFn<T> encodeFn_;
    CborTraitRegistry::CborDecodeFn<T> decodeFn_;
    CborTraitRegistry::CborGetSerializingSizeFn<T> getSerializingSizeFn_;
};

} // namespace eerie_leap::configuration::cbor
