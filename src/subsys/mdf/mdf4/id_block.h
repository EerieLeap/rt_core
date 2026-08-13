#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "subsys/mdf/serializable_block_base.h"

namespace eerie_leap::subsys::mdf::mdf4 {

class IdBlock : public SerializableBlockBase {
public:
    enum class StandardFlag : uint16_t {
        None = 0x00,
        InvalidCGCount = 0x01,          // Update CG(Channel group) count
        InvalidSRCount = 0x02,          // Update SR(Sample reduction) Count
        InvalidLastDTBlock = 0x04,      // Update length of last DT(Data) Block
        InvalidLastRDBlock = 0x08,      // Update length of last RD(Reduction data) Block
        InvalidLastDLBlock = 0x10,      // Update length of last DL(Data list) Block
        InvalidDataVLSDBlock = 0x20,    // Update length of data VLSD(Variable length signal data channel) Block
        InvalidOffsetVLSDBlock = 0x40   // Update length of offset VLSD(Variable length signal data channel) Block
    };

private:
    static constexpr uint16_t VERSION_NUMBER = 410;

    std::string id_;                        // 8 bytes, File identifier
    std::string version_str_;               // 8 bytes, Format version
    std::string program_id_;                // 8 bytes, Program identifier
    // uint8_t reserved_0_[4];              // 4 bytes, Reserved (byte order / float format in MDF 3.x)
    uint16_t version_num_;                  // 2 bytes, Version number
    // uint8_t reserved_1_[30];             // 30 bytes, Reserved
    uint16_t standard_flags_;               // 2 bytes, Flags
    uint16_t custom_flags_;                 // 2 bytes, Flags

    bool is_finalized_;

public:
    IdBlock();
    virtual ~IdBlock() = default;

    bool IsFinalized() const;
    /** Switches the file identifier between "MDF" and "UnFinMF"; clears the standard flags when finalizing. */
    void SetFinalized(bool is_finalized);

    uint64_t GetBlockSize() const override;
    std::unique_ptr<uint8_t[]> Serialize() const override;
    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override { return {}; }

    void AddStandardFlag(StandardFlag flag);
    void ClearStandardFlags();
};

} // namespace eerie_leap::subsys::mdf::mdf4
