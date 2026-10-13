#pragma once

#include <span>
#include <string>
#include <string_view>

#include "subsys/mdf/i_block.h"
#include "subsys/mdf/mdf_property.h"
#include "text_block_base.h"

namespace eerie_leap::subsys::mdf::mdf4 {

using eerie_leap::subsys::mdf::MdfProperty;

class MetadataBlock : public TextBlockBase {
public:
    MetadataBlock(): TextBlockBase("MD") {}
    virtual ~MetadataBlock() = default;

    /** Escapes & < > " ' and replaces the control characters XML 1.0 forbids (all of C0 but tab, LF, CR) with U+FFFD. */
    static std::string EscapeXml(std::string_view text);

    /** Wraps ready made XML in the <XXcomment> envelope; the caller escapes its content. */
    void SetText(const std::string& block_id, const std::string& text);
    /** Builds <XXcomment><TX>text</TX>[<common_properties>...]</XXcomment> with every value XML escaped. */
    void SetComment(const std::string& block_id, const std::string& text, std::span<const MdfProperty> properties = {});
};

} // namespace eerie_leap::subsys::mdf::mdf4
