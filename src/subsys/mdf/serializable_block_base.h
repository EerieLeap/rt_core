#pragma once

#include <cstdint>
#include <streambuf>

#include "i_serializable_block.h"

namespace eerie_leap::subsys::mdf {

class SerializableBlockBase : public virtual ISerializableBlock {
private:
    static void WriteBlockData(std::streambuf& stream, const uint8_t* data, uint64_t size);

protected:
    uint64_t address_;
    bool is_serialized_;

public:
    SerializableBlockBase();

    uint64_t GetSerializedSize() const override;
    uint64_t WriteToStream(std::streambuf& stream) override;
    uint64_t RewriteToStream(std::streambuf& stream) override;
    uint64_t GetAddress() const override;
    bool IsSerialized() const override;
    void Reset() override;
    uint64_t ResolveAddress(uint64_t parent_address) override;
};

} // namespace eerie_leap::subsys::mdf
