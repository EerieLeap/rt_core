#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/mdf/mdf4/channel_block.h"
#include "subsys/mdf/mdf4/channel_conversion_block.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/data_block.h"
#include "subsys/mdf/mdf4/data_group_block.h"
#include "subsys/mdf/mdf4/file_history_block.h"
#include "subsys/mdf/mdf4/header_block.h"
#include "subsys/mdf/mdf4/id_block.h"
#include "subsys/mdf/mdf4/metadata_block.h"
#include "subsys/mdf/mdf4/source_information_block.h"
#include "subsys/mdf/mdf4/text_block.h"

#include "mdf_test_support.h"

using namespace eerie_leap::subsys::mdf::mdf4;
using namespace mdf_test;
using eerie_leap::subsys::mdf::MdfProperty;

namespace {

constexpr size_t ID_VERSION_NUMBER_OFFSET = 28;
constexpr size_t ID_STANDARD_FLAGS_OFFSET = 60;
constexpr size_t ID_CUSTOM_FLAGS_OFFSET = 62;

uint64_t BaseSizeForLinks(size_t link_count) {
    return BLOCK_HEADER_SIZE + link_count * LINK_SIZE;
}

std::shared_ptr<TextBlock> MakeTextBlock(const std::string& text) {
    auto block = std::make_shared<TextBlock>();
    block->SetText(text);

    return block;
}

} // namespace

ZTEST_SUITE(mdf_blocks, NULL, NULL, NULL, NULL, NULL);

ZTEST(mdf_blocks, test_IdBlockLayout) {
    IdBlock block;

    zassert_equal(block.GetBlockSize(), ID_BLOCK_SIZE);
    zassert_equal(block.GetSerializedSize(), ID_BLOCK_SIZE);
    zassert_false(block.IsFinalized(), "a new file starts out unfinalized");

    const auto data = Serialize(block);

    zassert_equal(AsText(data, 0, 8), std::string("UnFinMF "));
    zassert_equal(AsText(data, 8, 8), std::string("4.10    "));
    zassert_equal(AsText(data, 16, 8), std::string("EL      "));
    zassert_equal(Read<uint16_t>(data, ID_VERSION_NUMBER_OFFSET), 410);

    // Everything the MDF 4.x spec marks reserved has to stay zero.
    for(size_t i = 24; i < 28; i++)
        zassert_equal(data[i], 0, "id_reserved1 byte %zu", i);

    for(size_t i = 30; i < ID_STANDARD_FLAGS_OFFSET; i++)
        zassert_equal(data[i], 0, "id_reserved2 byte %zu", i);

    zassert_equal(Read<uint16_t>(data, ID_CUSTOM_FLAGS_OFFSET), 0);
}

ZTEST(mdf_blocks, test_IdBlockFinalizedRoundTrip) {
    IdBlock block;
    block.AddStandardFlag(IdBlock::StandardFlag::InvalidCGCount);
    block.AddStandardFlag(IdBlock::StandardFlag::InvalidLastDTBlock);

    auto data = Serialize(block);
    zassert_equal(Read<uint16_t>(data, ID_STANDARD_FLAGS_OFFSET), 0x01 | 0x04);

    block.SetFinalized(true);
    zassert_true(block.IsFinalized());

    data = Serialize(block);
    zassert_equal(AsText(data, 0, 8), std::string("MDF     "));
    zassert_equal(Read<uint16_t>(data, ID_STANDARD_FLAGS_OFFSET), 0, "finalizing clears the flags");

    block.SetFinalized(false);
    data = Serialize(block);
    zassert_equal(AsText(data, 0, 8), std::string("UnFinMF "));
    zassert_equal(block.GetBlockSize(), ID_BLOCK_SIZE, "the block size never changes");
}

ZTEST(mdf_blocks, test_IdBlockStandardFlagsAreIdempotent) {
    IdBlock block;

    block.AddStandardFlag(IdBlock::StandardFlag::InvalidDataVLSDBlock);
    block.AddStandardFlag(IdBlock::StandardFlag::InvalidDataVLSDBlock);
    zassert_equal(Read<uint16_t>(Serialize(block), ID_STANDARD_FLAGS_OFFSET), 0x20);

    block.ClearStandardFlags();
    zassert_equal(Read<uint16_t>(Serialize(block), ID_STANDARD_FLAGS_OFFSET), 0);
}

