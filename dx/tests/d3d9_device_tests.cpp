// Direct3D 9 device/shader COM contracts through the actual guest ABI.
#include "../dx.h"
#include "../d3d9_pipeline.h"
#include "../d3d9_raster.h"
#include "../host_d9.h"
#include "../d3d9_msl.h"
#include "../d3d9_glsl.h"
#include "../d3d9_wgsl.h"
#include "../../host/gpu/render_scale.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"
#include "guest_abi.h"
#include <array>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
static uint32_t factory;
static bool gpu_capture;
static uint32_t gpu_draws;
static uint32_t shader_announcements;
static bool announced_pixel;
static HostD9Draw captured;
extern "C" int host_d9_active() {
    return gpu_capture ? 1 : 0;
}
extern "C" void host_d9_draw(const HostD9Draw *draw) {
    captured = *draw;
    ++gpu_draws;
}
extern "C" void host_d9_shader_created(const uint8_t *bytes, uint32_t size, uint64_t key,
                                       int pixel) {
    ++shader_announcements;
    announced_pixel = pixel != 0;
    CHECK(bytes && size && d9sh::code_key(bytes, size) == key);
    CHECK(d9sh::program_for_key(key, bytes, size).pixel == announced_pixel);
}
static uint32_t device() {
    const uint32_t p = sc(0);
    std::memset(gm_ptr(p), 0, 56);
    wr32(p, 8);
    wr32(p + 4, 8);
    wr32(p + 8, 21);
    wr32(p + 32, 1);
    CHECK(call_method(factory, 16, {0, 1, 0x1234, 0x20, p, sc(0x100)}) == 0);
    return rd32(sc(0x100));
}
static uint32_t shader(uint32_t dev, bool pixel, uint32_t code) {
    wr32(sc(0x104), 0xdeadbeef);
    const uint32_t hr = call_method(dev, pixel ? 106 : 91, {code, sc(0x104)});
    return hr ? 0 : rd32(sc(0x104));
}
static uint32_t code(bool pixel) {
    const uint32_t words[] = {pixel ? 0xffff0300u : 0xfffe0300u, 0x02000001u,
                              pixel ? 0x800f0800u : 0xe00f0000u, 0xa0e40000u, 0x0000ffffu};
    const uint32_t address = sc(0x1000);
    std::memcpy(gm_ptr(address), words, sizeof words);
    return address;
}
static void shader_contracts() {
    const uint32_t dev = device(), other = device();
    ComObj *owner = com_this(dev);
    CHECK(call_method(dev, 9, {sc(0x200)}) == 0);
    CHECK(rd32(sc(0x200)) == 0 && rd32(sc(0x204)) == 1 && rd32(sc(0x208)) == 0x1234 &&
          rd32(sc(0x20c)) == 0x20);
    CHECK(call_method(dev, 9, {GUEST_SIZE - 8}) != 0);
    CHECK(call_method(dev, 9, {0}) != 0);
    const uint32_t vs = shader(dev, false, code(false)), ps = shader(dev, true, code(true));
    CHECK(vs && ps);
    ComObj *v = com_this(vs), *p = com_this(ps);
    const uint32_t v_id = v->id, p_id = p->id;
    const auto original = *v->d9_shader;
    std::memset(gm_ptr(sc(0x1000)), 0, 20);
    CHECK(call_method(vs, 4, {0, sc(0x300)}) == 0 && rd32(sc(0x300)) == 20);
    wr32(sc(0x300), 19);
    wr32(sc(0x400), 0xdeadbeef);
    CHECK(call_method(vs, 4, {sc(0x400), sc(0x300)}) != 0 && rd32(sc(0x400)) == 0xdeadbeef);
    CHECK(rd32(sc(0x300)) == 20);
    CHECK(call_method(vs, 4, {sc(0x400), sc(0x300)}) == 0);
    CHECK(std::memcmp(gm_ptr(sc(0x400)), original.data(), 20) == 0);
    CHECK(call_method(vs, 4, {0, 0}) != 0);
    wr32(sc(0x300), 20);
    CHECK(call_method(vs, 4, {GUEST_SIZE - 4, sc(0x300)}) != 0);
    const int before = owner->refs;
    CHECK(call_method(vs, 3, {sc(0x300)}) == 0 && rd32(sc(0x300)) == dev &&
          owner->refs == before + 1);
    call_method(dev, 2);
    CHECK(call_method(vs, 3, {GUEST_SIZE - 1}) != 0);
    CHECK(call_method(dev, 92, {vs}) == 0 && v->refs == 2);
    CHECK(call_method(dev, 92, {vs}) == 0 && v->refs == 2);
    CHECK(d9_pipeline(owner->id).vs.bytes == v->d9_shader);
    CHECK(call_method(dev, 107, {ps}) == 0 && p->refs == 2);
    CHECK(call_method(dev, 92, {ps}) != 0 && d9_pipeline(owner->id).vs_object == v_id);
    CHECK(call_method(other, 92, {vs}) != 0);
    CHECK(call_method(dev, 93, {sc(0x300)}) == 0 && rd32(sc(0x300)) == vs && v->refs == 3);
    call_method(vs, 2);
    CHECK(call_method(dev, 108, {sc(0x300)}) == 0 && rd32(sc(0x300)) == ps && p->refs == 3);
    call_method(ps, 2);
    CHECK(call_method(dev, 93, {0}) != 0);
    call_method(vs, 2);
    call_method(ps, 2);
    CHECK(com_get(v_id) && com_get(p_id));
    CHECK(call_method(dev, 92, {0}) == 0 && !com_get(v_id));
    CHECK(call_method(dev, 93, {sc(0x300)}) == 0 && rd32(sc(0x300)) == 0);
    call_method(dev, 2);
    CHECK(!com_get(p_id)); // Device teardown drops the remaining binding.
    call_method(other, 2);
}
static void lifetime() {
    const uint32_t dev = device(), vs = shader(dev, false, code(false));
    const uint32_t dev_id = com_this(dev)->id, vs_id = com_this(vs)->id;
    CHECK(call_method(dev, 92, {vs}) == 0);
    call_method(dev, 2); // The external shader reference retains its device.
    CHECK(com_get(dev_id) && com_get(vs_id));
    CHECK(call_method(vs, 3, {sc(0x300)}) == 0 && rd32(sc(0x300)) == dev);
    call_method(dev, 2);
    call_method(vs, 2); // Dropping it breaks the binding/device cycle.
    CHECK(!com_get(dev_id) && !com_get(vs_id));
}
static void shader_creation_seam() {
    const uint32_t dev = device();
    const uint32_t before = shader_announcements;
    gpu_capture = false;
    uint32_t vs = shader(dev, false, code(false));
    CHECK(vs && shader_announcements == before);
    call_method(vs, 2);
    gpu_capture = true;
    vs = shader(dev, false, code(false));
    CHECK(vs && shader_announcements == before + 1 && !announced_pixel);
    const uint32_t ps = shader(dev, true, code(true));
    CHECK(ps && shader_announcements == before + 2 && announced_pixel);
    CHECK(!shader(dev, false, code(true)) && shader_announcements == before + 2);
    call_method(vs, 2);
    call_method(ps, 2);
    call_method(dev, 2);
    gpu_capture = false;
}
static void invalid_programs() {
    const uint32_t dev = device();
    CHECK(!shader(dev, true, code(false)) && rd32(sc(0x104)) == 0);
    for (uint32_t version : {0u, 0xfffe0400u, 0xfffe0301u, 0xffff0300u}) {
        wr32(sc(0x1000), version);
        wr32(sc(0x1004), 0xffff);
        CHECK(!shader(dev, false, sc(0x1000)) && rd32(sc(0x104)) == 0);
    }
    wr32(GUEST_SIZE - 4, 0xfffe0300);
    CHECK(!shader(dev, false, GUEST_SIZE - 4));
    const uint32_t huge = heap_alloc(1u << 20, true);
    CHECK(huge);
    wr32(huge, 0xfffe0300);
    CHECK(!shader(dev, false, huge)); // Missing END, bounded at one MiB.
    heap_free(huge);
    // END-looking comment data is not the end of the program.
    const uint32_t words[] = {0xfffe0300, 0x0001fffe, 0x0000ffff, 0, 0xffff};
    std::memcpy(gm_ptr(sc(0x1000)), words, sizeof words);
    const uint32_t vs = shader(dev, false, sc(0x1000));
    CHECK(vs);
    CHECK(call_method(vs, 4, {0, sc(0x300)}) == 0 && rd32(sc(0x300)) == sizeof words);
    call_method(vs, 2);
    call_method(dev, 2);
}

