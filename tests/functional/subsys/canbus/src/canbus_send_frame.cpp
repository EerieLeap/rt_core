#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "canbus_test_support.h"

using namespace canbus_test;

using eerie_leap::subsys::canbus::CanId;
using eerie_leap::subsys::canbus::ICanFrameTap;

namespace {

/// The payload bytes a DLC carries, spelled out so the tests don't lean on can_dlc_to_bytes.
constexpr std::array<size_t, 16> FD_LENGTHS = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

constexpr uint32_t STD_ID = 0x123;
constexpr uint32_t EXT_ID = 0x18FF0042;

std::vector<uint8_t> MakePayload(size_t size, uint8_t seed = 1) {
    std::vector<uint8_t> payload(size);
    for(size_t i = 0; i < size; ++i)
        payload[i] = static_cast<uint8_t>(seed + (i * 7));

    return payload;
}

bool IsFdLength(size_t size) {
    for(const size_t length : FD_LENGTHS) {
        if(length == size)
            return true;
    }

    return false;
}

CanFrame MakeFrame(uint32_t id, bool ide, std::span<const uint8_t> payload = {}) {
    CanFrame frame;
    frame.id = id;
    frame.is_extended = ide;
    frame.data.Assign(payload);

    return frame;
}

CanFrame MakeFdFrame(uint32_t id, bool ide, bool brs, std::span<const uint8_t> payload) {
    CanFrame frame = MakeFrame(id, ide, payload);
    frame.is_can_fd = true;
    frame.is_bitrate_switch = brs;

    return frame;
}

CanFrame MakeRemoteFrame(uint32_t id, bool ide, uint8_t dlc) {
    CanFrame frame = MakeFrame(id, ide);
    frame.is_remote_request = true;
    frame.dlc = dlc;

    return frame;
}

uint8_t FlagsOf(bool ide, bool fdf, bool brs, bool rtr) {
    uint8_t flags = 0;
    if(ide)
        flags |= CAN_FRAME_IDE;
    if(fdf)
        flags |= CAN_FRAME_FDF;
    if(brs)
        flags |= CAN_FRAME_BRS;
    if(rtr)
        flags |= CAN_FRAME_RTR;

    return flags;
}

// Takes what the controller delivers straight from the driver, so the flags are the wire's and no
// Canbus handler (which never sees a remote frame and rebuilds the frame) is in between. A failing
// assertion unwinds past destructors, so the suite's after-hook calls Stop().
class RawReceiver {
private:
    k_mutex lock_{};
    k_sem semaphore_{};
    std::vector<can_frame> frames_;
    std::array<int, 2> filter_ids_ = { -1, -1 };

    static void OnFrame(const device*, can_frame* frame, void* user_data) {
        auto* self = static_cast<RawReceiver*>(user_data);

        k_mutex_lock(&self->lock_, K_FOREVER);
        self->frames_.push_back(*frame);
        k_mutex_unlock(&self->lock_);

        k_sem_give(&self->semaphore_);
    }

public:
    // Not a constructor: the kernel objects are set up by the suite, not by static initialization.
    void Init() {
        k_mutex_init(&lock_);
        k_sem_init(&semaphore_, 0, K_SEM_MAX_LIMIT);
    }

    void Start() {
        Stop();

        const can_filter standard = { .id = 0, .mask = 0, .flags = 0 };
        const can_filter extended = { .id = 0, .mask = 0, .flags = CAN_FILTER_IDE };

        filter_ids_[0] = can_add_rx_filter(LoopbackDevice(), OnFrame, this, &standard);
        filter_ids_[1] = can_add_rx_filter(LoopbackDevice(), OnFrame, this, &extended);
        zassert_true(filter_ids_[0] >= 0 && filter_ids_[1] >= 0, "The loopback has no filters left");
    }

    void Stop() {
        for(int& id : filter_ids_) {
            if(id >= 0)
                can_remove_rx_filter(LoopbackDevice(), id);

            id = -1;
        }

        k_mutex_lock(&lock_, K_FOREVER);
        frames_.clear();
        k_mutex_unlock(&lock_);

        k_sem_reset(&semaphore_);
    }

    bool Wait(int timeout_ms = 500) {
        return k_sem_take(&semaphore_, K_MSEC(timeout_ms)) == 0;
    }

    size_t Count() {
        k_mutex_lock(&lock_, K_FOREVER);
        const size_t count = frames_.size();
        k_mutex_unlock(&lock_);

        return count;
    }

    can_frame At(size_t index) {
        k_mutex_lock(&lock_, K_FOREVER);
        const can_frame frame = frames_.at(index);
        k_mutex_unlock(&lock_);

        return frame;
    }