ZTEST(mdf_blocks, test_BlockHeaderLayout) {
    TestBlock block("XX", 16);

    zassert_equal(block.GetId(), std::string("XX"));
    zassert_equal(block.GetBaseSize(), BLOCK_HEADER_SIZE, "a block without links has no link area");
    zassert_equal(block.GetBlockSize(), BLOCK_HEADER_SIZE + 16);

    const auto data = Serialize(block);

    zassert_equal(BlockId(data), std::string("##XX"));
    zassert_equal(Read<uint32_t>(data, 4), 0, "reserved");
    zassert_equal(Read<uint64_t>(data, BLOCK_LENGTH_OFFSET), BLOCK_HEADER_SIZE + 16);
    zassert_equal(Read<uint64_t>(data, BLOCK_LINK_COUNT_OFFSET), 0);
}

ZTEST(mdf_blocks, test_BlockRejectsIdThatIsNotTwoCharacters) {
    for(const auto& id : {std::string(""), std::string("X"), std::string("XXX")}) {
        bool threw = false;
        try {
            TestBlock block(id);
        } catch(const std::invalid_argument&) {
            threw = true;
        }
        zassert_true(threw, "block id '%s' should be rejected", id.c_str());
    }
}

ZTEST(mdf_blocks, test_TextBlockPadsAndZeroTerminates) {
    // A text whose length is already a multiple of 8 still gets a full padding block.
    const struct {
        size_t text_size;
        size_t padded_size;
    } cases[] = {{0, 8}, {1, 8}, {7, 8}, {8, 16}, {9, 16}, {15, 16}, {16, 24}};

    for(const auto& test_case : cases) {
        auto block = MakeTextBlock(std::string(test_case.text_size, 'a'));

        zassert_equal(block->GetBlockSize(), BLOCK_HEADER_SIZE + test_case.padded_size,
            "text of %zu bytes", test_case.text_size);

        const auto data = Serialize(*block);
        zassert_equal(BlockId(data), std::string("##TX"));

        for(size_t i = 0; i < test_case.text_size; i++)
            zassert_equal(data[BLOCK_HEADER_SIZE + i], 'a');

        for(size_t i = test_case.text_size; i < test_case.padded_size; i++)
            zassert_equal(data[BLOCK_HEADER_SIZE + i], 0, "padding must be zero");
    }
}

