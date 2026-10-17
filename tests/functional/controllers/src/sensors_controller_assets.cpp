// The sensor scripts as an asset kind: SensorsController registers them with the AssetsManager it is
// given when the unit has an SD service, answers for the scripts its sensors use, and withdraws both
// when it goes. The volume of the suite serves as the SD card.
#ifdef CONFIG_EERIE_LEAP_ASSETS

#include <memory>
#include <string>

#include <zephyr/ztest.h>

#include "script_assets_test_support.h"

using namespace script_assets_test;

ZTEST_SUITE(sensors_controller_assets, NULL, NULL, NULL, NULL, NULL);

ZTEST(sensors_controller_assets, test_the_sensor_scripts_are_registered_with_what_the_kind_needs) {
    auto rig = MakeRig("sensor_assets_descriptor_wq");

    auto controller = MakeSensorsController(rig, rig.sd, rig.assets);
    zassert_true(rig.assets->GetProviders().empty(), "Nothing is registered before Initialize()");
    zassert_equal(controller->Initialize(), 0);

    const auto provider = rig.assets->Find(AssetKind::SENSOR_SCRIPTS);
    zassert_not_null(provider.get(), "The sensor scripts are registered once the controller is initialized");
    zassert_equal(provider->kind, AssetKind::SENSOR_SCRIPTS);
    zassert_true(provider->name == "sensor_scripts", "The kind's name");
    zassert_equal(provider->extensions.size(), 1);
    zassert_true(provider->extensions[0] == ".lua", "A Lua script");
    zassert_equal(provider->access, kAllScriptAccess, "Read, write and delete");
    zassert_equal(provider->max_size, CONFIG_EERIE_LEAP_SENSOR_SCRIPT_MAX_SIZE);
    zassert_not_null(provider->store.get());
    zassert_true(provider->store->GetDirectory() == "scripts/sensors", "The directory of the kind on the SD volume");
    zassert_true(static_cast<bool>(provider->is_in_use), "A referenced script is not deleted");

    zassert_true(rig.fs_service->Exists("scripts/sensors"), "The store creates its directory on a mounted volume");
    zassert_is_null(rig.assets->Find(AssetKind::CAN_SCRIPTS).get(), "Only the sensor scripts are the controller's");
    zassert_equal(rig.assets->GetProviders().size(), 1);

    // The store is on the SD service the controller was given, not on its internal file system.
    zassert_true(provider->store->IsAvailable());
    rig.sd->SetAvailable(false);
    zassert_false(provider->store->IsAvailable(), "The store follows the SD service");
    zassert_true(rig.fs_service->IsAvailable(), "and not the internal file system");
    rig.sd->SetAvailable(true);
    zassert_true(provider->store->IsAvailable());
}

ZTEST(sensors_controller_assets, test_nothing_is_registered_without_an_sd_service_or_a_manager) {
    auto rig = MakeRig("sensor_assets_missing_wq");
    WriteScript(*rig.fs_service, kSensorScript);

    {
        // Without an SD service the script path is not checked, so the sensor may name any script.
        auto without_sd = MakeSensorsController(rig, nullptr, rig.assets);
        zassert_equal(InitializeWithSensors(*without_sd, { MakeSensor("scripted", kSensorScript) }), 0,
            "A unit without an SD card initializes");
        zassert_true(rig.assets->GetProviders().empty(), "No SD service, no sensor scripts");
        zassert_false(rig.assets->IsReferenced(kSensorScript), "and no referrer, though a sensor uses the script");
    }

    {
        // It loads the configuration the first one stored, which refers to the script.
        auto without_manager = MakeSensorsController(rig, rig.sd, nullptr);
        zassert_equal(without_manager->Initialize(), 0, "A unit without a registry initializes");
        const auto sensors = without_manager->GetConfigurationManager()->Get();
        zassert_not_null(sensors.get());
        zassert_equal(sensors->size(), 1);
        zassert_true((*sensors)[0]->configuration.HasScript(), "The stored sensor uses the script");
        zassert_true(rig.assets->GetProviders().empty());
        zassert_false(rig.assets->IsReferenced(kSensorScript));
    }
}

ZTEST(sensors_controller_assets, test_a_script_is_referenced_by_any_sensor_enabled_or_not) {
    auto rig = MakeRig("sensor_assets_referrer_wq");
    for(const char* script : {kSensorScript, kDisabledSensorScript, kSensorScriptInCanDirectory})
        WriteScript(*rig.fs_service, script);

    auto controller = MakeSensorsController(rig, rig.sd, rig.assets);
    zassert_equal(InitializeWithSensors(*controller, {
        MakeSensor("enabled", kSensorScript),
        MakeSensor("disabled", kDisabledSensorScript, false),
        MakeSensor("in_can_directory", kSensorScriptInCanDirectory),
        MakeSensor("without_script", ""),
    }), 0);

    zassert_true(rig.assets->IsReferenced(kSensorScript), "A sensor uses the script");
    zassert_true(rig.assets->IsReferenced(kDisabledSensorScript), "A disabled sensor keeps its script");
    zassert_true(rig.assets->IsReferenced(kSensorScriptInCanDirectory), "A script of the other kind's directory counts");

    zassert_true(rig.assets->IsReferenced("SCRIPTS/Sensors/A.LUA"), "FAT folds the case of a name");
    zassert_true(rig.assets->IsReferenced("scripts//sensors/./a.lua"), "Empty names and . name nothing");

    zassert_false(rig.assets->IsReferenced("scripts/sensors/other.lua"), "No sensor uses it");
    zassert_false(rig.assets->IsReferenced("scripts/sensors"), "A directory is no script");
    zassert_false(rig.assets->IsReferenced("a.lua"), "The directory is part of the path");
    zassert_false(rig.assets->IsReferenced(""), "A sensor without a script refers to nothing");
}

