// The CAN scripts as an asset kind: CanbusController registers them with the AssetsManager it is
// given when the unit has an SD service, answers for the scripts its messages use on any channel, and
// withdraws both when it goes. The volume of the suite serves as the SD card.
#ifdef CONFIG_EERIE_LEAP_ASSETS

#include <memory>

#include <zephyr/ztest.h>

#include "script_assets_test_support.h"

using namespace script_assets_test;

ZTEST_SUITE(canbus_controller_assets, NULL, NULL, NULL, NULL, NULL);

ZTEST(canbus_controller_assets, test_the_can_scripts_are_registered_with_what_the_kind_needs) {
    auto rig = MakeRig("can_assets_descriptor_wq");

    auto controller = MakeCanbusController(rig, rig.sd, rig.assets);
    zassert_true(rig.assets->GetProviders().empty(), "Nothing is registered before Initialize()");
    zassert_equal(controller->Initialize(), 0);

    const auto provider = rig.assets->Find(AssetKind::CAN_SCRIPTS);
    zassert_not_null(provider.get(), "The CAN scripts are registered once the controller is initialized");
    zassert_equal(provider->kind, AssetKind::CAN_SCRIPTS);
    zassert_true(provider->name == "can_scripts", "The kind's name");
    zassert_equal(provider->extensions.size(), 1);
    zassert_true(provider->extensions[0] == ".lua", "A Lua script");
    zassert_equal(provider->access, kAllScriptAccess, "Read, write and delete");
    zassert_equal(provider->max_size, CONFIG_EERIE_LEAP_CAN_SCRIPT_MAX_SIZE);
    zassert_not_null(provider->store.get());
    zassert_true(provider->store->GetDirectory() == "scripts/can", "The directory of the kind on the SD volume");
    zassert_true(static_cast<bool>(provider->is_in_use), "A referenced script is not deleted");

    zassert_true(rig.fs_service->Exists("scripts/can"), "The store creates its directory on a mounted volume");
    zassert_is_null(rig.assets->Find(AssetKind::SENSOR_SCRIPTS).get(), "Only the CAN scripts are the controller's");
    zassert_equal(rig.assets->GetProviders().size(), 1);

    // The store is on the SD service the controller was given, not on its internal file system.
    zassert_true(provider->store->IsAvailable());
    rig.sd->SetAvailable(false);
    zassert_false(provider->store->IsAvailable(), "The store follows the SD service");
    zassert_true(rig.fs_service->IsAvailable(), "and not the internal file system");
    rig.sd->SetAvailable(true);
    zassert_true(provider->store->IsAvailable());
}

ZTEST(canbus_controller_assets, test_nothing_is_registered_without_an_sd_service_or_a_manager) {
    auto rig = MakeRig("can_assets_missing_wq");
    WriteScript(*rig.fs_service, kCanScript);

    {
        // Without an SD service the script path is not checked, so the message may name any script.
        auto without_sd = MakeCanbusController(rig, nullptr, rig.assets);
        zassert_equal(InitializeWithMessages(*without_sd, { { 0, 0x100, kCanScript } }), 0,
            "A unit without an SD card initializes");
        zassert_true(rig.assets->GetProviders().empty(), "No SD service, no CAN scripts");
        zassert_false(rig.assets->IsReferenced(kCanScript), "and no referrer, though a message uses the script");
    }

    {
        // It loads the configuration the first one stored, which refers to the script.
        auto without_manager = MakeCanbusController(rig, rig.sd, nullptr);
        zassert_equal(without_manager->Initialize(), 0, "A unit without a registry initializes");
        zassert_true(rig.assets->GetProviders().empty());
        zassert_false(rig.assets->IsReferenced(kCanScript));
    }
}

ZTEST(canbus_controller_assets, test_a_script_is_referenced_by_any_message_on_any_channel) {
    auto rig = MakeRig("can_assets_referrer_wq");
    for(const char* script : {kCanScript, kDisabledCanScript, kCanScriptInSensorDirectory})
        WriteScript(*rig.fs_service, script);

    auto controller = MakeCanbusController(rig, rig.sd, rig.assets);
    zassert_equal(InitializeWithMessages(*controller, {
        { 0, 0x100, kCanScript },
        { 0, 0x101, kDisabledCanScript, false },
        { 0, 0x102, "" },
        { 1, 0x200, kCanScriptInSensorDirectory },
    }), 0);

    zassert_true(rig.assets->IsReferenced(kCanScript), "A message uses the script");
    zassert_true(rig.assets->IsReferenced(kDisabledCanScript), "A disabled message keeps its script");
    zassert_true(rig.assets->IsReferenced(kCanScriptInSensorDirectory), "A message of another channel counts, and a script of the other kind's directory");

    zassert_true(rig.assets->IsReferenced("SCRIPTS/Can/A.LUA"), "FAT folds the case of a name");
    zassert_true(rig.assets->IsReferenced("./scripts//can/a.lua"), "Empty names and . name nothing");

    zassert_false(rig.assets->IsReferenced("scripts/can/other.lua"), "No message uses it");
    zassert_false(rig.assets->IsReferenced("scripts/can"), "A directory is no script");
    zassert_false(rig.assets->IsReferenced("a.lua"), "The directory is part of the path");
    zassert_false(rig.assets->IsReferenced(""), "A message without a script refers to nothing");
}