static std::vector<uint8_t> words(std::initializer_list<uint32_t> data) {
    std::vector<uint8_t> bytes(data.size() * 4);
    std::memcpy(bytes.data(), data.begin(), bytes.size());
    return bytes;
}
static void shader_features() {
    std::string source, why;
    for (uint32_t index : {0u, 1u}) {
        auto bytes = words({0xffff0300, 0x02000001, 0x800f0800, 0x90e41000u + index, 0xffff});
        const auto &program = d9sh::program_for(bytes);
        CHECK(program.ok);
        CHECK(d9msl::pixel_source(program, {}, &source, &why));
        CHECK(source.find(index ? "[[front_facing]]" : "vin.pos.xy - float2(0.5)") !=
              std::string::npos);
        CHECK(!d9glsl::pixel_source(program, {}, &source, &why) &&
              why.find("miscellaneous") != std::string::npos);
        CHECK(!d9wgsl::pixel_source(program, {}, &source, &why) &&
              why.find("miscellaneous") != std::string::npos);
    }
    for (uint32_t op : {d9sh::OP_DSX, d9sh::OP_DSY}) {
        auto bytes = words({0xffff0200, 0x02000000u | op, 0x800f0000, 0xa0e40000, 0x02000001,
                            0x800f0800, 0x80e40000, 0xffff});
        const auto &program = d9sh::program_for(bytes);
        CHECK(program.ok && program.code[0].op == op);
        CHECK(d9msl::pixel_source(program, {}, &source, &why));
        CHECK(source.find(op == d9sh::OP_DSX ? "dfdx(" : "dfdy(") != std::string::npos);
        CHECK(d9glsl::pixel_source(program, {}, &source, &why));
        CHECK(source.find(op == d9sh::OP_DSX ? "dFdx(" : "dFdy(") != std::string::npos);
        CHECK(d9wgsl::pixel_source(program, {}, &source, &why));
        CHECK(source.find(op == d9sh::OP_DSX ? "dpdx(" : "dpdy(") != std::string::npos);
        auto vertex = program;
        vertex.pixel = false;
        CHECK(!d9msl::vertex_source(vertex, &source, &why));
    }
    uint32_t base = 0;
    CHECK(!d9gpu::set_render_scale_base(1, base) && base == 0);
    CHECK(!d9gpu::set_render_scale_base(63, base) && base == 0);
    CHECK(d9gpu::set_render_scale_base(64, base) && base == 64);
    CHECK(d9gpu::set_render_scale_base(480, base) && base == 480);
    CHECK(!d9gpu::set_render_scale_base(1, base) && base == 480);
    CHECK(d9gpu::set_render_scale_base(720, base) && base == 720);
}

