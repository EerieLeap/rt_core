#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cbor_size_calculator.hpp"

namespace eerie_leap::utilities::cbor {

/**
 * Helper class for building size calculations fluently
 */
class CborSizeBuilder {
private:
    size_t size_;

public:
    CborSizeBuilder() :
        size_(0) {}
    explicit CborSizeBuilder(size_t initial_size) :
        size_(initial_size) {}

    // Integer types
    CborSizeBuilder& AddUint(uint64_t value) {
        size_ += CborSizeCalc::SizeOfUint(value);
        return *this;
    }

    CborSizeBuilder& AddInt(int64_t value) {
        size_ += CborSizeCalc::SizeOfInt(value);
        return *this;
    }

    CborSizeBuilder& AddUintFixed(size_t bytes) {
        size_ += CborSizeCalc::SizeOfUintFixed(bytes);
        return *this;
    }

    // Strings
    CborSizeBuilder& AddTstr(const zcbor_string& str) {
        size_ += CborSizeCalc::SizeOfTstr(str);
        return *this;
    }

    // Special values
    CborSizeBuilder& AddBool(bool value) {
        size_ += CborSizeCalc::SizeOfBool(value);
        return *this;
    }

    CborSizeBuilder& AddFloat(float value) {
        size_ += CborSizeCalc::SizeOfFloat(value);
        return *this;
    }

    CborSizeBuilder& AddDouble(double value) {
        size_ += CborSizeCalc::SizeOfDouble(value);
        return *this;
    }

    // Definite-length arrays and maps
    CborSizeBuilder& AddArrayStart(size_t count) {
        size_ += CborSizeCalc::SizeOfArrayStart(count);
        return *this;
    }

    CborSizeBuilder& AddMapStart(size_t pair_count) {
        size_ += CborSizeCalc::SizeOfMapStart(pair_count);
        return *this;
    }

    // Indefinite-length arrays and maps (zcbor style)
    CborSizeBuilder& AddIndefiniteArrayStart() {
        size_ += CborSizeCalc::SizeOfIndefiniteArrayStart();
        return *this;
    }

    CborSizeBuilder& AddIndefiniteMapStart() {
        size_ += CborSizeCalc::SizeOfIndefiniteMapStart();
        return *this;
    }

    // Generic size addition
    CborSizeBuilder& AddSize(size_t additional_size) {
        size_ += additional_size;
        return *this;
    }

    // Optional values
    template<typename T, typename SizeFunc>
    CborSizeBuilder& AddOptional(const T* value, SizeFunc size_func) {
        size_ += CborSizeCalc::SizeOfOptional(value, size_func);
        return *this;
    }

    template<typename T, typename SizeFunc>
    CborSizeBuilder& AddOptional(bool has_value, const T& value, SizeFunc size_func) {
        size_ += CborSizeCalc::SizeOfOptional(has_value, value, size_func);
        return *this;
    }

    // Get result
    size_t Build() const {
        return size_;
    }

    size_t GetSize() const {
        return size_;
    }

    explicit operator size_t() const {
        return size_;
    }

    // Reset
    void Reset() {
        size_ = 0;
    }
};

} // namespace eerie_leap::utilities::cbor
