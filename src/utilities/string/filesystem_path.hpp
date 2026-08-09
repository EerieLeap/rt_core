#pragma once

#include <string_view>

#include "static_string.hpp"

namespace eerie_leap::utilities::string {

template<size_t N>
class FilesystemPath {
private:
    static constexpr char kPathSeparator_ = '/';
    static constexpr std::string_view kRootPath_{&kPathSeparator_, 1};
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
     * Mirrors std::filesystem::path::parent_path, dropping the last element of
     * the path. A trailing separator only contributes an empty element, so it is
     * dropped on its own.
     * "foo/bar/baz" -> "foo/bar"
     * "foo"         -> ""        (no separator -> no parent)
     * "foo/"        -> "foo"
     * "foo//bar"    -> "foo"     (redundant separators are collapsed)
     * "/foo"        -> "/"
     * "/"           -> "/"       (root has itself as parent)
     * ""            -> ""
     *
     * @return FilesystemPath representing the parent path
     */
    [[nodiscard]] FilesystemPath parent_path() const noexcept {
        const std::string_view view = data_.ToString();

        if(view.empty())
            return FilesystemPath{};

        constexpr auto trim_separators = [](std::string_view value) {
            while(!value.empty() && value.back() == kPathSeparator_)
                value.remove_suffix(1);

            return value;
        };

        if(view.back() == kPathSeparator_) {
            const std::string_view trimmed = trim_separators(view);

            return trimmed.empty() ? FilesystemPath{kRootPath_} : FilesystemPath{trimmed};
        }

        const auto pos = view.rfind(kPathSeparator_);

        if(pos == std::string_view::npos)
            return FilesystemPath{};

        const std::string_view parent = trim_separators(view.substr(0, pos));

        return parent.empty() ? FilesystemPath{kRootPath_} : FilesystemPath{parent};
    }
};

} // namespace eerie_leap::utilities::string