    // The frame that arrives for a send that must succeed.
    can_frame SendAndReceive(const Canbus& canbus, const CanFrame& frame) {
        const size_t before = Count();

        zassert_ok(canbus.SendFrame(frame, K_MSEC(100)), "Frame 0x%x was refused", frame.id);
        zassert_true(Wait(), "Frame 0x%x did not arrive", frame.id);
        zassert_equal(Count(), before + 1);

        return At(before);
    }

    // A send that must be refused with -EINVAL, and put nothing on the bus.
    void ExpectRefused(const Canbus& canbus, const CanFrame& frame, const char* what) {
        const size_t before = Count();

        zassert_equal(canbus.SendFrame(frame, K_MSEC(100)), -EINVAL, "%s was not refused", what);
        k_sleep(K_MSEC(20));
        zassert_equal(Count(), before, "%s reached the bus", what);
    }
};

RawReceiver receiver;

void* SetupSuite() {
    receiver.Init();

    return nullptr;
}

void AfterTest(void* fixture) {
    receiver.Stop();
    ResetLoopbackDevice(fixture);
}

void ExpectFrame(
    const can_frame& raw,
    uint32_t id,
    uint8_t flags,
    uint8_t dlc,
    std::span<const uint8_t> payload,
    const char* what
) {
    zassert_equal(raw.id, id, "%s: ID 0x%x", what, raw.id);
    zassert_equal(raw.flags, flags, "%s: flags 0x%02x, expected 0x%02x", what, raw.flags, flags);
    zassert_equal(raw.dlc, dlc, "%s: DLC %u, expected %u", what, raw.dlc, dlc);

    for(size_t i = 0; i < payload.size(); ++i)
        zassert_equal(raw.data[i], payload[i], "%s: byte %zu", what, i);
}

// Sees the unit's own frames and the loopback's copies of them.
class EchoTap : public ICanFrameTap {
private:
    k_mutex lock_{};
    std::vector<CanFrame> echoes_;

public:
    EchoTap() {
        k_mutex_init(&lock_);
    }

    void OnFrame(const CanFrame& frame, uint64_t, bool is_transmit, int tx_error) override {
        if(!is_transmit || tx_error != 0)
            return;

        k_mutex_lock(&lock_, K_FOREVER);
        echoes_.push_back(frame);
        k_mutex_unlock(&lock_);
    }

    void OnState(can_state, can_bus_err_cnt, uint64_t) override {}
    void OnFilterSwap(bool, uint64_t) override {}

    std::vector<CanFrame> Echoes() {
        k_mutex_lock(&lock_, K_FOREVER);
        std::vector<CanFrame> copy = echoes_;
        k_mutex_unlock(&lock_);

        return copy;
    }
};

bool WaitForEchoes(EchoTap& tap, size_t count) {
    for(int waited = 0; waited <= 2000; waited += 5) {
        if(tap.Echoes().size() >= count)
            return true;

        k_sleep(K_MSEC(5));
    }

    return false;
}

} // namespace

ZTEST_SUITE(canbus_send_frame, NULL, SetupSuite, NULL, AfterTest, NULL);

ZTEST(canbus_send_frame, test_classical_frames_arrive_with_their_flags_dlc_and_payload) {
    auto canbus = MakeRunningCanbus();
    receiver.Start();

    for(const bool ide : { false, true }) {
        const uint32_t id = ide ? EXT_ID : STD_ID;

        for(size_t size = 0; size <= 8; ++size) {
            const auto payload = MakePayload(size, static_cast<uint8_t>(size + 1));
            const can_frame raw = receiver.SendAndReceive(*canbus, MakeFrame(id, ide, payload));

            ExpectFrame(raw, id, FlagsOf(ide, false, false, false), static_cast<uint8_t>(size), payload, "classical");
        }
    }
}

ZTEST(canbus_send_frame, test_remote_frames_arrive_with_their_dlc_and_no_payload) {
    auto canbus = MakeRunningCanbus();
    receiver.Start();

    for(const bool ide : { false, true }) {
        const uint32_t id = ide ? EXT_ID : STD_ID;

        for(uint8_t dlc = 0; dlc <= 8; ++dlc) {
            const can_frame raw = receiver.SendAndReceive(*canbus, MakeRemoteFrame(id, ide, dlc));

            ExpectFrame(raw, id, FlagsOf(ide, false, false, true), dlc, {}, "remote");
        }
    }
}

