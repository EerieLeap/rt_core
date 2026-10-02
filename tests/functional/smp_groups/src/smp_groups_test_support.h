#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "smp_test_client.h"

#include "subsys/smp/smp_header.h"
#include "subsys/smp/smp_group_id.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"
#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"

namespace smp_groups_test {

using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpHeader;
using eerie_leap::subsys::smp::SmpOperation;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;
using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;

using namespace smp_test;

inline Bytes Pattern(size_t size, uint8_t seed) {
    Bytes bytes(size);
    for(size_t i = 0; i < size; i++)
        bytes[i] = static_cast<uint8_t>(seed + i * 13);

    return bytes;
}

// A configuration manager that stores the raw CBOR it is given.
class FakeConfigurationManager : public ICborConfigurationManager {
public:
    Bytes stored;
    int apply_count = 0;
    bool reject = false;
    std::string rejection_reason;

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override {
        apply_count++;
        if(reject) {
            // Without a reason the buffer is left alone, as a real manager may.
            if(!rejection_reason.empty())
                SetReason(reason, rejection_reason);
            return false;
        }

        stored.assign(cbor_data.begin(), cbor_data.end());
        return true;
    }

    std::pmr::vector<uint8_t> GetCborConfiguration() override {
        return std::pmr::vector<uint8_t>(stored.begin(), stored.end());
    }

    eerie_leap::configuration::services::StoredCborInfo GetCborConfigurationInfo() override {
        return {.size = stored.size(), .crc = crc32_ieee(stored.data(), stored.size())};
    }
};

class FakeNetworkInfo : public ICdmpNetworkInfo {
public:
    CdmpDeviceInfo self{};
    std::vector<CdmpDeviceInfo> devices;

    [[nodiscard]] CdmpDeviceInfo GetDeviceInfo() const override { return self; }

    size_t GetNetworkDevices(std::span<CdmpDeviceInfo> out) const override {
        const size_t count = std::min(out.size(), devices.size());
        std::copy_n(devices.begin(), count, out.begin());
        return count;
    }

    [[nodiscard]] size_t GetNetworkDeviceCount() const override { return devices.size(); }
};

} // namespace smp_groups_test
