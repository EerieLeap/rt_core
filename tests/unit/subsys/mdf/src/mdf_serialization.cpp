#include <cstdint>
#include <ios>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/mdf/mdf4/data_block.h"
#include "subsys/mdf/mdf4/text_block.h"
#include "subsys/mdf/utilities/block_links.h"
#include "subsys/mdf/utilities/block_links_empty.h"

#include "mdf_test_support.h"

using namespace eerie_leap::subsys::mdf;
using namespace eerie_leap::subsys::mdf::mdf4;
using namespace eerie_leap::subsys::mdf::utilities;
using namespace mdf_test;

namespace {

enum class TestLink : int {
    First = 0,
    Second
};

std::shared_ptr<TestBlock> MakeBlock(const std::string& id, uint64_t payload_size = 0) {
    return std::make_shared<TestBlock>(id, payload_size);
}

} // namespace

ZTEST_SUITE(mdf_serialization, NULL, NULL, NULL, NULL, NULL);

ZTEST(mdf_serialization, test_ResolveAddressLaysBlocksOutInDepthFirstOrder) {
    auto root = MakeBlock("RT", 8);      // 32 bytes
    auto first = MakeBlock("AA", 16);    // 40 bytes
    auto second = MakeBlock("BB", 0);    // 24 bytes
    auto nested = MakeBlock("CC", 0);    // 24 bytes

    first->AddChild(nested);
    root->AddChild(first);
    root->AddChild(second);

    const auto end = root->ResolveAddress(100);

    zassert_equal(root->GetAddress(), 100);
    zassert_equal(first->GetAddress(), 132);
    zassert_equal(nested->GetAddress(), 172, "children are laid out before the next sibling");
    zassert_equal(second->GetAddress(), 196);
    zassert_equal(end, 220, "the returned address is one past the last block");
}

ZTEST(mdf_serialization, test_ResolveAddressVisitsSharedChildOnce) {
    auto root = MakeBlock("RT");
    auto first = MakeBlock("AA");
    auto second = MakeBlock("BB");
    auto shared = MakeBlock("SH");

    first->AddChild(shared);
    second->AddChild(shared);
    root->AddChild(first);
    root->AddChild(second);

    const auto end = root->ResolveAddress(64);

    zassert_equal(shared->GetAddress(), 112, "the shared block is placed under the first parent");
    zassert_equal(second->GetAddress(), 136);
    zassert_equal(end, 160, "a shared block only takes up space once");
}

// A *_next sibling comes after every child of the block before it, which is the order the
// recursive traversal produced when the next link was the last child.
ZTEST(mdf_serialization, test_NextChainIsLaidOutAfterTheChildren) {
    auto root = MakeBlock("RT");
    auto first = MakeBlock("AA");
    auto first_child = MakeBlock("AC");
    auto second = MakeBlock("BB");
    auto second_child = MakeBlock("BC");
    auto third = MakeBlock("CC");

    root->AddChild(first);
    first->AddChild(first_child);
    first->SetNext(second);
    second->AddChild(second_child);
    second->SetNext(third);

    const auto end = root->ResolveAddress(0);
    zassert_equal(end, 6 * BLOCK_HEADER_SIZE);

    const std::vector<std::shared_ptr<TestBlock>> expected_order{root, first, first_child, second, second_child, third};
    for(size_t i = 0; i < expected_order.size(); i++) {
        zassert_equal(expected_order[i]->GetAddress(), i * BLOCK_HEADER_SIZE,
            "block %s is not at position %zu", expected_order[i]->GetId().c_str(), i);
    }

    auto stream = MakeStream();
    zassert_equal(root->WriteToStream(stream), end, "every block of the chain is written once");

    const auto data = stream.str();
    for(const auto& block : expected_order) {
        zassert_equal(BlockId(AsBytes(data), block->GetAddress()), "##" + block->GetId(),
            "block %s is not at its resolved address", block->GetId().c_str());
    }

    root->Reset();
    for(const auto& block : expected_order) {
        zassert_equal(block->GetAddress(), 0, "block %s keeps an address after a reset", block->GetId().c_str());
        zassert_false(block->IsSerialized(), "block %s stays serialized after a reset", block->GetId().c_str());
    }
}

