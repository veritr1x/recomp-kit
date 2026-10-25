#include "../d3d8_program.h"
#include "../d3d9_shader.h"
#include "../d3d9_raster.h"
#include "../com.h"
#include "fixtures/d3d8_cases.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace {
int checks = 0, failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)

void fvf() {
    d8::Layout layout;
    std::string why;
    CHECK(d8::fvf_layout(0x1c4, &layout, &why)); // XYZRHW, diffuse, specular, texture0
    CHECK(layout.transformed && layout.stride[0] == 32 && layout.attributes.size() == 4);
    CHECK(layout.attributes[1].type == 4 && layout.attributes[1].reg == 5);
    CHECK(layout.attributes[2].usage == 10 && layout.attributes[2].usage_index == 1);
    CHECK(layout.attributes[3].usage == 5 && layout.attributes[3].offset == 24);
    CHECK(layout.declaration.size() == 40 && layout.declaration[32] == 0xff);
    CHECK(d8::fvf_layout(0x212 | (3u << 16) | (2u << 18), &layout, &why));
    CHECK(!layout.transformed && layout.stride[0] == 44); // XYZ, normal, float1, float4
    CHECK(layout.attributes[2].type == 0 && layout.attributes[3].type == 3);
    const auto saved = layout.declaration;
    for (uint32_t invalid : {0u, 1u, 6u, 0x14u, 0x902u, 0x1002u}) {
        CHECK(!d8::fvf_layout(invalid, &layout, &why));
        CHECK(layout.declaration == saved && !why.empty());
    }
}

