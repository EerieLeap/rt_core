#include <memory>
#include <stdexcept>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/ztest.h>

#include "subsys/canbus/canbus_proxy.hpp"

using eerie_leap::subsys::canbus::Canbus;
using eerie_leap::subsys::canbus::CanbusConfig;
using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::canbus::CanbusType;

ZTEST_SUITE(canbus_proxy, NULL, NULL, NULL, NULL, NULL);

namespace {

// The controller is never started here; Canbus only needs the device to exist.
std::unique_ptr<Canbus> MakeCanbus() {
    return std::make_unique<Canbus>(CanbusConfig(
        DEVICE_DT_GET(DT_NODELABEL(can_loopback0)),
        CanbusType::CLASSICAL_CAN,
        500000));
}

} // namespace

ZTEST(canbus_proxy, test_holds_and_exposes_instance) {
    auto canbus = MakeCanbus();
    auto* raw = canbus.get();

    CanbusProxy proxy(std::move(canbus));

    zassert_true(proxy.IsValid());
    zassert_equal(proxy.Get(), raw);
    zassert_equal(proxy.operator->(), raw);
}

ZTEST(canbus_proxy, test_release_empties_the_proxy) {
    CanbusProxy proxy(MakeCanbus());

    auto released = proxy.Release();

    zassert_not_null(released.get());
    zassert_false(proxy.IsValid());
    zassert_is_null(proxy.Get());
}

ZTEST(canbus_proxy, test_dereferencing_an_empty_proxy_throws) {
    CanbusProxy proxy(MakeCanbus());
    auto released = proxy.Release();

    bool threw = false;
    try {
        (void)proxy->GetState();
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw, "Dereferencing an empty proxy must throw, not return null");
}

ZTEST(canbus_proxy, test_update_replaces_instance) {
    CanbusProxy proxy(MakeCanbus());

    auto replacement = MakeCanbus();
    auto* raw = replacement.get();

    proxy.Update(std::move(replacement));

    zassert_true(proxy.IsValid());
    zassert_equal(proxy.Get(), raw);
}

ZTEST(canbus_proxy, test_updated_handlers_are_notified) {
    CanbusProxy proxy(MakeCanbus());

    int notifications = 0;
    Canbus* last = nullptr;

    int handler_id = proxy.RegisterUpdatedHandler([&](Canbus* canbus) {
        notifications++;
        last = canbus;
    });
    zassert_true(handler_id > 0);

    auto replacement = MakeCanbus();
    auto* raw = replacement.get();
    proxy.Update(std::move(replacement));

    zassert_equal(notifications, 1);
    zassert_equal(last, raw);

    auto released = proxy.Release();
    zassert_equal(notifications, 2);
    zassert_is_null(last, "Release() must report that the proxy is now empty");

    zassert_true(proxy.UnregisterUpdatedHandler(handler_id));
    zassert_false(proxy.UnregisterUpdatedHandler(handler_id));
}