// The VLSD channel group a CAN channel points at is also a sibling on the channel group chain.
ZTEST(mdf_serialization, test_ChainTraversalsTerminateOnAChildThatIsAlsoASibling) {
    auto head = MakeBlock("AA");
    auto sibling = MakeBlock("BB");
    auto last = MakeBlock("CC");

    head->AddChild(sibling);
    head->SetNext(sibling);
    sibling->SetNext(last);
    last->AddChild(head);

    const auto end = head->ResolveAddress(64);
    zassert_equal(end, 64 + 3 * BLOCK_HEADER_SIZE, "each block takes up space once");

    auto stream = MakeStream();
    head->WriteToStream(stream);
    zassert_equal(stream.str().size(), 3 * BLOCK_HEADER_SIZE, "no block is written twice");

    head->Reset();
    zassert_false(last->IsSerialized(), "the reset reaches the end of the chain");

    head->ClearChildren();
    head->SetNext(nullptr);
    last->ClearChildren();
}

ZTEST(mdf_serialization, test_WriteToStreamPlacesEveryBlockAtItsResolvedAddress) {
    auto root = MakeBlock("RT");
    auto first = MakeBlock("AA");
    auto second = MakeBlock("BB");
    auto shared = MakeBlock("SH");

    first->AddChild(shared);
    second->AddChild(shared);
    root->AddChild(first);
    root->AddChild(second);

    root->ResolveAddress(0);

    auto stream = MakeStream();
    const auto bytes_written = root->WriteToStream(stream);

    const auto data = stream.str();
    zassert_equal(bytes_written, data.size(), "the reported size must match what reached the stream");
    zassert_equal(data.size(), 4 * BLOCK_HEADER_SIZE, "the shared block is written exactly once");

    for(const auto& block : {root, first, shared, second}) {
        zassert_true(block->IsSerialized(), "block %s was not written", block->GetId().c_str());
        zassert_equal(BlockId(AsBytes(data), block->GetAddress()), "##" + block->GetId(),
            "block %s is not at its resolved address", block->GetId().c_str());
    }
}

ZTEST(mdf_serialization, test_ResetClearsTheWholeTree) {
    auto root = MakeBlock("RT");
    auto child = MakeBlock("AA");
    root->AddChild(child);

    root->ResolveAddress(64);

    auto stream = MakeStream();
    root->WriteToStream(stream);

    zassert_true(root->IsSerialized());
    zassert_true(child->IsSerialized());

    root->Reset();

    zassert_equal(root->GetAddress(), 0);
    zassert_false(root->IsSerialized());
    zassert_equal(child->GetAddress(), 0);
    zassert_false(child->IsSerialized());
}

ZTEST(mdf_serialization, test_ResetTerminatesOnCircularReferences) {
    // A channel referencing a VLSD channel group that chains back to the channel forms a cycle.
    auto first = MakeBlock("AA");
    auto second = MakeBlock("BB");
    first->AddChild(second);
    second->AddChild(first);

    first->ResolveAddress(64);

    auto stream = MakeStream();
    first->WriteToStream(stream);

    zassert_equal(stream.str().size(), 2 * BLOCK_HEADER_SIZE, "a cycle must not write a block twice");

    first->Reset();

    zassert_false(first->IsSerialized());
    zassert_false(second->IsSerialized());

    first->ClearChildren();
    second->ClearChildren();
}

ZTEST(mdf_serialization, test_ResolveAndWriteCanBeRepeated) {
    auto root = MakeBlock("RT");
    auto child = MakeBlock("AA");
    root->AddChild(child);

    auto first_stream = MakeStream();
    root->ResolveAddress(0);
    root->WriteToStream(first_stream);

    root->Reset();

    auto second_stream = MakeStream();
    root->ResolveAddress(0);
    root->WriteToStream(second_stream);

    zassert_equal(first_stream.str(), second_stream.str(), "rewriting the tree must be deterministic");
}

ZTEST(mdf_serialization, test_WriteToStreamThrowsOnShortWrite) {
    auto root = MakeBlock("RT");
    root->ResolveAddress(0);

    LimitedStreamBuf stream(BLOCK_HEADER_SIZE - 1);

    bool threw = false;
    try {
        root->WriteToStream(stream);
    } catch(const std::ios_base::failure&) {
        threw = true;
    }
    zassert_true(threw, "a truncated write must not be reported as success");
}

