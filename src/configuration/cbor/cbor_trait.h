#pragma once

#include <cstddef>
#include <cstdint>
#include <typeindex>
#include <unordered_map>

namespace eerie_leap::configuration::cbor {

struct CborTraitFunctions {
    void* encode;
    void* decode;
    void* get_size;
};

class CborTraitRegistry {
public:
    template<typename T>
    using CborEncodeFn = int (*)(uint8_t*, size_t, const T*, size_t*);
    template<typename T>
    using CborDecodeFn = int (*)(const uint8_t*, size_t, T*, size_t*);
    template<typename T>
    using CborGetSerializingSizeFn = size_t (*)(const T&);

    template<typename T>
    static void Register(CborEncodeFn<T> encode, CborDecodeFn<T> decode, CborGetSerializingSizeFn<T> get_size) {
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): function pointers of any T are stored type-erased as void* and cast back to the same type in Get<T>()
        registry[std::type_index(typeid(T))] = { .encode = reinterpret_cast<void*>(encode),
            .decode = reinterpret_cast<void*>(decode),
            .get_size = reinterpret_cast<void*>(get_size) };
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    }

    template<typename T>
    static auto Get() {
        struct Result {
            CborEncodeFn<T> encode;
            CborDecodeFn<T> decode;
            CborGetSerializingSizeFn<T> get_size;
        };

        const auto& funcs = registry.at(std::type_index(typeid(T)));
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the void* were stored from the same T in Register<T>()
        return Result{ .encode = reinterpret_cast<CborEncodeFn<T>>(funcs.encode),
            .decode = reinterpret_cast<CborDecodeFn<T>>(funcs.decode),
            .get_size = reinterpret_cast<CborGetSerializingSizeFn<T>>(funcs.get_size) };
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    }

private:
    static inline std::unordered_map<std::type_index, CborTraitFunctions> registry;
};

} // namespace eerie_leap::configuration::cbor