ZTEST(canbus_send_frame, test_fd_frames_arrive_with_and_without_brs_in_every_length) {
    auto canbus = MakeRunningCanbus(CanbusType::CANFD);
    receiver.Start();

    for(const bool ide : { false, true }) {
        const uint32_t id = ide ? EXT_ID : STD_ID;

        for(const bool brs : { false, true }) {
            for(size_t dlc = 0; dlc < FD_LENGTHS.size(); ++dlc) {
                const auto payload = MakePayload(FD_LENGTHS[dlc], static_cast<uint8_t>(dlc + 1));
                const can_frame raw = receiver.SendAndReceive(*canbus, MakeFdFrame(id, ide, brs, payload));

                ExpectFrame(raw, id, FlagsOf(ide, true, brs, false), static_cast<uint8_t>(dlc), payload, "FD");
            }
        }
    }
}

ZTEST(canbus_send_frame, test_an_fd_channel_also_sends_classical_and_remote_frames) {
    auto canbus = MakeRunningCanbus(CanbusType::CANFD);
    receiver.Start();

    const auto payload = MakePayload(8);

    ExpectFrame(
        receiver.SendAndReceive(*canbus, MakeFrame(STD_ID, false, payload)), STD_ID, 0, 8, payload,
        "classical on an FD channel"
    );
    ExpectFrame(
        receiver.SendAndReceive(*canbus, MakeFrame(EXT_ID, true, payload)), EXT_ID, CAN_FRAME_IDE, 8, payload,
        "classical, 29-bit, on an FD channel"
    );
    ExpectFrame(
        receiver.SendAndReceive(*canbus, MakeRemoteFrame(STD_ID, false, 3)), STD_ID, CAN_FRAME_RTR, 3, {},
        "remote on an FD channel"
    );
}

ZTEST(canbus_send_frame, test_the_error_state_indicator_is_never_sent) {
    auto canbus = MakeRunningCanbus(CanbusType::CANFD);
    receiver.Start();

    const auto payload = MakePayload(12);
    CanFrame fd_frame = MakeFdFrame(STD_ID, false, true, payload);
    fd_frame.is_error_state_indicator = true;
    fd_frame.is_transmit = true;

    ExpectFrame(
        receiver.SendAndReceive(*canbus, fd_frame), STD_ID, FlagsOf(false, true, true, false), 9, payload,
        "FD frame with ESI set"
    );
}

ZTEST(canbus_send_frame, test_a_frame_the_channel_cannot_carry_is_refused) {
    auto canbus = MakeRunningCanbus();
    receiver.Start();

    receiver.ExpectRefused(*canbus, MakeFdFrame(STD_ID, false, false, MakePayload(8)), "FD frame, classical channel");
    receiver.ExpectRefused(
        *canbus, MakeFdFrame(STD_ID, false, true, MakePayload(8)), "FD+BRS frame, classical channel"
    );
    receiver.ExpectRefused(*canbus, MakeFdFrame(STD_ID, false, false, {}), "empty FD frame, classical channel");
}

ZTEST(canbus_send_frame, test_malformed_flag_combinations_are_refused) {
    for(const CanbusType type : { CanbusType::CLASSICAL_CAN, CanbusType::CANFD }) {
        auto canbus = MakeRunningCanbus(type);
        receiver.Start();

        CanFrame brs_without_fdf = MakeFrame(STD_ID, false, MakePayload(4));
        brs_without_fdf.is_bitrate_switch = true;
        receiver.ExpectRefused(*canbus, brs_without_fdf, "BRS without FDF");

        CanFrame remote_brs = MakeRemoteFrame(STD_ID, false, 2);
        remote_brs.is_bitrate_switch = true;
        receiver.ExpectRefused(*canbus, remote_brs, "BRS on a remote frame");

        CanFrame remote_with_payload = MakeRemoteFrame(STD_ID, false, 2);
        remote_with_payload.data.Assign(MakePayload(2));
        receiver.ExpectRefused(*canbus, remote_with_payload, "remote frame with a payload");

        receiver.ExpectRefused(*canbus, MakeRemoteFrame(STD_ID, false, 9), "remote frame with DLC 9");
        receiver.ExpectRefused(*canbus, MakeRemoteFrame(STD_ID, false, 15), "remote frame with DLC 15");

        // CAN FD has no remote frames.
        CanFrame fd_remote = MakeRemoteFrame(STD_ID, false, 2);
        fd_remote.is_can_fd = true;
        receiver.ExpectRefused(*canbus, fd_remote, "remote FD frame");

        receiver.Stop();
        canbus.reset();
    }
}

