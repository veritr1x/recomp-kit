// Exercise the guest's path resolver with and without its writable profile.
#include "runtime/win32.h"
#include "runtime/memory.h"
#include "mods/mods_internal.h"
#include "mods/roots.h"
#include "platform/os.h"
#include <cstdio>
#include <filesystem>
#include <string>

static int failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
static void write(const std::string &path, const char *text) {
    FILE *file = std::fopen(path.c_str(), "wb");
    CHECK(file);
    if (file) {
        std::fputs(text, file);
        std::fclose(file);
    }
}

int main() {
    std::string root = std::string(os_temp_dir()) + "/recomp-hardening-XXXXXX";
    if (os_mkdtemp(root.data()) != 0)
        return 1;
    const std::string game = root + "/game", profile = root + "/profile",
                      outside = root + "/outside";
    CHECK(os_mkdir(game.c_str()) == 0);
    CHECK(os_mkdir(profile.c_str()) == 0);
    CHECK(os_mkdir(outside.c_str()) == 0);
    write(game + "/data.txt", "original");
    write(outside + "/private.txt", "outside");
    CHECK(os_path_is_absolute(root.c_str()));
    CHECK(!os_path_is_absolute("mods"));
    CHECK(!os_path_is_absolute("C:mods"));
    CHECK(os_path_is_within(game.c_str(), (game + "/data.txt").c_str()));
    CHECK(!os_path_is_within(game.c_str(), outside.c_str()));
    CHECK(os_path_is_within(game.c_str(), game.c_str()));
    mem_init();
    win32_init(game);
    win32_set_file_ops(nullptr, nullptr);
    CHECK(!win32_host_path_op("data.txt", WIN32_FILE_READ).empty());
    for (int op :
         {WIN32_FILE_WRITE, WIN32_FILE_DELETE, WIN32_FILE_RENAME_SRC, WIN32_FILE_RENAME_DST}) {
        CHECK(win32_host_path_op("data.txt", op).empty());
        CHECK(win32_host_path_op("new.txt", op).empty());
    }
    mods_overlay_set_profile_dir(profile.c_str());
    mods_overlay_reset();
    const std::string writable = win32_host_path_op("data.txt", WIN32_FILE_WRITE);
    CHECK(!writable.empty() && os_path_is_within(profile.c_str(), writable.c_str()));
    CHECK(!win32_host_path_op("fresh/slot.sav", WIN32_FILE_WRITE).empty());
    CHECK(win32_host_path_op("data.txt", WIN32_FILE_DELETE) == writable);
    CHECK(win32_host_path_op("missing.txt", WIN32_FILE_DELETE).empty());
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, game + "/escape", error);
    if (!error) {
        win32_invalidate_dir_cache();
        CHECK(win32_host_path_op("escape/private.txt", WIN32_FILE_READ).empty());
        win32_set_file_ops(nullptr, nullptr);
        CHECK(win32_host_path_op("escape/private.txt", WIN32_FILE_READ).empty());
        mods_overlay_reset();
        std::filesystem::create_directory_symlink(outside, profile + "/escape", error);
        CHECK(!error);
        for (int op : {WIN32_FILE_READ, WIN32_FILE_WRITE, WIN32_FILE_DELETE, WIN32_FILE_RENAME_DST})
            CHECK(win32_host_path_op("escape/private.txt", op).empty());
    } else {
        std::printf("symlink probes skipped: host privilege unavailable (%s)\n",
                    error.message().c_str());
    }
    os_unsetenv("RECOMP_MODS_DIR");
    CHECK(mods_roots().user.empty());
    os_setenv("RECOMP_MODS_DIR", "mods");
    CHECK(mods_roots().user.empty());
    os_setenv("RECOMP_MODS_DIR", root.c_str());
    CHECK(mods_roots().user == root);
    mem_shutdown();
    std::filesystem::remove_all(root, error);
    CHECK(!error);
    return failures ? 1 : 0;
}
