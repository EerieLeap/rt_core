#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <new>
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
#include "subsys/fs/services/i_fs_service.h"
#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"

namespace smp_groups_test {

using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpHeader;
using eerie_leap::subsys::smp::SmpOperation;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;
using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::IFsService;
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

// An SD card whose directories are lists in memory, read as FsService::ListDirectory() reads them;
// the rest of the service does nothing.
class FakeFsService : public IFsService {
public:
    // By path relative to the mount point; "" is the root.
    std::map<std::string, std::vector<DirectoryEntry>, std::less<>> directories;
    bool available = true;
    // Returned by ListDirectory() in place of a listing, when set.
    int error = 0;
    bool throws_bad_alloc = false;
    mutable int list_calls = 0;

    bool Initialize() override { return true; }
    bool IsAvailable() const override { return available; }
    bool WriteFile(std::string_view, const void*, size_t, bool) override { return false; }
    bool ReadFile(std::string_view, void*, size_t, size_t& out_len) override { out_len = 0; return false; }
    int OpenFile(std::string_view, fs_mode_t, fs_file_t*) override { return -ENOTSUP; }
    bool CreateDirectory(std::string_view) override { return false; }
    bool Exists(std::string_view) const override { return false; }
    bool DeleteFile(std::string_view) override { return false; }
    bool DeleteRecursive(std::string_view) override { return false; }
    std::vector<std::string> ListFiles(std::string_view) const override { return {}; }
    std::optional<size_t> GetFileSize(std::string_view) const override { return std::nullopt; }
    uint64_t GetTotalSpace() const override { return 0; }
    uint64_t GetUsedSpace() const override { return 0; }
    bool Format() override { return false; }

    int ListDirectory(std::string_view relative_path, size_t first, size_t max_entries,
        std::vector<DirectoryEntry>& entries, size_t& count) const override {

        list_calls++;
        entries.clear();
        count = 0;

        if(throws_bad_alloc)
            throw std::bad_alloc();

        if(!available)
            return -ENODEV;

        if(error != 0)
            return error;

        if(relative_path.starts_with('/') || HasParentReference(relative_path))
            return -EINVAL;

        const auto directory = directories.find(relative_path);
        if(directory == directories.end())
            return -ENOENT;

        count = directory->second.size();
        for(size_t i = first; i < count && entries.size() < max_entries; i++)
            entries.push_back(directory->second[i]);

        return 0;
    }

private:
    // As FsService::TryBuildPath() checks it.
    static bool HasParentReference(std::string_view path) {
        for(size_t start = 0; start <= path.size();) {
            const size_t end = std::min(path.find('/', start), path.size());
            if(path.substr(start, end - start) == "..")
                return true;

            start = end + 1;
        }

        return false;
    }
};

} // namespace smp_groups_test
