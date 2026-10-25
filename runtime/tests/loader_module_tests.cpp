// Real synthetic PE files through relocation, IAT binding and DLL lifetimes.
#include "../loader.h"
#include "../memory.h"
#include "../imports.h"
#include "../win32.h"
#include "../../platform/os.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static int checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s (%s)\n", __LINE__, #x, loader_error());                   \
        }                                                                                          \
    } while (0)
namespace {
constexpr uint32_t preferred = 0x10000000, a_base = 0x20000, b_base = 0x30000;
std::vector<uint32_t> visits;
bool fail_a;
std::vector<uint8_t> image(const char *dependency, uint16_t reloc = 0x3300) {
    std::vector<uint8_t> bytes(0x1200);
    auto word = [&](size_t at, uint16_t value) { std::memcpy(bytes.data() + at, &value, 2); };
    auto dword = [&](size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    word(0, 0x5a4d);
    dword(0x3c, 0x80);
    dword(0x80, 0x4550);
    word(0x84, 0x14c);
    word(0x86, 1);
    word(0x94, 0xe0);
    word(0x96, 0x2102);
    const size_t opt = 0x98, sh = opt + 0xe0;
    word(opt, 0x10b);
    dword(opt + 16, 0x1000);
    dword(opt + 28, preferred);
    dword(opt + 32, 0x1000);
    dword(opt + 36, 0x200);
    dword(opt + 56, 0x2000);
    dword(opt + 60, 0x200);
    dword(opt + 92, 16);
    dword(opt + 96, 0x1100);
    dword(opt + 100, 0x100);
    dword(opt + 96 + 5 * 8, 0x1800);
    dword(opt + 100 + 5 * 8, 12);
    std::memcpy(bytes.data() + sh, ".text", 5);
    dword(sh + 8, 0x1000);
    dword(sh + 12, 0x1000);
    dword(sh + 16, 0x1000);
    dword(sh + 20, 0x200);
    dword(sh + 36, 0x60000060);
    bytes[0x200] = 0xc3;
    // Export directory, one named function: Code -> RVA 0x1000.
    dword(0x300 + 16, 1);
    dword(0x300 + 20, 1);
    dword(0x300 + 24, 1);
    dword(0x300 + 28, 0x1140);
    dword(0x300 + 32, 0x1150);
    dword(0x300 + 36, 0x1158);
    dword(0x340, 0x1000);
    dword(0x350, 0x1160);
    word(0x358, 0);
    std::memcpy(bytes.data() + 0x360, "Code", 5);
    if (dependency) {
        dword(opt + 104, 0x1200);
        dword(opt + 108, 40);
        dword(0x400, 0x1270);
        dword(0x40c, 0x1240);
        dword(0x410, 0x1260);
        std::memcpy(bytes.data() + 0x440, dependency, std::strlen(dependency) + 1);
        dword(0x470, 0x1280);
        dword(0x460, 0x1280);
        std::memcpy(bytes.data() + 0x482, "Code", 5);
    }
    dword(0x500, preferred + 0x1000);
    dword(0xa00, 0x1000);
    dword(0xa04, 12);
    word(0xa08, reloc);
    word(0xa0a, 0);
    return bytes;
}
std::string save(const char *name, const std::vector<uint8_t> &bytes) {
    std::string path = std::string("build/recomp/loader-module-test/") + name;
    FILE *f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
        CHECK(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
        std::fclose(f);
    }
    return path;
}
bool map(const char *name, const std::string &path, uint32_t base) {
    const std::string digest = loader_hash_file(path.c_str());
    return loader_test_map_module(name, path.c_str(), digest.c_str(), base, 0x2000);
}
void dll_main(X86 *c) {
    const uint32_t base = arg(c, 0), reason = arg(c, 1);
    CHECK(arg(c, 2) == 0);
    visits.push_back(base | reason);
    if (reason == 1) {
        CHECK(rd32(base + 0x1300) == base + 0x1000);
        CHECK(rd32(base + 0x1400) == 0); // pristine globals on every fresh attach
        wr32(base + 0x1400, 42);
    } else
        CHECK(rd32(base + 0x1400) == 42);
    c->r[R_EAX] = reason == 1 && base == a_base && fail_a ? 0 : 1;
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 16;
    recomp_return(c);
}
uint32_t invoke(X86 *c, const char *name, uint32_t value) {
    return guest_call(c, imports_resolve("kernel32.dll", name), value);
}
} // namespace
int main() {
    mem_init();
    imports_init();
    win32_init(".");
    os_mkdir("build/recomp");
    os_mkdir("build/recomp/loader-module-test");
    const std::string a = save("a.dll", image("b.dll")), b = save("b.dll", image(nullptr));
    const std::string bad = save("bad.dll", image(nullptr, 0x1300));
    loader_test_reset_modules();
    CHECK(!map("bad.dll", bad, a_base));
    CHECK(std::strstr(loader_error(), "relocation") != nullptr);
    CHECK(loader_module_count() == 0);
    CHECK(rd32(a_base + 0x1300) == preferred + 0x1000); // invalid relocation wrote no site
    CHECK(!map("a.dll", a, GUEST_SHIM_BASE));
    CHECK(!map("a.dll", a, GUEST_HEAP_BASE));
    CHECK(!map("a.dll", a, GUEST_STACK_TOP - 0x10000));
    if (uint64_t(GUEST_LAYOUT_END) + 0x2000 <= GUEST_SIZE) {
        CHECK(map("high.dll", b, GUEST_LAYOUT_END));
        CHECK(rd32(GUEST_LAYOUT_END + 0x1300) == GUEST_LAYOUT_END + 0x1000);
        CHECK(!map("overlap.dll", b, GUEST_LAYOUT_END));
        loader_test_reset_modules();
    }
    CHECK(map("a.dll", a, a_base)); // map importer first, bind only after every DLL exists
    CHECK(map("b.dll", b, b_base));
    CHECK(loader_test_bind_modules());
    CHECK(rd32(a_base + 0x1260) == b_base + 0x1000);
    CHECK(rd32(a_base + 0x1300) == a_base + 0x1000);
    CHECK(rd32(a_base + 0x98 + 28) == a_base);
    LoaderModule *am = loader_module_named("a.dll"), *bm = loader_module_named("b.dll");
    CHECK(am && bm && am->dependencies == std::vector<std::string>{"b.dll"});
    static const uint32_t addresses[] = {a_base + 0x1000, b_base + 0x1000};
    static void (*const functions[])(X86 *) = {dll_main, dll_main};
    static RecompHookFn hooks[2]{};
    static uint8_t hooked[2]{};
    static const RecompModule module = {"loader-test", a_base, b_base + 0x2000, addresses, 2,
                                        functions,     hooks,  hooked,          nullptr,   0,
                                        nullptr};
    recomp_module_register(&module);
    X86 c;
    loader_init_context(&c);
    const uint32_t s = heap_alloc(512, true);
    gm_put_str(s, "a.dll", 32);
    CHECK(invoke(&c, "LoadLibraryA", s) == a_base);
    CHECK(visits == (std::vector<uint32_t>{b_base | 1, a_base | 1}));
    CHECK(invoke(&c, "LoadLibraryA", s) == a_base);
    CHECK(am->load_refs == 2 && bm->load_refs == 1 && visits.size() == 2);
    CHECK(invoke(&c, "FreeLibrary", a_base) == 1 && visits.size() == 2);
    CHECK(invoke(&c, "FreeLibrary", a_base) == 1);
    CHECK(visits == (std::vector<uint32_t>{b_base | 1, a_base | 1, a_base, b_base}));
    CHECK(!am->attached && !bm->attached);
    // Cyclic imports do not recurse forever or retain an extra reference.
    bm->dependencies = {"a.dll"};
    visits.clear();
    CHECK(invoke(&c, "LoadLibraryA", s) == a_base);
    CHECK(am->load_refs == 1 && bm->load_refs == 1);
    CHECK(invoke(&c, "FreeLibrary", a_base) == 1);
    CHECK(!am->attached && !bm->attached);
    bm->dependencies.clear();
    visits.clear();
    fail_a = true;
    CHECK(invoke(&c, "LoadLibraryA", s) == 0 && get_last_error() == 1114);
    CHECK(visits == (std::vector<uint32_t>{b_base | 1, a_base | 1, a_base, b_base}));
    CHECK(!am->attached && !bm->attached && am->load_refs == 0 && bm->load_refs == 0);
    // Static EXE imports follow the same transaction, and pin the complete graph.
    auto &main = loader_test_main_module();
    main.name = RECOMP_EXECUTABLE;
    main.base = IMAGE_BASE;
    main.dependencies = {"a.dll"};
    main.attached = true;
    visits.clear();
    CHECK(!win32_attach_static_modules(&c));
    CHECK(visits == (std::vector<uint32_t>{b_base | 1, a_base | 1, a_base, b_base}));
    visits.clear();
    fail_a = false;
    CHECK(win32_attach_static_modules(&c));
    CHECK(visits == (std::vector<uint32_t>{b_base | 1, a_base | 1}));
    CHECK(am->pinned && bm->pinned);
    CHECK(invoke(&c, "FreeLibrary", a_base) == 1 && am->attached && bm->attached);
    guest_call(&c, imports_resolve("kernel32.dll", "GetModuleFileNameA"), a_base, s + 32, 256);
    CHECK(gm_str(s + 32).find("loader-module-test\\a.dll") != std::string::npos);
    os_unlink(a.c_str());
    os_unlink(b.c_str());
    os_unlink(bad.c_str());
    os_rmdir("build/recomp/loader-module-test");
    std::printf("loader modules: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