ZTEST(canbus_send_frame, test_more_than_eight_bytes_without_fdf_are_refused) {
    for(const CanbusType type : { CanbusType::CLASSICAL_CAN, CanbusType::CANFD }) {
        auto canbus = MakeRunningCanbus(type);
        receiver.Start();

        for(const size_t size : { 9U, 12U, 64U })
            receiver.ExpectRefused(
                *canbus, MakeFrame(STD_ID, false, MakePayload(size)), "classical frame over 8 bytes"
            );

        receiver.Stop();
        canbus.reset();
    }
}

ZTEST(canbus_send_frame, test_an_fd_length_no_dlc_gives_is_refused) {
    auto canbus = MakeRunningCanbus(CanbusType::CANFD);
    receiver.Start();

    for(size_t size = 0; size <= 64; ++size) {
        if(IsFdLength(size))
            continue;

        receiver.ExpectRefused(*canbus, MakeFdFrame(STD_ID, false, true, MakePayload(size)), "FD length with no DLC");
    }

    // The neighbours on either side of a refused length still go.
    const auto payload = MakePayload(12);
    ExpectFrame(
        receiver.SendAndReceive(*canbus, MakeFdFrame(STD_ID, false, true, payload)), STD_ID,
        FlagsOf(false, true, true, false), 9, payload, "FD frame of 12 bytes"
    );
}

ZTEST(canbus_send_frame, test_an_identifier_that_does_not_fit_its_format_is_refused) {
    auto canbus = MakeRunningCanbus();
    receiver.Start();

    receiver.ExpectRefused(*canbus, MakeFrame(0x800, false, MakePayload(1)), "11-bit ID 0x800");
    receiver.ExpectRefused(*canbus, MakeFrame(0x20000000, true, MakePayload(1)), "29-bit ID 0x20000000");
    receiver.ExpectRefused(*canbus, MakeRemoteFrame(0x800, false, 1), "remote with 11-bit ID 0x800");

    // The widest of each format goes.
    ExpectFrame(
        receiver.SendAndReceive(*canbus, MakeFrame(0x7FF, false, MakePayload(1))), 0x7FF, 0, 1, MakePayload(1),
        "11-bit ID 0x7FF"
    );
    ExpectFrame(
        receiver.SendAndReceive(*canbus, MakeFrame(0x1FFFFFFF, true, MakePayload(1))), 0x1FFFFFFF, CAN_FRAME_IDE, 1,
        MakePayload(1), "29-bit ID 0x1FFFFFFF"
    );
}

ZTEST(canbus_send_frame, test_a_bus_that_does_not_run_answers_enetdown_before_anything_else) {
    Canbus canbus(MakeConfig());

    zassert_equal(canbus.SendFrame(MakeFrame(STD_ID, false, MakePayload(1))), -ENETDOWN);
    zassert_equal(
        canbus.SendFrame(MakeFdFrame(0x800, false, true, MakePayload(9))), -ENETDOWN,
        "A bus that is down says so before it judges the frame"
    );

    zassert_true(canbus.Initialize());
    zassert_equal(canbus.SendFrame(MakeFrame(STD_ID, false, MakePayload(1))), -ENETDOWN, "Initialized, not started");

    zassert_true(canbus.Start());
    zassert_ok(canbus.SendFrame(MakeFrame(STD_ID, false, MakePayload(1))));

    zassert_true(canbus.Stop());
    zassert_equal(canbus.SendFrame(MakeFrame(STD_ID, false, MakePayload(1))), -ENETDOWN);
}

ZTEST(canbus_send_frame, test_the_older_overload_sends_classical_frames_on_a_classical_channel) {
    auto canbus = MakeRunningCanbus();
    receiver.Start();

    const auto payload = MakePayload(5);

    zassert_ok(canbus->SendFrame(CanId::Standard(STD_ID), payload));
    zassert_true(receiver.Wait());
    ExpectFrame(receiver.At(0), STD_ID, 0, 5, payload, "older overload, 11-bit");

    zassert_ok(canbus->SendFrame(CanId::Extended(EXT_ID), payload));
    zassert_true(receiver.Wait());
    ExpectFrame(receiver.At(1), EXT_ID, CAN_FRAME_IDE, 5, payload, "older overload, 29-bit");

    const auto too_long = MakePayload(9);
    zassert_equal(canbus->SendFrame(CanId::Standard(STD_ID), too_long), -EINVAL);
    zassert_equal(canbus->SendFrame(CanId::Standard(0x800), payload), -EINVAL);
    k_sleep(K_MSEC(20));
    zassert_equal(receiver.Count(), 2);
}

