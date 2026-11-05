// The fetch backend owns a JS proxy worker. Only the guest worker builds
// this index, before boot; the fetch worker only reads it after publication.
#include "web_stream.h"
#include "../platform/os.h"
#include <emscripten/wasmfs.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace {
std::map<std::string, uint64_t> g_sizes;

bool path_valid(const std::string &path) {
    if (path.empty() || path.size() > 1024 || path.front() == '/' ||
        path.find_first_of("\\:") != std::string::npos)
        return false;
    for (unsigned char c : path)
        if (c < 32)
            return false;
    std::istringstream parts(path);
    std::string part;
    while (std::getline(parts, part, '/'))
        if (part.empty() || part == "." || part == "..")
            return false;
    return path.back() != '/';
}

std::string folded(std::string path) {
    for (char &c : path)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return path;
}
} // namespace

// Called on the fetch proxy worker. Without manifest sizes, scanning the
// installation would HEAD/download every file before the first game frame.
extern "C" double recomp_web_stream_size(const char *url) {
    auto found = g_sizes.find(url ? url : "");
    return found == g_sizes.end() ? -1.0 : double(found->second);
}

bool web_stream_mount(const std::string &base_url, std::string *error) {
    auto fail = [&](const char *message) {
        *error = message;
        return false;
    };
    if ((base_url.rfind("http://", 0) != 0 && base_url.rfind("https://", 0) != 0) ||
        base_url.back() == '/')
        return fail("invalid data URL");
    backend_t backend = wasmfs_create_fetch_backend(base_url.c_str(), 4 * 1024 * 1024);
    if (!backend || wasmfs_create_directory("/data", 0777, backend) != 0)
        return fail("could not mount streamed data");
    int fd = wasmfs_create_file("/data/manifest.txt", 0444, backend);
    if (fd < 0)
        return fail("could not register the asset manifest");
    os_fd_close(fd);
    FILE *file = fopen("/data/manifest.txt", "rb");
    if (!file)
        return fail("could not open the asset manifest");
    std::string text;
    char buffer[16384];
    size_t count;
    while ((count = fread(buffer, 1, sizeof buffer, file)) != 0 && text.size() <= 4 * 1024 * 1024)
        text.append(buffer, count);
    bool bad = ferror(file) || text.size() > 4 * 1024 * 1024;
    fclose(file);
    if (bad)
        return fail("asset manifest is unreadable or too large");
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line) || line != "recomp-stream-v1")
        return fail("unsupported asset manifest");
    std::vector<std::pair<std::string, uint64_t>> files;
    std::set<std::string> names;
    while (std::getline(lines, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0)
            return fail("invalid asset record");
        std::string size = line.substr(0, tab), name = line.substr(tab + 1);
        if (!path_valid(name) || size.find_first_not_of("0123456789") != std::string::npos ||
            !names.insert(folded(name)).second || folded(name) == "manifest.txt" ||
            folded(name) == ".recomp-stream" || files.size() >= 100000)
            return fail("invalid or duplicate asset path");
        errno = 0;
        uint64_t length = strtoull(size.c_str(), nullptr, 10);
        if (errno || length > 9007199254740991ULL)
            return fail("invalid asset size");
        files.emplace_back(name, length);
    }
    std::set<std::string> directories;
    for (const auto &[name, length] : files) {
        for (size_t slash = name.find('/'); slash != std::string::npos;
             slash = name.find('/', slash + 1)) {
            std::string dir = "/data/" + name.substr(0, slash);
            if (directories.insert(dir).second &&
                wasmfs_create_directory(dir.c_str(), 0777, backend) != 0)
                return fail("could not register an asset directory");
        }
        fd = wasmfs_create_file(("/data/" + name).c_str(), 0444, backend);
        if (fd < 0)
            return fail("could not register an asset file");
        os_fd_close(fd);
        g_sizes.emplace(base_url + "//" + name, length);
    }
    g_sizes.emplace(base_url + "//manifest.txt", text.size());
    for (const auto &dir : directories)
        if (os_set_readonly(dir.c_str(), 1) != 0)
            return fail("could not protect an asset directory");
    if (os_set_readonly("/data", 1) != 0)
        return fail("could not protect streamed data");
    printf("[stream] mounted %zu read-only files; 4 MiB chunks, 64 MiB download cache\n",
           files.size());
    return true;
}