ZTEST(canbus_controller_assets, test_only_deleting_a_referenced_script_is_refused) {
    auto rig = MakeRig("can_assets_in_use_wq");
    WriteScript(*rig.fs_service, kCanScript);
    WriteScript(*rig.fs_service, kDisabledCanScript);

    auto controller = MakeCanbusController(rig, rig.sd, rig.assets);
    zassert_equal(InitializeWithMessages(*controller, {
        { 0, 0x100, kCanScript },
        { 1, 0x200, kDisabledCanScript, false },
    }), 0);

    const auto provider = rig.assets->Find(AssetKind::CAN_SCRIPTS);
    zassert_not_null(provider.get());

    zassert_true(provider->is_in_use(AssetAccess::DELETE, kCanScriptName), "A referenced script is not deleted");
    zassert_true(provider->is_in_use(AssetAccess::DELETE, "A.LUA"), "whatever the case of its name");
    zassert_true(provider->is_in_use(AssetAccess::DELETE, "d.lua"), "also one of a disabled message");
    zassert_false(provider->is_in_use(AssetAccess::WRITE, kCanScriptName), "A referenced script may be replaced");
    zassert_false(provider->is_in_use(AssetAccess::READ, kCanScriptName), "or read");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, "other.lua"), "Another script may be deleted");

    // The referrer reads the configuration in force, not the one the controller started with. The
    // reconfiguration it schedules is cancelled with the controller.
    StoreCanConfiguration(*controller->GetConfigurationManager(), { { 0, 0x100, "" } });
    zassert_false(provider->is_in_use(AssetAccess::DELETE, kCanScriptName), "A script no message uses is deleted");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, "d.lua"));
}

// A sensor may use a script from the CAN scripts' directory and a CAN message one from the sensors':
// each kind asks every referrer, not its own controller's alone.
ZTEST(canbus_controller_assets, test_a_script_of_either_kind_is_kept_while_either_controller_uses_it) {
    auto rig = MakeRig("can_assets_cross_wq");
    WriteScript(*rig.fs_service, kSensorScriptInCanDirectory);
    WriteScript(*rig.fs_service, kCanScriptInSensorDirectory);

    auto sensors = MakeSensorsController(rig, rig.sd, rig.assets);
    zassert_equal(InitializeWithSensors(*sensors, { MakeSensor("in_can_directory", kSensorScriptInCanDirectory) }), 0);

    auto canbus = MakeCanbusController(rig, rig.sd, rig.assets);
    zassert_equal(InitializeWithMessages(*canbus, { { 0, 0x100, kCanScriptInSensorDirectory } }), 0);

    const auto sensor_scripts = rig.assets->Find(AssetKind::SENSOR_SCRIPTS);
    const auto can_scripts = rig.assets->Find(AssetKind::CAN_SCRIPTS);
    zassert_not_null(sensor_scripts.get());
    zassert_not_null(can_scripts.get());

    zassert_true(sensor_scripts->is_in_use(AssetAccess::DELETE, "s.lua"), "A CAN message uses a sensor script");
    zassert_true(can_scripts->is_in_use(AssetAccess::DELETE, "c.lua"), "A sensor uses a CAN script");
    zassert_false(sensor_scripts->is_in_use(AssetAccess::WRITE, "s.lua"));
    zassert_false(can_scripts->is_in_use(AssetAccess::READ, "c.lua"));

    // The sensors' controller goes: the CAN message still uses its script, the sensor no longer does.
    sensors.reset();
    zassert_true(rig.assets->IsReferenced(kCanScriptInSensorDirectory));
    zassert_false(rig.assets->IsReferenced(kSensorScriptInCanDirectory), "The sensors' referrer went with its controller");
    zassert_is_null(rig.assets->Find(AssetKind::SENSOR_SCRIPTS).get());
    zassert_not_null(rig.assets->Find(AssetKind::CAN_SCRIPTS).get(), "The CAN scripts stay");
}

ZTEST(canbus_controller_assets, test_destroying_the_controller_withdraws_the_kind_and_its_referrer) {
    auto rig = MakeRig("can_assets_destroy_wq");
    WriteScript(*rig.fs_service, kCanScript);

    std::shared_ptr<const eerie_leap::subsys::assets::AssetProvider> provider;
    {
        auto controller = MakeCanbusController(rig, rig.sd, rig.assets);
        zassert_equal(InitializeWithMessages(*controller, { { 0, 0x100, kCanScript } }), 0);

        provider = rig.assets->Find(AssetKind::CAN_SCRIPTS);
        zassert_not_null(provider.get());
        zassert_true(rig.assets->IsReferenced(kCanScript));
    }

    zassert_is_null(rig.assets->Find(AssetKind::CAN_SCRIPTS).get(), "The CAN scripts are no longer served");
    zassert_false(rig.assets->IsReferenced(kCanScript), "and the messages no longer refer to anything");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, kCanScriptName),
        "A provider found before keeps working, and refuses nothing");

    std::weak_ptr<AssetsManager> weak_assets = rig.assets;
    rig.assets.reset();

    zassert_true(weak_assets.expired(), "Nothing the controller registered holds the registry");
    zassert_false(provider->is_in_use(AssetAccess::DELETE, kCanScriptName), "A provider outliving its registry refuses nothing");
}

#endif // CONFIG_EERIE_LEAP_ASSETS
