#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "controllers/dfu_controller.h"
#include "subsys/dfu/zephyr_watchdog.h"

#include "dfu_test_support.h"

// bootutil_public.c: writes the trailer magic of a slot. It is not in the public header.
extern "C" int boot_write_magic(const struct flash_area* fap);

using namespace dfu_test;

using eerie_leap::controllers::DfuController;
using eerie_leap::subsys::dfu::IBootControl;
using eerie_leap::subsys::dfu::ImageConfirmationConfig;
using eerie_leap::subsys::dfu::IWatchdog;
using eerie_leap::subsys::dfu::ZephyrWatchdog;
using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr uint8_t STATUS = 0;

// The timing of the injected configuration: ticks every 100 ms, and a probe may wait 200 ms.
constexpr ImageConfirmationConfig FAKE_CONFIG{
    .self_confirm = true,
    .boot_deadline_ms = 600,
    .confirm_delay_ms = 300,
    .watchdog_timeout_ms = 400,
};

// The same, with a confirmation delay that outlasts the tests which block a queue after boot complete: they
// must not race the confirmation.
constexpr ImageConfirmationConfig SLOW_CONFIRM_CONFIG{
    .self_confirm = true,
    .boot_deadline_ms = 600,
    .confirm_delay_ms = 3000,
    .watchdog_timeout_ms = 400,
};

class FakeBootControl final : public IBootControl {
public:
    std::atomic<bool> is_confirmed{ false };
    std::atomic<int> confirm_calls{ 0 };

    [[nodiscard]] bool IsConfirmed() const override {
        return is_confirmed.load();
    }

    [[nodiscard]] int Confirm() override {
        confirm_calls++;
        is_confirmed = true;

        return 0;
    }
};

class FakeWatchdog final : public IWatchdog {
public:
    std::atomic<int> installs{ 0 };
    std::atomic<int> starts{ 0 };
    std::atomic<int> feeds{ 0 };
    std::atomic<int> disables{ 0 };

    [[nodiscard]] int Install(uint32_t) override {
        installs++;
        return 0;
    }

    [[nodiscard]] int Start() override {
        starts++;
        return 0;
    }

    [[nodiscard]] int Feed() override {
        feeds++;
        return 0;
    }

    [[nodiscard]] int Disable() override {
        disables++;
        return 0;
    }
};

// A work queue of its own, so that a test can keep it busy. One for the whole suite: a queue's thread
// outlives any object that a test could keep its stack in.
class TestWorkQueue {
private:
    k_work_q work_q_{};
    K_KERNEL_STACK_MEMBER(stack_, 2048) {};

public:
    TestWorkQueue() {
        k_work_queue_init(&work_q_);
        k_work_queue_start(&work_q_, stack_, K_KERNEL_STACK_SIZEOF(stack_), K_PRIO_PREEMPT(10), nullptr);
    }

    static TestWorkQueue& Instance() {
        static TestWorkQueue instance;

        return instance;
    }

    TestWorkQueue(const TestWorkQueue&) = delete;
    TestWorkQueue& operator=(const TestWorkQueue&) = delete;
    TestWorkQueue(TestWorkQueue&&) = delete;
    TestWorkQueue& operator=(TestWorkQueue&&) = delete;
    ~TestWorkQueue() = default;

    k_work_q* Get() {
        return &work_q_;
    }
};

// Blocks a work queue's thread until released.
struct Blocker {
    k_work work{};
    k_sem release{};

    Blocker() {
        k_sem_init(&release, 0, 1);
        k_work_init(&work, [](k_work* item) {
            k_sem_take(&CONTAINER_OF(item, Blocker, work)->release, K_FOREVER);
        });
    }

    void Block(k_work_q* work_q) {
        k_work_submit_to_queue(work_q, &work);
    }

    // Lets the queue go on and waits until the work is done with this object.
    void Release() {
        k_sem_give(&release);

        k_work_sync sync{};
        k_work_flush(&work, &sync);
    }
};

bool WaitFor(const std::function<bool()>& condition, int timeout_ms) {
    for(int waited = 0; waited < timeout_ms; waited += 10) {
        if(condition())
            return true;

        k_msleep(10);
    }

    return condition();
}

// Marks the running image (slot 0) unconfirmed, as MCUboot leaves it after a swap for a test: the trailer
// has the magic, and image_ok is not set.
void MarkSlot0Unconfirmed() {
    const flash_area* area = OpenSlot(SLOT0_ID);
    zassert_equal(boot_write_magic(area), 0);
    flash_area_close(area);

    zassert_false(boot_is_img_confirmed(), "The slot trailer does not mark slot 0 unconfirmed");
}