static uint32_t buffer(uint32_t dev, const void *data, uint32_t bytes, bool index = false) {
    CHECK(call_method(dev, index ? 27 : 26, {bytes, 0, index ? 101u : 0u, 0, sc(0x800), 0}) == 0);
    const uint32_t view = rd32(sc(0x800));
    CHECK(view);
    CHECK(call_method(view, 11, {0, bytes, sc(0x804), 0}) == 0);
    std::memcpy(gm_ptr(rd32(sc(0x804))), data, bytes);
    CHECK(call_method(view, 12) == 0);
    return view;
}
static void instancing(uint32_t stream, uint32_t divisor) {
    const uint32_t dev = device(), color_stream = stream == 7 ? 6 : 7;
    const float vertices[6][4] = {{-1, 1, 0.5f, 1}, {0, 1, 0.5f, 1},  {-1, -1, 0.5f, 1},
                                  {0, 1, 0.5f, 1},  {0, -1, 0.5f, 1}, {-1, -1, 0.5f, 1}};
    const float offsets[2][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}};
    const uint32_t colors[4] = {0xffff0000, divisor == 1 ? 0xff00ff00u : 0xff0000ffu, 0xff00ff00,
                                0xffffff00};
    const uint16_t indices[6] = {0, 1, 2, 3, 4, 5};
    const uint32_t vb = buffer(dev, vertices, sizeof vertices),
                   ib = buffer(dev, indices, sizeof indices, true),
                   offsets_vb = buffer(dev, offsets, sizeof offsets),
                   colors_vb = buffer(dev, colors, sizeof colors);
    const uint8_t decl[32] = {0,
                              0,
                              0,
                              0,
                              3,
                              0,
                              0,
                              0,
                              uint8_t(stream),
                              0,
                              0,
                              0,
                              3,
                              0,
                              5,
                              0,
                              uint8_t(color_stream),
                              0,
                              0,
                              0,
                              4,
                              0,
                              10,
                              0,
                              0xff,
                              0,
                              0,
                              0,
                              17,
                              0,
                              0,
                              0};
    std::memcpy(gm_ptr(sc(0x2000)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x2000), sc(0x810)}) == 0);
    const uint32_t declaration = rd32(sc(0x810));
    CHECK(call_method(dev, 87, {declaration}) == 0);
    CHECK(call_method(dev, 100, {0, vb, 0, 16}) == 0);
    CHECK(call_method(dev, 100, {stream, offsets_vb, 0, 16}) == 0);
    CHECK(call_method(dev, 100, {color_stream, colors_vb, 0, 4}) == 0);
    CHECK(call_method(dev, 104, {ib}) == 0);
    const auto vs = words({0xfffe0200, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000005,
                           0x900f0001, 0x0200001f, 0x8000000a, 0x900f0002, 0x03000002, 0xc00f0000,
                           0x90e40000, 0x90e40001, 0x02000001, 0xd00f0000, 0x90e40002, 0xffff});
    std::memcpy(gm_ptr(sc(0x1000)), vs.data(), vs.size());
    const uint32_t vertex_shader = shader(dev, false, sc(0x1000));
    const auto ps = words({0xffff0101, 1, 0x800f0000, 0x90e40000, 0xffff});
    std::memcpy(gm_ptr(sc(0x1000)), ps.data(), ps.size());
    const uint32_t pixel_shader = shader(dev, true, sc(0x1000));
    CHECK(call_method(dev, 92, {vertex_shader}) == 0 && call_method(dev, 107, {pixel_shader}) == 0);
    call_method(vertex_shader, 2);
    call_method(pixel_shader, 2);
    CHECK(call_method(dev, 102, {0, HOST_D9_STREAM_INDEXED | 2u * divisor}) == 0);
    CHECK(call_method(dev, 102, {stream, HOST_D9_STREAM_INSTANCE | divisor}) == 0);
    CHECK(call_method(dev, 102, {color_stream, HOST_D9_STREAM_INSTANCE | 1}) == 0);
    CHECK(call_method(dev, 103, {stream, sc(0x820)}) == 0 &&
          rd32(sc(0x820)) == (HOST_D9_STREAM_INSTANCE | divisor));
    CHECK(call_method(dev, 103, {8, sc(0x820)}) != 0 && call_method(dev, 103, {0, 0}) != 0);
    for (auto invalid : {std::array<uint32_t, 2>{8, 1},
                         {0, 0},
                         {0, HOST_D9_STREAM_INSTANCE | 1},
                         {1, HOST_D9_STREAM_INDEXED | 1},
                         {1, 0xc0000001u}})
        CHECK(call_method(dev, 102, {invalid[0], invalid[1]}) != 0);
    auto &pipeline = d9_pipeline(com_this(dev)->id);
    pipeline.rs[7] = 0;
    pipeline.rs[22] = 1;
    pipeline.rs_set[22] = true;
    CHECK(call_method(dev, 43, {0, 0, 1, 0xff000000, 0x3f800000, 0}) == 0);
    CHECK(call_method(dev, 82, {4, 0, 0, 6, 0, 2}) == 0);
    CHECK(call_method(dev, 18, {0, 0, 0, sc(0x830)}) == 0);
    const uint32_t target = rd32(sc(0x830));
    D9SurfaceInfo surface;
    CHECK(d9_surface_info(com_this(target)->id, &surface));
    uint32_t left = 0, right = 0;
    std::memcpy(&left, surface.data + 2 * surface.pitch + 1 * 4, 4);
    std::memcpy(&right, surface.data + 2 * surface.pitch + 6 * 4, 4);
    CHECK((left & 0xffffff) == (divisor == 1 ? 0xff0000u : 0x0000ffu));
    CHECK((right & 0xffffff) == (divisor == 1 ? 0x00ff00u : 0xffff00u));
    const uint32_t before = gpu_draws;
    gpu_capture = true;
    CHECK(call_method(dev, 82, {4, 0, 0, 6, 0, 2}) == 0);
    gpu_capture = false;
    CHECK(gpu_draws == before + 1 && captured.instance_count == 2 * divisor);
    CHECK(captured.stream[stream].frequency == (HOST_D9_STREAM_INSTANCE | divisor));
    CHECK(captured.stream[color_stream].frequency == (HOST_D9_STREAM_INSTANCE | 1));
    CHECK(call_method(dev, 43, {0, 0, 1, 0xff000000, 0x3f800000, 0}) == 0);
    CHECK(call_method(dev, 100, {0, vb, 0, 0xffffffffu}) == 0);
    CHECK(call_method(dev, 82, {4, 0x7fffffffu, 0, 6, 0, 2}) == 0);
    std::memcpy(&left, surface.data + 2 * surface.pitch + 1 * 4, 4);
    CHECK((left & 0xffffff) == 0); // Overflowing vertex offsets cannot escape a buffer.
    for (uint32_t view : {target, dev, declaration, vb, ib, offsets_vb, colors_vb})
        call_method(view, 2);
}
int main() {
    mem_init();
    imports_init();
    dx_register_shims();
    win32_init(".");
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true);
    cpu_reset();
    factory = call_shim(tramp("d3d9.dll", "Direct3DCreate9"), {32});
    shader_contracts();
    shader_creation_seam();
    lifetime();
    invalid_programs();
    shader_features();
    for (uint32_t stream = 1; stream < 8; ++stream)
        for (uint32_t divisor : {1u, 2u})
            instancing(stream, divisor);
    call_method(factory, 2);
    std::printf("D3D9 device: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
