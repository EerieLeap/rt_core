#include <cstring>
#include <stdexcept>
#include <utility>

#include "channel_conversion_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

ChannelConversionBlock::ChannelConversionBlock(ConversionType conversion_type)
    : BlockBase("CC") {

    conversion_type_ = conversion_type;
    precision_ = 0;
    flags_ = 0;
    min_physical_value_ = 0.0;
    max_physical_value_ = 0.0;
}

std::shared_ptr<ChannelConversionBlock> ChannelConversionBlock::CreateAlgebraicConversion(std::shared_ptr<TextBlock> formula) {
    if(!formula)
        throw std::invalid_argument("Algebraic conversion requires a formula text block");

    auto block = std::make_shared<ChannelConversionBlock>(ConversionType::Algebraic);
    block->links_.AddExtraLink(std::move(formula));

    return block;
}

uint64_t ChannelConversionBlock::GetBlockSize() const {
    return GetBaseSize() + 1 + 1 + 2 + 2 + 2 + 8 + 8 + 8 * values_.size();
}

std::unique_ptr<uint8_t[]> ChannelConversionBlock::Serialize() const {
    auto buffer = SerializeBase();
    uint64_t offset = GetBaseSize();

    std::memcpy(buffer.get() + offset, &conversion_type_, sizeof(uint8_t));
    offset += sizeof(uint8_t);

    std::memcpy(buffer.get() + offset, &precision_, sizeof(precision_));
    offset += sizeof(precision_);

    std::memcpy(buffer.get() + offset, &flags_, sizeof(flags_));
    offset += sizeof(flags_);

    auto reference_count = static_cast<uint16_t>(links_.Count() - FIXED_LINK_COUNT);
    std::memcpy(buffer.get() + offset, &reference_count, sizeof(reference_count));
    offset += sizeof(reference_count);

    auto value_count = static_cast<uint16_t>(values_.size());
    std::memcpy(buffer.get() + offset, &value_count, sizeof(value_count));
    offset += sizeof(value_count);

    std::memcpy(buffer.get() + offset, &min_physical_value_, sizeof(min_physical_value_));
    offset += sizeof(min_physical_value_);

    std::memcpy(buffer.get() + offset, &max_physical_value_, sizeof(max_physical_value_));
    offset += sizeof(max_physical_value_);

    for(const auto& value : values_) {
        std::memcpy(buffer.get() + offset, &value, sizeof(value));
        offset += sizeof(value);
    }

    return buffer;
}

} // namespace eerie_leap::subsys::mdf::mdf4
