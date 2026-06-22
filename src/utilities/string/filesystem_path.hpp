#pragma once

#include <string_view>

#include "static_string.hpp"

namespace eerie_leap::utilities::string {

template<size_t N>
class FilesystemPath {
private:
    static constexpr char kPathSeparator_ = '/';
    StaticString<N> data_;

public:
    FilesystemPath() = default;

    explicit FilesystemPath(std::string_view path) : data_(path) {}

    StaticString<N>& String() noexcept { return data_; }
    const StaticString<N>& String() const noexcept { return data_; }

    /**
     * @brief Appends a path component with separator
     *
     * Mirrors std::filesystem::path::operator/=:
     *   - If rhs is empty, no-op.
     *   - Ensures exactly one separator between the existing path and rhs,
     *     regardless of trailing slashes.
     *
     * @param rhs The path component to append
     * @return Reference to this FilesystemPath
     */
    FilesystemPath& operator/=(std::string_view rhs) noexcept {
        if(rhs.empty())
            return *this;

        if(rhs.empty())
            return *this;

        if(rhs.front() == kPathSeparator_)
            data_.Clear();

        if(!data_.Empty()) {
            // Compute trimmed size without clearing — string_view stays valid
            size_t trimmed = data_.Size();
            while(trimmed > 0 && data_[trimmed - 1] == kPathSeparator_)
                --trimmed;

            data_.Truncate(trimmed);
            data_.Append(kPathSeparator_);
        }

        data_.Append(rhs);

        return *this;
    }

    /**
     * @brief Returns the parent path
     *
     * Returns everything before the last path separator.
     * "foo/bar/baz" -> "foo/bar"
     * "foo"         -> ""        (no separator -> no parent)
     * "foo/"        -> "foo"     (trailing slash is ignored, same as std::filesystem)
     * "/"           -> "/"       (root has itself as parent)
     *
     * @return FilesystemPath representing the parent path
     */
    [[nodiscard]] FilesystemPath parent_path() const noexcept {
        std::string_view view = data_.ToString();

        // Ignore trailing separator (but preserve bare "/" as root)
        if(view.size() > 1 && view.back() == kPathSeparator_)
            view.remove_suffix(1);

        const auto pos = view.rfind(kPathSeparator_);

        if(pos == std::string_view::npos)
            return FilesystemPath{};  // no separator -> empty parent

        if(pos == 0)
            return FilesystemPath{std::string_view{&view[0], 1}};  // root "/"

        return FilesystemPath{view.substr(0, pos)};
    }
};

} // namespace eerie_leap::utilities::string
