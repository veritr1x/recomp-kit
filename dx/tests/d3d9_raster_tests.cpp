// Direct3D 9 CPU raster rules checked against small, hand-computed pixels.
#include "../dx.h"
#include "../d3d8_program.h"
#include "../d3d9_raster.h"
#include "../../runtime/memory.h"
#include "guest_abi.h"
#include <cmath>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
namespace {
struct Vertex {
    float x, y, z, w;
    uint32_t color;
    float u, v;
};
struct Fixture {
    uint32_t factory = 0, view = 0, target_view = 0;
    ComObj *device = nullptr, *target = nullptr;
    d8::Layout layout;
    Vertex vertices[6] = {};
    Fixture() {
        factory = call_shim(tramp("d3d8.dll", "Direct3DCreate8"), {220});
        const uint32_t p = sc(0);
        const uint32_t present[] = {8, 8, 21, 1, 0, 1, 0, 1, 1, 75, 0, 0, 0};
        std::memcpy(gm_ptr(p), present, sizeof present);
        CHECK(call_method(factory, 15, {0, 1, 0, 0x20, p, sc(0x80)}) == 0);
        view = rd32(sc(0x80));
        device = com_this(view);
        CHECK(call_method(view, 16, {0, 0, sc(0x80)}) == 0);
        target_view = rd32(sc(0x80));
        target = com_this(target_view);
        std::string why;
        CHECK(d8::fvf_layout(0x144, &layout, &why));
        state().rs[22] = 1;
        state().rs_set[22] = true;
        state().rs[7] = 0;
        quad(0xffff0000, 0.5f);
        clear(0, 3, 1);
    }
    ~Fixture() {
        call_method(target_view, 2);
        call_method(view, 2);
        call_method(factory, 2);
    }
    D9Pipeline &state() {
        return d9_pipeline(device->id);
    }
    void rs(uint32_t name, uint32_t value) {
        state().rs[name] = value;
        state().rs_set[name] = true;
    }
    void quad(uint32_t color, float z) {
        const float xy[6][2] = {{0, 0}, {8, 0}, {0, 8}, {8, 0}, {8, 8}, {0, 8}};
        for (int i = 0; i < 6; ++i)
            vertices[i] = {xy[i][0], xy[i][1], z, 1, color, 0.5f, 0.5f};
    }
    void clear(uint32_t color, uint32_t flags = 1, float z = 1, uint32_t stencil = 0) {
        uint32_t bits;
        std::memcpy(&bits, &z, 4);
        CHECK(call_method(view, 36, {0, 0, flags, color, bits, stencil}) == 0);
    }
    void draw(uint32_t primitives = 2) {
        d8::FixedPrograms code;
        std::string why;
        CHECK(d8::fixed_programs(layout, state(), 8, 8, &code, &why));
        state().vs.bytes = std::make_shared<const std::vector<uint8_t>>(code.vertex);
        state().ps.bytes = std::make_shared<const std::vector<uint8_t>>(code.pixel);
        std::memcpy(state().vconst, code.vertex_constants, sizeof code.vertex_constants);
        std::memcpy(state().pconst, code.pixel_constants, sizeof code.pixel_constants);
        D9DrawCall call;
        call.prim = 4;
        call.prim_count = primitives;
        call.stride = sizeof(Vertex);
        call.vertex_data = reinterpret_cast<const uint8_t *>(vertices);
        call.vertex_bytes = sizeof vertices;
        d9_raster_draw(device, target, layout.declaration, call);
    }
    uint32_t pixel(int x, int y) {
        uint32_t value = 0;
        std::memcpy(&value, target->blob.data() + y * target->pitch + x * 4, 4);
        return value;
    }
};
void fill_and_clip() {
    Fixture f;
    f.vertices[0] = {-0.25f, -0.25f, 0.5f, 1, 0xffff0000, 0, 0};
    f.vertices[1] = {0.75f, -0.25f, 0.5f, 1, 0xffff0000, 0, 0};
    f.vertices[2] = {-0.25f, 0.75f, 0.5f, 1, 0xffff0000, 0, 0};
    f.draw(1);
    CHECK((f.pixel(0, 0) & 0xffffff) == 0xff0000);
    f.clear(0);
    f.quad(0xff400000, 0.5f);
    f.rs(27, 1);
    f.rs(19, 2);
    f.rs(20, 2);
    f.draw();
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            CHECK((f.pixel(x, y) & 0xffffff) == 0x400000);
    f.rs(27, 0);
    f.clear(0);
    f.state().viewport_set = true;
    f.state().viewport[0] = f.state().viewport[1] = 2;
    f.state().viewport[2] = f.state().viewport[3] = 4;
    f.draw();
    CHECK(f.pixel(1, 3) == 0 && f.pixel(6, 3) == 0);
    CHECK((f.pixel(2, 3) & 0xffffff) == 0x400000);
}
void depth() {
    Fixture f;
    f.rs(7, 1);
    f.rs(23, 2); // LESS
    f.quad(0xff00ff00, 0.75f);
    f.draw();
    f.quad(0xffff0000, 0.25f);
    f.draw();
    f.quad(0xff0000ff, 0.8f);
    f.draw();
    CHECK((f.pixel(2, 2) & 0xffffff) == 0xff0000);
    f.clear(0, 3, 0.5f);
    f.quad(0xffff0000, 0.75f);
    f.draw();
    CHECK(f.pixel(2, 2) == 0);
    f.rs(14, 0);
    f.quad(0xffff0000, 0.25f);
    f.draw();
    f.quad(0xff00ff00, 0.4f);
    f.draw();
    CHECK((f.pixel(2, 2) & 0xffffff) == 0x00ff00); // first draw did not write depth
    f.rs(23, 8);
    f.quad(0xff0000ff, 0.8f);
    f.draw();
    CHECK((f.pixel(2, 2) & 0xffffff) == 0x0000ff);
    f.clear(0, 6, 0.5f, 0x5a);
    ComObj *z = com_get(f.device->zbuffer_obj);
    uint32_t raw;
    std::memcpy(&raw, z->blob.data(), 4);
    CHECK(raw == 0x8000005a);
    f.clear(0, 2, 1, 0);
    std::memcpy(&raw, z->blob.data(), 4);
    CHECK(raw == 0xffffff5a);
    f.rs(14, 0);
    const bool passes[] = {false, false, true, true, false, false, true, true};
    for (uint32_t cmp = 1; cmp <= 8; ++cmp) {
        f.clear(0, 3, 0.5f);
        f.rs(23, cmp);
        f.quad(0xffff0000, 0.5f);
        f.draw();
        CHECK((f.pixel(2, 2) != 0) == passes[cmp - 1]);
    }
    z->rmask = 80; // D16: comparison must quantize incoming depth to storage precision.
    z->pitch = 16;
    z->blob.resize(128);
    f.clear(0, 3, 0.5f);
    f.rs(23, 3);
    f.quad(0xffff0000, 0.5f);
    f.draw();
    CHECK((f.pixel(2, 2) & 0xffffff) == 0xff0000);
    uint16_t raw16;
    std::memcpy(&raw16, z->blob.data(), 2);
    CHECK(raw16 == 32768);
}
void alpha_and_blend() {
    Fixture f;
    f.quad(0x80ff0000, 0.5f);
    f.rs(15, 1);
    f.rs(24, 128);
    const bool passes[] = {false, false, true, true, false, false, true, true};
    for (uint32_t cmp = 1; cmp <= 8; ++cmp) {
        f.clear(0);
        f.rs(25, cmp);
        f.draw();
        CHECK((f.pixel(2, 2) != 0) == passes[cmp - 1]);
    }
    f.rs(15, 0);
    f.rs(27, 1);
    f.quad(0x40c86432, 0.5f);
    struct Blend {
        uint32_t factor, expected;
    } cases[] = {
        {11, 0x4032190d}, {12, 0x70413739}, {13, 0x509b5535}, {14, 0x20323226}, {15, 0x2096320c}};
    f.rs(193, 0x804080c0);
    for (const auto &c : cases) {
        f.clear(0x8014283c);
        f.rs(19, c.factor);
        f.rs(20, 1);
        f.draw();
        CHECK(f.pixel(2, 2) == c.expected);
    }
    f.rs(19, 2);
    f.rs(20, 2);
    const uint32_t expected[] = {0xc0dc8c6e, 0x00b43c00, 0x4000000a, 0x40142832, 0x80c8643c};
    for (uint32_t op = 1; op <= 5; ++op) {
        f.clear(0x8014283c);
        f.rs(171, op);
        f.draw();
        CHECK(f.pixel(2, 2) == expected[op - 1]);
    }
    f.rs(27, 0);
    f.rs(168, 1);
    f.clear(0x8014283c);
    f.draw();
    CHECK(f.pixel(2, 2) == 0x80c8283c);
}
void addressing() {
    Fixture f;
    ComObj *tex = com_new(K_D3D9SURFACE);
    tex->width = 2;
    tex->height = 1;
    tex->pitch = 8;
    tex->rmask = 21;
    tex->blob = {0, 0, 255, 255, 0, 255, 0, 255};
    f.state().sampler_tex[0] = tex->id;
    auto &s = f.state().sampler_state[0];
    f.state().tss[0][1] = f.state().tss[0][4] = 2;
    auto at = [&](float u) {
        for (auto &v : f.vertices) {
            v.u = u;
            v.v = 0.5f;
        }
        f.draw();
        return f.pixel(2, 2);
    };
    s[1] = s[2] = 3;
    s[5] = 1;
    CHECK(at(0.5f) == 0xff00ff00); // point sampling chooses the green texel
    s[5] = 2;
    CHECK(at(0.5f) == 0xff808000);
    CHECK(at(0) == 0xffff0000 && at(1) == 0xff00ff00); // clamp at the edge, including bilinear taps
    s[5] = 1;
    s[1] = 5;
    CHECK(at(-0.75f) == 0xff00ff00);
    s[1] = 2;
    CHECK(at(1.25f) == 0xff00ff00);
    s[1] = 1;
    CHECK(at(1.25f) == 0xffff0000);
    s[1] = 4;
    s[4] = 0xff123456;
    CHECK(at(1.5f) == 0xff123456);
    s[5] = 2;
    CHECK(at(0) == 0xff891a2b);
    com_release(tex);
}
} // namespace
int main() {
    mem_init();
    imports_init();
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    cpu_reset();
    fill_and_clip();
    depth();
    alpha_and_blend();
    addressing();
    std::printf("d3d9 CPU raster: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
