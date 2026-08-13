#include <ios>
#include <stdexcept>

#include "subsys/mdf/serializable_block_base.h"

namespace eerie_leap::subsys::mdf {

SerializableBlockBase::SerializableBlockBase()
    : address_(0), is_serialized_(false) {}

uint64_t SerializableBlockBase::GetSerializedSize() const {
    return GetBlockSize();
}

uint64_t SerializableBlockBase::WriteToStream(std::streambuf& stream) {
    const uint64_t size = GetSerializedSize();
    const auto block_data = Serialize();
    is_serialized_ = true;

    WriteBlockData(stream, block_data.get(), size);

    uint64_t bytes_written = size;
    for(auto& child : GetChildren()) {
        if(child && !child->IsSerialized())
            bytes_written += child->WriteToStream(stream);
    }

    return bytes_written;
}

uint64_t SerializableBlockBase::RewriteToStream(std::streambuf& stream) {
    if(!is_serialized_)
        throw std::runtime_error("Block was never written to the stream");

    const auto position = stream.pubseekpos(
        std::streambuf::pos_type(static_cast<std::streamoff>(address_)), std::ios_base::out);

    if(position == std::streambuf::pos_type(std::streambuf::off_type(-1)))
        throw std::ios_base::failure("Stream does not support seeking.");

    const uint64_t size = GetSerializedSize();
    const auto block_data = Serialize();

    WriteBlockData(stream, block_data.get(), size);

    return size;
}

uint64_t SerializableBlockBase::GetAddress() const {
    return address_;
}

bool SerializableBlockBase::IsSerialized() const {
    return is_serialized_;
}

void SerializableBlockBase::Reset() {
    // Already clean; also breaks the CN -> SignalData -> CG -> CN reference cycle.
    if(address_ == 0 && is_serialized_ == false)
        return;

    address_ = 0;
    is_serialized_ = false;

    for(auto& child : GetChildren()) {
        if(child)
            child->Reset();
    }
}

uint64_t SerializableBlockBase::ResolveAddress(uint64_t parent_address) {
    address_ = parent_address;
    uint64_t current_address = address_ + GetBlockSize();

    for(auto& child : GetChildren()) {
        if(child && child->GetAddress() == 0)
            current_address = child->ResolveAddress(current_address);
    }

    return current_address;
}

void SerializableBlockBase::WriteBlockData(std::streambuf& stream, const uint8_t* data, uint64_t size) {
    const auto written = stream.sputn(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));

    if(written < 0 || static_cast<uint64_t>(written) != size)
        throw std::ios_base::failure("Failed to write block to stream.");
}

} // namespace eerie_leap::subsys::mdf
