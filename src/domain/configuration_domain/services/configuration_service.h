#pragma once

#include <memory>
#include <span>
#include <unordered_map>
#include <streambuf>
#include <functional>

#include "../utilities/i_cbor_configuration_manager.h"

namespace eerie_leap::domain::configuration_domain::services {

using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;
using eerie_leap::domain::configuration_domain::utilities::StoredCborInfo;

/**
 * @brief Routes CBOR configuration import and export to the manager registered for each type.
 *
 * Managers are registered during startup; the queries are safe from any thread afterwards.
 */
class ConfigurationService {
public:
    enum class Type : uint8_t {
        System = 0x01,
        Adc = 0x02,
        Canbus = 0x03,
        Sensors = 0x04,
        Logging = 0x05,
        Ui = 0x06,
        Display = 0x07,
    };

    /// Largest CBOR configuration accepted for a type unless it registers another cap.
    static constexpr size_t DEFAULT_MAX_CBOR_SIZE = 64 * 1024;

private:
    struct Registration {
        std::shared_ptr<ICborConfigurationManager> manager;
        size_t max_cbor_size;
    };

    std::unordered_map<Type, Registration> cbor_configuration_managers_;

public:
    ConfigurationService() = default;
    virtual ~ConfigurationService() = default;

    /**
     * @param reason Receives the manager's reason when it rejects or fails to store the data.
     * @return false for an unregistered type, or when the manager rejects or fails to store the data.
     */
    bool ApplyCborConfiguration(Type type, std::span<const uint8_t> cbor_data, std::span<char> reason = {});
    /** @return The current configuration, empty for an unregistered type. */
    std::pmr::vector<uint8_t> GetCborConfiguration(Type type);
    /** @return Size and CRC32 of the stored configuration, zero for an unregistered type. */
    [[nodiscard]] StoredCborInfo GetCborConfigurationInfo(Type type) const;

    /** @param max_cbor_size Cap on imported CBOR, which bounds the staging buffer of a transfer. */
    void RegisterCborConfigurationManager(
        Type type,
        std::shared_ptr<ICborConfigurationManager> cbor_configuration_manager,
        size_t max_cbor_size = DEFAULT_MAX_CBOR_SIZE);

    [[nodiscard]] bool IsRegistered(Type type) const;
    /** @return The size cap of @p type, 0 for an unregistered type. */
    [[nodiscard]] size_t GetMaxCborSize(Type type) const;

    /**
     * @brief Copies the registered types, in ascending order, into a caller-owned buffer.
     * @return The number of types copied, at most @p types.size().
     */
    size_t GetRegisteredTypes(std::span<Type> types) const;
};

} // namespace eerie_leap::domain::configuration_domain::services
