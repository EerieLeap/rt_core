#pragma once

#include <cstdint>
#include <memory>
#include <streambuf>
#include <vector>

namespace eerie_leap::subsys::mdf {

class ISerializableBlock {
public:
    virtual ~ISerializableBlock() = default;

    /** Value written to the block length field. For DT blocks it also covers the appended records. */
    virtual uint64_t GetBlockSize() const = 0;
    /** Number of bytes actually produced by Serialize(). */
    virtual uint64_t GetSerializedSize() const = 0;
    virtual std::unique_ptr<uint8_t[]> Serialize() const = 0;
    virtual uint64_t WriteToStream(std::streambuf& stream) = 0;
    /** Seeks back to the resolved address and overwrites this block only. */
    virtual uint64_t RewriteToStream(std::streambuf& stream) = 0;
    virtual uint64_t GetAddress() const = 0;
    virtual bool IsSerialized() const = 0;
    virtual void Reset() = 0;
    virtual uint64_t ResolveAddress(uint64_t parent_address) = 0;

    virtual std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const = 0;
};

} // namespace eerie_leap::subsys::mdf
