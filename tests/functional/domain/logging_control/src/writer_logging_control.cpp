#include <memory>
#include <stdexcept>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/logging_domain/control/writer_logging_control.h"

#include "logging_control_test_support.h"

using namespace logging_control_test;

using eerie_leap::domain::logging_domain::control::WriterLoggingControl;

namespace {

constexpr uint32_t OWN_UID = 0x0A0A0A0A;
constexpr uint32_t K_OTHER_UID = 0x0B0B0B0B;

using Targets = std::vector<uint32_t>;

struct Fixture {
    std::shared_ptr<FakeLoggingControl> local = std::make_shared<FakeLoggingControl>();
    std::shared_ptr<FakeLoggingControl> network = std::make_shared<FakeLoggingControl>();
    std::unique_ptr<WriterLoggingControl> control = std::make_unique<WriterLoggingControl>(local, network, OWN_UID);
};

} // namespace

ZTEST_SUITE(writer_logging_control, NULL, NULL, NULL, NULL, NULL);

ZTEST(writer_logging_control, test_AllUnitsAreThisWriterAndABroadcast) {
    Fixture f;

    f.control->RequestStart(ALL_UNITS);
    f.control->RequestStop(ALL_UNITS);

    zassert_true(f.local->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "The writer follows");
    zassert_true(f.network->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "and so do the other units");
    zassert_true(f.network->Targets() == Targets({ ALL_UNITS, ALL_UNITS }), "as one request for all");
}

ZTEST(writer_logging_control, test_ThisUnitsOwnUidIsTheWriterOnly) {
    Fixture f;

    f.control->RequestStart(OWN_UID);
    f.control->RequestStop(OWN_UID);

    zassert_true(f.local->Kinds() == Kinds({ Kind::Start, Kind::Stop }));
    zassert_equal(f.network->Requests().size(), 0U, "Nothing goes out for this unit alone");
}

ZTEST(writer_logging_control, test_AnotherUidIsTheNetworkOnly) {
    Fixture f;

    f.control->RequestStart(K_OTHER_UID);
    f.control->RequestStop(K_OTHER_UID);

    zassert_equal(f.local->Requests().size(), 0U, "The writer is not asked");
    zassert_true(f.network->Kinds() == Kinds({ Kind::Start, Kind::Stop }));
    zassert_true(f.network->Targets() == Targets({ K_OTHER_UID, K_OTHER_UID }), "with the target kept");
}

ZTEST(writer_logging_control, test_AReceivedStopStopsTheWriterAndDropsTheNetworksStarts) {
    Fixture f;

    zassert_true(f.control->HandleLoggingCommand(false));

    zassert_true(f.local->Kinds() == Kinds({ Kind::Stop }));
    zassert_true(f.network->Kinds() == Kinds({ Kind::Stop }), "The network half drops its wanted starts");
}

ZTEST(writer_logging_control, test_AReceivedStartIsTheWritersOnly) {
    Fixture f;

    zassert_true(f.control->HandleLoggingCommand(true));

    zassert_true(f.local->Kinds() == Kinds({ Kind::Start }));
    zassert_equal(f.network->Requests().size(), 0U);
}

ZTEST(writer_logging_control, test_WithoutANetworkOnlyTheWriterIsServed) {
    const auto local = std::make_shared<FakeLoggingControl>();
    WriterLoggingControl control(local, nullptr, 0);

    control.RequestStart(ALL_UNITS);
    control.RequestStart(K_OTHER_UID);
    zassert_true(control.HandleLoggingCommand(false));

    zassert_true(local->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "A request for another unit is dropped");
}

ZTEST(writer_logging_control, test_IsActiveIsTheWriters) {
    Fixture f;

    zassert_equal(f.control->IsActive(), f.local->IsActive());
    zassert_equal(f.control->GetReason(), f.local->GetReason());
}

ZTEST(writer_logging_control, test_ANullWriterIsRefused) {
    bool is_refused = false;
    try {
        WriterLoggingControl control(nullptr, std::make_shared<FakeLoggingControl>(), 0);
    } catch(const std::invalid_argument&) {
        is_refused = true;
    }

    zassert_true(is_refused);
}
