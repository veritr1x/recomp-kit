// Guest root and Windows path semantics for root and nested EXE layouts.
#include "../../runtime/guest_layout.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"
#include "../../platform/os.h"
#include "guest_abi.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)

static uint32_t input(const std::string &text, bool wide = false) {
    if (wide)
        gm_put_wstr(sc(0), text, 512);
    else
        gm_put_str(sc(0), text.c_str(), 512);
    return sc(0);
}
static std::string full(const std::string &path, bool wide = false) {
    const uint32_t n =
        call_shim(tramp("kernel32.dll", wide ? "GetFullPathNameW" : "GetFullPathNameA"),
                  {input(path, wide), 512, sc(0x500), sc(0xf00)});
    CHECK(n < 512);
    return wide ? gm_wstr(sc(0x500)) : gm_str(sc(0x500));
}
int main() {
    mem_init();
    imports_init();
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    cpu_reset();
    std::string root;
    CHECK(guest_layout::root_from_exe("C:\\Games\\Title\\bin\\Win32\\", "Bin/Win32", &root));
    CHECK(root == "C:/Games/Title");
    CHECK(!guest_layout::root_from_exe("/Games/NotBinaries/", "Binaries", &root));
    CHECK(root == "/Games/NotBinaries");
    CHECK(guest_layout::root_from_exe("/Binaries", "Binaries", &root) && root == "/");
    const std::string base = "build/recomp/guest-layout-test";
    os_mkdir("build/recomp");
    os_mkdir(base.c_str());
    os_mkdir((base + "/Binaries").c_str());
    os_mkdir((base + "/Content").c_str());
    for (bool nested : {false, true}) {
        win32_init_layout(base + (nested ? "/Binaries///" : ""), nested ? "Binaries" : "");
        CHECK(win32_game_dir() == base);
        const std::string exe_folder =
            std::string(RECOMP_GUEST_ROOT) + (nested ? "\\Binaries" : "");
        call_shim(tramp("kernel32.dll", "GetCurrentDirectoryA"), {512, sc(0x500)});
        CHECK(gm_str(sc(0x500)) == exe_folder);
        call_shim(tramp("kernel32.dll", "GetModuleFileNameA"), {0, sc(0x500), 512});
        CHECK(gm_str(sc(0x500)) == exe_folder + "\\" RECOMP_EXECUTABLE);
        CHECK(gm_str(call_shim(tramp("kernel32.dll", "GetCommandLineA"), {})) ==
              exe_folder + "\\" RECOMP_EXECUTABLE);
        const uint32_t dll =
            call_shim(tramp("kernel32.dll", "LoadLibraryA"), {input("kernel32.dll")});
        call_shim(tramp("kernel32.dll", "GetModuleFileNameA"), {dll, sc(0x500), 512});
        CHECK(gm_str(sc(0x500)) == exe_folder + "\\kernel32.dll");
        call_shim(tramp("kernel32.dll", "GetEnvironmentVariableA"),
                  {input("PATH"), sc(0x500), 512});
        CHECK(gm_str(sc(0x500)) == exe_folder + (nested ? ";" RECOMP_GUEST_ROOT : ""));
        CHECK(gm_str(call_shim(tramp("kernel32.dll", "GetEnvironmentStringsA"), {})) ==
              "PATH=" + exe_folder + (nested ? ";" RECOMP_GUEST_ROOT : ""));
        CHECK(gm_wstr(call_shim(tramp("kernel32.dll", "GetEnvironmentStringsW"), {})) ==
              "PATH=" + exe_folder + (nested ? ";" RECOMP_GUEST_ROOT : ""));
        for (bool wide : {false, true}) {
            CHECK(full(".\\foo\\..\\bar", wide) == exe_folder + "\\bar");
            CHECK(full(".\\", wide) == exe_folder + "\\");
            CHECK(rd32(sc(0xf00)) == 0); // trailing separator has no filename
            CHECK(full("C:\\one\\two\\..\\three", wide) == "C:\\one\\three");
            CHECK(full("C:\\..\\..\\", wide) == "C:\\");
        }
        const std::string content = nested ? "..\\Content\\.\\" : "Content\\.\\";
        CHECK(call_shim(tramp("kernel32.dll", "SetCurrentDirectoryA"), {input(content)}) == 1);
        call_shim(tramp("kernel32.dll", "GetCurrentDirectoryA"), {512, sc(0x500)});
        CHECK(gm_str(sc(0x500)) == RECOMP_GUEST_ROOT "\\Content");
        CHECK(win32_host_path(".") == base + "/Content");
        CHECK(call_shim(tramp("kernel32.dll", "SetCurrentDirectoryA"), {input("..")}) == 1);
        CHECK(win32_host_path(".") == base);
    }
    win32_init_layout(base + "/", "Binaries");
    CHECK(win32_game_dir() == base);
    call_shim(tramp("kernel32.dll", "GetModuleFileNameA"), {0, sc(0x500), 512});
    CHECK(gm_str(sc(0x500)) == RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE);
    os_rmdir((base + "/Binaries").c_str());
    os_rmdir((base + "/Content").c_str());
    os_rmdir(base.c_str());
    std::printf("guest layout: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
