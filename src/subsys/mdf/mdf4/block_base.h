#pragma once

#include <bit>
#include <cstdint>
#include <string>

#include "subsys/mdf/i_block.h"
#include "subsys/mdf/serializable_block_base.h"

namespace eerie_leap::subsys::mdf::mdf4 {

// Blocks and records are serialized by copying the in-memory representation of their fields.
static_assert(std::endian::native == std::endian::little, "MDF 4 data is little endian");

using eerie_leap::subsys::mdf::utilities::IBlockLinks;

class BlockBase : public SerializableBlockBase, public IBlock {
protected:
    static constexpr uint64_t BLOCK_ID_SIZE = 4; // "##" + 2 character id

    std::string id_; // 4 bytes, File identifier
    // uint8_t reserved_0_[4];              // 4 bytes, Reserved
    // uint64_t length_;                    // 8 bytes, Length of the block
    // uint64_t link_count_;                // 8 bytes, Number of links

public:
    explicit BlockBase(std::string id);
    ~BlockBase() override = default;

    std::string GetId() const override;
    uint64_t GetBaseSize() const;
    std::unique_ptr<uint8_t[]> SerializeBase() const;
    const IBlockLinks* GetBlockLinks() const override;
};

} // namespace eerie_leap::subsys::mdf::mdf4
