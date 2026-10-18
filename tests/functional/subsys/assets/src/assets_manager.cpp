#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "subsys/assets/assets_manager.h"

#include "assets_test_support.h"

using eerie_leap::subsys::assets::ASSET_KIND_COUNT;
using eerie_leap::subsys::assets::AssetAccess;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetProvider;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::assets::HasAccess;
using eerie_leap::subsys::assets::ToMask;

using assets_test::MakeUnavailableStore;

// Lets a test take the memory away: while set, every allocation through operator new fails as it
// does when the heap is full. Replacement functions cannot be inline, so this is the only file of the
// binary that defines them; the assets_fat suite builds asset_store.cpp alone.
namespace {

std::atomic<bool> g_fail_allocations{ false };

void* Allocate(size_t size) {
    if(g_fail_allocations.load())
        throw std::bad_alloc();

    void* pointer = std::malloc(size == 0 ? 1 : size);
    if(pointer == nullptr)
        throw std::bad_alloc();

    return pointer;
}

void* Allocate(size_t size, const std::nothrow_t&) noexcept {
    return g_fail_allocations.load() ? nullptr : std::malloc(size == 0 ? 1 : size);
}

} // namespace

void* operator new(size_t size) {
    return Allocate(size);
}
void* operator new[](size_t size) {
    return Allocate(size);
}
void* operator new(size_t size, const std::nothrow_t& tag) noexcept {
    return Allocate(size, tag);
}
void* operator new[](size_t size, const std::nothrow_t& tag) noexcept {
    return Allocate(size, tag);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}

namespace {

constexpr int k_helper_priority = 5;

// Extensions live in static storage: a provider keeps only the span.
constexpr std::array<std::string_view, 2> kImageExtensions = { ".bin", ".png" };

K_THREAD_STACK_DEFINE(referrer_helper_stack, 4096);
k_thread referrer_helper;

AssetProvider
MakeProvider(AssetKind kind, std::string_view name, std::shared_ptr<AssetStore> store = MakeUnavailableStore()) {
    return AssetProvider{
        .kind = kind,
        .name = name,
        .store = std::move(store),
        .extensions = kImageExtensions,
        .access = static_cast<uint8_t>(ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE)),
        .max_size = 4096,
    };
}

// Owned jointly by the test and the helper thread, so that a failed assertion, which aborts the test
// thread without unwinding, never leaves the helper running on a dead stack frame.
struct BlockedReferrer {
    AssetsManager manager;
    k_sem entered;
    k_sem release;

    BlockedReferrer() {
        k_sem_init(&entered, 0, 1);
        k_sem_init(&release, 0, 1);
    }
};

void AskIfReferenced(void* holder, void*, void*) {
    const std::unique_ptr<std::shared_ptr<BlockedReferrer>> state(
        static_cast<std::shared_ptr<BlockedReferrer>*>(holder)
    );

    (*state)->manager.IsReferenced("anything.lua");
}

} // namespace

ZTEST_SUITE(assets_manager, NULL, NULL, NULL, NULL, NULL);

// ======== Kinds and access ========

// The values go over the wire (the SMP assets group): changing one breaks every client.
ZTEST(assets_manager, test_kind_and_access_values_are_the_wire_values) {
    zassert_equal(std::to_underlying(AssetKind::IMAGES), 0);
    zassert_equal(std::to_underlying(AssetKind::ANIMATIONS), 1);
    zassert_equal(std::to_underlying(AssetKind::SENSOR_SCRIPTS), 2);
    zassert_equal(std::to_underlying(AssetKind::CAN_SCRIPTS), 3);
    zassert_equal(std::to_underlying(AssetKind::LOGS), 4);
    zassert_equal(ASSET_KIND_COUNT, 5);

    zassert_equal(ToMask(AssetAccess::READ), 1);
    zassert_equal(ToMask(AssetAccess::WRITE), 2);
    zassert_equal(ToMask(AssetAccess::DELETE), 4);
}

ZTEST(assets_manager, test_has_access_tests_the_bit_of_the_operation) {
    const uint8_t read_write = ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE);

    zassert_true(HasAccess(read_write, AssetAccess::READ));
    zassert_true(HasAccess(read_write, AssetAccess::WRITE));
    zassert_false(HasAccess(read_write, AssetAccess::DELETE));
    zassert_false(HasAccess(0, AssetAccess::READ));
    zassert_true(HasAccess(ToMask(AssetAccess::DELETE), AssetAccess::DELETE));
}

