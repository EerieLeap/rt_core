#include <string>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/sdmmc_service.h"

#include "fs_test_support.h"

using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::SdmmcService;

using fs_test::ReadText;

// Emulated targets have no SD host: the service runs against the internal volume and the disk
// ioctls it issues on this name fail harmlessly.
static constexpr const char* k_disk_name = "no-sd-card";

static atomic_t card_present = ATOMIC_INIT(1);

// The service mounts the volume but does not own the devicetree mount entry, so instances are
// free to come and go; a single shared one keeps the monitor tests simple.
static SdmmcService* GetSdmmcService() {
    static SdmmcService* sdmmc_service = nullptr;

    if(sdmmc_service == nullptr) {
        DtFs::InitInternalFs();
        sdmmc_service = new SdmmcService(DtFs::GetInternalFsMp(), k_disk_name);
    }

    return sdmmc_service;
}

static bool WaitForAvailability(SdmmcService* sdmmc_service, bool expected) {
    for(int i = 0; i < 100; ++i) {
        if(sdmmc_service->IsAvailable() == expected)
            return true;

        k_msleep(20);
    }

    return false;
}

static void SetUpSdmmc(void*) {
    atomic_set(&card_present, 1);
    GetSdmmcService()->RegisterIsSdCardPresentHandler(nullptr);
    fs_test::ResetFs();
}

static void TearDownSdmmc(void*) {
    GetSdmmcService()->SdMonitorStop();
}

ZTEST_SUITE(sdmmc_service, NULL, NULL, SetUpSdmmc, TearDownSdmmc, NULL);

ZTEST(sdmmc_service, test_Initialize) {
    auto* sdmmc_service = GetSdmmcService();

    zassert_true(sdmmc_service->Initialize());
    zassert_true(sdmmc_service->IsAvailable());

    zassert_true(sdmmc_service->WriteFile("test_sdmmc_initialize.txt", "abc", 3));
    zassert_str_equal(ReadText("test_sdmmc_initialize.txt").c_str(), "abc");
}

ZTEST(sdmmc_service, test_SdMonitorStop_without_start) {
    zassert_equal(GetSdmmcService()->SdMonitorStop(), 0);
}

ZTEST(sdmmc_service, test_SdMonitor_keeps_state_without_handler) {
    auto* sdmmc_service = GetSdmmcService();

    zassert_true(sdmmc_service->Initialize());
    zassert_equal(sdmmc_service->SdMonitorStart(), 0);

    k_msleep(150);

    zassert_true(sdmmc_service->IsAvailable());
    zassert_equal(sdmmc_service->SdMonitorStop(), 0);
}

ZTEST(sdmmc_service, test_SdMonitor_tracks_card_removal_and_insertion) {
    auto* sdmmc_service = GetSdmmcService();

    zassert_true(sdmmc_service->Initialize());
    sdmmc_service->RegisterIsSdCardPresentHandler([] { return atomic_get(&card_present) == 1; });

    zassert_equal(sdmmc_service->SdMonitorStart(), 0);
    zassert_true(WaitForAvailability(sdmmc_service, true));

    atomic_set(&card_present, 0);
    zassert_true(WaitForAvailability(sdmmc_service, false), "card removal was not detected");

    atomic_set(&card_present, 1);
    zassert_true(WaitForAvailability(sdmmc_service, true), "card insertion was not detected");

    zassert_equal(sdmmc_service->SdMonitorStop(), 0);

    // The volume is remounted on insertion, so it has to be writable again.
    zassert_true(sdmmc_service->WriteFile("test_sdmmc_reinsert.txt", "abc", 3));
    zassert_str_equal(ReadText("test_sdmmc_reinsert.txt").c_str(), "abc");
}

ZTEST(sdmmc_service, test_SdMonitorStop_is_idempotent) {
    auto* sdmmc_service = GetSdmmcService();

    zassert_equal(sdmmc_service->SdMonitorStart(), 0);
    zassert_equal(sdmmc_service->SdMonitorStop(), 0);
    zassert_equal(sdmmc_service->SdMonitorStop(), 0);
}

ZTEST(sdmmc_service, test_SdMonitorStart_is_idempotent) {
    auto* sdmmc_service = GetSdmmcService();

    zassert_equal(sdmmc_service->SdMonitorStart(), 0);
    zassert_equal(sdmmc_service->SdMonitorStart(), 0);

    k_msleep(100);

    zassert_equal(sdmmc_service->SdMonitorStop(), 0);
}

// A running monitor must not keep the destructor waiting forever.
ZTEST(sdmmc_service, test_destructor_stops_the_monitor) {
    DtFs::InitInternalFs();

    {
        SdmmcService local_service(DtFs::GetInternalFsMp(), k_disk_name);

        zassert_true(local_service.Initialize());
        local_service.RegisterIsSdCardPresentHandler([] { return true; });
        zassert_equal(local_service.SdMonitorStart(), 0);

        k_msleep(100);
    }

    zassert_true(fs_test::GetFsService()->IsAvailable());
}

ZTEST(sdmmc_service, test_IsAvailable_requires_a_mounted_volume) {
    auto* sdmmc_service = GetSdmmcService();

    zassert_true(sdmmc_service->Initialize());
    zassert_true(sdmmc_service->IsAvailable());

    SdmmcService orphan_service(nullptr, k_disk_name);

    zassert_false(orphan_service.Initialize());
    zassert_false(orphan_service.IsAvailable());
}