ZTEST(mdf_blocks, test_MetadataBlockWrapsContentInXml) {
    MetadataBlock block;
    block.SetText("HD", "<TX>hello</TX>");

    const auto data = Serialize(block);
    zassert_equal(BlockId(data), std::string("##MD"));

    const auto text = AsText(data, BLOCK_HEADER_SIZE, block.GetBlockSize() - BLOCK_HEADER_SIZE);
    zassert_true(text.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?><HDcomment>"), "%s", text.c_str());
    zassert_not_equal(text.find("<TX>hello</TX>"), std::string::npos);
    zassert_not_equal(text.find("</HDcomment>"), std::string::npos);
}

ZTEST(mdf_blocks, test_MetadataCommentEscapesXml) {
    MetadataBlock block;
    const std::array<MdfProperty, 1> properties{{{"vendor & model", "Acme <X>"}}};
    block.SetComment("CN", "Coolant \"inlet\" <probe> & 'spare'", properties);

    auto data = Serialize(block);
    auto text = AsText(data, BLOCK_HEADER_SIZE, block.GetBlockSize() - BLOCK_HEADER_SIZE);
    text = text.substr(0, text.find('\0'));

    zassert_true(text.starts_with("<CNcomment>"), "readers match the root element at the start: %s", text.c_str());
    zassert_true(text.ends_with("</CNcomment>"), "%s", text.c_str());
    zassert_not_equal(text.find("<TX>Coolant &quot;inlet&quot; &lt;probe&gt; &amp; &apos;spare&apos;</TX>"), std::string::npos,
        "a description with markup characters must not break the XML: %s", text.c_str());
    zassert_not_equal(text.find("<common_properties><e name=\"vendor &amp; model\">Acme &lt;X&gt;</e></common_properties>"), std::string::npos,
        "property names and values must be escaped too: %s", text.c_str());

    block.SetComment("CG", "plain");
    data = Serialize(block);
    text = AsText(data, BLOCK_HEADER_SIZE, block.GetBlockSize() - BLOCK_HEADER_SIZE);
    text = text.substr(0, text.find('\0'));
    zassert_equal(text, std::string("<CGcomment><TX>plain</TX></CGcomment>"),
        "without properties there must be no <common_properties> element: %s", text.c_str());

    // Sensor names and descriptions are free text: a NUL would cut the comment short and the
    // other C0 controls are not well-formed XML even as character references.
    std::string controls = "a\tb\nc\rd";
    controls += '\0';
    controls += 'e';
    controls += '\x01';
    controls += 'f';
    controls += '\x1B';
    controls += 'g';
    block.SetComment("CN", controls);
    data = Serialize(block);
    text = AsText(data, BLOCK_HEADER_SIZE, block.GetBlockSize() - BLOCK_HEADER_SIZE);
    text = text.substr(0, text.find('\0'));
    zassert_equal(text, std::string("<CNcomment><TX>a\tb\nc\rd\xEF\xBF\xBD" "e\xEF\xBF\xBD" "f\xEF\xBF\xBD" "g</TX></CNcomment>"),
        "tab, LF and CR pass, every other control becomes U+FFFD and the comment stays closed: %s", text.c_str());
}

ZTEST(mdf_blocks, test_HeaderBlockLayout) {
    HeaderBlock block;

    zassert_equal(block.GetBlockSize(), BaseSizeForLinks(6) + 32);

    block.SetCurrentTimeNs(1234567890123456789ULL);

    const auto data = ResolveAndSerialize(block);
    zassert_equal(BlockId(data), std::string("##HD"));
    zassert_equal(Read<uint64_t>(data, BLOCK_LINK_COUNT_OFFSET), 6);
    zassert_equal(Read<uint64_t>(data, BaseSizeForLinks(6)), 1234567890123456789ULL);

    // The file history block is created up front and shares the start time.
    auto children = block.GetChildren();
    auto file_history = std::dynamic_pointer_cast<FileHistoryBlock>(children[1]);
    zassert_not_null(file_history, "the header always owns a file history block");
    zassert_equal(Read<uint64_t>(Serialize(*file_history), BaseSizeForLinks(2)), 1234567890123456789ULL);
}

ZTEST(mdf_blocks, test_HeaderBlockChainsDataGroups) {
    HeaderBlock block;

    auto first = std::make_shared<DataGroupBlock>(1);
    auto second = std::make_shared<DataGroupBlock>(1);
    block.AddDataGroup(first);
    block.AddDataGroup(second);

    // The first data group is linked from the header, the rest chain off it.
    auto children = block.GetChildren();
    zassert_equal(children.back(), std::static_pointer_cast<eerie_leap::subsys::mdf::ISerializableBlock>(first));
    zassert_equal(first->GetNext(), std::static_pointer_cast<eerie_leap::subsys::mdf::ISerializableBlock>(second),
        "the second data group is the first one's dg_dg_next");
}

ZTEST(mdf_blocks, test_FileHistoryBlockLayout) {
    FileHistoryBlock block;

    zassert_equal(block.GetBlockSize(), BaseSizeForLinks(2) + 16);
    block.SetTimeNs(42);

    const auto data = ResolveAndSerialize(block);
    zassert_equal(BlockId(data), std::string("##FH"));
    zassert_equal(Read<uint64_t>(data, BLOCK_LINK_COUNT_OFFSET), 2);
    zassert_equal(Read<uint64_t>(data, BaseSizeForLinks(2)), 42);

    auto comment = std::dynamic_pointer_cast<MetadataBlock>(block.GetChildren()[0]);
    zassert_not_null(comment, "the file history block always carries a comment");

    const auto comment_data = Serialize(*comment);
    const auto text = AsText(comment_data, BLOCK_HEADER_SIZE, comment->GetBlockSize() - BLOCK_HEADER_SIZE);
    zassert_not_equal(text.find("<FHcomment>"), std::string::npos, "%s", text.c_str());
    zassert_not_equal(text.find("<tool_id>none</tool_id>"), std::string::npos,
        "a file history that was never told its tool keeps the placeholder: %s", text.c_str());
    zassert_not_equal(text.find("<tool_vendor>Eerie Leap</tool_vendor>"), std::string::npos, "%s", text.c_str());
    zassert_not_equal(text.find("<tool_version>0.0.0</tool_version>"), std::string::npos, "%s", text.c_str());
}

ZTEST(mdf_blocks, test_DataGroupBlockLayout) {
    DataGroupBlock block(4);

    zassert_equal(block.GetRecordIdSizeBytes(), 4);
    zassert_equal(block.GetBlockSize(), BaseSizeForLinks(4) + 8);
    zassert_not_null(block.GetDataBlock().get(), "a data group always owns a data block");

    const auto data = ResolveAndSerialize(block);
    zassert_equal(BlockId(data), std::string("##DG"));
    zassert_equal(Read<uint64_t>(data, BLOCK_LINK_COUNT_OFFSET), 4);
    zassert_equal(data[BaseSizeForLinks(4)], 4, "dg_rec_id_size");
}

ZTEST(mdf_blocks, test_DataGroupBlockValidatesRecordIdSize) {
    for(uint8_t size : {0, 1, 2, 4, 8})
        zassert_equal(DataGroupBlock(size).GetRecordIdSizeBytes(), size);

    for(uint8_t size : {3, 5, 6, 7, 9, 255}) {
        bool threw = false;
        try {
            DataGroupBlock block(size);
        } catch(const std::runtime_error&) {
            threw = true;
        }
        zassert_true(threw, "record id size %u should be rejected", size);
    }
}

ZTEST(mdf_blocks, test_DataBlockSeparatesLengthFromSerializedSize) {
    DataBlock block;

    zassert_equal(block.GetDataSizeBytes(), 0);
    zassert_equal(block.GetBlockSize(), BLOCK_HEADER_SIZE);
    zassert_equal(block.GetSerializedSize(), BLOCK_HEADER_SIZE);

    block.SetDataSizeBytes(500);

    // The length field covers the appended records while Serialize() still emits only the header.
    zassert_equal(block.GetDataSizeBytes(), 500);
    zassert_equal(block.GetBlockSize(), BLOCK_HEADER_SIZE + 500);
    zassert_equal(block.GetSerializedSize(), BLOCK_HEADER_SIZE);

    const auto data = Serialize(block);
    zassert_equal(data.size(), BLOCK_HEADER_SIZE);
    zassert_equal(BlockId(data), std::string("##DT"));
    zassert_equal(Read<uint64_t>(data, BLOCK_LENGTH_OFFSET), BLOCK_HEADER_SIZE + 500);
}

ZTEST(mdf_blocks, test_ChannelGroupRecordIdData) {
    const auto none = ChannelGroupBlock(0, 0).GetRecordIdData();
    zassert_true(std::ranges::all_of(none, [](uint8_t byte) { return byte == 0; }), "a group without record IDs has none");

    const auto one = ChannelGroupBlock(1, 0xAB).GetRecordIdData();
    zassert_equal(one[0], 0xAB);
    zassert_equal(one[1], 0, "the bytes past the record ID size stay zero");

    const auto two = ChannelGroupBlock(2, 0xBEEF).GetRecordIdData();
    zassert_equal(two[0], 0xEF, "little endian");
    zassert_equal(two[1], 0xBE);
    zassert_equal(two[2], 0, "the bytes past the record ID size stay zero");

    const auto four = ChannelGroupBlock(4, 0xDEADBEEF).GetRecordIdData();
    zassert_equal(four[0], 0xEF);
    zassert_equal(four[3], 0xDE);
    zassert_equal(four[4], 0, "the bytes past the record ID size stay zero");

    const auto eight = ChannelGroupBlock(8, 0x0123456789ABCDEFULL).GetRecordIdData();
    zassert_equal(eight[0], 0xEF);
    zassert_equal(eight[7], 0x01);
}

ZTEST(mdf_blocks, test_ChannelGroupRejectsRecordIdThatDoesNotFit) {
    const struct {
        uint8_t size;
        uint64_t record_id;
    } cases[] = {{0, 1}, {1, 256}, {2, 65536}, {4, 0x100000000ULL}};

    for(const auto& test_case : cases) {
        bool threw = false;
        try {
            ChannelGroupBlock block(test_case.size, test_case.record_id);
        } catch(const std::runtime_error&) {
            threw = true;
        }
        zassert_true(threw, "record id %llu should not fit into %u bytes",
            static_cast<unsigned long long>(test_case.record_id), test_case.size);
    }

    // The largest value that still fits is accepted.
    zassert_equal(ChannelGroupBlock(1, 255).GetRecordId(), 255);
    zassert_equal(ChannelGroupBlock(2, 65535).GetRecordId(), 65535);
}

ZTEST(mdf_blocks, test_ChannelGroupAccumulatesChannelOffsets) {
    ChannelGroupBlock group(1, 1);

    auto first = std::make_shared<ChannelBlock>(
        ChannelBlock::Type::Master, ChannelBlock::SyncType::Time, ChannelBlock::DataType::FloatLe, 32);
    auto second = std::make_shared<ChannelBlock>(
        ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, ChannelBlock::DataType::UnsignedIntegerLe, 64);
    auto third = std::make_shared<ChannelBlock>(
        ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, ChannelBlock::DataType::SignedIntegerLe, 32);

    group.AddChannel(first);
    group.AddChannel(second);
    group.AddChannel(third);

    zassert_equal(first->GetDataOffsetBytes(), 0);
    zassert_equal(second->GetDataOffsetBytes(), 4);
    zassert_equal(third->GetDataOffsetBytes(), 12);
    zassert_equal(group.GetDataSizeBytes(), 16);

    const auto channels = group.GetChannels();
    zassert_equal(channels.size(), 3, "channels are walked in insertion order");
    zassert_equal(channels[0], first);
    zassert_equal(channels[1], second);
    zassert_equal(channels[2], third);
}

ZTEST(mdf_blocks, test_ChannelGroupRejectsChannelWithoutBitCount) {
    ChannelGroupBlock group(1, 1);
    auto channel = std::make_shared<ChannelBlock>(
        ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, ChannelBlock::DataType::ByteArray, 0);

    bool threw = false;
    try {
        group.AddChannel(channel);
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a channel with no bit count would silently overlap its neighbour");
    zassert_equal(group.GetDataSizeBytes(), 0, "the rejected channel must not change the record size");
}

ZTEST(mdf_blocks, test_ChannelGroupLayoutAndCounters) {
    ChannelGroupBlock group(1, 7);

    zassert_equal(group.GetBlockSize(), BaseSizeForLinks(6) + 32);
    zassert_equal(group.GetCycleCount(), 0);

    group.SetFlags(std::to_underlying(ChannelGroupBlock::Flag::BusEvent));
    group.SetPathSeparator('.');
    group.IncrementCycleCount();
    group.IncrementCycleCount();

    const auto base = BaseSizeForLinks(6);
    const auto data = Serialize(group);

    zassert_equal(BlockId(data), std::string("##CG"));
    zassert_equal(Read<uint64_t>(data, BLOCK_LINK_COUNT_OFFSET), 6);
    zassert_equal(Read<uint64_t>(data, base), 7, "cg_record_id");
    zassert_equal(Read<uint64_t>(data, base + 8), 2, "cg_cycle_count");
    zassert_equal(Read<uint16_t>(data, base + 16), 0x0002, "cg_flags");
    zassert_equal(Read<uint16_t>(data, base + 18), '.', "cg_path_separator");
    zassert_equal(Read<uint32_t>(data, base + 20), 0, "cg_reserved");

    group.ResetCounters();
    zassert_equal(group.GetCycleCount(), 0);
    zassert_equal(Read<uint64_t>(Serialize(group), base + 8), 0);
}

ZTEST(mdf_blocks, test_VlsdChannelGroupReportsTotalValueSize) {
    ChannelGroupBlock group(1, 1);
    group.SetFlags(std::to_underlying(ChannelGroupBlock::Flag::VlsdChannel));

    // The spec stores the total size of all VLSD values as a UINT64 split over both size fields.
    group.AddVlsdDataBytes(0x0000000100000002ULL);
    zassert_equal(group.GetVlsdDataSizeBytes(), 0x0000000100000002ULL);

    const auto base = BaseSizeForLinks(6);
    const auto data = Serialize(group);

    zassert_equal(Read<uint32_t>(data, base + 24), 0x00000002, "low part in cg_data_bytes");
    zassert_equal(Read<uint32_t>(data, base + 28), 0x00000001, "high part in cg_inval_bytes");

    group.ResetCounters();
    zassert_equal(group.GetVlsdDataSizeBytes(), 0);
}

ZTEST(mdf_blocks, test_ChannelGroupInvalidationBytes) {
    ChannelGroupBlock group(1, 1);
    group.AddChannel(std::make_shared<ChannelBlock>(
        ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, ChannelBlock::DataType::FloatLe, 32));
    zassert_equal(group.GetInvalidationBytes(), 0, "a group has no invalidation bytes until they are set");

    group.SetInvalidationBytes(1);
    zassert_equal(group.GetInvalidationBytes(), 1);
    zassert_equal(group.GetDataSizeBytes(), 4, "the invalidation bytes are not part of the data bytes");

    const auto base = BaseSizeForLinks(6);
    const auto data = ResolveAndSerialize(group);
    zassert_equal(Read<uint32_t>(data, base + 24), 4, "cg_data_bytes");
    zassert_equal(Read<uint32_t>(data, base + 28), 1, "cg_inval_bytes");

    // A VLSD group's cg_inval_bytes is the high half of its total value size.
    ChannelGroupBlock vlsd_group(1, 2);
    vlsd_group.SetFlags(std::to_underlying(ChannelGroupBlock::Flag::VlsdChannel));
    zassert_true(Throws<std::runtime_error>([&] { vlsd_group.SetInvalidationBytes(1); }),
        "a VLSD group must refuse invalidation bytes its serialization would drop");
    zassert_equal(vlsd_group.GetInvalidationBytes(), 0);

    zassert_true(Throws<std::runtime_error>([&] { group.SetFlags(std::to_underlying(ChannelGroupBlock::Flag::VlsdChannel)); }),
        "a group with invalidation bytes must not turn into a VLSD group");
    zassert_equal(group.GetFlags(), 0, "the refused flags are not set");
}

ZTEST(mdf_blocks, test_ChannelBlockInvalidationBit) {
    ChannelBlock channel(
        ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, ChannelBlock::DataType::FloatLe, 32);
    channel.SetFlags(std::to_underlying(ChannelBlock::Flag::BusEvent));

    const auto base = BaseSizeForLinks(8);
    zassert_false(channel.HasInvalidationBit());
    zassert_equal(Read<uint32_t>(Serialize(channel), base + 16), 0, "cn_inval_bit_pos without a bit");

    channel.SetInvalidationBit(5);
    zassert_true(channel.HasInvalidationBit());
    zassert_equal(channel.GetInvalidationBitPosition(), 5);

    const auto data = Serialize(channel);
    zassert_equal(Read<uint32_t>(data, base + 12),
        std::to_underlying(ChannelBlock::Flag::BusEvent) | std::to_underlying(ChannelBlock::Flag::InvalidValid),
        "cn_flags gains InvalidValid and keeps the flags set before");
    zassert_equal(Read<uint32_t>(data, base + 16), 5, "cn_inval_bit_pos");
}

ZTEST(mdf_blocks, test_ChannelBlockLayout) {
    ChannelBlock channel(
        ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, ChannelBlock::DataType::FloatLe, 32);

    zassert_equal(channel.GetBlockSize(), BaseSizeForLinks(8) + 72, "cn_data is 72 bytes of REAL and integers");

    channel.SetOffsetBytes(12);
    channel.SetOffsetBits(3);
    channel.SetFlags(std::to_underlying(ChannelBlock::Flag::BusEvent));

    const auto base = BaseSizeForLinks(8);
    const auto data = Serialize(channel);

    zassert_equal(BlockId(data), std::string("##CN"));
    zassert_equal(Read<uint64_t>(data, BLOCK_LINK_COUNT_OFFSET), 8);
    zassert_equal(data[base], std::to_underlying(ChannelBlock::Type::FixedLength));
    zassert_equal(data[base + 1], std::to_underlying(ChannelBlock::SyncType::NoSync));
    zassert_equal(data[base + 2], std::to_underlying(ChannelBlock::DataType::FloatLe));
    zassert_equal(data[base + 3], 3, "cn_bit_offset");
    zassert_equal(Read<uint32_t>(data, base + 4), 12, "cn_byte_offset");
    zassert_equal(Read<uint32_t>(data, base + 8), 32, "cn_bit_count");
    zassert_equal(Read<uint32_t>(data, base + 12), 0x0400, "cn_flags");

    // The six range and limit values are REAL, so they must occupy the last 48 bytes.
    for(size_t i = base + 24; i < base + 72; i++)
        zassert_equal(data[i], 0, "unset REAL byte %zu", i);
}

ZTEST(mdf_blocks, test_ChannelBlockRoundsBitCountUpToWholeBytes) {
    const struct {
        uint32_t bit_count;
        uint8_t bit_offset;
        uint32_t expected_bytes;
    } cases[] = {{1, 0, 1}, {7, 0, 1}, {8, 0, 1}, {9, 0, 2}, {12, 0, 2}, {29, 2, 4}, {32, 0, 4}, {80, 0, 10}};

    for(const auto& test_case : cases) {
        ChannelBlock channel(
            ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync,
            ChannelBlock::DataType::UnsignedIntegerLe, test_case.bit_count);
        channel.SetOffsetBits(test_case.bit_offset);

        zassert_equal(channel.GetDataSizeBytes(), test_case.expected_bytes,
            "%u bits at bit offset %u", test_case.bit_count, test_case.bit_offset);
        zassert_equal(channel.GetBitCount(), test_case.bit_count);
    }
}

ZTEST(mdf_blocks, test_ChannelBlockAppendsToTheEndOfTheChain) {
    auto make = []() {
        return std::make_shared<ChannelBlock>(
            ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, ChannelBlock::DataType::FloatLe, 32);
    };

    auto first = make();
    auto second = make();
    auto third = make();

    first->LinkBlock(second);
    first->LinkBlock(third);

    zassert_equal(first->GetLinkedChannel(), second);
    zassert_equal(second->GetLinkedChannel(), third);
    zassert_is_null(third->GetLinkedChannel().get());
}

ZTEST(mdf_blocks, test_SourceInformationBlockLayout) {
    SourceInformationBlock block(SourceInformationBlock::SourceType::Bus, SourceInformationBlock::BusType::Can);

    zassert_equal(block.GetBlockSize(), BaseSizeForLinks(3) + 8);

    const auto base = BaseSizeForLinks(3);
    const auto data = Serialize(block);

    zassert_equal(BlockId(data), std::string("##SI"));
    zassert_equal(Read<uint64_t>(data, BLOCK_LINK_COUNT_OFFSET), 3);
    zassert_equal(data[base], std::to_underlying(SourceInformationBlock::SourceType::Bus));
    zassert_equal(data[base + 1], std::to_underlying(SourceInformationBlock::BusType::Can));
    zassert_equal(data[base + 2], 0, "si_flags");
}

ZTEST(mdf_blocks, test_AlgebraicConversionAddsFormulaAsExtraLink) {
    auto formula = MakeTextBlock("x * 0.1");
    auto conversion = ChannelConversionBlock::CreateAlgebraicConversion(formula);

    // The formula reference is appended after the four fixed links.
    zassert_equal(conversion->GetBlockSize(), BaseSizeForLinks(5) + 24);
    zassert_equal(conversion->GetBlockLinks()->Count(), 5);

    // Links carry addresses, so the tree has to be resolved before it can be serialized.
    const auto base = BaseSizeForLinks(5);
    const auto data = ResolveAndSerialize(*conversion, 1024);

    zassert_equal(BlockId(data), std::string("##CC"));
    zassert_equal(Read<uint64_t>(data, BLOCK_LINK_COUNT_OFFSET), 5);
    zassert_equal(Read<uint64_t>(data, BLOCK_LINKS_OFFSET + 4 * LINK_SIZE), formula->GetAddress());
    zassert_equal(data[base], std::to_underlying(ChannelConversionBlock::ConversionType::Algebraic));
    zassert_equal(Read<uint16_t>(data, base + 4), 1, "cc_ref_count");
    zassert_equal(Read<uint16_t>(data, base + 6), 0, "cc_val_count");
}

ZTEST(mdf_blocks, test_AlgebraicConversionRequiresAFormula) {
    bool threw = false;
    try {
        ChannelConversionBlock::CreateAlgebraicConversion(nullptr);
    } catch(const std::invalid_argument&) {
        threw = true;
    }
    zassert_true(threw, "an algebraic conversion without a formula is meaningless");
}

ZTEST(mdf_blocks, test_DataGroupRejectsANullChannelGroup) {
    DataGroupBlock block(1);
    block.AddChannelGroup(std::make_shared<ChannelGroupBlock>(1, 1));

    zassert_true(Throws<std::invalid_argument>([&] { block.AddChannelGroup(nullptr); }),
        "a null group would reset the tail and drop the chain on the next add");
}

