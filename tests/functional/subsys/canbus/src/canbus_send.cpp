#include <array>
#include <climits>
#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "canbus_test_support.h"

using namespace canbus_test;

using eerie_leap::subsys::canbus::CanId;

namespace {

constexpr std::array<uint8_t, 1> PAYLOAD = {0x01};
constexpr CanId FILL_ID = CanId::Standard(0x7F2);

// Parks the loopback TX thread in a TX-complete callback, which runs outside the
// driver's filter lock, so queued frames stay queued until Release().
k_sem tx_release;

void BlockingTxCallback(const device*, int, void* user_data) {
    k_sem_take(static_cast<k_sem*>(user_data), K_SECONDS(10));
}

bool BlockTxThread() {
    k_sem_reset(&tx_release);

    can_frame frame = {.id = 0x7F0, .dlc = 0, .flags = 0};
    if(can_send(LoopbackDevice(), &frame, K_NO_WAIT, BlockingTxCallback, &tx_release) != 0)
        return false;

    // Let the TX thread take the frame and park in the callback.
    k_sleep(K_MSEC(20));

    return true;
}

void ReleaseTxThread() {
    k_sem_give(&tx_release);
}

int FillTxQueue(Canbus& canbus) {
    int result = 0;
    for(int i = 0; i < 256 && result == 0; i++)
        result = canbus.SendFrame(FILL_ID, PAYLOAD, K_NO_WAIT);

    return result;
}

void* SetupSuite() {
    k_sem_init(&tx_release, 0, 1);

    return nullptr;
}

void AfterTest(void* fixture) {
    ReleaseTxThread();
    ResetLoopbackDevice(fixture);
}

K_THREAD_STACK_DEFINE(sender_stack, 4096);
k_thread sender_thread;
atomic_t sender_result;

constexpr atomic_val_t SENDER_PENDING = INT_MIN;

void SendWithLongTimeout(void* canbus, void*, void*) {
    atomic_set(&sender_result, static_cast<Canbus*>(canbus)->SendFrame(FILL_ID, PAYLOAD, K_SECONDS(5)));
}

} // namespace

ZTEST_SUITE(canbus_send, NULL, SetupSuite, NULL, AfterTest, NULL);

ZTEST(canbus_send, test_full_tx_queue_returns_eagain) {
    auto canbus = MakeRunningCanbus();

    zassert_true(BlockTxThread());
    zassert_equal(FillTxQueue(*canbus), -EAGAIN, "A full TX queue must report -EAGAIN without blocking");

    ReleaseTxThread();
}

ZTEST(canbus_send, test_blocked_send_does_not_hold_the_bus_lock) {
    auto canbus = MakeRunningCanbus();

    zassert_true(BlockTxThread());
    zassert_equal(FillTxQueue(*canbus), -EAGAIN);

    atomic_set(&sender_result, SENDER_PENDING);
    k_thread_create(&sender_thread, sender_stack, K_THREAD_STACK_SIZEOF(sender_stack),
        SendWithLongTimeout, canbus.get(), nullptr, nullptr, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);

    // Let the sender enter can_send() and wait for a TX slot.
    k_sleep(K_MSEC(20));

    const int64_t start = k_uptime_get();
    int id = canbus->RegisterFrameReceivedHandler(CanId::Standard(0x7F3), [](const CanFrame&) {});
    const int64_t elapsed = k_uptime_get() - start;

    zassert_true(id > 0);
    zassert_equal(atomic_get(&sender_result), SENDER_PENDING, "The sender must still be waiting for a TX slot");
    zassert_true(elapsed < 100, "Registration waited %lld ms behind a blocked sender", elapsed);

    ReleaseTxThread();

    zassert_ok(k_thread_join(&sender_thread, K_SECONDS(2)));
    zassert_ok(atomic_get(&sender_result), "The blocked send must complete once the queue drains");

    zassert_true(canbus->RemoveFrameReceivedHandler(id));
}

ZTEST(canbus_send, test_send_before_initialize_reports_network_down) {
    Canbus canbus(MakeConfig());

    zassert_equal(canbus.SendFrame(CanId::Standard(0x100), PAYLOAD), -ENETDOWN);
}

ZTEST(canbus_send, test_explicit_timeout_send_succeeds) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x7F4), [&](const CanFrame& f) { collector.Collect(f); }) > 0);

    zassert_ok(canbus->SendFrame(CanId::Standard(0x7F4), PAYLOAD, K_NO_WAIT));
    zassert_true(collector.Wait());
}
