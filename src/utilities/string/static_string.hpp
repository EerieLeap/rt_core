#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <string_view>

namespace eerie_leap::utilities::string {

template<size_t N>
class StaticString {
private:
    std::array<char, N + 1> data_ = {};
    size_t size_ = 0;

public:
    StaticString() = default;

    explicit StaticString(std::string_view str) {
        assert(str.size() <= N && "StaticString: input exceeds capacity");
        const size_t count = std::min(str.size(), N);
        std::copy_n(str.data(), count, data_.data());
        size_ = count;
    }

    [[nodiscard]] const char* CStr() const noexcept {
        return data_.data();
    }

    [[nodiscard]] std::string_view ToString() const noexcept {
        return {data_.data(), size_};
    }

    [[nodiscard]] size_t Size() const noexcept { return size_; }
    [[nodiscard]] bool Empty() const noexcept { return size_ == 0; }
    static constexpr size_t Capacity() noexcept { return N; }

    void Clear() noexcept {
        data_[0] = '\0';
        size_ = 0;
    }

    bool Append(std::string_view str) noexcept {
        if(str.size() > N - size_)
            return false;

        std::copy_n(str.data(), str.size(), data_.data() + size_);
        size_ += str.size();
        data_[size_] = '\0';

        return true;
    }

    bool Append(char c) noexcept {
        if(size_ >= N)
            return false;

        data_[size_++] = c;
        data_[size_] = '\0';

        return true;
    }

    bool Truncate(size_t new_size) noexcept {
        if(new_size > size_)
            return false;

        size_ = new_size;
        data_[size_] = '\0';

        return true;
    }

    void operator+=(std::string_view str) noexcept { Append(str); }
    void operator+=(char c) noexcept { Append(c); }

    [[nodiscard]] char operator[](size_t index) const noexcept {
        assert(index < size_);
        return data_[index];
    }

    [[nodiscard]] bool operator==(std::string_view other) const noexcept {
        return ToString() == other;
    }

    template<size_t M>
    [[nodiscard]] bool operator==(const StaticString<M>& other) const noexcept {
        return ToString() == other.ToString();
    }
};

} // namespace eerie_leap::utilities::string