ZTEST(sensors_controller_assets, test_only_deleting_a_referenced_script_is_refused) {
    auto rig = MakeRig("sensor_assets_in_use_wq");
    WriteScript(*rig.fs_service, kSensorScript);
    WriteScript(*rig.fs_service, kDisabledSensorScript);

    auto controller = MakeSensorsController(rig, rig.sd, rig.assets);
    zassert_equal(InitializeWithSensors(*controller, {
        MakeSensor("enabled", kSensorScript),
        MakeSensor("disabled", kDisabledSensorScript, false),
    }), 0);

    const auto provider = rig.assets->Find(AssetKind::SENSOR_SCRIPTS);
    zassert_not_null(provider.get());

    zassert_true(provider->is_in_use(AssetAccess::DELETE, kSensorScriptName), "A referenced script is not deleted");
    zassert_true(provider->is_in_use(AssetAccess::DELETE, "A.LUA"), "whatever the case of its name");
    zassert_true(provider->is_in_use(AssetAccess::DELETE, "d.lua"), "also one of a disabled sensor");
    zassert_false(provider->is_in_use(AssetAccess::WRITE, kSensorScriptName), "A referenced script may be replaced");
    zassert_false(provider->is_in_use(AssetAccess::READ, kSensorScriptName), "or read");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, kOtherSensorScriptName), "Another script may be deleted");

    // The referrer reads the configuration in force, not the one the controller started with.
    zassert_true(controller->GetConfigurationManager()->Update({ MakeSensor("enabled", "") }));
    zassert_false(provider->is_in_use(AssetAccess::DELETE, kSensorScriptName), "A script no sensor uses is deleted");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, "d.lua"));
}

ZTEST(sensors_controller_assets, test_destroying_the_controller_withdraws_the_kind_and_its_referrer) {
    auto rig = MakeRig("sensor_assets_destroy_wq");
    WriteScript(*rig.fs_service, kSensorScript);

    std::shared_ptr<const eerie_leap::subsys::assets::AssetProvider> provider;
    {
        auto controller = MakeSensorsController(rig, rig.sd, rig.assets);
        zassert_equal(InitializeWithSensors(*controller, { MakeSensor("enabled", kSensorScript) }), 0);

        provider = rig.assets->Find(AssetKind::SENSOR_SCRIPTS);
        zassert_not_null(provider.get());
        zassert_true(rig.assets->IsReferenced(kSensorScript));
    }

    zassert_is_null(rig.assets->Find(AssetKind::SENSOR_SCRIPTS).get(), "The sensor scripts are no longer served");
    zassert_false(rig.assets->IsReferenced(kSensorScript), "and the sensors no longer refer to anything");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, kSensorScriptName),
        "A provider found before keeps working, and refuses nothing");
}

// The registry owns the provider, so the provider must not own the registry.
ZTEST(sensors_controller_assets, test_the_provider_does_not_keep_the_registry_alive) {
    auto rig = MakeRig("sensor_assets_weak_wq");
    WriteScript(*rig.fs_service, kSensorScript);

    std::shared_ptr<const eerie_leap::subsys::assets::AssetProvider> provider;
    {
        auto controller = MakeSensorsController(rig, rig.sd, rig.assets);
        zassert_equal(InitializeWithSensors(*controller, { MakeSensor("enabled", kSensorScript) }), 0);

        provider = rig.assets->Find(AssetKind::SENSOR_SCRIPTS);
        zassert_not_null(provider.get());
    }

    std::weak_ptr<AssetsManager> weak_assets = rig.assets;
    rig.assets.reset();

    zassert_true(weak_assets.expired(), "Nothing the controller registered holds the registry");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, kSensorScriptName), "A provider outliving its registry refuses nothing");
}

// A kind registered already stays with its owner: the second controller serves nothing, and must not
// withdraw the first one's kind, or leave a referrer behind that nothing removes.
ZTEST(sensors_controller_assets, test_a_controller_that_could_not_register_withdraws_nothing_of_another) {
    auto rig = MakeRig("sensor_assets_second_wq");
    WriteScript(*rig.fs_service, kSensorScript);

    auto first = MakeSensorsController(rig, rig.sd, rig.assets);
    zassert_equal(InitializeWithSensors(*first, { MakeSensor("enabled", kSensorScript) }), 0);
    const auto provider = rig.assets->Find(AssetKind::SENSOR_SCRIPTS);
    zassert_not_null(provider.get());

    {
        // It loads the configuration the first one stored, so it refers to the same script.
        auto second = MakeSensorsController(rig, rig.sd, rig.assets);
        zassert_equal(second->Initialize(), 0, "A kind registered already is no failure");
        zassert_equal(rig.assets->Find(AssetKind::SENSOR_SCRIPTS).get(), provider.get(), "The first registration stays");

        // The second controller is alive and still sees the script: had it kept its referrer, the script
        // would stay referenced once the first one's configuration lets go of it.
        zassert_true(first->GetConfigurationManager()->Update({ MakeSensor("enabled", "") }));
        zassert_false(rig.assets->IsReferenced(kSensorScript),
            "The second controller left no referrer that still sees the script it loaded");
    }

    zassert_equal(rig.assets->Find(AssetKind::SENSOR_SCRIPTS).get(), provider.get(), "The second one's end withdraws nothing");

    zassert_true(first->GetConfigurationManager()->Update({ MakeSensor("enabled", kSensorScript) }));
    zassert_true(rig.assets->IsReferenced(kSensorScript), "and the first one's referrer stays");
}

#endif // CONFIG_EERIE_LEAP_ASSETS