struct dfu_controller_fixture {
    SmpTestClient client;
    FakeBootControl boot_control;
    FakeWatchdog watchdog;
    std::optional<DfuController> controller;
    // Owned by the fixture, not by a test: a failed assertion leaves the test body without running its
    // destructors, and a blocker that is gone while its work still holds the static queue would wedge the
    // rest of the suite.
    std::vector<std::unique_ptr<Blocker>> blockers;

    Blocker& NewBlocker() {
        blockers.push_back(std::make_unique<Blocker>());

        return *blockers.back();
    }

    // Lets every blocked queue go on, and forgets the blockers.
    void ReleaseBlockers() {
        for(const auto& blocker : blockers)
            blocker->Release();

        blockers.clear();
    }
};

void* SetUp() {
    return new dfu_controller_fixture();
}

// Every test starts with an empty slot 0 (a confirmed image, as a preprogrammed one counts) and no controller.
void Before(void* data) {
    auto* fixture = static_cast<dfu_controller_fixture*>(data);

    fixture->ReleaseBlockers();
    fixture->controller.reset();
    fixture->boot_control.is_confirmed = false;
    fixture->boot_control.confirm_calls = 0;
    fixture->watchdog.installs = 0;
    fixture->watchdog.starts = 0;
    fixture->watchdog.feeds = 0;
    fixture->watchdog.disables = 0;

    EraseSlot(SLOT0_ID);
    EraseSlot(SLOT1_ID);
}

void After(void* data) {
    auto* fixture = static_cast<dfu_controller_fixture*>(data);

    // The blocked queues first: the probes of the controller wait for their work items.
    fixture->ReleaseBlockers();
    fixture->controller.reset();
}

void TearDown(void* data) {
    delete static_cast<dfu_controller_fixture*>(data);
}

} // namespace

ZTEST_SUITE(dfu_controller, NULL, SetUp, Before, After, TearDown);

ZTEST_F(dfu_controller, test_confirmed_image_arms_nothing) {
    fixture->controller.emplace();
    zassert_equal(fixture->controller->Initialize(), 0);

    zassert_equal(fixture->controller->GetState(), DfuState::CONFIRMED);

    const Response response = fixture->client.Read(SmpGroupId::DFU, STATUS);
    zassert_true(response.IsOk());
    zassert_true(response.Flag("confirmed"));
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::CONFIRMED));
    zassert_false(response.Has("confirm_in_ms"));
    zassert_true(response.Has("reset_cause"));

    // Boot complete has nothing to do, and nothing runs in the background.
    fixture->controller->ReportBootComplete();
    k_msleep(100);
    zassert_equal(fixture->controller->GetState(), DfuState::CONFIRMED);
    zassert_true(boot_is_img_confirmed());
}

ZTEST_F(dfu_controller, test_confirmed_image_skips_the_probes) {
    TestWorkQueue& work_q = TestWorkQueue::Instance();
    Blocker& blocker = fixture->NewBlocker();

    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    fixture->boot_control.is_confirmed = true;
    zassert_equal(fixture->controller->Initialize(), 0);

    // A stuck queue changes nothing for a confirmed image.
    blocker.Block(work_q.Get());
    fixture->controller->RegisterLivenessProbe("stuck", work_q.Get());
    k_msleep(300);

    zassert_equal(fixture->controller->GetState(), DfuState::CONFIRMED);
    zassert_equal(fixture->watchdog.installs.load(), 0, "A confirmed image must not arm the watchdog");
    zassert_equal(fixture->watchdog.feeds.load(), 0);

    blocker.Release();
}

#ifndef CONFIG_EERIE_LEAP_DFU_BENCH_FAULT_NO_REPORT

// The real bootutil on the flash simulator: slot 0 with the trailer of an unconfirmed image.
ZTEST_F(dfu_controller, test_unconfirmed_image_confirms_after_boot_complete) {
    zassert_false(ZephyrWatchdog::IsSupported(), "native_sim has no watchdog0");

    MarkSlot0Unconfirmed();

    fixture->controller.emplace();
    zassert_equal(fixture->controller->Initialize(), 0);
    zassert_equal(fixture->controller->GetState(), DfuState::VERIFYING);

    Response response = fixture->client.Read(SmpGroupId::DFU, STATUS);
    zassert_true(response.IsOk());
    zassert_false(response.Flag("confirmed"));
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::VERIFYING));
    zassert_false(response.Has("confirm_in_ms"));

    // Without boot complete the image stays as it is, however long it runs.
    k_msleep(3 * CONFIG_EERIE_LEAP_DFU_CONFIRM_DELAY_MS);
    zassert_equal(fixture->controller->GetState(), DfuState::VERIFYING);
    zassert_false(boot_is_img_confirmed());

    fixture->controller->ReportBootComplete();
    zassert_equal(fixture->controller->GetState(), DfuState::CONFIRMING);

    response = fixture->client.Read(SmpGroupId::DFU, STATUS);
    zassert_true(response.IsOk());
    zassert_false(response.Flag("confirmed"));
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::CONFIRMING));
    zassert_true(response.Has("confirm_in_ms"));
    zassert_true(response.Value("confirm_in_ms") <= CONFIG_EERIE_LEAP_DFU_CONFIRM_DELAY_MS);

    zassert_true(
        WaitFor(
            [] {
                return boot_is_img_confirmed();
            },
            3 * CONFIG_EERIE_LEAP_DFU_CONFIRM_DELAY_MS + 500
        ),
        "The image did not confirm itself"
    );
    zassert_true(WaitFor(
        [&] {
            return fixture->controller->GetState() == DfuState::CONFIRMED;
        },
        500
    ));

    response = fixture->client.Read(SmpGroupId::DFU, STATUS);
    zassert_true(response.IsOk());
    zassert_true(response.Flag("confirmed"));
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::CONFIRMED));
    zassert_false(response.Has("confirm_in_ms"));
}