void declarations() {
    d8::Layout layout;
    std::vector<uint32_t> defs;
    std::string why;
    const std::vector<uint32_t> tokens = {0x20000000u, 0x40020000u, 0x50010000u, 0x40040005u,
                                          0x20000001u, 0x40010007u, 0x82000002u, 0x3f800000u,
                                          0,           0,           0x3f800000u, 0xffffffffu};
    CHECK(d8::token_layout(tokens, &layout, &defs, &why));
    CHECK(layout.attributes.size() == 3 && layout.stride[0] == 20 && layout.stride[1] == 8);
    CHECK(layout.attributes[1].offset == 16 && layout.attributes[1].reg == 5);
    CHECK(layout.attributes[2].stream == 1 && layout.attributes[2].usage_index == 0);
    CHECK(defs.size() == 6 && defs[0] == d9sh::OP_DEF && defs[1] == 0xa00f0002u);
    const auto saved = layout.declaration;
    for (const std::vector<uint32_t> &invalid :
         {std::vector<uint32_t>{0xffffffffu},
          {0x40020000u, 0xffffffffu},
          {0x20000008u, 0x40020000u, 0xffffffffu},
          {0x20000000u, 0x40020000u, 0x40020000u, 0xffffffffu},
          {0x20000000u, 0x40080000u, 0xffffffffu},
          {0x20000000u, 0x40020000u},
          {0x82000002u, 0xffffffffu},
          {0x20000000u, 0x40020000u, 0xffffffffu, 0}}) {
        CHECK(!d8::token_layout(invalid, &layout, &defs, &why));
        CHECK(layout.declaration == saved && !why.empty());
    }
    std::vector<uint8_t> original;
    for (uint32_t value : {0xfffe0101u, 1u, 0xc00f0000u, 0x90e40000u, 0xffffu})
        for (int k = 0; k < 4; ++k)
            original.push_back(uint8_t(value >> (8 * k)));
    d8::PreparedShader converted;
    CHECK(d8::vertex_shader(layout, original, {}, &converted, &why));
    const auto &program = d9sh::program_for(converted.code);
    CHECK(program.ok && program.inputs.size() == 3);
    CHECK(program.inputs[1].index == 5 && program.inputs[1].usage == 10);
    CHECK(program.code.size() == 1 && program.code[0].op == d9sh::OP_MOV);
    CHECK(original.size() == 20); // the caller's bytecode is immutable
    CHECK(d8::vertex_shader(layout, original,
                            {d9sh::OP_DEF, 0xa00f0002u, 0x3f800000u, 0xffffu, 0, 0}, &converted,
                            &why));
    CHECK(converted.constants.size() == 1 && converted.constants.at(2)[0] == 1.0f);
    CHECK(d9sh::program_for(converted.code).defs.empty());
    size_t extent = 99;
    CHECK(d9sh::bytecode_extent(converted.code.data(), converted.code.size(), &extent, &why));
    CHECK(extent == converted.code.size());
    CHECK(!d9sh::bytecode_extent(original.data(), original.size() - 4, &extent, &why));
    auto truncated = original;
    truncated.resize(8);
    CHECK(!d8::vertex_shader(layout, truncated, {}, &converted, &why));
    original[1] = 2;
    CHECK(!d8::vertex_shader(layout, original, {}, &converted, &why));
}
// Pixel checks use the production CPU shader interpreter and rasterizer;
// expected colours follow the texture-stage equations, not generated tokens.
void fixed_pixels() {
    d8::Layout layout;
    std::string why;
    CHECK(d8::fvf_layout(0x1c4, &layout, &why));
    ComObj *device = com_new(K_D3D9DEVICE);
    ComObj *target = com_new(K_D3D9SURFACE);
    target->width = target->height = 8;
    target->pitch = 32;
    target->rmask = 21;
    target->blob.resize(8 * 8 * 4);
    ComObj *texture = com_new(K_D3D9SURFACE);
    texture->width = texture->height = 1;
    texture->pitch = 4;
    texture->rmask = 21;
    texture->blob = {153, 102, 51, 128}; // BGRA
    D9Pipeline &state = d9_pipeline(device->id);
    state.sampler_tex[0] = texture->id;
    state.rs[22] = 1; // no culling
    state.rs[60] = 0x99cc6633u;
    state.tss[0][26] = 3; // COLORARG0 = texture factor
    struct Vertex {
        float x, y, z, rhw;
        uint32_t diffuse, specular;
        float u, v;
    } vertices[] = {{0, 0, 0.5f, 1, 0xc0994d33u, 0x661a334du, 0, 0},
                    {8, 0, 0.5f, 1, 0xc0994d33u, 0x661a334du, 0, 0},
                    {0, 8, 0.5f, 1, 0xc0994d33u, 0x661a334du, 0, 0}};
    D9DrawCall draw;
    draw.prim = 4;
    draw.prim_count = 1;
    draw.stride = sizeof(Vertex);
    draw.vertex_data = reinterpret_cast<const uint8_t *>(vertices);
    draw.vertex_bytes = sizeof vertices;
    for (const auto &test : d8_combiner_cases) {
        state.tss[0][1] = test.op;
        state.tss[0][2] = test.arg1;
        d8::FixedPrograms programs;
        CHECK(d8::fixed_programs(layout, state, 8, 8, &programs, &why));
        state.vs.bytes = std::make_shared<const std::vector<uint8_t>>(programs.vertex);
        state.ps.bytes = std::make_shared<const std::vector<uint8_t>>(programs.pixel);
        std::memcpy(state.vconst, programs.vertex_constants, sizeof programs.vertex_constants);
        std::memcpy(state.pconst, programs.pixel_constants, sizeof programs.pixel_constants);
        std::fill(target->blob.begin(), target->blob.end(), 0);
        d9_raster_draw(device, target, layout.declaration, draw);
        const uint8_t *pixel = &target->blob[(1 * 8 + 1) * 4];
        CHECK(std::abs(int(pixel[2]) - test.r) <= 1);
        CHECK(std::abs(int(pixel[1]) - test.g) <= 1);
        CHECK(std::abs(int(pixel[0]) - test.b) <= 1);
        CHECK(pixel[3] == test.alpha);
    }
    state.tss[0][2] = 2;
    for (uint32_t op = 2; op <= 26; ++op) {
        state.tss[0][1] = op;
        d8::FixedPrograms programs;
        const bool supported = d8::texture_operation_caps() & (1u << (op - 1));
        CHECK(d8::fixed_programs(layout, state, 8, 8, &programs, &why) == supported);
        if (supported) {
            CHECK(d9sh::program_for(programs.vertex).ok);
            CHECK(d9sh::program_for(programs.pixel).ok);
        }
    }
    state.tss[0][1] = 4;
    d8::FixedPrograms before, after;
    CHECK(d8::fixed_programs(layout, state, 8, 8, &before, &why));
    CHECK(d8::fixed_programs(layout, state, 16, 16, &after, &why));
    CHECK(before.vertex == after.vertex && before.pixel == after.pixel);
    CHECK(before.vertex_constants[4][0] == 2 * after.vertex_constants[4][0]);
    auto check_stages = [&](int r, int g, int b, int alpha) {
        d8::FixedPrograms programs;
        CHECK(d8::fixed_programs(layout, state, 8, 8, &programs, &why));
        state.vs.bytes = std::make_shared<const std::vector<uint8_t>>(programs.vertex);
        state.ps.bytes = std::make_shared<const std::vector<uint8_t>>(programs.pixel);
        std::memcpy(state.vconst, programs.vertex_constants, sizeof programs.vertex_constants);
        std::memcpy(state.pconst, programs.pixel_constants, sizeof programs.pixel_constants);
        d9_raster_draw(device, target, layout.declaration, draw);
        const uint8_t *p = &target->blob[(1 * 8 + 1) * 4];
        CHECK(std::abs(int(p[2]) - r) <= 1 && std::abs(int(p[1]) - g) <= 1 &&
              std::abs(int(p[0]) - b) <= 1 && std::abs(int(p[3]) - alpha) <= 1);
    };
    state.sampler_tex[1] = texture->id;
    state.tss[0][1] = 17; // PREMODULATE feeds the next stage's current colour
    state.tss[1][1] = state.tss[1][4] = 3;
    check_stages(10, 41, 92, 64);
    state.tss[0][1] = 2;
    state.tss[0][28] = 5; // write TEMP instead of CURRENT
    state.tss[1][1] = state.tss[1][4] = 7;
    state.tss[1][2] = state.tss[1][5] = 5;
    check_stages(204, 179, 204, 255);
    state.tss[0][1] = 1;
    state.rs[29] = 1; // post-combiner specular preserves alpha
    check_stages(179, 128, 128, 192);
}
} // namespace

int main() {
    fvf();
    declarations();
    fixed_pixels();
    std::printf("d3d8 programs: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