// ======== Providers ========

ZTEST(assets_manager, test_register_and_find_return_the_provider) {
    AssetsManager manager;
    const auto store = MakeUnavailableStore("ui_assets/images");

    zassert_true(manager.Register(MakeProvider(AssetKind::IMAGES, "images", store)));

    const auto provider = manager.Find(AssetKind::IMAGES);

    zassert_not_null(provider.get());
    zassert_true(provider->kind == AssetKind::IMAGES);
    zassert_true(provider->name == "images");
    zassert_true(provider->store == store);
    zassert_equal(provider->extensions.size(), kImageExtensions.size());
    zassert_true(provider->extensions[0] == ".bin");
    zassert_true(provider->extensions[1] == ".png");
    zassert_true(HasAccess(provider->access, AssetAccess::READ));
    zassert_true(HasAccess(provider->access, AssetAccess::WRITE));
    zassert_false(HasAccess(provider->access, AssetAccess::DELETE));
    zassert_equal(provider->max_size, 4096);
    zassert_false(static_cast<bool>(provider->is_in_use), "A provider without a callback has none");
}

ZTEST(assets_manager, test_find_of_a_kind_that_is_not_registered_is_null) {
    AssetsManager manager;

    zassert_true(manager.Find(AssetKind::IMAGES) == nullptr);

    zassert_true(manager.Register(MakeProvider(AssetKind::IMAGES, "images")));

    zassert_true(manager.Find(AssetKind::ANIMATIONS) == nullptr);
    zassert_true(manager.Find(AssetKind::LOGS) == nullptr);
}

ZTEST(assets_manager, test_registering_a_kind_twice_is_refused) {
    AssetsManager manager;
    const auto first_store = MakeUnavailableStore("first");
    const auto second_store = MakeUnavailableStore("second");

    zassert_true(manager.Register(MakeProvider(AssetKind::SENSOR_SCRIPTS, "sensor_scripts", first_store)));
    zassert_false(manager.Register(MakeProvider(AssetKind::SENSOR_SCRIPTS, "other_name", second_store)));

    const auto provider = manager.Find(AssetKind::SENSOR_SCRIPTS);

    zassert_not_null(provider.get());
    zassert_true(provider->store == first_store, "The first provider stays");
    zassert_true(provider->name == "sensor_scripts");
    zassert_equal(manager.GetProviders().size(), 1);
}

ZTEST(assets_manager, test_registering_without_a_store_is_refused) {
    AssetsManager manager;

    zassert_false(manager.Register(MakeProvider(AssetKind::IMAGES, "images", nullptr)));
    zassert_true(manager.Find(AssetKind::IMAGES) == nullptr);
    zassert_true(manager.GetProviders().empty());

    // The refusal did not take the kind.
    zassert_true(manager.Register(MakeProvider(AssetKind::IMAGES, "images")));
}

ZTEST(assets_manager, test_registering_a_kind_out_of_range_is_refused) {
    AssetsManager manager;

    const auto first_out_of_range = static_cast<AssetKind>(ASSET_KIND_COUNT);
    const auto far_out_of_range = static_cast<AssetKind>(255);

    zassert_false(manager.Register(MakeProvider(first_out_of_range, "nothing")));
    zassert_false(manager.Register(MakeProvider(far_out_of_range, "nothing")));

    zassert_true(manager.Find(first_out_of_range) == nullptr);
    zassert_true(manager.Find(far_out_of_range) == nullptr);
    zassert_true(manager.GetProviders().empty());

    // Nothing to remove, and no harm in asking.
    manager.Unregister(first_out_of_range);
    manager.Unregister(far_out_of_range);
}

ZTEST(assets_manager, test_every_kind_can_be_registered) {
    AssetsManager manager;

    for(size_t kind = 0; kind < ASSET_KIND_COUNT; ++kind)
        zassert_true(
            manager.Register(MakeProvider(static_cast<AssetKind>(kind), "kind")), "Kind %d", static_cast<int>(kind)
        );

    zassert_equal(manager.GetProviders().size(), ASSET_KIND_COUNT);
}