// A client that confirms through img_mgmt makes the controller stand down.
ZTEST_F(dfu_controller, test_client_confirm_stands_down) {
    MarkSlot0Unconfirmed();

    fixture->controller.emplace();
    zassert_equal(fixture->controller->Initialize(), 0);
    zassert_equal(fixture->controller->GetState(), DfuState::VERIFYING);

    zassert_equal(boot_write_img_confirmed(), 0);

    zassert_true(WaitFor(
        [&] {
            return fixture->controller->GetState() == DfuState::CONFIRMED;
        },
        2000
    ));
}

ZTEST_F(dfu_controller, test_unconfirmed_image_arms_the_watchdog_and_disables_it_once_confirmed) {
    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);

    zassert_equal(fixture->controller->GetState(), DfuState::VERIFYING);
    zassert_equal(fixture->watchdog.installs.load(), 1);
    zassert_equal(fixture->watchdog.starts.load(), 1);

    zassert_true(
        WaitFor(
            [&] {
                return fixture->watchdog.feeds.load() >= 2;
            },
            500
        ),
        "A healthy verifying image feeds the watchdog"
    );

    fixture->controller->ReportBootComplete();

    zassert_true(WaitFor(
        [&] {
            return fixture->controller->GetState() == DfuState::CONFIRMED;
        },
        2000
    ));
    zassert_equal(fixture->boot_control.confirm_calls.load(), 1);
    zassert_true(WaitFor(
        [&] {
            return fixture->watchdog.disables.load() == 1;
        },
        500
    ));
}

ZTEST_F(dfu_controller, test_a_stuck_queue_stops_the_feeding) {
    TestWorkQueue& work_q = TestWorkQueue::Instance();
    Blocker& blocker = fixture->NewBlocker();

    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, SLOW_CONFIRM_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);
    fixture->controller->RegisterLivenessProbe("test_queue", work_q.Get());
    fixture->controller->ReportBootComplete();

    zassert_true(WaitFor(
        [&] {
            return fixture->watchdog.feeds.load() >= 2;
        },
        500
    ));

    blocker.Block(work_q.Get());

    zassert_true(
        WaitFor(
            [&] {
                return fixture->controller->GetState() == DfuState::FAILED;
            },
            1000
        ),
        "A stuck probe must fail the image"
    );

    const int feeds = fixture->watchdog.feeds.load();
    k_msleep(300);
    zassert_equal(fixture->watchdog.feeds.load(), feeds, "A failed image must not feed the watchdog");
    zassert_equal(fixture->boot_control.confirm_calls.load(), 0, "A failed image must not confirm");

    blocker.Release();
}

// A queue that is stuck from the moment the boot completes never runs its probe: the image fails and is never
// confirmed, although the confirmation delay (300 ms) ends before the probe's time is up and the failure is found.
ZTEST_F(dfu_controller, test_a_queue_stuck_at_boot_complete_fails_the_image_without_confirming) {
    TestWorkQueue& work_q = TestWorkQueue::Instance();
    Blocker& blocker = fixture->NewBlocker();

    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);
    fixture->controller->RegisterLivenessProbe("test_queue", work_q.Get());
    blocker.Block(work_q.Get());
    fixture->controller->ReportBootComplete();

    zassert_true(
        WaitFor(
            [&] {
                return fixture->controller->GetState() == DfuState::FAILED;
            },
            1000
        ),
        "A stuck probe must fail the image"
    );
    k_msleep(300);
    zassert_equal(fixture->boot_control.confirm_calls.load(), 0, "The image must not confirm with a probe outstanding");

    blocker.Release();
}

