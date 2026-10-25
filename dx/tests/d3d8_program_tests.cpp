#include "../d3d8_program.h"
#include "../d3d9_shader.h"
#include "../d3d9_raster.h"
#include "../com.h"
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
    std::vector<uint8_t> converted;
    CHECK(d8::vertex_shader(layout, original, {}, &converted, &why));
    const auto &program = d9sh::program_for(converted);
    CHECK(program.ok && program.inputs.size() == 3);
    CHECK(program.inputs[1].index == 5 && program.inputs[1].usage == 10);
    CHECK(program.code.size() == 1 && program.code[0].op == d9sh::OP_MOV);
    CHECK(original.size() == 20); // the caller's bytecode is immutable
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
    struct Case {
        uint32_t op, arg1;
        int r, g, b, alpha = 128;
    };
    const Case cases[] = {{1, 2, 153, 77, 51, 192}, {2, 2, 51, 102, 153},   {3, 2, 153, 77, 51},
                          {4, 2, 31, 31, 31},       {5, 2, 61, 62, 61},     {6, 2, 122, 123, 122},
                          {7, 2, 204, 179, 204},    {8, 2, 77, 52, 77},     {9, 2, 153, 103, 153},
                          {10, 2, 0, 25, 102},      {11, 2, 173, 148, 173}, {12, 2, 76, 96, 128},
                          {13, 2, 102, 90, 102},    {14, 2, 92, 92, 112},   {15, 2, 127, 140, 178},
                          {16, 2, 76, 96, 128},     {17, 2, 51, 102, 153},  {18, 2, 128, 141, 179},
                          {19, 2, 159, 159, 159},   {20, 2, 127, 140, 178}, {21, 2, 250, 174, 148},
                          {24, 2, 0, 0, 0},         {25, 2, 235, 133, 82},  {26, 2, 71, 87, 71},
                          {4, 0x12, 122, 46, 20},   {4, 0x22, 77, 39, 26},  {2, 3, 204, 102, 51}};
    for (const Case &test : cases) {
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
}
} // namespace

int main() {
    fvf();
    declarations();
    fixed_pixels();
    std::printf("d3d8 programs: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
