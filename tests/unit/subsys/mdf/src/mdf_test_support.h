#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/mdf/i_serializable_block.h"
#include "subsys/mdf/mdf4/block_base.h"

namespace mdf_test {

using eerie_leap::subsys::mdf::ISerializableBlock;
using eerie_leap::subsys::mdf::mdf4::BlockBase;

constexpr size_t ID_BLOCK_SIZE = 64;

// Common part of every ##XX block: id(4) + reserved(4) + length(8) + link count(8).
constexpr size_t BLOCK_HEADER_SIZE = 24;
constexpr size_t BLOCK_LENGTH_OFFSET = 8;
constexpr size_t BLOCK_LINK_COUNT_OFFSET = 16;
constexpr size_t BLOCK_LINKS_OFFSET = 24;
constexpr size_t LINK_SIZE = 8;

template<typename T>
T Read(std::span<const uint8_t> data, size_t offset) {
    T value{};
    zassert_true(offset + sizeof(T) <= data.size(), "read past the end of the buffer");
    std::memcpy(&value, data.data() + offset, sizeof(T));

    return value;
}

inline std::span<const uint8_t> AsBytes(const std::string& data) {
    return {reinterpret_cast<const uint8_t*>(data.data()), data.size()};
}

// A span into a temporary string would dangle before the first assertion runs.
inline std::span<const uint8_t> AsBytes(std::string&&) = delete;

inline std::string AsText(std::span<const uint8_t> data, size_t offset, size_t size) {
    zassert_true(offset + size <= data.size(), "read past the end of the buffer");
    return {reinterpret_cast<const char*>(data.data() + offset), size};
}

inline std::vector<uint8_t> Serialize(const ISerializableBlock& block) {
    const auto size = block.GetSerializedSize();
    const auto buffer = block.Serialize();

    return {buffer.get(), buffer.get() + size};
}

// Link fields hold absolute addresses, so a block that has links must be resolved before serializing.
inline std::vector<uint8_t> ResolveAndSerialize(ISerializableBlock& block, uint64_t base_address = ID_BLOCK_SIZE) {
    block.ResolveAddress(base_address);

    return Serialize(block);
}

inline std::string BlockId(std::span<const uint8_t> data, size_t offset = 0) {
    return AsText(data, offset, 4);
}

inline std::stringbuf MakeStream() {
    return std::stringbuf(std::ios::in | std::ios::out | std::ios::binary);
}

// Any other exception type propagates so an unexpected failure is not mistaken for the expected one.
template<typename Exception = std::exception>
bool Throws(auto&& action) {
    try {
        action();
    } catch(const Exception&) {
        return true;
    }

    return false;
}

// Minimal concrete block used to exercise the shared base class behaviour.
class TestBlock : public BlockBase {
private:
    uint64_t payload_size_;
    std::vector<std::shared_ptr<ISerializableBlock>> children_;

public:
    explicit TestBlock(const std::string& id, uint64_t payload_size = 0)
        : BlockBase(id), payload_size_(payload_size) {}

    void AddChild(std::shared_ptr<ISerializableBlock> child) {
        children_.push_back(std::move(child));
    }

    void ClearChildren() {
        children_.clear();
    }

    uint64_t GetBlockSize() const override {
        return GetBaseSize() + payload_size_;
    }

    std::unique_ptr<uint8_t[]> Serialize() const override {
        return SerializeBase();
    }

    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override {
        return children_;
    }
};

// Neither helper supports seeking. The default std::streambuf seek implementations are not
// linkable on every supported platform, so they are overridden rather than inherited.
class UnseekableStreamBuf : public std::streambuf {
protected:
    pos_type seekoff(off_type, std::ios_base::seekdir, std::ios_base::openmode) override {
        return pos_type(off_type(-1));
    }

    pos_type seekpos(pos_type, std::ios_base::openmode) override {
        return pos_type(off_type(-1));
    }
};

// Accepts at most `limit` bytes so short writes can be exercised.
class LimitedStreamBuf : public UnseekableStreamBuf {
private:
    size_t limit_;
    std::vector<uint8_t> data_;

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        const auto remaining = static_cast<std::streamsize>(limit_ - std::min(limit_, data_.size()));
        const auto accepted = std::min(n, remaining);
        data_.insert(data_.end(), s, s + accepted);

        return accepted;
    }

public:
    explicit LimitedStreamBuf(size_t limit) : limit_(limit) {}

    size_t Size() const { return data_.size(); }
};

// Accepts every write but does not implement seeking.
class NonSeekableStreamBuf : public UnseekableStreamBuf {
private:
    size_t size_ = 0;

protected:
    std::streamsize xsputn(const char*, std::streamsize n) override {
        size_ += static_cast<size_t>(n);
        return n;
    }

public:
    size_t Size() const { return size_; }
};

} // namespace mdf_test
