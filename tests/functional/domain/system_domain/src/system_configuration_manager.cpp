#include <algorithm>
#include <memory>
#include <span>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_system_config/cbor_system_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "domain/system_domain/configuration/system_configuration_manager.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"

using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::domain::system_domain::models;
using namespace eerie_leap::domain::system_domain::configuration;

ZTEST_SUITE(system_configuration_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(system_configuration_manager, test_SystemConfigurationManager_Save_config_successfully_saved) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_system_configuration_service = std::make_unique<CborConfigurationService<CborSystemConfig>>("system_config", fs_service);
    auto system_configuration_manager = std::make_shared<SystemConfigurationManager>(
        std::move(cbor_system_configuration_service));

    SystemConfiguration system_configuration(std::allocator_arg, Mrm::GetDefaultPmr());

    bool result = system_configuration_manager->Update(system_configuration);
    zassert_true(result);

    auto saved_system_configuration = system_configuration_manager->Get();

    zassert_equal(saved_system_configuration->hw_version, system_configuration.hw_version);
    zassert_equal(saved_system_configuration->sw_version, system_configuration.sw_version);
}

ZTEST(system_configuration_manager, test_SystemConfigurationManager_Save_config_and_Load) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_system_configuration_service = std::make_unique<CborConfigurationService<CborSystemConfig>>("system_config", fs_service);
    auto system_configuration_manager = std::make_shared<SystemConfigurationManager>(
        std::move(cbor_system_configuration_service));

    SystemConfiguration system_configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    system_configuration.device_id = 14;

    bool result = system_configuration_manager->Update(system_configuration);
    zassert_true(result);

    cbor_system_configuration_service = std::make_unique<CborConfigurationService<CborSystemConfig>>("system_config", fs_service);
    system_configuration_manager = nullptr;
    system_configuration_manager = std::make_shared<SystemConfigurationManager>(
        std::move(cbor_system_configuration_service));

    auto saved_system_configuration = system_configuration_manager->Get();

    zassert_equal(saved_system_configuration->device_id, system_configuration.device_id);
    zassert_equal(saved_system_configuration->hw_version, SystemConfiguration::hw_version);
    zassert_equal(saved_system_configuration->sw_version, SystemConfiguration::sw_version);
}

namespace {

std::shared_ptr<FsService> FormattedFs() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();
    return fs_service;
}

std::shared_ptr<SystemConfigurationManager> CreateManager(const std::shared_ptr<FsService>& fs_service) {
    return std::make_shared<SystemConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSystemConfig>>("system_config", fs_service));
}

uint32_t Crc(std::span<const uint8_t> data) {
    return crc32_ieee(data.data(), data.size());
}

// The same two-element array with the other length encoding than zcbor's.
std::vector<uint8_t> OtherArrayEncoding(std::span<const uint8_t> cbor) {
    constexpr uint8_t INDEFINITE_ARRAY = 0x9F;
    constexpr uint8_t BREAK = 0xFF;
    constexpr uint8_t TWO_ELEMENT_ARRAY = 0x82;

    if(cbor.front() == INDEFINITE_ARRAY) {
        zassert_equal(cbor.back(), BREAK);
        std::vector<uint8_t> other{TWO_ELEMENT_ARRAY};
        other.insert(other.end(), cbor.begin() + 1, cbor.end() - 1);
        return other;
    }

    zassert_equal(cbor.front(), TWO_ELEMENT_ARRAY);
    std::vector<uint8_t> other{INDEFINITE_ARRAY};
    other.insert(other.end(), cbor.begin() + 1, cbor.end());
    other.push_back(BREAK);
    return other;
}

} // namespace

ZTEST(system_configuration_manager, test_SystemConfigurationManager_ApplyCborConfiguration_keeps_the_senders_encoding) {
    auto fs_service = FormattedFs();
    auto manager = CreateManager(fs_service);

    const auto exported = manager->GetCborConfiguration();
    zassert_false(exported.empty());
    const auto received = OtherArrayEncoding(exported);

    zassert_true(manager->ApplyCborConfiguration(received));

    const auto stored = manager->GetCborConfiguration();
    zassert_true(std::ranges::equal(stored, received));
    zassert_equal(manager->GetCborConfigurationInfo().size, received.size());
    zassert_equal(manager->GetCborConfigurationInfo().crc, Crc(received));

    manager = CreateManager(fs_service);
    zassert_equal(manager->GetCborConfigurationInfo().crc, Crc(received), "The CRC survives a reload");
}

ZTEST(system_configuration_manager, test_SystemConfigurationManager_ApplyCborConfiguration_rejects_trailing_bytes) {
    auto fs_service = FormattedFs();
    auto manager = CreateManager(fs_service);

    const auto before = manager->GetCborConfigurationInfo();
    auto padded = manager->GetCborConfiguration();
    padded.push_back(0x00);

    zassert_false(manager->ApplyCborConfiguration(padded));
    zassert_equal(manager->GetCborConfigurationInfo().crc, before.crc);
}

ZTEST(system_configuration_manager, test_SystemConfigurationManager_Update_reports_the_crc_of_its_own_encoding) {
    auto fs_service = FormattedFs();
    auto manager = CreateManager(fs_service);
    zassert_true(manager->ApplyCborConfiguration(OtherArrayEncoding(manager->GetCborConfiguration())));

    SystemConfiguration system_configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    system_configuration.device_id = 21;
    zassert_true(manager->Update(system_configuration));

    const auto stored = manager->GetCborConfiguration();
    zassert_equal(manager->GetCborConfigurationInfo().size, stored.size());
    zassert_equal(manager->GetCborConfigurationInfo().crc, Crc(stored));
}
