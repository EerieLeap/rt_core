#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include <zephyr/logging/log.h>
#include <eerie_memory.hpp>

#include "configuration/services/cbor_configuration_service.h"

#include "i_cbor_configuration_manager.h"

namespace eerie_leap::domain::configuration_domain::utilities {

// Derived classes call LoadOrCreateDefault() from their constructor, once their parser exists.
template <typename TConfiguration, typename TCborConfig>
class CborConfigurationManagerBase : public ICborConfigurationManager {
private:
    const char* name_;
    std::unique_ptr<eerie_leap::configuration::services::CborConfigurationService<TCborConfig>> cbor_configuration_service_;
    std::shared_ptr<TConfiguration> configuration_;

    // Both validate; Deserialize must accept everything Serialize produces.
    virtual eerie_memory::pmr_unique_ptr<TCborConfig> Serialize(const TConfiguration& configuration) = 0;
    virtual eerie_memory::pmr_unique_ptr<TConfiguration> Deserialize(const TCborConfig& cbor_config) = 0;
    virtual bool CreateDefaultConfiguration() = 0;
    virtual void OnConfigurationChanged(const std::shared_ptr<TConfiguration>&) {}

    // Takes over the parsed allocation instead of moving it into a new one.
    static std::shared_ptr<TConfiguration> Share(eerie_memory::pmr_unique_ptr<TConfiguration> configuration) {
        auto deleter = configuration.get_deleter();
        std::pmr::polymorphic_allocator<> control_block_allocator(deleter.get_memory_resource());

        return std::shared_ptr<TConfiguration>(configuration.release(), deleter, control_block_allocator);
    }

    void SetConfiguration(std::shared_ptr<TConfiguration> configuration) {
        configuration_ = std::move(configuration);
        OnConfigurationChanged(configuration_);
    }

protected:
    CborConfigurationManagerBase(
        const char* name,
        std::unique_ptr<eerie_leap::configuration::services::CborConfigurationService<TCborConfig>> cbor_configuration_service)
            : name_(name), cbor_configuration_service_(std::move(cbor_configuration_service)) {}

    // Returns whether a configuration is available afterwards.
    bool LoadOrCreateDefault() {
        LOG_MODULE_DECLARE(configuration_manager_logger);

        try {
            if(Get(true) != nullptr) {
                LOG_INF("%s configuration loaded successfully.", name_);
                return true;
            }
        } catch(const std::exception& e) {
            LOG_ERR("Failed to load %s configuration. %s", name_, e.what());
        } catch(...) {
            LOG_ERR("Failed to load %s configuration.", name_);
        }

        if(!CreateDefaultConfiguration()) {
            LOG_ERR("Failed to create default %s configuration.", name_);
            return false;
        }

        LOG_INF("Default %s configuration created successfully.", name_);

        return true;
    }

    // Keeps the given instance, so it is validated only once; callers must not modify it afterwards.
    bool Adopt(std::shared_ptr<TConfiguration> configuration) {
        LOG_MODULE_DECLARE(configuration_manager_logger);

        try {
            auto cbor_config = Serialize(*configuration);

            if(!cbor_configuration_service_->Save(cbor_config.get()))
                return false;
        } catch(const std::exception& e) {
            LOG_ERR("Failed to update %s configuration. %s", name_, e.what());
            return false;
        }

        SetConfiguration(std::move(configuration));

        return true;
    }

public:
    std::shared_ptr<TConfiguration> Get(bool force_load = false) {
        if(configuration_ != nullptr && !force_load)
            return configuration_;

        auto loaded_config = cbor_configuration_service_->Load();
        if(!loaded_config.has_value())
            return nullptr;

        // loaded_config still owns the bytes that decoded strings point into.
        SetConfiguration(Share(Deserialize(*loaded_config->config)));

        return configuration_;
    }

    bool Update(const TConfiguration& configuration) {
        LOG_MODULE_DECLARE(configuration_manager_logger);

        std::shared_ptr<TConfiguration> updated_configuration;

        try {
            auto cbor_config = Serialize(configuration);

            // Parsing the encoding back gives what a reload would, and nothing unparsable is saved.
            updated_configuration = Share(Deserialize(*cbor_config));

            if(!cbor_configuration_service_->Save(cbor_config.get()))
                return false;
        } catch(const std::exception& e) {
            LOG_ERR("Failed to update %s configuration. %s", name_, e.what());
            return false;
        }

        SetConfiguration(std::move(updated_configuration));

        return true;
    }

    using ICborConfigurationManager::ApplyCborConfiguration;

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override {
        LOG_MODULE_DECLARE(configuration_manager_logger);

        auto cbor_config = cbor_configuration_service_->Deserialize(cbor_data);
        if(cbor_config == nullptr) {
            SetReason(reason, "Malformed CBOR");
            return false;
        }

        std::shared_ptr<TConfiguration> configuration;

        try {
            configuration = Share(Deserialize(*cbor_config));

            // Stored as received, so its CRC stays the one the sender computed.
            if(!cbor_configuration_service_->Save(cbor_data)) {
                SetReason(reason, "Could not store the configuration");
                return false;
            }
        } catch(const std::exception& e) {
            LOG_ERR("Failed to apply %s CBOR configuration. %s", name_, e.what());
            SetReason(reason, e.what());
            return false;
        }

        SetConfiguration(std::move(configuration));

        LOG_INF("%s CBOR configuration applied successfully.", name_);

        return true;
    }

    std::pmr::vector<uint8_t> GetCborConfiguration() override {
        if(Get() == nullptr)
            return {};

        auto config_bytes = cbor_configuration_service_->LoadRaw();
        if(!config_bytes.has_value())
            return {};

        return std::move(*config_bytes);
    }

    StoredCborInfo GetCborConfigurationInfo() override {
        if(configuration_ == nullptr)
            return {};

        return cbor_configuration_service_->GetStoredInfo();
    }
};

} // namespace eerie_leap::domain::configuration_domain::utilities