ZTEST(assets_manager, test_unregister_removes_the_provider) {
    AssetsManager manager;

    zassert_true(manager.Register(MakeProvider(AssetKind::IMAGES, "images")));
    zassert_true(manager.Register(MakeProvider(AssetKind::ANIMATIONS, "animations")));

    manager.Unregister(AssetKind::IMAGES);

    zassert_true(manager.Find(AssetKind::IMAGES) == nullptr);
    zassert_not_null(manager.Find(AssetKind::ANIMATIONS).get());
    zassert_equal(manager.GetProviders().size(), 1);

    // Unregistering what is not there is harmless.
    manager.Unregister(AssetKind::IMAGES);
    manager.Unregister(AssetKind::LOGS);

    // The kind is free again.
    zassert_true(manager.Register(MakeProvider(AssetKind::IMAGES, "images")));
    zassert_not_null(manager.Find(AssetKind::IMAGES).get());
}

ZTEST(assets_manager, test_get_providers_is_ordered_by_kind) {
    AssetsManager manager;

    zassert_true(manager.GetProviders().empty());

    zassert_true(manager.Register(MakeProvider(AssetKind::LOGS, "logs")));
    zassert_true(manager.Register(MakeProvider(AssetKind::IMAGES, "images")));
    zassert_true(manager.Register(MakeProvider(AssetKind::CAN_SCRIPTS, "can_scripts")));
    zassert_true(manager.Register(MakeProvider(AssetKind::ANIMATIONS, "animations")));

    const auto providers = manager.GetProviders();

    zassert_equal(providers.size(), 4);
    zassert_true(providers[0]->kind == AssetKind::IMAGES);
    zassert_true(providers[1]->kind == AssetKind::ANIMATIONS);
    zassert_true(providers[2]->kind == AssetKind::CAN_SCRIPTS);
    zassert_true(providers[3]->kind == AssetKind::LOGS);
    zassert_true(providers[3]->name == "logs");
}

// A widget that found its provider keeps drawing from it while a controller unregisters the kind.
ZTEST(assets_manager, test_a_found_provider_stays_usable_after_unregister) {
    AssetsManager manager;
    const auto store = MakeUnavailableStore("ui_assets/images");

    zassert_true(manager.Register(MakeProvider(AssetKind::IMAGES, "images", store)));

    const auto found = manager.Find(AssetKind::IMAGES);
    const auto listed = manager.GetProviders();

    manager.Unregister(AssetKind::IMAGES);

    zassert_true(manager.Find(AssetKind::IMAGES) == nullptr);
    zassert_true(manager.GetProviders().empty());

    zassert_not_null(found.get());
    zassert_true(found->name == "images");
    zassert_true(found->store == store);
    zassert_str_equal(found->store->GetDirectory().c_str(), "ui_assets/images");
    zassert_equal(listed.size(), 1);
    zassert_true(listed[0] == found);
}

// The registry hands the callback out and never runs it: the caller does, without the lock.
ZTEST(assets_manager, test_the_in_use_callback_is_the_providers_own) {
    AssetsManager manager;
    int calls = 0;

    auto provider = MakeProvider(AssetKind::IMAGES, "images");
    provider.is_in_use = [&calls](AssetAccess operation, std::string_view name) {
        ++calls;

        return operation == AssetAccess::DELETE && name == "busy.bin";
    };

    zassert_true(manager.Register(std::move(provider)));
    zassert_equal(calls, 0, "Registering does not ask");

    const auto found = manager.Find(AssetKind::IMAGES);

    zassert_not_null(found.get());
    zassert_true(static_cast<bool>(found->is_in_use));
    zassert_true(found->is_in_use(AssetAccess::DELETE, "busy.bin"));
    zassert_false(found->is_in_use(AssetAccess::READ, "busy.bin"));
    zassert_false(found->is_in_use(AssetAccess::DELETE, "idle.bin"));
    zassert_equal(calls, 3);
}

// ======== Referrers ========

ZTEST(assets_manager, test_is_referenced_without_referrers_is_false) {
    AssetsManager manager;

    zassert_false(manager.IsReferenced("scripts/sensors/oil.lua"));
    zassert_false(manager.IsReferenced(""));
}

