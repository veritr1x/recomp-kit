// Direct3D 8's unlit fixed-function path, expressed as ordinary bytecode so
// the CPU, Metal, Vulkan and WebGPU renderers consume the same operations.
#include "d3d8_program.h"
#include "d3d9_shader.h"
#include <algorithm>
#include <initializer_list>

namespace d8 {
namespace {
using namespace d9sh;
uint32_t reg(uint32_t type, uint32_t index) {
    return 0x80000000u | ((type & 7) << 28) | ((type & 24) << 8) | index;
}
uint32_t dst(uint32_t type, uint32_t index, uint32_t mask = 15, bool sat = false) {
    return reg(type, index) | (mask << 16) | (uint32_t(sat) << 20);
}
uint32_t src(uint32_t type, uint32_t index, uint32_t swizzle = 0xe4, uint32_t mod = 0) {
    return reg(type, index) | (swizzle << 16) | (mod << 24);
}
void word(std::vector<uint8_t> &code, uint32_t value) {
    for (int k = 0; k < 4; ++k)
        code.push_back(uint8_t(value >> (8 * k)));
}
void emit(std::vector<uint8_t> &code, uint32_t op, std::initializer_list<uint32_t> args) {
    word(code, op | (uint32_t(args.size()) << 24));
    for (uint32_t value : args)
        word(code, value);
}
void dcl(std::vector<uint8_t> &code, uint32_t usage, uint32_t index, uint32_t type,
         uint32_t input) {
    emit(code, OP_DCL, {0x80000000u | usage | (index << 16), dst(type, input)});
}
bool fail(std::string *why, const char *message) {
    if (why)
        *why = message;
    return false;
}

// r0=current, r1=temp, r2=texture, r3=result, r4..r6=arguments, r7=scratch.
// Copy arguments before changing either channel: CURRENT means the preceding
// stage for both RGB and alpha, even when the two stages use different ops.
bool argument(std::vector<uint8_t> &code, uint32_t value, uint32_t target) {
    uint32_t token;
    switch (value & 15) {
    case 0:
        token = src(R_INPUT, 0);
        break;
    case 1:
        token = src(R_TEMP, 0);
        break;
    case 2:
        token = src(R_TEMP, 2);
        break;
    case 3:
        token = src(R_CONST, 0);
        break;
    case 4:
        token = src(R_INPUT, 1);
        break;
    case 5:
        token = src(R_TEMP, 1);
        break;
    default:
        return false;
    }
    if (value & ~0x3fu)
        return false;
    if (value & 0x20)
        token = (token & ~0x00ff0000u) | 0x00ff0000u;
    if (value & 0x10)
        token |= 6u << 24; // complement after swizzle
    emit(code, OP_MOV, {dst(R_TEMP, target), token});
    return true;
}

bool combine(std::vector<uint8_t> &code, uint32_t op, uint32_t mask) {
    const uint32_t result = dst(R_TEMP, 3, mask), a = src(R_TEMP, 4), b = src(R_TEMP, 5);
    const uint32_t one = src(R_CONST, 1, 0xaa), half = src(R_CONST, 1, 0x55);
    const uint32_t two = src(R_CONST, 1, 0xff), scratch = dst(R_TEMP, 7), tmp = src(R_TEMP, 7);
    switch (op) {
    case 1:
        return true;
    case 2:
    case 17:
        emit(code, OP_MOV, {result, a});
        break;
    case 3:
        emit(code, OP_MOV, {result, b});
        break;
    case 4:
    case 5:
    case 6:
        emit(code, OP_MUL, {result, a, b});
        if (op != 4)
            emit(code, OP_MUL, {result, src(R_TEMP, 3), op == 5 ? two : src(R_CONST, 2, 0)});
        break;
    case 7:
    case 8:
    case 9:
        emit(code, OP_ADD, {result, a, b});
        if (op != 7)
            emit(code, OP_SUB, {result, src(R_TEMP, 3), half});
        if (op == 9)
            emit(code, OP_MUL, {result, src(R_TEMP, 3), two});
        break;
    case 10:
        emit(code, OP_SUB, {result, a, b});
        break;
    case 11:
        emit(code, OP_MUL, {scratch, a, b});
        emit(code, OP_ADD, {result, a, b});
        emit(code, OP_SUB, {result, src(R_TEMP, 3), tmp});
        break;
    case 12:
    case 13:
    case 14:
    case 16: {
        const uint32_t alpha = op == 12   ? src(R_INPUT, 0, 0xff)
                               : op == 13 ? src(R_TEMP, 2, 0xff)
                               : op == 14 ? src(R_CONST, 0, 0xff)
                                          : src(R_TEMP, 0, 0xff);
        emit(code, OP_LRP, {result, alpha, a, b});
        break;
    }
    case 15:
        emit(code, OP_SUB, {scratch, one, src(R_TEMP, 2, 0xff)});
        emit(code, OP_MAD, {result, b, tmp, a});
        break;
    case 18:
    case 19:
    case 20:
    case 21:
        if (mask == 8)
            return false; // these four operations are RGB-only
        if (op == 18)
            emit(code, OP_MAD, {result, src(R_TEMP, 4, 0xff), b, a});
        if (op == 19)
            emit(code, OP_MAD, {result, a, b, src(R_TEMP, 4, 0xff)});
        if (op == 20) {
            emit(code, OP_SUB, {scratch, one, src(R_TEMP, 4, 0xff)});
            emit(code, OP_MAD, {result, tmp, b, a});
        }
        if (op == 21) {
            emit(code, OP_SUB, {scratch, one, a});
            emit(code, OP_MAD, {result, tmp, b, src(R_TEMP, 4, 0xff)});
        }
        break;
    case 24:
        emit(code, OP_MAD, {dst(R_TEMP, 4), a, two, src(R_CONST, 1, 0xaa, 1)});
        emit(code, OP_MAD, {dst(R_TEMP, 5), b, two, src(R_CONST, 1, 0xaa, 1)});
        emit(code, OP_DP3, {result, src(R_TEMP, 4), src(R_TEMP, 5)});
        break;
    case 25:
        emit(code, OP_MAD, {result, a, b, src(R_TEMP, 6)});
        break;
    case 26:
        emit(code, OP_LRP, {result, src(R_TEMP, 6), a, b});
        break;
    default:
        return false;
    }
    return true;
}

bool needs_texture(const uint32_t *tss, bool alpha) {
    const uint32_t op = tss[alpha ? 4 : 1];
    if (op == 1)
        return false;
    if (op == 13 || op == 15)
        return true;
    const uint32_t a = tss[alpha ? 5 : 2] & 15, b = tss[alpha ? 6 : 3] & 15;
    return (op != 3 && a == 2) || (op != 2 && op != 17 && b == 2) ||
           ((op == 25 || op == 26) && (tss[alpha ? 27 : 26] & 15) == 2);
}
} // namespace

uint32_t texture_operation_caps() {
    uint32_t result = 0;
    for (uint32_t op = 1; op <= 26; ++op)
        if (op != 22 && op != 23)
            result |= 1u << (op - 1);
    return result;
}

bool fixed_programs(const Layout &layout, const D9Pipeline &state, uint32_t width, uint32_t height,
                    FixedPrograms *out, std::string *why) {
    if (!out || layout.attributes.empty() || !width || !height)
        return fail(why, "missing layout or render target");
    if (!layout.transformed && state.rs[137])
        return fail(why, "fixed-function lighting is not supported");
    if (state.rs[28])
        return fail(why, "fixed-function fog is not supported");
    FixedPrograms result;
    auto &vs = result.vertex;
    auto &ps = result.pixel;
    word(vs, 0xfffe0200u);
    int colors[2] = {-1, -1}, coordinates[8];
    std::fill(std::begin(coordinates), std::end(coordinates), -1);
    for (const Attribute &a : layout.attributes) {
        dcl(vs, a.usage, a.usage_index, R_INPUT, a.reg);
        if (a.usage == 10 && a.usage_index < 2)
            colors[a.usage_index] = a.reg;
        if (a.usage == 5 && a.usage_index < 8)
            coordinates[a.usage_index] = a.reg;
    }
    if (layout.transformed) {
        const float x = state.viewport_set ? float(state.viewport[0]) : 0;
        const float y = state.viewport_set ? float(state.viewport[1]) : 0;
        const float w = state.viewport_set ? float(state.viewport[2]) : float(width);
        const float h = state.viewport_set ? float(state.viewport[3]) : float(height);
        if (w <= 0 || h <= 0)
            return fail(why, "empty fixed-function viewport");
        auto &scale = result.vertex_constants[4];
        auto &bias = result.vertex_constants[5];
        scale[0] = 2 / w;
        scale[1] = -2 / h;
        scale[2] = 1;
        bias[0] = -1 - x * scale[0];
        bias[1] = 1 - y * scale[1];
        emit(vs, OP_RCP, {dst(R_TEMP, 0, 8), src(R_INPUT, 0, 0xff)});
        emit(vs, OP_MAD, {dst(R_TEMP, 0, 7), src(R_INPUT, 0), src(R_CONST, 4), src(R_CONST, 5)});
        emit(vs, OP_MUL, {dst(R_RASTOUT, 0, 7), src(R_TEMP, 0), src(R_TEMP, 0, 0xff)});
        emit(vs, OP_MOV, {dst(R_RASTOUT, 0, 8), src(R_TEMP, 0, 0xff)});
    } else {
        float world_view[16] = {}, combined[16] = {};
        const float *world = state.transform[24], *view = state.transform[2],
                    *projection = state.transform[3];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 4; ++k)
                    world_view[4 * r + c] += world[4 * r + k] * view[4 * k + c];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 4; ++k)
                    combined[4 * r + c] += world_view[4 * r + k] * projection[4 * k + c];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                result.vertex_constants[r][c] = combined[4 * c + r];
        emit(vs, OP_M4X4, {dst(R_RASTOUT, 0), src(R_INPUT, 0), src(R_CONST, 0)});
    }
    emit(vs, OP_DEF, {dst(R_CONST, 6), 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u});
    emit(vs, OP_DEF, {dst(R_CONST, 7), 0, 0, 0, 0});
    for (uint32_t i = 0; i < 2; ++i)
        emit(vs, OP_MOV,
             {dst(R_ATTROUT, i),
              colors[i] < 0 ? src(R_CONST, 6 + i) : src(R_INPUT, uint32_t(colors[i]))});
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t index = state.tss[i][11];
        if (index >= 8 || state.tss[i][24])
            return fail(why, "generated texture coordinates or transforms are not supported");
        emit(vs, OP_MOV,
             {dst(R_TEXCRDOUT, i), coordinates[index] < 0
                                       ? src(R_CONST, 7)
                                       : src(R_INPUT, uint32_t(coordinates[index]))});
    }
    word(vs, OP_END);
    word(ps, 0xffff0200u);
    dcl(ps, 10, 0, R_INPUT, 0);
    dcl(ps, 10, 1, R_INPUT, 1);
    emit(ps, OP_MOV, {dst(R_TEMP, 0), src(R_INPUT, 0)});
    emit(ps, OP_MOV, {dst(R_TEMP, 1), src(R_CONST, 1, 0)});
    result.pixel_constants[0][0] = ((state.rs[60] >> 16) & 255) / 255.0f;
    result.pixel_constants[0][1] = ((state.rs[60] >> 8) & 255) / 255.0f;
    result.pixel_constants[0][2] = (state.rs[60] & 255) / 255.0f;
    result.pixel_constants[0][3] = (state.rs[60] >> 24) / 255.0f;
    const float constants[] = {0, 0.5f, 1, 2};
    std::copy(std::begin(constants), std::end(constants), result.pixel_constants[1]);
    std::fill(std::begin(result.pixel_constants[2]), std::end(result.pixel_constants[2]), 4.0f);
    bool premodulate = false;
    for (uint32_t stage = 0; stage < 8; ++stage) {
        const auto &tss = state.tss[stage];
        if (tss[1] == 1)
            break;
        if (premodulate || needs_texture(tss, false) || needs_texture(tss, true)) {
            if (!state.sampler_tex[stage])
                break;
            dcl(ps, 5, stage, R_ADDR, stage);
            emit(ps, OP_DCL, {0x90000000u, dst(R_SAMPLER, stage)});
            emit(ps, OP_TEX, {dst(R_TEMP, 2), src(R_ADDR, stage), src(R_SAMPLER, stage)});
        }
        if (premodulate)
            emit(ps, OP_MUL, {dst(R_TEMP, 0), src(R_TEMP, 0), src(R_TEMP, 2)});
        emit(ps, OP_MOV, {dst(R_TEMP, 3), src(R_TEMP, 0)});
        for (uint32_t channel = 0; channel < 2; ++channel) {
            const uint32_t op = tss[channel ? 4 : 1];
            if (op == 1)
                continue;
            if (!argument(ps, tss[channel ? 5 : 2], 4) || !argument(ps, tss[channel ? 6 : 3], 5) ||
                !argument(ps, tss[channel ? 27 : 26], 6) || !combine(ps, op, channel ? 8 : 7))
                return fail(why, "unsupported fixed-function texture operation or argument");
        }
        const uint32_t result_arg = tss[28] ? tss[28] : 1;
        if (result_arg != 1 && result_arg != 5)
            return fail(why, "invalid texture result argument");
        emit(ps, OP_MOV, {dst(R_TEMP, result_arg == 5 ? 1 : 0, 15, true), src(R_TEMP, 3)});
        premodulate = tss[1] == 17;
    }
    if (state.rs[29])
        emit(ps, OP_ADD, {dst(R_TEMP, 0, 7, true), src(R_TEMP, 0), src(R_INPUT, 1)});
    emit(ps, OP_MOV, {dst(R_COLOROUT, 0), src(R_TEMP, 0)});
    word(ps, OP_END);
    if (!program_for(vs).ok || !program_for(ps).ok)
        return fail(why, "generated fixed-function program did not decode");
    *out = std::move(result);
    return true;
}
} // namespace d8
