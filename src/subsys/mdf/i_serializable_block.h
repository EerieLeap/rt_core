#pragma once

#include <cstdint>
#include <memory>
#include <streambuf>
#include <vector>

namespace eerie_leap::subsys::mdf {

class ISerializableBlock {
    friend class SerializableBlockBase;

private:
    // One block and the trees below its children, without the blocks after it on its *_next chain:
    // the public traversals walk a chain in a loop and call these on every block of it, so their
    // recursion depth is the depth of the tree, not the length of a chain.
    virtual uint64_t WriteBlockToStream(std::streambuf& stream) = 0;
    virtual uint64_t ResolveBlockAddress(uint64_t address) = 0;
    virtual void ResetBlock() = 0;

public:
    virtual ~ISerializableBlock() = default;

    /** Value written to the block length field. For DT blocks it also covers the appended records. */
    virtual uint64_t GetBlockSize() const = 0;
    /** Number of bytes actually produced by Serialize(). */
    virtual uint64_t GetSerializedSize() const = 0;
    virtual std::unique_ptr<uint8_t[]> Serialize() const = 0;
    /** Writes this block, its children and the blocks after it on its *_next chain, depth first. */
    virtual uint64_t WriteToStream(std::streambuf& stream) = 0;
    /** Seeks back to the resolved address and overwrites this block only. */
    virtual uint64_t RewriteToStream(std::streambuf& stream) = 0;
    virtual uint64_t GetAddress() const = 0;
    virtual bool IsSerialized() const = 0;
    virtual void Reset() = 0;
    virtual uint64_t ResolveAddress(uint64_t parent_address) = 0;

    /** The block's *_next link; nullptr when the block has none or is the last of its chain. */
    virtual std::shared_ptr<ISerializableBlock> GetNext() const = 0;
    /** Every other link, in the order the blocks are laid out in the file. */
    virtual std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const = 0;
};

} // namespace eerie_leap::subsys::mdf