ZTEST(assets_manager, test_is_referenced_asks_the_referrers) {
    AssetsManager manager;
    int owner = 0;
    int asked = 0;

    manager.AddReferrer(&owner, [&asked](std::string_view path) {
        ++asked;

        return path == "scripts/sensors/oil.lua";
    });

    zassert_true(manager.IsReferenced("scripts/sensors/oil.lua"));
    zassert_equal(asked, 1);
    zassert_false(manager.IsReferenced("scripts/sensors/boost.lua"));
    zassert_equal(asked, 2);
}

ZTEST(assets_manager, test_is_referenced_is_true_when_any_referrer_refers_to_the_path) {
    AssetsManager manager;
    int sensors = 0;
    int can = 0;

    manager.AddReferrer(&sensors, [](std::string_view path) {
        return path == "oil.lua";
    });
    manager.AddReferrer(&sensors, [](std::string_view path) {
        return path == "boost.lua";
    });
    manager.AddReferrer(&can, [](std::string_view path) {
        return path == "dash.lua";
    });

    zassert_true(manager.IsReferenced("oil.lua"));
    zassert_true(manager.IsReferenced("boost.lua"));
    zassert_true(manager.IsReferenced("dash.lua"));
    zassert_false(manager.IsReferenced("other.lua"));
}

ZTEST(assets_manager, test_remove_referrers_removes_only_those_of_the_owner) {
    AssetsManager manager;
    int sensors = 0;
    int can = 0;
    int unknown = 0;

    manager.AddReferrer(&sensors, [](std::string_view path) {
        return path == "oil.lua";
    });
    manager.AddReferrer(&sensors, [](std::string_view path) {
        return path == "boost.lua";
    });
    manager.AddReferrer(&can, [](std::string_view path) {
        return path == "dash.lua";
    });

    manager.RemoveReferrers(&unknown);

    zassert_true(manager.IsReferenced("oil.lua"), "An owner that never added any removes nothing");
    zassert_true(manager.IsReferenced("dash.lua"));

    manager.RemoveReferrers(&sensors);

    zassert_false(manager.IsReferenced("oil.lua"));
    zassert_false(manager.IsReferenced("boost.lua"));
    zassert_true(manager.IsReferenced("dash.lua"), "The other owner's referrer stays");

    manager.RemoveReferrers(&can);

    zassert_false(manager.IsReferenced("dash.lua"));

    // Removing again is harmless.
    manager.RemoveReferrers(&can);
}

// A controller removes its referrers and its kind from its destructor, where an exception would end the
// program, so neither may throw: not even when there is no memory to hold the referrers aside.
ZTEST(assets_manager, test_unregistering_and_removing_referrers_never_throw) {
    static_assert(std::is_nothrow_invocable_v<decltype(&AssetsManager::Unregister), AssetsManager&, AssetKind>);
    static_assert(std::is_nothrow_invocable_v<decltype(&AssetsManager::RemoveReferrers), AssetsManager&, const void*>);

    AssetsManager manager;
    int owner = 0;
    int other = 0;
    const auto token = std::make_shared<int>(0);

    manager.AddReferrer(&owner, [token](std::string_view path) {
        return path == "oil.lua";
    });
    manager.AddReferrer(&owner, [](std::string_view path) {
        return path == "boost.lua";
    });
    manager.AddReferrer(&other, [](std::string_view path) {
        return path == "dash.lua";
    });
    zassert_true(manager.Register(MakeProvider(AssetKind::IMAGES, "images")));
    zassert_equal(token.use_count(), 2, "The referrer holds the token");

    g_fail_allocations = true;
    manager.RemoveReferrers(&owner);
    manager.Unregister(AssetKind::IMAGES);
    g_fail_allocations = false;

    zassert_false(manager.IsReferenced("oil.lua"), "The owner's referrers are gone");
    zassert_false(manager.IsReferenced("boost.lua"));
    zassert_true(manager.IsReferenced("dash.lua"), "The other owner's referrer stays");
    zassert_equal(token.use_count(), 1, "and the removed ones were destroyed");
    zassert_true(manager.Find(AssetKind::IMAGES) == nullptr, "The kind is gone");
}

