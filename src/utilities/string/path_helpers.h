#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace eerie_leap::utilities::string {

class PathHelpers {
private:
    // The next name of a path. Empty names ("a//b", a leading or trailing slash) and "." name
    // nothing and are skipped; the result is empty once the path is used up.
    static std::string_view NextName(std::string_view& path) {
        while(!path.empty()) {
            const size_t end = std::min(path.find('/'), path.size());
            const std::string_view name = path.substr(0, end);
            path.remove_prefix(std::min(end + 1, path.size()));

            if(!name.empty() && name != ".")
                return name;
        }

        return {};
    }

    static constexpr char ToLowerAscii(char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

public:
    /**
     * @brief Whether two paths relative to a volume name the same entry on a volume that folds ASCII
     *        case, as FAT does.
     *
     * The names are compared ASCII case-insensitively; empty names and "." are skipped, so
     * "Scripts//a.lua" and "./scripts/A.LUA" are the same entry. A path without a name, an empty one
     * say, names nothing and matches nothing, not even itself. ".." is not resolved.
     */
    static bool IsSameEntryIgnoringCase(std::string_view a, std::string_view b) {
        bool has_name = false;

        while(true) {
            const std::string_view name_a = NextName(a);
            const std::string_view name_b = NextName(b);

            if(!std::ranges::equal(name_a, name_b, [](char x, char y) {
                   return ToLowerAscii(x) == ToLowerAscii(y);
               }))
                return false;

            if(name_a.empty())
                return has_name;

            has_name = true;
        }
    }
};

} // namespace eerie_leap::utilities::string
