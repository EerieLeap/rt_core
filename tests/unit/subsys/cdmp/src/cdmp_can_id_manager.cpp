#include <stdexcept>

#include <zephyr/ztest.h>

#include "subsys/cdmp/utilities/cdmp_can_id_manager.h"

using eerie_leap::subsys::cdmp::utilities::CdmpCanIdManager;

ZTEST_SUITE(cdmp_can_id_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_can_id_manager, test_ids_are_standard_offsets_from_the_base) {
    CdmpCanIdManager manager;

    zassert_equal(manager.GetBaseCanId(), CdmpCanIdManager::DEFAULT_BASE_CAN_ID);
    zassert_equal(manager.GetManagementCanId().id, 0x700);
    zassert_equal(manager.GetHeartbeatCanId().id, 0x701);
    zassert_equal(manager.GetCommandRequestCanId().id, 0x702);
    zassert_equal(manager.GetCommandResponseCanId().id, 0x703);
    zassert_equal(manager.GetCapabilityCanId(0).id, 0x714);
    zassert_false(manager.GetHeartbeatCanId().is_extended, "CDMP uses 11-bit identifiers only");
}

ZTEST(cdmp_can_id_manager, test_base_must_leave_room_for_the_id_range) {
    zassert_true(CdmpCanIdManager::IsValidBaseCanId(0x000));
    zassert_true(CdmpCanIdManager::IsValidBaseCanId(0x79C), "Base + 99 = 0x7FF still fits");
    zassert_false(CdmpCanIdManager::IsValidBaseCanId(0x79D));
    zassert_false(CdmpCanIdManager::IsValidBaseCanId(0xFFFFFFF0), "Must not wrap around");
}

ZTEST(cdmp_can_id_manager, test_base_can_be_changed) {
    CdmpCanIdManager manager;

    manager.SetBaseCanId(0x600);

    zassert_equal(manager.GetBaseCanId(), 0x600);
    zassert_equal(manager.GetHeartbeatCanId().id, 0x601);
}

ZTEST(cdmp_can_id_manager, test_invalid_base_is_rejected) {
    CdmpCanIdManager manager;
    bool threw = false;

    try {
        manager.SetBaseCanId(0x7F0);
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
    zassert_equal(manager.GetBaseCanId(), CdmpCanIdManager::DEFAULT_BASE_CAN_ID, "A rejected base must not be applied");
}

ZTEST(cdmp_can_id_manager, test_capability_bit_out_of_range_is_rejected) {
    CdmpCanIdManager manager;
    bool threw = false;

    try {
        (void)manager.GetCapabilityCanId(32);
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}