ZTEST(assets_manager, test_an_empty_referrer_is_ignored) {
    AssetsManager manager;
    int owner = 0;

    manager.AddReferrer(&owner, AssetsManager::Referrer{});

    zassert_false(manager.IsReferenced("oil.lua"), "An empty function must not be called");
}

// The configuration's referrer asks a controller, which may ask the registry about the kind it
// belongs to. With the lock held across the call, the same thread would only get through because
// Zephyr's mutexes are recursive; the next test checks the lock itself.
ZTEST(assets_manager, test_a_referrer_may_call_back_into_the_manager) {
    AssetsManager manager;
    int owner = 0;

    zassert_true(manager.Register(MakeProvider(AssetKind::SENSOR_SCRIPTS, "sensor_scripts")));

    AssetsManager* registry = &manager;

    // "probe" ends the recursion: the nested question reaches this referrer too.
    manager.AddReferrer(&owner, [registry](std::string_view path) {
        if(path == "probe")
            return false;

        return registry->Find(AssetKind::SENSOR_SCRIPTS) != nullptr
            && !registry->IsReferenced("probe")
            && path == "used.lua";
    });

    zassert_true(manager.IsReferenced("used.lua"));
    zassert_false(manager.IsReferenced("unused.lua"));
}

ZTEST(assets_manager, test_a_referrer_may_change_the_referrers_while_it_runs) {
    AssetsManager manager;
    int owner = 0;
    int calls = 0;

    AssetsManager* registry = &manager;

    // The referrer removes itself: the call in progress keeps it alive.
    manager.AddReferrer(&owner, [registry, &owner, &calls](std::string_view) {
        ++calls;
        registry->RemoveReferrers(&owner);
        registry->AddReferrer(&calls, [](std::string_view) {
            return false;
        });

        return true;
    });

    zassert_true(manager.IsReferenced("used.lua"));
    zassert_equal(calls, 1);

    zassert_false(manager.IsReferenced("used.lua"), "The referrer is gone");
    zassert_equal(calls, 1);

    manager.RemoveReferrers(&calls);
}

// A referrer that is stuck, say behind the LVGL lock, must not hold up whoever needs the registry:
// widgets call Find() under the LVGL lock, so a registry lock held across a referrer could deadlock.
ZTEST(assets_manager, test_referrers_run_without_the_registry_lock) {
    const auto state = std::make_shared<BlockedReferrer>();
    BlockedReferrer* blocked = state.get();

    blocked->manager.AddReferrer(blocked, [blocked](std::string_view) {
        k_sem_give(&blocked->entered);

        // Stays inside the referrer until the test is done, or gives up.
        k_sem_take(&blocked->release, K_SECONDS(5));

        return false;
    });

    k_thread_create(
        &referrer_helper, referrer_helper_stack, K_THREAD_STACK_SIZEOF(referrer_helper_stack), AskIfReferenced,
        new std::shared_ptr<BlockedReferrer>(state), nullptr, nullptr, k_helper_priority, 0, K_NO_WAIT
    );

    zassert_ok(k_sem_take(&blocked->entered, K_SECONDS(2)), "The helper never reached the referrer");

    // The helper is inside a referrer now. Each call below waits for the registry's mutex if
    // IsReferenced() holds it, and the helper lets go only after its five seconds.
    const int64_t started = k_uptime_get();

    const bool registered = blocked->manager.Register(MakeProvider(AssetKind::IMAGES, "images"));
    const bool found = blocked->manager.Find(AssetKind::IMAGES) != nullptr;
    const size_t listed = blocked->manager.GetProviders().size();
    blocked->manager.AddReferrer(&listed, [](std::string_view) {
        return false;
    });
    blocked->manager.RemoveReferrers(&listed);
    blocked->manager.Unregister(AssetKind::IMAGES);

    const int64_t elapsed = k_uptime_get() - started;

    k_sem_give(&blocked->release);

    zassert_ok(k_thread_join(&referrer_helper, K_SECONDS(2)));

    zassert_true(registered);
    zassert_true(found);
    zassert_equal(listed, 1);
    zassert_true(
        elapsed < 1000, "The registry stayed locked for %d ms while a referrer ran", static_cast<int>(elapsed)
    );
}
