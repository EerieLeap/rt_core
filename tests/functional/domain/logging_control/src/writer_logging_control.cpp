#include <memory>
#include <stdexcept>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/logging_domain/control/writer_logging_control.h"

#include "logging_control_test_support.h"

using namespace logging_control_test;

using eerie_leap::domain::logging_domain::control::WriterLoggingControl;

namespace {

constexpr uint32_t kOwnUid = 0x0A0A0A0A;
constexpr uint32_t kOtherUid = 0x0B0B0B0B;

using Targets = std::vector<uint32_t>;

struct Fixture {
    std::shared_ptr<FakeLoggingControl> local = std::make_shared<FakeLoggingControl>();
    std::shared_ptr<FakeLoggingControl> network = std::make_shared<FakeLoggingControl>();
    std::unique_ptr<WriterLoggingControl> control = std::make_unique<WriterLoggingControl>(local, network, kOwnUid);
};

} // namespace

ZTEST_SUITE(writer_logging_control, NULL, NULL, NULL, NULL, NULL);

ZTEST(writer_logging_control, test_AllUnitsAreThisWriterAndABroadcast) {
    Fixture f;

    f.control->RequestStart(kAllUnits);
    f.control->RequestStop(kAllUnits);

    zassert_true(f.local->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "The writer follows");
    zassert_true(f.network->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "and so do the other units");
    zassert_true(f.network->Targets() == Targets({ kAllUnits, kAllUnits }), "as one request for all");
}

ZTEST(writer_logging_control, test_ThisUnitsOwnUidIsTheWriterOnly) {
    Fixture f;

    f.control->RequestStart(kOwnUid);
    f.control->RequestStop(kOwnUid);

    zassert_true(f.local->Kinds() == Kinds({ Kind::Start, Kind::Stop }));
    zassert_equal(f.network->Requests().size(), 0U, "Nothing goes out for this unit alone");
}

ZTEST(writer_logging_control, test_AnotherUidIsTheNetworkOnly) {
    Fixture f;

    f.control->RequestStart(kOtherUid);
    f.control->RequestStop(kOtherUid);

    zassert_equal(f.local->Requests().size(), 0U, "The writer is not asked");
    zassert_true(f.network->Kinds() == Kinds({ Kind::Start, Kind::Stop }));
    zassert_true(f.network->Targets() == Targets({ kOtherUid, kOtherUid }), "with the target kept");
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
    auto local = std::make_shared<FakeLoggingControl>();
    WriterLoggingControl control(local, nullptr, 0);

    control.RequestStart(kAllUnits);
    control.RequestStart(kOtherUid);
    zassert_true(control.HandleLoggingCommand(false));

    zassert_true(local->Kinds() == Kinds({ Kind::Start, Kind::Stop }), "A request for another unit is dropped");
}

ZTEST(writer_logging_control, test_IsActiveIsTheWriters) {
    Fixture f;

    zassert_equal(f.control->IsActive(), f.local->IsActive());
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
