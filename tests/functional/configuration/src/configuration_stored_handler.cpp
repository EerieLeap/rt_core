#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <eerie_memory.hpp>

#include "configuration/cbor/cbor_system_config/cbor_system_config.h"
#include "configuration/cbor/cbor_system_config/cbor_system_config_cbor_encode.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/threading/work_queue_thread.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"

using namespace eerie_memory;

using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;

namespace {

using Type = ConfigurationService::Type;

struct TestConfiguration {
    uint64_t device_id = 0;
    uint32_t build_number = 0;
};

constexpr uint64_t REJECTED_DEVICE_ID = 0xBAD;

// A manager over the system configuration's codec: the base's store paths and nothing else.
class TestManager : public CborConfigurationManagerBase<TestConfiguration, CborSystemConfig> {
private:
    PmrUniquePtr<CborSystemConfig> Serialize(const TestConfiguration& configuration) override {
        auto cbor_config = make_unique_pmr<CborSystemConfig>(Mrm::GetDefaultPmr());
        cbor_config->device_id = configuration.device_id;
        cbor_config->build_number = configuration.build_number;

        return cbor_config;
    }

    PmrUniquePtr<TestConfiguration> Deserialize(const CborSystemConfig& cbor_config) override {
        auto configuration = make_unique_pmr<TestConfiguration>(Mrm::GetDefaultPmr());
        configuration->device_id = cbor_config.device_id;
        configuration->build_number = cbor_config.build_number;

        return configuration;
    }

    bool CreateDefaultConfiguration() override {
        return Update(TestConfiguration{ .device_id = 1, .build_number = 0 });
    }

    void ValidateApply(const TestConfiguration& configuration) override {
        if(configuration.device_id == REJECTED_DEVICE_ID)
            throw std::invalid_argument("The device ID is refused.");
    }

public:
    explicit TestManager(std::unique_ptr<CborConfigurationService<CborSystemConfig>> cbor_configuration_service) :
        CborConfigurationManagerBase("Test", std::move(cbor_configuration_service)) {

        LoadOrCreateDefault();
    }

    bool AdoptForTest(uint64_t device_id) {
        return Adopt(std::make_shared<TestConfiguration>(TestConfiguration{ .device_id = device_id }));
    }
};

// What a stored handler saw, in order: the type and the thread.
struct Recorder {
    struct Entry {
        Type type;
        k_tid_t thread;
    };

    k_mutex lock{};
    std::vector<Entry> entries;

    Recorder() {
        k_mutex_init(&lock);
    }

    std::vector<Entry> Get() {
        ScopedMutex guard(lock);

        return entries;
    }

    ConfigurationService::ConfigurationStoredHandler Handler() {
        return [this](Type type) {
            ScopedMutex guard(lock);
            entries.push_back({ type, k_current_get() });
        };
    }
};

std::vector<uint8_t> Encode(uint64_t device_id, uint32_t build_number = 0) {
    const CborSystemConfig config{ .device_id = device_id, .build_number = build_number };
    std::vector<uint8_t> bytes(64);
    size_t size = 0;
    zassert_equal(cbor_encode_CborSystemConfig(bytes.data(), bytes.size(), &config, &size), 0);
    bytes.resize(size);

    return bytes;
}

// The internal file system, formatted, a service with a recorder, and a registered manager.
struct Fixture {
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<ConfigurationService> service = std::make_shared<ConfigurationService>();
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();
    int next_name = 0;

