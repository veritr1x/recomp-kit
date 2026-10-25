#include "d3d8_program.h"
#include "d3d9_shader.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace d8 {
namespace {
bool fail(std::string *why, const char *message) {
    if (why)
        *why = message;
    return false;
}

void word(std::vector<uint8_t> &bytes, uint32_t value) {
    for (int k = 0; k < 4; ++k)
        bytes.push_back(uint8_t(value >> (8 * k)));
}

// The first sixteen Direct3D 8 vertex registers have fixed semantics when
// converted to a declaration understood by the shared version 9 renderer.
bool semantic(uint32_t reg, uint8_t *usage, uint8_t *index) {
    static const uint8_t first[] = {0, 1, 2, 3, 4, 10, 10};
    *index = 0;
    if (reg < std::size(first)) {
        *usage = first[reg];
        *index = reg == 6 ? 1 : 0;
        return true;
    }
    if (reg < 15) {
        *usage = 5;
        *index = uint8_t(reg - 7);
        return true;
    }
    return false;
}

uint32_t type_bytes(uint32_t type) {
    static const uint32_t sizes[] = {4, 8, 12, 16, 4, 4, 4, 8};
    return type < std::size(sizes) ? sizes[type] : 0;
}

void finish(Layout &layout) {
    for (const Attribute &a : layout.attributes) {
        layout.declaration.push_back(uint8_t(a.stream));
        layout.declaration.push_back(uint8_t(a.stream >> 8));
        layout.declaration.push_back(uint8_t(a.offset));
        layout.declaration.push_back(uint8_t(a.offset >> 8));
        layout.declaration.insert(layout.declaration.end(), {a.type, 0, a.usage, a.usage_index});
    }
    layout.declaration.insert(layout.declaration.end(), {0xff, 0, 0, 0, 17, 0, 0, 0});
}
} // namespace

bool fvf_layout(uint32_t fvf, Layout *out, std::string *why) {
    Layout result;
    if (!out || (fvf & 0xe001) || ((fvf >> 8) & 15) > 8)
        return fail(why, "invalid FVF bits or texture coordinate count");
    const uint32_t position = fvf & 14;
    if (position != 2 && position != 4)
        return fail(why, "blended FVF positions are not supported");
    result.transformed = position == 4;
    auto add = [&](uint8_t type, uint8_t usage, uint8_t index, uint8_t reg) {
        result.attributes.push_back({0, uint16_t(result.stride[0]), type, usage, index, reg});
        result.stride[0] += type_bytes(type);
    };
    add(result.transformed ? 3 : 2, 0, 0, 0);
    if (fvf & 0x10) {
        if (result.transformed)
            return fail(why, "a transformed FVF cannot contain normals");
        add(2, 3, 0, 3);
    }
    if (fvf & 0x20)
        add(0, 4, 0, 4);
    if (fvf & 0x40)
        add(4, 10, 0, 5);
    if (fvf & 0x80)
        add(4, 10, 1, 6);
    const uint32_t count = (fvf >> 8) & 15;
    static const uint8_t types[] = {1, 2, 3, 0};
    for (uint32_t i = 0; i < count; ++i)
        add(types[(fvf >> (16 + 2 * i)) & 3], 5, uint8_t(i), uint8_t(7 + i));
    if (fvf & 0x1000)
        return fail(why, "LASTBETA_UBYTE4 requires blended positions");
    finish(result);
    *out = std::move(result);
    return true;
}

