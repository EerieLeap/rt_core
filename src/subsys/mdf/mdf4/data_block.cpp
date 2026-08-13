#include <cstdint>
#include <memory>

#include "data_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

DataBlock::DataBlock(uint64_t size_bytes): BlockBase("DT"), size_bytes_(size_bytes) {}

void DataBlock::SetDataSizeBytes(uint64_t size_bytes) {
    size_bytes_ = size_bytes;
}

uint64_t DataBlock::GetDataSizeBytes() const {
    return size_bytes_;
}

uint64_t DataBlock::GetBlockSize() const {
    return GetBaseSize() + size_bytes_;
}

uint64_t DataBlock::GetSerializedSize() const {
    return GetBaseSize();
}

std::unique_ptr<uint8_t[]> DataBlock::Serialize() const {
    return SerializeBase();
}

} // namespace eerie_leap::subsys::mdf::mdf4
