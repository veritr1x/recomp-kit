// Exercise the published Direct3D 8 slots through guest trampolines, including
// their stack cleanup and the pixels produced by the common CPU renderer.
#include "../dx.h"
#include "../d3d9_pipeline.h"
#include "../../runtime/memory.h"
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

static uint32_t words(uint32_t off, std::initializer_list<uint32_t> values) {
    uint32_t p = sc(off);
    for (uint32_t value : values) {
        wr32(p, value);
        p += 4;
    }
    return sc(off);
}
static uint32_t pixel(uint32_t surface) {
    ComObj *o = com_this(surface);
    uint32_t color = 0;
    if (o && o->blob.size() >= 3 * o->pitch)
        std::memcpy(&color, o->blob.data() + 2 * o->pitch + 8, 4);
    return color;
}
static void test_interfaces() {
    const uint32_t factory = call_shim(tramp("d3d8.dll", "Direct3DCreate8"), {220});
    CHECK(factory && com_iface_of(factory) == IF_D3D8);
    CHECK(call_method(factory, 4) == 1);
    // CAPS8 is exactly 212 bytes. The suffix must survive untouched.
    std::memset(gm_ptr(sc(0x100)), 0xa5, 320);
    CHECK(call_method(factory, 13, {0, 1, sc(0x100)}) == 0);
    CHECK(rd32(sc(0x100 + 196)) == 0xfffe0101u);
    CHECK(rd32(sc(0x100 + 204)) == 0xffff0101u);
    CHECK(rd32(sc(0x100 + 212)) == 0xa5a5a5a5u);
    // Identifier8 lacks the DeviceName field present in Identifier9.
    std::memset(gm_ptr(sc(0x300)), 0xa5, 1080);
    CHECK(call_method(factory, 5, {0, 0, sc(0x300)}) == 0);
    CHECK(rd32(sc(0x300 + 1068)) == 0xa5a5a5a5u);
    CHECK(call_method(factory, 7, {0, 0, sc(0x800)}) == 0);
    CHECK(call_method(factory, 7, {0, 1, sc(0x800)}) != 0);
    CHECK(call_method(factory, 10, {0, 1, 22, 0, 4, 22}) != 0); // no volumes
    CHECK(call_method(factory, 10, {0, 1, 22, 0, 3, 0xdead}) != 0);
    words(0x800, {8, 8, 21, 1, 0, 1, 0, 1, 0, 75, 0, 0, 0});
    CHECK(call_method(factory, 15, {0, 1, 0, 0x20, sc(0x800), sc(0x900)}) == 0);
    uint32_t dev = rd32(sc(0x900));
    CHECK(com_iface_of(dev) == IF_D3DDEVICE8);
    CHECK(call_method(dev, 6, {sc(0x900)}) == 0 && rd32(sc(0x900)) == factory);
    call_method(factory, 2);
    CHECK(call_method(dev, 7, {sc(0x100)}) == 0);
    CHECK(call_method(dev, 9, {sc(0x900)}) == 0 && rd32(sc(0x90c)) == 0x20);
    CHECK(call_method(dev, 16, {0, 0, sc(0x900)}) == 0);
    uint32_t target = rd32(sc(0x900));
    CHECK(com_iface_of(target) == IF_D3DSURFACE8);
    CHECK(call_method(target, 8, {sc(0x900)}) == 0);
    CHECK(rd32(sc(0x900)) == 21 && rd32(sc(0x904)) == 1);
    CHECK(rd32(sc(0x910)) == 256 && rd32(sc(0x918)) == 8);
    CHECK(rd32(sc(0x914)) == 0); // D3DMULTISAMPLE_NONE is zero, not sample count one
    CHECK(call_method(target, 3, {sc(0x900)}) == 0 && rd32(sc(0x900)) == dev);
    call_method(dev, 2);
    CHECK(call_method(dev, 20, {4, 4, 2, 0, 21, 1, sc(0x900)}) == 0);
    uint32_t tex = rd32(sc(0x900));
    CHECK(com_iface_of(tex) == IF_D3DTEXTURE8);
    CHECK(call_method(tex, 14, {1, sc(0x900)}) == 0 && rd32(sc(0x918)) == 2);
    CHECK(call_method(tex, 15, {1, sc(0x900)}) == 0);
    uint32_t level = rd32(sc(0x900));
    CHECK(com_iface_of(level) == IF_D3DSURFACE8);
    const uint8_t tex_iid[] = {0x75, 0xd5, 0xcd, 0xe4, 0x66, 0x28, 0x01, 0x4f,
                               0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58};
    std::memcpy(gm_ptr(sc(0x950)), tex_iid, 16);
    CHECK(call_method(level, 7, {sc(0x950), sc(0x900)}) == 0 && rd32(sc(0x900)) == tex);
    call_method(tex, 2);
    words(0x980, {1, 1, 2, 2});
    CHECK(call_method(level, 9, {sc(0x900), sc(0x980), 0}) == 0);
    const uint32_t bits = rd32(sc(0x904));
    ComObj *level_obj = com_this(level);
    CHECK(bits == level_obj->pixels + level_obj->pitch + 4);
    wr32(bits, 0xff123456);
    CHECK(call_method(level, 10) == 0);
    uint32_t stored = 0;
    std::memcpy(&stored, level_obj->blob.data() + level_obj->pitch + 4, 4);
    CHECK(stored == 0xff123456);
    CHECK(com_view(com_this(dev), IF_D3DDEVICE9) == 0);
    CHECK(call_method(dev, 13, {sc(0x800), sc(0x900)}) != 0); // named unsupported slot

    CHECK(call_method(dev, 23, {60, 0, 0x44, 1, sc(0x900)}) == 0);
    uint32_t vb = rd32(sc(0x900));
    CHECK(com_iface_of(vb) == IF_D3DVERTEXBUFFER8);
    CHECK(call_method(vb, 13, {sc(0x900)}) == 0 && rd32(sc(0x914)) == 0x44);
    CHECK(call_method(dev, 83, {0, vb, 20}) == 0);
    CHECK(call_method(dev, 84, {0, sc(0x900), sc(0x904)}) == 0);
    CHECK(rd32(sc(0x900)) == vb && rd32(sc(0x904)) == 20);
    call_method(vb, 2);
    CHECK(call_method(dev, 24, {6, 0, 101, 1, sc(0x900)}) == 0);
    uint32_t ib = rd32(sc(0x900));
    CHECK(com_iface_of(ib) == IF_D3DINDEXBUFFER8);
    CHECK(call_method(dev, 85, {ib, 7}) == 0);
    CHECK(call_method(dev, 86, {sc(0x900), sc(0x904)}) == 0 && rd32(sc(0x904)) == 7);
    call_method(ib, 2);
    CHECK(call_method(vb, 11, {59, 2, sc(0x900), 0}) != 0);
    CHECK(call_method(dev, 63, {0, 13, 3}) == 0); // ADDRESSU moves to sampler1
    CHECK(d9_pipeline(com_this(dev)->id).sampler_state[0][1] == 3);
    CHECK(call_method(dev, 62, {0, 13, sc(0x900)}) == 0 && rd32(sc(0x900)) == 3);

    struct Vertex {
        float x, y, z, w;
        uint32_t color;
    } vertices[] = {
        {0, 0, 0.5f, 1, 0xffff0000}, {8, 0, 0.5f, 1, 0xffff0000}, {0, 8, 0.5f, 1, 0xffff0000}};
    std::memcpy(gm_ptr(sc(0xa00)), vertices, sizeof vertices);
    CHECK(call_method(dev, 76, {0x44}) == 0);    // SetVertexShader(FVF)
    CHECK(call_method(dev, 50, {22, 1}) == 0);   // no cull
    CHECK(call_method(dev, 63, {0, 1, 1}) == 0); // combiner disabled: diffuse colour
    CHECK(call_method(dev, 72, {4, 1, sc(0xa00), 20}) == 0);
    CHECK((pixel(target) & 0xffffff) == 0xff0000);
    CHECK(d9_pipeline(com_this(dev)->id).stream[0].vb == 0);

    // ps_1_1 DEF loads c0 on binding. An API write can then override it;
    // binding the same handle again must restore the declared value.
    uint32_t ps = words(0xb00, {0xffff0101u, 81, 0xa00f0000u, 0x3f800000u, 0, 0, 0x3f800000u, 1,
                                0x800f0000u, 0xa0e40000u, 0xffffu});
    CHECK(call_method(dev, 87, {ps, sc(0x900)}) == 0);
    uint32_t handle = rd32(sc(0x900));
    CHECK(call_method(dev, 88, {handle}) == 0);
    words(0xc00, {0, 0x3f800000u, 0, 0x3f800000u});
    CHECK(call_method(dev, 91, {0, sc(0xc00), 1}) == 0);
    CHECK(call_method(dev, 72, {4, 1, sc(0xa00), 20}) == 0);
    CHECK((pixel(target) & 0xffffff) == 0x00ff00);
    CHECK(call_method(dev, 88, {handle}) == 0);
    CHECK(call_method(dev, 72, {4, 1, sc(0xa00), 20}) == 0);
    CHECK((pixel(target) & 0xffffff) == 0xff0000);
    CHECK(call_method(dev, 90, {handle}) == 0);
    CHECK(call_method(dev, 88, {handle}) != 0);
    CHECK(call_method(dev, 72, {4, 1, sc(0xa00), 20}) == 0); // bound storage survives deletion

    // Inject semantics for a programmable vs_1_1 whose colour is register v5.
    uint32_t declaration = words(0xd00, {0x20000000, 0x40030000, 0x40040005, 0xffffffff});
    uint32_t vs =
        words(0xd40, {0xfffe0101, 1, 0xc00f0000, 0x90e40000, 1, 0xd00f0000, 0x90e40005, 0xffff});
    CHECK(call_method(dev, 75, {declaration, vs, sc(0x900), 0}) == 0);
    uint32_t vertex_handle = rd32(sc(0x900));
    CHECK(call_method(dev, 76, {vertex_handle}) == 0);
    CHECK(call_method(dev, 88, {0}) == 0);
    vertices[0].x = vertices[0].y = -1;
    vertices[1].x = 1;
    vertices[1].y = -1;
    vertices[2].x = -1;
    vertices[2].y = 1;
    std::memcpy(gm_ptr(sc(0xa00)), vertices, sizeof vertices);
    CHECK(call_method(dev, 72, {4, 1, sc(0xa00), 20}) == 0);
    CHECK((pixel(target) & 0xffffff) == 0xff0000);
    CHECK(call_method(dev, 78, {vertex_handle}) == 0);

    // A level retains its container only while externally referenced. Repeated
    // getters must not leak one parent reference on every call.
    CHECK(call_method(dev, 20, {2, 2, 1, 0, 21, 1, sc(0x900)}) == 0);
    uint32_t owned = rd32(sc(0x900));
    ComObj *owned_obj = com_this(owned);
    const uint32_t owned_id = owned_obj->id;
    CHECK(call_method(owned, 15, {0, sc(0x900)}) == 0);
    uint32_t owned_level = rd32(sc(0x900));
    CHECK(call_method(owned, 15, {0, sc(0x900)}) == 0 && rd32(sc(0x900)) == owned_level);
    CHECK(owned_obj->refs == 2);
    call_method(owned, 2);
    call_method(owned_level, 2);
    CHECK(com_get(owned_id) != nullptr);
    call_method(owned_level, 2);
    CHECK(com_get(owned_id) == nullptr);

    call_method(level, 2);
    call_method(tex, 2);
    call_method(target, 2);
    call_method(vb, 2);
    call_method(ib, 2);
    call_method(dev, 2);
    call_method(factory, 2);
    // A version 9 factory still writes the full CAPS9 and cannot yield an 8 view.
    uint32_t nine = call_shim(tramp("d3d9.dll", "Direct3DCreate9"), {32});
    CHECK(com_iface_of(nine) == IF_D3D9 && !com_view(com_this(nine), IF_D3D8));
    std::memset(gm_ptr(sc(0x100)), 0xa5, 320);
    CHECK(call_method(nine, 14, {0, 1, sc(0x100)}) == 0);
    CHECK(rd32(sc(0x100 + 296)) == 32768 && rd32(sc(0x100 + 304)) == 0xa5a5a5a5u);
    call_method(nine, 2);
}
int main() {
    mem_init();
    imports_init();
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    cpu_reset();
    test_interfaces();
    std::printf("d3d8: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
