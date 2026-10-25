// Startup APIs and 32-bit ABI edge cases checked without a game image.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../../platform/os.h"
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>
#include <set>

static int checks, failures;
static uint32_t scratch;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
static uint32_t call(X86 *c, const char *dll, const char *name,
                     std::initializer_list<uint32_t> args = {}) {
    return guest_call(c, imports_resolve(dll, name), args.begin(), int(args.size()));
}
static std::vector<std::string> argv(X86 *c, const std::string &text) {
    gm_put_wstr(scratch, text, 1024);
    const uint32_t block = call(c, "shell32.dll", "CommandLineToArgvW", {scratch, scratch + 0x900});
    CHECK(block && heap_owns(block));
    std::vector<std::string> out;
    if (block) {
        for (uint32_t i = 0; i < rd32(scratch + 0x900); ++i)
            out.push_back(gm_wstr(rd32(block + 4 * i)));
        CHECK(call(c, "kernel32.dll", "LocalFree", {block}) == 0);
        CHECK(!heap_owns(block));
    }
    return out;
}
static std::set<std::string> find(X86 *c, const std::string &pattern) {
    gm_put_str(scratch, pattern.c_str(), 1024);
    std::set<std::string> names;
    uint32_t handle = call(c, "kernel32.dll", "FindFirstFileA", {scratch, scratch + 0x1000});
    if (handle != 0xffffffffu) {
        do {
            names.insert(gm_str(scratch + 0x1000 + 44));
        } while (call(c, "kernel32.dll", "FindNextFileA", {handle, scratch + 0x1000}));
        call(c, "kernel32.dll", "FindClose", {handle});
    }
    return names;
}
static void value(X86 *c) {
    set_eax(c, 0x12345678);
}
int main() {
    mem_init();
    imports_init();
    win32_init(".");
    X86 *c = loader_context();
    loader_init_context(c);
    scratch = heap_alloc(0x4000, true);
    uint32_t a = call(c, "kernel32.dll", "VirtualAlloc", {0, 4096, 0x3000, 4});
    uint32_t b = call(c, "kernel32.dll", "VirtualAlloc", {0, 4096, 0x3000, 4});
    CHECK(a && b && (a & 65535) == 0 && (b & 65535) == 0 && a != b);
    call(c, "kernel32.dll", "VirtualFree", {a, 0, 0x8000});
    call(c, "kernel32.dll", "VirtualFree", {b, 0, 0x8000});
    call(c, "kernel32.dll", "RaiseException", {0x406d1388u, 0, 0, 0});
    for (const char *name : {"GetComputerNameA", "GetComputerNameW"}) {
        wr32(scratch + 0x900, 3);
        wr32(scratch, 0xdeadbeef);
        CHECK(call(c, "kernel32.dll", name, {scratch, scratch + 0x900}) == 0);
        CHECK(rd32(scratch + 0x900) == 7 && rd32(scratch) == 0xdeadbeef);
        CHECK(call(c, "kernel32.dll", "GetLastError") == 111);
        wr32(scratch + 0x900, 32);
        CHECK(call(c, "kernel32.dll", name, {scratch, scratch + 0x900}) == 1);
        CHECK(rd32(scratch + 0x900) == 6);
        CHECK((name[15] == 'W' ? gm_wstr(scratch) : gm_str(scratch)) == "RECOMP");
    }
    const std::string temp = RECOMP_GUEST_ROOT "\\Temp\\";
    CHECK(call(c, "kernel32.dll", "GetTempPathW", {0, 0}) == temp.size() + 1);
    CHECK(call(c, "kernel32.dll", "GetTempPathW", {512, scratch}) == temp.size());
    CHECK(gm_wstr(scratch) == temp);
    // Golden quoting cases also verified against Windows' shell32 API.
    CHECK(argv(c, "prog \"a b\" c") == (std::vector<std::string>{"prog", "a b", "c"}));
    CHECK(argv(c, "prog a\\\"b") == (std::vector<std::string>{"prog", "a\"b"}));
    CHECK(argv(c, "prog \"a\"\"b\"") == (std::vector<std::string>{"prog", "a\"b"}));
    CHECK(argv(c, "prog \"b\"\" tail") == (std::vector<std::string>{"prog", "b\"", "tail"}));
    CHECK(argv(c, "prog \"\"\"\"x") == (std::vector<std::string>{"prog", "\"x"}));
    CHECK(argv(c, "\"pro\"g a") == (std::vector<std::string>{"pro", "g", "a"}));
    CHECK(argv(c, "pro\"g a\"") == (std::vector<std::string>{"pro\"g", "a"}));
    CHECK(argv(c, "   prog x") == (std::vector<std::string>{"", "prog", "x"}));
    CHECK(argv(c, "\"C:\\Program Files\\game.exe\" -x") ==
          (std::vector<std::string>{"C:\\Program Files\\game.exe", "-x"}));
    CHECK(argv(c, "prog a\\\\\"b c") == (std::vector<std::string>{"prog", "a\\b c"}));
    CHECK(argv(c, "prog \"\" x") == (std::vector<std::string>{"prog", "", "x"}));
    CHECK(argv(c, "prog \"a\\\\\" c") == (std::vector<std::string>{"prog", "a\\", "c"}));
    CHECK(argv(c, "") == (std::vector<std::string>{win32_executable_guest_path()}));
    win32_init_layout("build/Program Files", "Program Files");
    const std::string command = gm_str(call(c, "kernel32.dll", "GetCommandLineA"));
    CHECK(command == '"' + win32_executable_guest_path() + '"');
    CHECK(argv(c, command) == (std::vector<std::string>{win32_executable_guest_path()}));
    win32_init(".");
    const std::string base = "build/recomp/startup-files";
    os_mkdir("build/recomp");
    os_mkdir(base.c_str());
    os_mkdir((base + "/Folder").c_str());
    FILE *file = std::fopen((base + "/note.txt").c_str(), "wb");
    CHECK(file != nullptr);
    if (file)
        std::fclose(file);
    auto all = find(c, base + "/*.*"), dotless = find(c, base + "/*.");
    CHECK(all.count("Folder") && all.count("note.txt"));
    CHECK(dotless.count("Folder") && !dotless.count("note.txt"));
    os_unlink((base + "/note.txt").c_str());
    os_rmdir((base + "/Folder").c_str());
    os_rmdir(base.c_str());
    // Async presses survive release, are consumed per key, and do not repeat
    // just because a host sends another down event for an already held key.
    for (uint32_t key : {0x52u, 0x01u}) {
        host_set_key_state(int(key), false);
        call(c, "user32.dll", "GetAsyncKeyState", {key});
        CHECK(call(c, "user32.dll", "GetAsyncKeyState", {key}) == 0);
        host_set_key_state(int(key), true);
        CHECK(call(c, "user32.dll", "GetKeyState", {key}) & 0x8000);
        CHECK(call(c, "user32.dll", "GetAsyncKeyState", {key}) == 0x8001);
        CHECK(call(c, "user32.dll", "GetAsyncKeyState", {key}) == 0x8000);
        host_set_key_state(int(key), true);
        CHECK(call(c, "user32.dll", "GetAsyncKeyState", {key}) == 0x8000);
        host_set_key_state(int(key), false);
        CHECK(call(c, "user32.dll", "GetAsyncKeyState", {key}) == 0);
        host_set_key_state(int(key), true);
        host_set_key_state(int(key), false);
        CHECK(call(c, "user32.dll", "GetKeyState", {key}) == 0);
        CHECK(call(c, "user32.dll", "GetAsyncKeyState", {0xffffffff}) == 0);
        CHECK(call(c, "user32.dll", "GetAsyncKeyState", {key}) == 1);
        CHECK(call(c, "user32.dll", "GetAsyncKeyState", {key}) == 0);
    }
    // Fill the entire shim region last: later API resolutions cannot allocate.
    uint32_t last = 0;
    const uint32_t start = imports_count();
    for (uint32_t i = start; i < TRAMP_MAX; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "slot%u", i);
        last = imports_alloc_trampoline("capacity", name, value, 0);
        if (!last) {
            CHECK(false);
            break;
        }
    }
    CHECK(last == GUEST_SHIM_END - TRAMP_STRIDE && imports_count() == TRAMP_MAX);
    CHECK(guest_call(c, last) == 0x12345678);
    CHECK(imports_alloc_trampoline("capacity", "overflow", value, 0) == 0);
    std::printf("startup: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