    Fixture() {
        DtFs::InitInternalFs();
        fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        zassert_true(fs_service->Format());
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    ~Fixture() {
        if(service != nullptr)
            service->RegisterConfigurationStoredHandler(nullptr);
    }

    std::shared_ptr<TestManager> MakeManager() {
        return std::make_shared<TestManager>(std::make_unique<CborConfigurationService<CborSystemConfig>>(
            "stored_handler_" + std::to_string(next_name++), fs_service
        ));
    }

    // A manager registered under @p type, with the recorder as the service's handler.
    std::shared_ptr<TestManager> Register(Type type = Type::System) {
        auto manager = MakeManager();
        service->RegisterCborConfigurationManager(type, manager);
        service->RegisterConfigurationStoredHandler(recorder->Handler());

        return manager;
    }
};

Fixture*& Current() {
    static Fixture* fixture = nullptr;

    return fixture;
}

void Before(void*) {
    Current() = new Fixture();
}

void After(void*) {
    delete Current();
    Current() = nullptr;
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(configuration_stored_handler, NULL, NULL, Before, After, NULL);

ZTEST(configuration_stored_handler, test_AnUpdateReportsItsTypeOnTheStoringThread) {
    const auto manager = F().Register(Type::Canbus);

    zassert_true(manager->Update(TestConfiguration{ .device_id = 7, .build_number = 1 }));

    const auto entries = F().recorder->Get();
    zassert_equal(entries.size(), 1U);
    zassert_equal(entries[0].type, Type::Canbus, "The type the manager is registered under");
    zassert_equal(entries[0].thread, k_current_get(), "on the thread that stored it");
    zassert_equal(manager->Get()->device_id, 7U);
}

ZTEST(configuration_stored_handler, test_TheStoreIsDoneWhenTheHandlerRuns) {
    auto manager = F().MakeManager();
    F().service->RegisterCborConfigurationManager(Type::System, manager);

    uint64_t seen = 0;
    F().service->RegisterConfigurationStoredHandler([&seen, &manager](Type) {
        seen = manager->Get()->device_id;
    });

    zassert_true(manager->Update(TestConfiguration{ .device_id = 21 }));
    zassert_equal(seen, 21U, "A handler reads the configuration it was told about");
}

ZTEST(configuration_stored_handler, test_AnApplyThroughTheServiceReportsItsType) {
    const auto manager = F().Register(Type::Adc);

    zassert_true(F().service->ApplyCborConfiguration(Type::Adc, Encode(9)));

    const auto entries = F().recorder->Get();
    zassert_equal(entries.size(), 1U);
    zassert_equal(entries[0].type, Type::Adc);
    zassert_equal(manager->Get()->device_id, 9U);
}

// rt_daq's `logging reapply-sensors` applies through the manager, not the service.
ZTEST(configuration_stored_handler, test_AnApplyThatBypassesTheServiceReportsItsType) {
    const auto manager = F().Register(Type::Sensors);

    zassert_true(manager->ApplyCborConfiguration(Encode(10)));

    const auto entries = F().recorder->Get();
    zassert_equal(entries.size(), 1U);
    zassert_equal(entries[0].type, Type::Sensors);
}

ZTEST(configuration_stored_handler, test_AnAdoptReportsItsType) {
    const auto manager = F().Register(Type::Ui);

    zassert_true(manager->AdoptForTest(11));

    const auto entries = F().recorder->Get();
    zassert_equal(entries.size(), 1U);
    zassert_equal(entries[0].type, Type::Ui);
    zassert_equal(manager->Get()->device_id, 11U);
}

ZTEST(configuration_stored_handler, test_UpdateOwnSettingStoresAndReportsNothing) {
    const auto manager = F().Register(Type::Display);

    zassert_true(manager->UpdateOwnSetting(TestConfiguration{ .device_id = 12 }));

    zassert_equal(manager->Get()->device_id, 12U, "The setting is stored");
    zassert_equal(F().recorder->Get().size(), 0U, "and no update is reported");

    // It is stored, not only held: a reload reads it.
    zassert_equal(manager->Get(true)->device_id, 12U);
}

ZTEST(configuration_stored_handler, test_ARejectedApplyReportsNothing) {
    const auto manager = F().Register();

    const std::vector<uint8_t> malformed = { 0xFF, 0x01, 0x02 };
    zassert_false(F().service->ApplyCborConfiguration(Type::System, malformed), "Malformed CBOR");

    char reason[64] = {};
    zassert_false(
        F().service->ApplyCborConfiguration(Type::System, Encode(REJECTED_DEVICE_ID), reason),
        "A configuration the manager's validator refuses"
    );

    zassert_equal(F().recorder->Get().size(), 0U, "A rejected write is not an update");
    zassert_equal(manager->Get()->device_id, 1U, "and the configuration is as it was");
}

ZTEST(configuration_stored_handler, test_LoadingAndTheDefaultReportNothing) {
    const auto manager = F().Register();

    zassert_not_null(manager->Get(true).get(), "The stored default loads");
    zassert_not_null(manager->Get().get());

    zassert_equal(
        F().recorder->Get().size(), 0U, "A load, and the default made while the manager was built, are no updates"
    );
}

ZTEST(configuration_stored_handler, test_TheSameBytesStoredAgainReportAgain) {
    const auto manager = F().Register();
    const auto bytes = Encode(5);

    zassert_true(manager->ApplyCborConfiguration(bytes));
    zassert_true(manager->ApplyCborConfiguration(bytes));

    zassert_equal(F().recorder->Get().size(), 2U, "Every successful store counts, even of unchanged bytes");
}

ZTEST(configuration_stored_handler, test_AManagerRegisteredAfterTheHandlerReports) {
    F().service->RegisterConfigurationStoredHandler(F().recorder->Handler());

    const auto manager = F().MakeManager();
    F().service->RegisterCborConfigurationManager(Type::LoggingControl, manager);

    zassert_true(manager->Update(TestConfiguration{ .device_id = 3 }));

    const auto entries = F().recorder->Get();
    zassert_equal(entries.size(), 1U);
    zassert_equal(entries[0].type, Type::LoggingControl);
}

ZTEST(configuration_stored_handler, test_ARefusedSecondManagerOfATypeIsNotWired) {
    const auto first = F().Register(Type::System);
    const auto second = F().MakeManager();
    F().service->RegisterCborConfigurationManager(Type::System, second);

    zassert_true(second->Update(TestConfiguration{ .device_id = 2 }));
    zassert_equal(F().recorder->Get().size(), 0U, "The service keeps the first manager, so the second reports nothing");

    zassert_true(first->Update(TestConfiguration{ .device_id = 4 }));
    zassert_equal(F().recorder->Get().size(), 1U);
}

ZTEST(configuration_stored_handler, test_AClearedHandlerIsNotCalledAndASecondReplacesTheFirst) {
    const auto manager = F().Register();
    const auto second = std::make_shared<Recorder>();

    F().service->RegisterConfigurationStoredHandler(second->Handler());
    zassert_true(manager->Update(TestConfiguration{ .device_id = 6 }));
    zassert_equal(F().recorder->Get().size(), 0U, "A second handler replaces the first");
    zassert_equal(second->Get().size(), 1U);

    F().service->RegisterConfigurationStoredHandler(nullptr);
    zassert_true(manager->Update(TestConfiguration{ .device_id = 8 }));
    zassert_equal(second->Get().size(), 1U, "and a cleared one is not called");
}

ZTEST(configuration_stored_handler, test_AHandlerThatThrowsDoesNotFailTheStore) {
    const auto manager = F().MakeManager();
    F().service->RegisterCborConfigurationManager(Type::System, manager);
    F().service->RegisterConfigurationStoredHandler([](Type) {
        throw std::runtime_error("The handler failed.");
    });

    zassert_true(
        manager->Update(TestConfiguration{ .device_id = 13 }),
        "The configuration is stored by then: the store must not turn into a failure a client retries"
    );
    zassert_equal(manager->Get()->device_id, 13U);

    zassert_true(F().service->ApplyCborConfiguration(Type::System, Encode(14)));
    zassert_equal(manager->Get()->device_id, 14U);
}

// A manager that is not registered reports to nobody: the gauge's System manager is such a one.
ZTEST(configuration_stored_handler, test_AManagerThatIsNotRegisteredReportsNothing) {
    F().service->RegisterConfigurationStoredHandler(F().recorder->Handler());

    const auto manager = F().MakeManager();
    zassert_true(manager->Update(TestConfiguration{ .device_id = 15 }));

    zassert_equal(F().recorder->Get().size(), 0U);
}

// The handler a manager holds reaches the service's slot, not the service.
ZTEST(configuration_stored_handler, test_AManagerThatOutlivesItsServiceStillStores) {
    const auto manager = F().Register();
    F().service->RegisterConfigurationStoredHandler(nullptr);
    F().service.reset();

    zassert_true(manager->Update(TestConfiguration{ .device_id = 16 }));
    zassert_equal(manager->Get()->device_id, 16U);
}

// A preview shows a configuration and keeps none: it is checked as an upload is, and stores nothing.
ZTEST(configuration_stored_handler, test_AParseReturnsTheModelAndLeavesEverythingAsItWas) {
    const auto manager = F().Register(Type::Ui);
    zassert_true(manager->Update(TestConfiguration{ .device_id = 40 }));
    F().recorder->entries.clear();

    const auto stored_info = manager->GetCborConfigurationInfo();
    const auto stored_bytes = manager->GetCborConfiguration();
    zassert_true(stored_info.size > 0);

    char reason[64] = {};
    const auto parsed = manager->ParseCborConfiguration(Encode(41, 2), reason);

    zassert_not_null(parsed.get());
    zassert_equal(parsed->device_id, 41U);
    zassert_equal(parsed->build_number, 2U);

    const auto info = manager->GetCborConfigurationInfo();
    zassert_equal(info.size, stored_info.size, "The stored file's size is as it was");
    zassert_equal(info.crc, stored_info.crc, "and its CRC");
    zassert_true(manager->GetCborConfiguration() == stored_bytes, "and its bytes");
    zassert_equal(manager->Get()->device_id, 40U, "The configuration in effect stays");
    zassert_equal(F().recorder->Get().size(), 0U, "No stored handler is called");
}

ZTEST(configuration_stored_handler, test_AParseRefusesWhatAnApplyRefusesWithTheSameReason) {
    const auto manager = F().Register(Type::Ui);
    const auto stored_info = manager->GetCborConfigurationInfo();

    char parse_reason[64] = {};
    char apply_reason[64] = {};
    const std::vector<uint8_t> malformed = { 0xFF, 0x01, 0x02 };

    zassert_is_null(manager->ParseCborConfiguration(malformed, parse_reason).get());
    zassert_false(manager->ApplyCborConfiguration(malformed, apply_reason));
    zassert_equal(std::string(parse_reason), std::string(apply_reason));
    zassert_equal(std::string(parse_reason), "Malformed CBOR");

    parse_reason[0] = '\0';
    apply_reason[0] = '\0';
    zassert_is_null(manager->ParseCborConfiguration(Encode(REJECTED_DEVICE_ID), parse_reason).get());
    zassert_false(manager->ApplyCborConfiguration(Encode(REJECTED_DEVICE_ID), apply_reason));
    zassert_equal(std::string(parse_reason), "The device ID is refused.");
    zassert_equal(std::string(parse_reason), std::string(apply_reason));

    zassert_equal(manager->GetCborConfigurationInfo().crc, stored_info.crc, "Nothing was stored");
    zassert_equal(F().recorder->Get().size(), 0U);
}

// What a preview accepts a store of the same bytes then stores, and reports.
ZTEST(configuration_stored_handler, test_WhatAParseAcceptsAnApplyStores) {
    const auto manager = F().Register(Type::Ui);
    const auto bytes = Encode(50);

    zassert_not_null(manager->ParseCborConfiguration(bytes, {}).get());
    zassert_equal(F().recorder->Get().size(), 0U);

    zassert_true(manager->ApplyCborConfiguration(bytes));
    zassert_equal(manager->Get()->device_id, 50U);
    zassert_equal(F().recorder->Get().size(), 1U);
}
