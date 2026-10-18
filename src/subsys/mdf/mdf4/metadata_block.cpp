#include "metadata_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

namespace {

constexpr std::string_view REPLACEMENT_CHARACTER = "\xEF\xBF\xBD"; // U+FFFD in UTF-8

} // namespace

std::string MetadataBlock::EscapeXml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());

    for(const char character : text) {
        switch(character) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&apos;";
                break;
            // XML 1.0 has no form for the other C0 controls, escaped or not, and a NUL would
            // end the MD text early: both stand out as the replacement character instead.
            case '\t':
            case '\n':
            case '\r':
                escaped += character;
                break;
            default:
                if(static_cast<unsigned char>(character) < 0x20)
                    escaped += REPLACEMENT_CHARACTER;
                else
                    escaped += character;
                break;
        }
    }

    return escaped;
}

void MetadataBlock::SetText(const std::string& block_id, const std::string& text) {
    std::string xml_string = R"(<?xml version="1.0" encoding="UTF-8"?>)";
    xml_string += "<" + block_id + "comment>";
    xml_string += text;
    xml_string += "</" + block_id + "comment>";

    text_ = xml_string;
}

void MetadataBlock::SetComment(
    const std::string& block_id,
    const std::string& text,
    std::span<const MdfProperty> properties
) {
    std::string content = "<TX>" + EscapeXml(text) + "</TX>";

    if(!properties.empty()) {
        content += "<common_properties>";
        for(const auto& property : properties)
            content += "<e name=\"" + EscapeXml(property.name) + "\">" + EscapeXml(property.value) + "</e>";
        content += "</common_properties>";
    }

    // No XML declaration: asammdf only parses <common_properties> when the text starts with <HDcomment.
    text_ = "<" + block_id + "comment>" + content + "</" + block_id + "comment>";
}

} // namespace eerie_leap::subsys::mdf::mdf4
