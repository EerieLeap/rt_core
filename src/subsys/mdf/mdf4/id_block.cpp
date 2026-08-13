#include <algorithm>
#include <cstring>
#include <utility>

#include "utilities/string/string_helpers.h"

#include "id_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

using namespace eerie_leap::utilities::string;

IdBlock::IdBlock() {
    version_str_ = "4.10";
    program_id_ = "EL";
    version_num_ = VERSION_NUMBER;
    standard_flags_ = 0;
    custom_flags_ = 0;

    SetFinalized(false);
}

bool IdBlock::IsFinalized() const {
    return is_finalized_;
}

void IdBlock::SetFinalized(bool is_finalized) {
    is_finalized_ = is_finalized;
    id_ = is_finalized_ ? "MDF" : "UnFinMF";

    if(is_finalized_)
        ClearStandardFlags();
}

uint64_t IdBlock::GetBlockSize() const {
    return 8 + 8 + 8 + 4 + 2 + 30 + 2 + 2; // = 64 bytes
}

std::unique_ptr<uint8_t[]> IdBlock::Serialize() const {
    const uint64_t size = GetBlockSize();
    auto buffer = std::make_unique<uint8_t[]>(size);
    std::memset(buffer.get(), 0, size);

    uint64_t offset = 0;

    auto id_char_array = StringHelpers::ToPaddedCharArray(id_, 8);
    std::copy(id_char_array.get(), id_char_array.get() + 8, buffer.get() + offset);
    offset += 8;

    auto version_str_char_array = StringHelpers::ToPaddedCharArray(version_str_, 8);
    std::copy(version_str_char_array.get(), version_str_char_array.get() + 8, buffer.get() + offset);
    offset += 8;

    auto program_id_char_array = StringHelpers::ToPaddedCharArray(program_id_, 8);
    std::copy(program_id_char_array.get(), program_id_char_array.get() + 8, buffer.get() + offset);
    offset += 8;

    offset += 4; // reserved_0_

    std::memcpy(buffer.get() + offset, &version_num_, sizeof(version_num_));
    offset += sizeof(version_num_);

    offset += 30; // reserved_1_

    std::memcpy(buffer.get() + offset, &standard_flags_, sizeof(standard_flags_));
    offset += sizeof(standard_flags_);

    std::memcpy(buffer.get() + offset, &custom_flags_, sizeof(custom_flags_));
    offset += sizeof(custom_flags_);

    return buffer;
}

void IdBlock::AddStandardFlag(StandardFlag flag) {
    standard_flags_ |= std::to_underlying(flag);
}

void IdBlock::ClearStandardFlags() {
    standard_flags_ = 0;
}

} // namespace eerie_leap::subsys::mdf::mdf4
