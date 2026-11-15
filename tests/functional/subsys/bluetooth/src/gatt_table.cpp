#include <cstddef>
#include <cstdint>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/ztest.h>

#include "subsys/bluetooth/access_point/access_point_service.h"

namespace {

const bt_uuid_128 ACCESS_POINT_SERVICE_UUID = BT_UUID_INIT_128(BT_UUID_ACCESS_POINT_SERVICE_VAL);

// The GATT service's declaration, then Service Changed's declaration and value.
constexpr uint16_t SERVICE_CHANGED_VALUE_HANDLE = 3;

const bt_uuid* ServiceUuidOf(const bt_gatt_service_static& service) {
    return static_cast<const bt_uuid*>(service.attrs[0].user_data);
}

} // namespace

// The two below iterate the section, whose bounds must be declared outside the anonymous namespace.

// The handle the stack gives the declaration of the static service `uuid`, 0 if there is none.
static uint16_t HandleOfService(const bt_uuid* uuid) {
    uint16_t handle = 1;
    STRUCT_SECTION_FOREACH(bt_gatt_service_static, service) {
        if(bt_uuid_cmp(ServiceUuidOf(*service), uuid) == 0)
            return handle;

        handle += service->attr_count;
    }

    return 0;
}

// The handle the stack gives the first static attribute of type `uuid`, 0 if there is none.
static uint16_t HandleOfFirstAttribute(const bt_uuid* uuid) {
    uint16_t handle = 1;
    STRUCT_SECTION_FOREACH(bt_gatt_service_static, service) {
        for(size_t i = 0; i < service->attr_count; ++i, ++handle) {
            if(bt_uuid_cmp(service->attrs[i].uuid, uuid) == 0)
                return handle;
        }
    }

    return 0;
}

ZTEST_SUITE(gatt_table, NULL, NULL, NULL, NULL, NULL);

// A bonded central keeps the handle where it subscribed to Service Changed, and the stack restores its
// subscriptions by handle: a service ahead of the GATT service would move it whenever it grows.
ZTEST(gatt_table, test_service_changed_keeps_the_first_characteristic_handles) {
    zassert_equal(HandleOfService(BT_UUID_GATT), 1);
    zassert_equal(HandleOfFirstAttribute(BT_UUID_GATT_SC), SERVICE_CHANGED_VALUE_HANDLE);
}

ZTEST(gatt_table, test_the_access_point_service_follows_the_gap_service) {
    const uint16_t gap = HandleOfService(BT_UUID_GAP);
    const uint16_t access_point = HandleOfService(&ACCESS_POINT_SERVICE_UUID.uuid);

    zassert_not_equal(gap, 0);
    zassert_not_equal(access_point, 0);
    zassert_true(gap < access_point, "GAP at %u, the access point at %u", gap, access_point);
}