ZTEST(canbus_send_frame, test_the_older_overload_makes_every_frame_on_an_fd_channel_an_fd_frame_with_brs) {
    auto canbus = MakeRunningCanbus(CanbusType::CANFD);
    receiver.Start();

    const auto four = MakePayload(4);
    zassert_ok(canbus->SendFrame(CanId::Standard(STD_ID), four));
    zassert_true(receiver.Wait());
    ExpectFrame(receiver.At(0), STD_ID, FlagsOf(false, true, true, false), 4, four, "older overload, FD, 4 bytes");

    zassert_ok(canbus->SendFrame(CanId::Extended(EXT_ID), four));
    zassert_true(receiver.Wait());
    ExpectFrame(receiver.At(1), EXT_ID, FlagsOf(true, true, true, false), 4, four, "older overload, FD, 29-bit");

    // 9 bytes have no DLC: the older overload rounds up to 12 and pads with zeros.
    const auto nine = MakePayload(9);
    zassert_ok(canbus->SendFrame(CanId::Standard(STD_ID), nine));
    zassert_true(receiver.Wait());

    std::vector<uint8_t> padded = nine;
    padded.resize(12, 0);
    ExpectFrame(receiver.At(2), STD_ID, FlagsOf(false, true, true, false), 9, padded, "older overload, FD, padded");

    const auto full = MakePayload(64);
    zassert_ok(canbus->SendFrame(CanId::Standard(STD_ID), full));
    zassert_true(receiver.Wait());
    ExpectFrame(receiver.At(3), STD_ID, FlagsOf(false, true, true, false), 15, full, "older overload, FD, 64 bytes");

    // Past 64 bytes it refuses instead of cutting the payload.
    const auto too_long = MakePayload(65);
    zassert_equal(canbus->SendFrame(CanId::Standard(STD_ID), too_long), -EINVAL);
    k_sleep(K_MSEC(20));
    zassert_equal(receiver.Count(), 4);
}

ZTEST(canbus_send_frame, test_a_tap_hears_the_frames_of_the_new_overload_as_transmitted_ones) {
    auto canbus = MakeRunningCanbus(CanbusType::CANFD);
    EchoTap tap;

    zassert_true(canbus->SetTap(&tap));

    const auto eight = MakePayload(8);
    const auto sixteen = MakePayload(16);

    zassert_ok(canbus->SendFrame(MakeFrame(STD_ID, false, eight)));
    zassert_ok(canbus->SendFrame(MakeRemoteFrame(EXT_ID, true, 5)));
    zassert_ok(canbus->SendFrame(MakeFdFrame(STD_ID, false, false, sixteen)));
    zassert_ok(canbus->SendFrame(MakeFdFrame(EXT_ID, true, true, sixteen)));
    zassert_true(WaitForEchoes(tap, 4));

    const std::vector<CanFrame> echoes = tap.Echoes();
    zassert_equal(echoes.size(), 4);

    // Classical, on an FD channel.
    zassert_equal(echoes[0].id, STD_ID);
    zassert_false(echoes[0].is_extended);
    zassert_false(echoes[0].is_can_fd);
    zassert_false(echoes[0].is_bitrate_switch);
    zassert_false(echoes[0].is_remote_request);
    zassert_equal(echoes[0].dlc, 8);
    zassert_equal(echoes[0].data.size(), 8);
    zassert_equal(echoes[0].data[7], eight[7]);

    // Remote.
    zassert_equal(echoes[1].id, EXT_ID);
    zassert_true(echoes[1].is_extended);
    zassert_true(echoes[1].is_remote_request);
    zassert_false(echoes[1].is_can_fd);
    zassert_equal(echoes[1].dlc, 5);
    zassert_true(echoes[1].data.empty());

    // FD without BRS.
    zassert_true(echoes[2].is_can_fd);
    zassert_false(echoes[2].is_bitrate_switch);
    zassert_equal(echoes[2].dlc, 10);
    zassert_equal(echoes[2].data.size(), 16);
    zassert_equal(echoes[2].data[15], sixteen[15]);

    // FD with BRS, 29-bit.
    zassert_true(echoes[3].is_extended);
    zassert_true(echoes[3].is_can_fd);
    zassert_true(echoes[3].is_bitrate_switch);
    zassert_equal(echoes[3].dlc, 10);
    zassert_equal(echoes[3].data[0], sixteen[0]);

    zassert_equal(canbus->GetTxEchoLostCount(), 0);

    // A frame refused for its flags takes no slot and leaves no echo.
    zassert_equal(canbus->SendFrame(MakeFdFrame(STD_ID, false, false, MakePayload(9))), -EINVAL);
    k_sleep(K_MSEC(50));
    zassert_equal(tap.Echoes().size(), 4);
    zassert_equal(canbus->GetTxEchoLostCount(), 0);

    zassert_true(canbus->SetTap(nullptr));
}
