#pragma once

#include "text_block_base.h"

namespace eerie_leap::subsys::mdf::mdf4 {

class TextBlock : public TextBlockBase {
public:
    TextBlock() :
        TextBlockBase("TX") {}
    ~TextBlock() override = default;

    void SetText(const std::string& text) {
        text_ = text;
    }
};

} // namespace eerie_leap::subsys::mdf::mdf4
