// Hardware probing through the guest ABI, without loading a game image.
#include "../dx.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"
#include "../../platform/os.h"
#include "game_config.h"
#include "guest_abi.h"
#include <algorithm>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)

int main() {
    mem_init();
    imports_init();
    dx_register_shims();
    win32_init(".");
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    cpu_reset();
    os_unsetenv("SMOKE_DRAWABLE");
    const uint32_t factory = call_shim(tramp("d3d9.dll", "Direct3DCreate9"), {32});
    CHECK(factory != 0);
    CHECK(call_method(factory, 6, {0, 22}) == 17);
    CHECK(call_method(factory, 6, {0, 21}) == 0);
    CHECK(call_method(factory, 6, {1, 22}) == 0);
    for (uint32_t i = 0; i < 17; ++i) {
        CHECK(call_method(factory, 7, {0, 22, i, sc(0)}) == 0);
        CHECK(rd32(sc(0)) >= 640 && rd32(sc(4)) >= 480);
        CHECK(rd32(sc(8)) == 60 && rd32(sc(12)) == 22);
    }
    CHECK(rd32(sc(0)) == 2560 && rd32(sc(4)) == 1600);
    CHECK(call_method(factory, 7, {0, 22, 17, sc(0)}) != 0);
    CHECK(call_method(factory, 7, {1, 22, 0, sc(0)}) != 0);
    CHECK(call_method(factory, 7, {0, 21, 0, sc(0)}) != 0);
    CHECK(call_method(factory, 7, {0, 22, 0, 0}) != 0);
    CHECK(call_method(factory, 8, {0, sc(0)}) == 0);
    CHECK(rd32(sc(0)) == RECOMP_DESKTOP_WIDTH && rd32(sc(4)) == RECOMP_DESKTOP_HEIGHT);
    CHECK(call_shim(tramp("user32.dll", "GetSystemMetrics"), {0}) == RECOMP_DESKTOP_WIDTH);
    CHECK(call_shim(tramp("user32.dll", "GetSystemMetrics"), {1}) == RECOMP_DESKTOP_HEIGHT);
    CHECK(call_method(factory, 5, {0, 0, sc(0)}) == 0);
    CHECK(gm_str(sc(512)) == RECOMP_ADAPTER_DESCRIPTION);
    CHECK(rd32(sc(1064)) == RECOMP_ADAPTER_VENDOR_ID);
    CHECK(rd32(sc(1068)) == RECOMP_ADAPTER_DEVICE_ID);
    CHECK(call_method(factory, 5, {1, 0, sc(0)}) != 0);
    CHECK(call_method(factory, 5, {0, 0, GUEST_SIZE - 8}) != 0);

    std::memset(gm_ptr(sc(0)), 0, 56);
    wr32(sc(0), 8);
    wr32(sc(4), 8);
    wr32(sc(8), 21);
    wr32(sc(20), 1);
    wr32(sc(24), 0x10001);
    wr32(sc(28), 1);
    CHECK(call_method(factory, 16, {0, 1, 0x10001, 0x20, sc(0), sc(0x800)}) == 0);
    const uint32_t device = rd32(sc(0x800));
    CHECK(call_method(device, 4) == std::max(256u, RECOMP_ADAPTER_VIDEO_MEMORY_MB) * 1024u * 1024u);
    call_method(device, 2);
    call_method(factory, 2);

    uint32_t dd = 0;
    CHECK(call_shim(tramp("ddraw.dll", "DirectDrawCreate"), {0, sc(0x800), 0}) == 0);
    dd = rd32(sc(0x800));
    // QueryInterface for IDirectDraw2, whose GetAvailableVidMem is slot 23.
    const uint8_t iid[] = {0xe0, 0xf3, 0xa6, 0xb3, 0x43, 0x2b, 0xcf, 0x11,
                           0xa2, 0xde, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56};
    std::memcpy(gm_ptr(sc(0x900)), iid, 16);
    CHECK(call_method(dd, 0, {sc(0x900), sc(0x904 + 16)}) == 0);
    uint32_t dd2 = rd32(sc(0x914));
    CHECK(call_method(dd2, 23, {0, sc(0), sc(4)}) == 0);
    CHECK(rd32(sc(0)) == RECOMP_ADAPTER_VIDEO_MEMORY_MB * 1024u * 1024u);
    CHECK(rd32(sc(4)) == uint64_t(RECOMP_ADAPTER_VIDEO_MEMORY_MB) * 1024 * 1024 * 3 / 4);
    call_method(dd2, 2);
    call_method(dd, 2);

    const uint64_t total = uint64_t(RECOMP_MEMORY_MB) * 1024 * 1024;
    call_shim(tramp("kernel32.dll", "GlobalMemoryStatus"), {sc(0)});
    CHECK(rd32(sc(8)) == std::min(total, uint64_t(UINT32_MAX)));
    CHECK(rd32(sc(12)) == std::min(total * 3 / 4, uint64_t(UINT32_MAX)));
    wr32(sc(0), 64);
    CHECK(call_shim(tramp("kernel32.dll", "GlobalMemoryStatusEx"), {sc(0)}) == 1);
    auto read64 = [](uint32_t p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; };
    CHECK(read64(sc(8)) == total && read64(sc(16)) == total * 3 / 4);
    CHECK(read64(sc(24)) == total * 2 && read64(sc(32)) == total * 3 / 2);
    wr32(sc(0), 32);
    CHECK(call_shim(tramp("kernel32.dll", "GlobalMemoryStatusEx"), {sc(0)}) == 0);
    std::printf("hardware: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