// While the image verifies, a unit that is still booting may keep its queues from running for a long time.
ZTEST_F(dfu_controller, test_a_stuck_queue_does_not_fail_a_verifying_image) {
    TestWorkQueue& work_q = TestWorkQueue::Instance();
    Blocker& blocker = fixture->NewBlocker();

    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);
    fixture->controller->RegisterLivenessProbe("test_queue", work_q.Get());
    blocker.Block(work_q.Get());

    // Well past what a probe may wait, still inside the boot deadline.
    k_msleep(350);
    zassert_equal(fixture->controller->GetState(), DfuState::VERIFYING);
    const int feeds = fixture->watchdog.feeds.load();
    zassert_true(feeds >= 2, "The watchdog is fed while the boot may still complete");

    blocker.Release();
}

// The queue of a WorkQueueThread is kept alive for its probe, whoever else releases it.
ZTEST_F(dfu_controller, test_a_probe_keeps_its_queue_alive) {
    auto queue_thread = std::make_shared<WorkQueueThread>("dfu_test_queue", 2048, 5);
    zassert_true(queue_thread->Initialize());

    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);
    fixture->controller->RegisterLivenessProbe("owned_queue", queue_thread);
    queue_thread.reset();

    fixture->controller->ReportBootComplete();

    zassert_true(WaitFor(
        [&] {
            return fixture->controller->GetState() == DfuState::CONFIRMED;
        },
        2000
    ));
}

// A queue that is not running cannot be probed: it is skipped, and the unit goes on.
ZTEST_F(dfu_controller, test_a_queue_that_is_not_running_is_skipped) {
    const auto queue_thread = std::make_shared<WorkQueueThread>("dfu_test_idle", 2048, 5);

    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);
    fixture->controller->RegisterLivenessProbe("idle_queue", queue_thread);
    fixture->controller->RegisterLivenessProbe("missing_queue", std::shared_ptr<WorkQueueThread>());
    fixture->controller->ReportBootComplete();

    zassert_true(WaitFor(
        [&] {
            return fixture->controller->GetState() == DfuState::CONFIRMED;
        },
        2000
    ));
}

ZTEST_F(dfu_controller, test_a_running_queue_keeps_the_image_healthy) {
    TestWorkQueue& work_q = TestWorkQueue::Instance();

    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);
    fixture->controller->RegisterLivenessProbe("test_queue", work_q.Get());
    fixture->controller->ReportBootComplete();

    zassert_true(WaitFor(
        [&] {
            return fixture->controller->GetState() == DfuState::CONFIRMED;
        },
        2000
    ));
    zassert_equal(fixture->boot_control.confirm_calls.load(), 1);
}

#endif // !CONFIG_EERIE_LEAP_DFU_BENCH_FAULT_NO_REPORT

ZTEST_F(dfu_controller, test_a_missed_deadline_stops_the_feeding) {
    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);

    zassert_true(WaitFor(
        [&] {
            return fixture->controller->GetState() == DfuState::FAILED;
        },
        FAKE_CONFIG.boot_deadline_ms + 500
    ));

    const int feeds = fixture->watchdog.feeds.load();
    k_msleep(300);
    zassert_equal(fixture->watchdog.feeds.load(), feeds);

    // Too late: the report does not revive a failed image.
    fixture->controller->ReportBootComplete();
    zassert_equal(fixture->controller->GetState(), DfuState::FAILED);

    const Response response = fixture->client.Read(SmpGroupId::DFU, STATUS);
    zassert_true(response.IsOk());
    zassert_false(response.Flag("confirmed"));
    zassert_equal(response.Value("state"), std::to_underlying(DfuState::FAILED));
}

#ifdef CONFIG_EERIE_LEAP_DFU_BENCH_FAULT_NO_REPORT

// The no_report bench fault: an unconfirmed image never gets to confirm itself.
ZTEST_F(dfu_controller, test_bench_fault_ignores_boot_complete) {
    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);

    fixture->controller->ReportBootComplete();
    zassert_equal(fixture->controller->GetState(), DfuState::VERIFYING);

    zassert_true(WaitFor(
        [&] {
            return fixture->controller->GetState() == DfuState::FAILED;
        },
        FAKE_CONFIG.boot_deadline_ms + 500
    ));
    zassert_equal(fixture->boot_control.confirm_calls.load(), 0);
}

// A confirmed image runs normally, whatever the fault: a reverted image is not hurt by it.
ZTEST_F(dfu_controller, test_bench_fault_leaves_a_confirmed_image_alone) {
    fixture->boot_control.is_confirmed = true;
    fixture->controller.emplace(fixture->boot_control, &fixture->watchdog, FAKE_CONFIG);
    zassert_equal(fixture->controller->Initialize(), 0);

    fixture->controller->ReportBootComplete();
    zassert_equal(fixture->controller->GetState(), DfuState::CONFIRMED);
}

#endif // CONFIG_EERIE_LEAP_DFU_BENCH_FAULT_NO_REPORT
