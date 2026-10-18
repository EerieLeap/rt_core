#include <algorithm>
#include <utility>

#include <zephyr/kernel.h>

#include "subsys/threading/scoped_mutex.h"

#include "configuration_service.h"

namespace eerie_leap::domain::configuration_domain::services {

using eerie_leap::subsys::threading::ScopedMutex;

// A store copies the handler under the lock and calls it outside, so a handler cleared during its
// call finishes that call, and a call allocates nothing.
class ConfigurationService::StoredHandlerSlot {
private:
    k_mutex mutex_;
    std::shared_ptr<const ConfigurationStoredHandler> handler_;

public:
    StoredHandlerSlot() {
        k_mutex_init(&mutex_);
    }

    void Set(ConfigurationStoredHandler handler) {
        std::shared_ptr<const ConfigurationStoredHandler> slot;
        if(handler)
            slot = std::make_shared<const ConfigurationStoredHandler>(std::move(handler));

        ScopedMutex guard(mutex_);
        handler_.swap(slot);
    }

    void Notify(Type type) {
        std::shared_ptr<const ConfigurationStoredHandler> handler;
        {
            ScopedMutex guard(mutex_);
            handler = handler_;
        }

        if(handler != nullptr)
            (*handler)(type);
    }
};

ConfigurationService::ConfigurationService() :
    stored_handler_slot_(std::make_shared<StoredHandlerSlot>()) {}

ConfigurationService::~ConfigurationService() = default;

bool ConfigurationService::ApplyCborConfiguration(
    Type type,
    std::span<const uint8_t> cbor_data,
    std::span<char> reason
) {

    if(!cbor_configuration_managers_.contains(type))
        return false;

    return cbor_configuration_managers_.at(type).manager->ApplyCborConfiguration(cbor_data, reason);
}

std::pmr::vector<uint8_t> ConfigurationService::GetCborConfiguration(Type type) {
    if(!cbor_configuration_managers_.contains(type))
        return {};

    return cbor_configuration_managers_.at(type).manager->GetCborConfiguration();
}

StoredCborInfo ConfigurationService::GetCborConfigurationInfo(Type type) const {
    const auto it = cbor_configuration_managers_.find(type);

    return it != cbor_configuration_managers_.end() ? it->second.manager->GetCborConfigurationInfo() : StoredCborInfo{};
}

void ConfigurationService::RegisterCborConfigurationManager(
    Type type,
    std::shared_ptr<ICborConfigurationManager> cbor_configuration_manager,
    size_t max_cbor_size
) {

    const auto [it, is_inserted] = cbor_configuration_managers_.try_emplace(
        type, Registration{ .manager = std::move(cbor_configuration_manager), .max_cbor_size = max_cbor_size }
    );

    // The first manager of a type; a refused second one is not wired. The handler holds the slot, not
    // the service, so a manager that outlives the service reports into a live object.
    if(is_inserted && it->second.manager != nullptr)
        it->second.manager->SetStoredHandler([slot = stored_handler_slot_, type] {
            slot->Notify(type);
        });
}

void ConfigurationService::RegisterConfigurationStoredHandler(ConfigurationStoredHandler handler) {
    stored_handler_slot_->Set(std::move(handler));
}

bool ConfigurationService::IsRegistered(Type type) const {
    return cbor_configuration_managers_.contains(type);
}

size_t ConfigurationService::GetMaxCborSize(Type type) const {
    const auto it = cbor_configuration_managers_.find(type);

    return it != cbor_configuration_managers_.end() ? it->second.max_cbor_size : 0;
}

size_t ConfigurationService::GetRegisteredTypes(std::span<Type> types) const {
    size_t count = 0;
    for(const auto& [type, _] : cbor_configuration_managers_) {
        if(count == types.size())
            break;

        types[count++] = type;
    }

    std::sort(types.begin(), types.begin() + count);

    return count;
}

} // namespace eerie_leap::domain::configuration_domain::services
