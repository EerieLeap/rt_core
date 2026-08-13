#pragma once

#include <cstdint>
#include <memory>

#include "block_base.h"

namespace eerie_leap::subsys::mdf::mdf4 {

class DataBlock : public BlockBase {
private:
    uint64_t size_bytes_;

public:
    explicit DataBlock(uint64_t size_bytes = 0);
    virtual ~DataBlock() = default;

    /** Number of record bytes appended after the block header. */
    void SetDataSizeBytes(uint64_t size_bytes);
    uint64_t GetDataSizeBytes() const;

    uint64_t GetBlockSize() const override;
    uint64_t GetSerializedSize() const override;
    std::unique_ptr<uint8_t[]> Serialize() const override;
    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override {
        return {};
    }
};

} // namespace eerie_leap::subsys::mdf::mdf4