ZTEST(mdf_serialization, test_RewriteToStreamRequiresAPreviousWrite) {
    DataBlock block;
    auto stream = MakeStream();

    bool threw = false;
    try {
        block.RewriteToStream(stream);
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a block that was never written has no address to patch");
}

ZTEST(mdf_serialization, test_RewriteToStreamPatchesInPlace) {
    auto root = MakeBlock("RT");
    auto data_block = std::make_shared<DataBlock>();
    root->AddChild(data_block);

    root->ResolveAddress(0);

    auto stream = MakeStream();
    const auto bytes_written = root->WriteToStream(stream);

    // Records would be appended here before the length is known.
    const std::string records(48, '\x5A');
    stream.sputn(records.data(), static_cast<std::streamsize>(records.size()));

    data_block->SetDataSizeBytes(records.size());
    zassert_equal(data_block->RewriteToStream(stream), BLOCK_HEADER_SIZE, "only the header is rewritten");

    const auto data = stream.str();
    zassert_equal(data.size(), bytes_written + records.size(), "patching must not grow the stream");
    zassert_equal(Read<uint64_t>(AsBytes(data), data_block->GetAddress() + BLOCK_LENGTH_OFFSET),
        BLOCK_HEADER_SIZE + records.size());
    zassert_mem_equal(data.data() + data.size() - records.size(), records.data(), records.size(),
        "the appended records must stay untouched");
}

ZTEST(mdf_serialization, test_RewriteToStreamThrowsWhenTheStreamCannotSeek) {
    DataBlock block;
    block.ResolveAddress(0);

    NonSeekableStreamBuf stream;
    block.WriteToStream(stream);

    bool threw = false;
    try {
        block.RewriteToStream(stream);
    } catch(const std::ios_base::failure&) {
        threw = true;
    }
    zassert_true(threw, "finalization needs a seekable stream and must say so");
}

ZTEST(mdf_serialization, test_BlockLinksTrackFixedAndExtraLinks) {
    BlockLinks<TestLink, 2> links;

    zassert_equal(links.Count(), 2);
    zassert_equal(links.GetLinksSizeBytes(), 2 * LINK_SIZE);
    zassert_is_null(links.GetLink(TestLink::First).get());

    auto first = MakeBlock("AA");
    links.SetLink(TestLink::First, first);
    zassert_equal(links.GetLink(TestLink::First), std::static_pointer_cast<IBlock>(first));

    links.AddExtraLink(MakeBlock("BB"));
    zassert_equal(links.Count(), 3, "extra links are appended after the fixed ones");
    zassert_equal(links.GetLinksSizeBytes(), 3 * LINK_SIZE);
    zassert_equal(links.GetLinks().size(), 3);
}

ZTEST(mdf_serialization, test_BlockLinksRejectAnOutOfRangeLinkType) {
    BlockLinks<TestLink, 2> links;

    bool threw = false;
    try {
        links.SetLink(static_cast<TestLink>(2), MakeBlock("AA"));
    } catch(const std::out_of_range&) {
        threw = true;
    }
    zassert_true(threw, "writing past the fixed link area would corrupt the extra links");

    threw = false;
    try {
        (void)links.GetLink(static_cast<TestLink>(-1));
    } catch(const std::out_of_range&) {
        threw = true;
    }
    zassert_true(threw, "a negative link type must be rejected");
}

ZTEST(mdf_serialization, test_BlockLinksSerializeAddresses) {
    BlockLinks<TestLink, 2> links;

    auto first = MakeBlock("AA");
    first->ResolveAddress(256);
    links.SetLink(TestLink::First, first);

    const auto buffer = links.Serialize();
    zassert_equal(Read<uint64_t>({buffer.get(), static_cast<size_t>(links.GetLinksSizeBytes())}, 0), 256);
    zassert_equal(Read<uint64_t>({buffer.get(), static_cast<size_t>(links.GetLinksSizeBytes())}, LINK_SIZE), 0, "unset links stay null");
}

ZTEST(mdf_serialization, test_BlockLinksRejectUnresolvedAddresses) {
    BlockLinks<TestLink, 2> links;
    links.SetLink(TestLink::First, MakeBlock("AA"));

    bool threw = false;
    try {
        (void)links.Serialize();
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "serializing a link before its target has an address would produce a dangling offset");
}

ZTEST(mdf_serialization, test_EmptyBlockLinksHaveNoLinkArea) {
    BlockLinksEmpty links;

    zassert_equal(links.Count(), 0);
    zassert_equal(links.GetLinksSizeBytes(), 0);
    zassert_equal(links.GetLinks().size(), 0);
}