bool token_layout(const std::vector<uint32_t> &tokens, Layout *out,
                  std::vector<uint32_t> *constant_defs, std::string *why) {
    if (!out || !constant_defs)
        return fail(why, "missing declaration output");
    Layout result;
    std::vector<uint32_t> defs;
    std::set<uint32_t> registers;
    uint32_t stream = 8;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const uint32_t token = tokens[i], kind = token >> 29;
        if (token == 0xffffffffu) {
            if (result.attributes.empty() || i + 1 != tokens.size())
                return fail(why, "empty declaration or bytes after its end");
            finish(result);
            *out = std::move(result);
            *constant_defs = std::move(defs);
            return true;
        }
        if (kind == 0 && token == 0)
            continue;
        if (kind == 1) {
            if ((token & ~0x20000007u) || (token & 15) >= 8)
                return fail(why, "unsupported vertex stream");
            stream = token & 7;
            continue;
        }
        if (kind == 2 && stream < 8) {
            if (token & 0x10000000u) {
                if (token & ~0x500f0000u)
                    return fail(why, "invalid declaration skip");
                result.stride[stream] += ((token >> 16) & 15) * 4;
                continue;
            }
            Attribute a;
            a.stream = uint16_t(stream);
            a.offset = uint16_t(result.stride[stream]);
            a.type = uint8_t((token >> 16) & 15);
            a.reg = uint8_t(token & 31);
            if ((token & ~0x400f001fu) || !type_bytes(a.type) ||
                !semantic(a.reg, &a.usage, &a.usage_index) || !registers.insert(a.reg).second)
                return fail(why, "invalid or repeated vertex register");
            result.attributes.push_back(a);
            result.stride[stream] += type_bytes(a.type);
            if (result.stride[stream] > 65535)
                return fail(why, "vertex declaration stride is too large");
            continue;
        }
        if (kind == 4) {
            const uint32_t first = token & 127, count = (token >> 25) & 15;
            if ((token & ~0x9e00007fu) || !count || first + count > 96 ||
                count * 4 > tokens.size() - i - 1)
                return fail(why, "invalid declaration constants");
            for (uint32_t k = 0; k < count; ++k) {
                defs.push_back(d9sh::OP_DEF);
                defs.push_back(0xa00f0000u | (first + k));
                for (uint32_t component = 0; component < 4; ++component)
                    defs.push_back(tokens[++i]);
            }
            continue;
        }
        return fail(why, "unsupported vertex declaration token");
    }
    return fail(why, "vertex declaration has no end token");
}

namespace {
bool prepare(const std::vector<uint8_t> &bytes, PreparedShader *out, std::string *why) {
    size_t extent = 0;
    if (!out || !d9sh::bytecode_extent(bytes.data(), bytes.size(), &extent, why) ||
        extent != bytes.size())
        return fail(why, "invalid shader extent");
    const auto &program = d9sh::program_for(bytes);
    if (!program.ok)
        return fail(why, "unsupported shader instruction");
    const uint32_t limit = program.pixel ? 8 : 96;
    for (const auto &constant : program.defs)
        if (constant.first >= limit)
            return fail(why, "shader constant exceeds the version 8 register file");
    if (!program.relative && program.max_const > limit)
        return fail(why, "shader constant exceeds the version 8 register file");
    for (const auto &instruction : program.code)
        for (uint32_t i = 0; i < instruction.nsrc; ++i)
            if (instruction.src[i].type == d9sh::R_CONST && instruction.src[i].index >= limit)
                return fail(why, "shader constant exceeds the version 8 register file");
    PreparedShader result;
    result.constants = program.defs;
    result.code.insert(result.code.end(), bytes.begin(), bytes.begin() + 4);
    auto read = [&](size_t at) {
        uint32_t value;
        std::memcpy(&value, bytes.data() + at, 4);
        return value;
    };
    const uint32_t version = read(0);
    for (size_t at = 4; at < bytes.size();) {
        const uint32_t token = read(at);
        const size_t length = 4 * d9sh::instruction_words(version, token);
        if ((token & 0xffffu) != d9sh::OP_DEF)
            result.code.insert(result.code.end(), bytes.begin() + at, bytes.begin() + at + length);
        at += length;
    }
    *out = std::move(result);
    return true;
}
} // namespace

bool vertex_shader(const Layout &layout, const std::vector<uint8_t> &original,
                   const std::vector<uint32_t> &constant_defs, PreparedShader *out,
                   std::string *why) {
    if (!out || original.size() < 8 || original.size() % 4 || original[0] != 1 ||
        original[1] != 1 || original[2] != 0xfe || original[3] != 0xff)
        return fail(why, "Direct3D 8 requires a version 1.1 vertex shader");
    std::vector<uint8_t> result(original.begin(), original.begin() + 4);
    for (const Attribute &a : layout.attributes) {
        word(result, d9sh::OP_DCL);
        word(result, 0x80000000u | a.usage | (uint32_t(a.usage_index) << 16));
        word(result, 0x900f0000u | a.reg);
    }
    for (uint32_t token : constant_defs)
        word(result, token);
    result.insert(result.end(), original.begin() + 4, original.end());
    return prepare(result, out, why);
}

bool pixel_shader(const std::vector<uint8_t> &original, PreparedShader *out, std::string *why) {
    if (original.size() < 8 || original[0] > 4 || original[1] != 1 || original[2] != 0xff ||
        original[3] != 0xff)
        return fail(why, "Direct3D 8 requires a version 1 pixel shader");
    return prepare(original, out, why);
}
} // namespace d8
