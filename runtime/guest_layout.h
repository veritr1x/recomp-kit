// Layout operations shared by runtime and launcher; no filesystem access.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace guest_layout {
inline std::string slashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}
inline bool same(std::string a, std::string b) {
    auto fold = [](unsigned char c) { return char(std::tolower(c)); };
    std::transform(a.begin(), a.end(), a.begin(), fold);
    std::transform(b.begin(), b.end(), b.begin(), fold);
    return a == b;
}
// Strip only a whole directory suffix. A mismatched installation retains the
// old layout as a unit: neither the root nor the guest EXE path is half changed.
inline bool root_from_exe(const std::string &folder, const std::string &exe_dir,
                          std::string *root) {
    const std::string path = slashes(folder), suffix = slashes(exe_dir);
    *root = path;
    if (suffix.empty())
        return true;
    if (same(path, suffix)) {
        *root = "";
        return true;
    }
    if (path.size() <= suffix.size() || path[path.size() - suffix.size() - 1] != '/' ||
        !same(path.substr(path.size() - suffix.size()), suffix))
        return false;
    *root = path.substr(0, path.size() - suffix.size() - 1);
    if (root->empty())
        *root = "/";
    if (root->size() == 2 && (*root)[1] == ':')
        *root += '/';
    return true;
}
} // namespace guest_layout
