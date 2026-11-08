// d3d9_ffp.cpp - Direct3D 9's fixed-function pipeline, as shader bytecode. See d3d9_ffp.h.
// Adapted from the HommaRe port's kit (MIT); the device state it reads beyond
// D9Pipeline comes in a FixedState.
//
// The programs are written as Direct3D 9 tokens, the same encoding a game's
// own shaders arrive in, by a small assembler below. The formulas are the ones
// the Direct3D 9 documentation gives for the fixed-function pipeline: the
// lighting model in camera space, the D3DTOP_* stage operations, D3DTSS_
// TEXCOORDINDEX generation and D3DTTFF_* texture transforms.
#include "d3d9_ffp.h"

#include "d3d9_shader.h"

#include "../runtime/guest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace d9ffp {
namespace {

// ---- Fixed-function render states, texture stage states and enums ----------
enum : uint32_t {
    RS_FOGENABLE = 28,
    RS_SPECULARENABLE = 29,
    RS_FOGTABLEMODE = 35,
    RS_FOGSTART = 36,
    RS_FOGEND = 37,
    RS_FOGDENSITY = 38,
    RS_RANGEFOGENABLE = 48,
    RS_TEXTUREFACTOR = 60,
    RS_LIGHTING = 137,
    RS_AMBIENT = 139,
    RS_FOGVERTEXMODE = 140,
    RS_COLORVERTEX = 141,
    RS_LOCALVIEWER = 142,
    RS_NORMALIZENORMALS = 143,
    RS_DIFFUSEMATERIALSOURCE = 145,
    RS_SPECULARMATERIALSOURCE = 146,
    RS_AMBIENTMATERIALSOURCE = 147,
    RS_EMISSIVEMATERIALSOURCE = 148,
    RS_VERTEXBLEND = 151,
};
enum : uint32_t {
    TSS_COLOROP = 1,
    TSS_COLORARG1 = 2,
    TSS_COLORARG2 = 3,
    TSS_ALPHAOP = 4,
    TSS_ALPHAARG1 = 5,
    TSS_ALPHAARG2 = 6,
    TSS_TEXCOORDINDEX = 11,
    TSS_TEXTURETRANSFORMFLAGS = 24,
    TSS_COLORARG0 = 26,
    TSS_ALPHAARG0 = 27,
    TSS_RESULTARG = 28,
    TSS_CONSTANT = 32,
};
enum : uint32_t {
    TOP_DISABLE = 1,
    TOP_SELECTARG1,
    TOP_SELECTARG2,
    TOP_MODULATE,
    TOP_MODULATE2X,
    TOP_MODULATE4X,
    TOP_ADD,
    TOP_ADDSIGNED,
    TOP_ADDSIGNED2X,
    TOP_SUBTRACT,
    TOP_ADDSMOOTH,
    TOP_BLENDDIFFUSEALPHA,
    TOP_BLENDTEXTUREALPHA,
    TOP_BLENDFACTORALPHA,
    TOP_BLENDTEXTUREALPHAPM,
    TOP_BLENDCURRENTALPHA,
    TOP_PREMODULATE,
    TOP_MODULATEALPHA_ADDCOLOR,
    TOP_MODULATECOLOR_ADDALPHA,
    TOP_MODULATEINVALPHA_ADDCOLOR,
    TOP_MODULATEINVCOLOR_ADDALPHA,
    TOP_BUMPENVMAP,
    TOP_BUMPENVMAPLUMINANCE,
    TOP_DOTPRODUCT3,
    TOP_MULTIPLYADD,
    TOP_LERP,
};
enum : uint32_t {
    TA_DIFFUSE = 0,
    TA_CURRENT = 1,
    TA_TEXTURE = 2,
    TA_TFACTOR = 3,
    TA_SPECULAR = 4,
    TA_TEMP = 5,
    TA_CONSTANT = 6,
    TA_SELECTMASK = 0xf,
    TA_COMPLEMENT = 0x10,
    TA_ALPHAREPLICATE = 0x20,
};
// D3DDECLUSAGE
enum : uint8_t {
    U_POSITION = 0,
    U_BLENDWEIGHT = 1,
    U_BLENDINDICES = 2,
    U_NORMAL = 3,
    U_PSIZE = 4,
    U_TEXCOORD = 5,
    U_POSITIONT = 9,
    U_COLOR = 10,
};

// ---- An assembler for the subset of the token format these programs use ----
// Register types, as the token encodes them (D3DSHADER_PARAM_REGISTER_TYPE).
enum : uint32_t {
    T_TEMP = 0,
    T_INPUT = 1,
    T_CONST = 2,
    T_TEXTURE = 3, // ps: tN
    T_RASTOUT = 4, // vs: oPos 0, oFog 1, oPts 2
    T_ATTROUT = 5, // vs: oD0, oD1
    T_TEXCRDOUT = 6,
    T_COLOROUT = 8,
    T_SAMPLER = 10,
};
constexpr uint32_t XYZW = 0xe4;
constexpr uint32_t splat(uint32_t comp) {
    return comp * 0x55u;
}
constexpr uint32_t M_X = 1, M_Y = 2, M_Z = 4, M_W = 8, M_XYZ = 7, M_ALL = 15;

// One operand: a register, its swizzle and its source modifier (1: negate).
struct R {
    uint32_t type = T_TEMP, index = 0, swz = XYZW, mod = 0;
    // Component `k` of this operand, replicated to all four.
    R c(uint32_t k) const {
        R r = *this;
        r.swz = splat((swz >> (2 * k)) & 3);
        return r;
    }
    R x() const {
        return c(0);
    }
    R y() const {
        return c(1);
    }
    R z() const {
        return c(2);
    }
    R w() const {
        return c(3);
    }
    R operator-() const {
        R r = *this;
        r.mod = mod == 1 ? 0 : 1;
        return r;
    }
    R swizzled(uint32_t s) const {
        R r = *this;
        r.swz = s;
        return r;
    }
};
R reg(uint32_t type, uint32_t index) {
    R r;
    r.type = type;
    r.index = index;
    return r;
}
R tmp(uint32_t i) {
    return reg(T_TEMP, i);
}
R cst(uint32_t i) {
    return reg(T_CONST, i);
}
R in(uint32_t i) {
    return reg(T_INPUT, i);
}

struct Asm {
    std::vector<uint32_t> t;
    explicit Asm(uint32_t version) {
        t.push_back(version);
    }
    static uint32_t bits(uint32_t type, uint32_t index) {
        return 0x80000000u | (type & 7) << 28 | (type & 0x18) << 8 | (index & 0x7ff);
    }
    void dst(const R &r, uint32_t mask, bool sat) {
        t.push_back(bits(r.type, r.index) | (mask & 0xf) << 16 | (sat ? 1u << 20 : 0u));
    }
    void src(const R &r) {
        t.push_back(bits(r.type, r.index) | (r.swz & 0xff) << 16 | (r.mod & 0xf) << 24);
    }
    // An arithmetic instruction: the destination, then its sources.
    void op(uint32_t opcode, const R &d, uint32_t mask, std::initializer_list<R> s,
            bool sat = false, uint32_t ctrl = 0) {
        t.push_back(opcode | ctrl << 16 | (uint32_t)(1 + s.size()) << 24);
        dst(d, mask, sat);
        for (const R &x : s)
            src(x);
    }
    void dcl(uint32_t usage, uint32_t usage_index, const R &r) {
        t.push_back(d9sh::OP_DCL | 2u << 24);
        t.push_back(0x80000000u | usage | usage_index << 16);
        dst(r, M_ALL, false);
    }
    void dcl_sampler(uint32_t kind, uint32_t stage) {
        t.push_back(d9sh::OP_DCL | 2u << 24);
        t.push_back(0x80000000u | kind << 27);
        dst(reg(T_SAMPLER, stage), M_ALL, false);
    }
    void def(uint32_t index, float a, float b, float c, float d) {
        t.push_back(d9sh::OP_DEF | 5u << 24);
        dst(cst(index), M_ALL, false);
        for (float f : {a, b, c, d}) {
            uint32_t u;
            memcpy(&u, &f, 4);
            t.push_back(u);
        }
    }
    std::vector<uint8_t> finish() {
        t.push_back(0x0000ffffu);
        std::vector<uint8_t> b(t.size() * 4);
        for (size_t i = 0; i < t.size(); ++i)
            for (int k = 0; k < 4; ++k)
                b[i * 4 + k] = (uint8_t)(t[i] >> (8 * k));
        return b;
    }
    // Shorthands.
    void mov(const R &d, uint32_t m, const R &a, bool sat = false) {
        op(d9sh::OP_MOV, d, m, {a}, sat);
    }
    void add(const R &d, uint32_t m, const R &a, const R &b, bool sat = false) {
        op(d9sh::OP_ADD, d, m, {a, b}, sat);
    }
    void mul(const R &d, uint32_t m, const R &a, const R &b, bool sat = false) {
        op(d9sh::OP_MUL, d, m, {a, b}, sat);
    }
    void mad(const R &d, uint32_t m, const R &a, const R &b, const R &c, bool sat = false) {
        op(d9sh::OP_MAD, d, m, {a, b, c}, sat);
    }
    void lrp(const R &d, uint32_t m, const R &a, const R &b, const R &c, bool sat = false) {
        op(d9sh::OP_LRP, d, m, {a, b, c}, sat);
    }
};

struct VsKey;
// The program cache: generated programs by the bytes of their key struct.
// Programs are never freed, so a draw can hand out pointers into them.
template <typename Key> Program &cached(const Key &k, std::vector<uint8_t> (*build)(const Key &)) {
    static std::mutex mutex;
    static auto *cache = new std::unordered_map<std::string, Program *>();
    std::string id((const char *)&k, sizeof k);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache->find(id);
    if (it != cache->end())
        return *it->second;
    Program *p = new Program;
    p->bytes = build(k);
    p->key = d9sh::code_key(p->bytes.data(), p->bytes.size());
    (*cache)[id] = p;
    if (cache->size() <= 24) { // which fixed-function states a game draws with, for diagnosis
        std::string hex;
        char b[4];
        for (char ch : id) {
            snprintf(b, sizeof b, "%02x", (unsigned)(uint8_t)ch);
            hex += b;
        }
        LOGW("d3d9: fixed-function %s program %016llx for state %s",
             std::is_same<Key, VsKey>::value ? "vertex" : "pixel", (unsigned long long)p->key,
             hex.c_str());
    }
    return *p;
}

// ---- Matrices: Direct3D's, row-major, for row vectors (v' = v * M) ----------
struct M4 {
    float m[16];
};
M4 mul(const M4 &a, const M4 &b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
M4 of(const float *m) {
    M4 r;
    memcpy(r.m, m, sizeof r.m);
    return r;
}
// Transform slot `slot` (D9Pipeline::transform), or the identity Direct3D
// starts every transform as when the game has not set it.
M4 transform(const D9Pipeline &pl, const FixedState &fs, uint32_t slot) {
    if (fs.transform_set >> slot & 1)
        return of(pl.transform[slot]);
    M4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
// A texture stage state, with D3DTA_CURRENT for the three that start there.
uint32_t stage_state(const D9Pipeline &pl, const FixedState &fs, uint32_t s, uint32_t type) {
    if ((type == 26 || type == 27 || type == 28) && !(fs.tss_set[s] >> type & 1))
        return 1; // COLORARG0, ALPHAARG0, RESULTARG: D3DTA_CURRENT
    return pl.tss[s][type];
}
// Column j of `m` into a register, so a dp4 with it gives component j of v * M.
void put_columns(float (*c)[4], const M4 &m, int rows) {
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < 4; ++i)
            c[j][i] = m.m[i * 4 + j];
}
void put4(float *c, float a, float b, float d, float e) {
    c[0] = a;
    c[1] = b;
    c[2] = d;
    c[3] = e;
}
void put_color(float *c, uint32_t argb) {
    put4(c, ((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f,
         ((argb >> 24) & 255) / 255.0f);
}
float as_float(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// ---- The vertex half ---------------------------------------------------------
// Input registers of the generated vertex program.
enum : uint32_t { V_POS = 0, V_NORMAL = 1, V_DIFFUSE = 2, V_SPECULAR = 3, V_PSIZE = 4, V_TEX = 5 };
// Its constant registers.
enum : uint32_t {
    C_WVP = 0,    // 4: world x view x projection, by column
    C_WV = 4,     // 4: world x view, by column
    C_NORMAL = 8, // 3: the normal matrix, by column
    C_MAT_D = 11, // material diffuse, ambient, specular, emissive
    C_MAT_A = 12,
    C_MAT_S = 13,
    C_MAT_E = 14,
    C_POWER = 15, // (power, specular on, 0, 0)
    C_AMBIENT = 16,
    C_FOG = 17,    // (start, end, density, 1 / (end - start))
    C_SCREEN = 18, // POSITIONT: x, y scale and offset to clip space
    C_DEPTH = 19,  // POSITIONT: z scale and offset
    C_LIGHT = 20, // 8 per light: diffuse, specular, ambient, position, direction, attenuation, spot
    C_TEXM = 84,  // 4 per stage: the texture transform, by column
    C_VIEWDIR = 125, // def: (0, 0, -1, 0), the non-local viewer
    C_LIT2 = 126,    // def: (tiny, log2(e), 0, 0)
    C_LIT = 127,     // def: (0, 0.5, 1, 2)
};
static_assert(C_TEXM + 32 <= C_VIEWDIR && C_LIT + 1 == VS_CONSTANTS, "vertex constant layout");

// Everything the vertex program's code depends on. Bytes, so it compares and
// hashes as raw memory with no padding.
struct VsKey {
    uint8_t pretransformed, has_normal, has_diffuse, has_specular, has_psize;
    uint8_t tex_dims[8]; // components of each TEXCOORD usage index, 0 when absent
    uint8_t lighting, specular, local_viewer, normalize;
    // Diffuse, ambient, specular, emissive: 0 material, 1 vertex diffuse, 2 vertex specular.
    uint8_t mat_src[4];
    uint8_t lights, light_type[8];
    uint8_t fog;       // computed vertex fog: 0 none, 1 linear, 2 exp, 3 exp2
    uint8_t range_fog; // distance rather than depth
    uint8_t stages;    // texture coordinate sets written
    uint8_t tci_index[8], tci_gen[8], ttf_count[8], ttf_projected[8];
};

std::vector<uint8_t> build_vertex(const VsKey &k) {
    using namespace d9sh;
    Asm a(0xfffe0200u); // vs_2_0
    a.def(C_LIT, 0.0f, 0.5f, 1.0f, 2.0f);
    a.def(C_LIT2, 1e-20f, 1.4426950409f, 0.0f, 0.0f);
    a.def(C_VIEWDIR, 0.0f, 0.0f, -1.0f, 0.0f);
    const R zero = cst(C_LIT).x(), half = cst(C_LIT).y(), one = cst(C_LIT).z(),
            two = cst(C_LIT).w();
    a.dcl(k.pretransformed ? U_POSITIONT : U_POSITION, 0, in(V_POS));
    if (k.has_normal)
        a.dcl(U_NORMAL, 0, in(V_NORMAL));
    if (k.has_diffuse)
        a.dcl(U_COLOR, 0, in(V_DIFFUSE));
    if (k.has_specular)
        a.dcl(U_COLOR, 1, in(V_SPECULAR));
    if (k.has_psize)
        a.dcl(U_PSIZE, 0, in(V_PSIZE));
    for (uint32_t i = 0; i < 8; ++i)
        if (k.tex_dims[i])
            a.dcl(U_TEXCOORD, i, in(V_TEX + i));

    const R opos = reg(T_RASTOUT, 0), ofog = reg(T_RASTOUT, 1), opts = reg(T_RASTOUT, 2);
    const R pos = tmp(0), nrm = tmp(1);
    bool texgen = false;
    for (uint32_t s = 0; s < k.stages; ++s)
        texgen |= k.tci_gen[s] != 0;
    if (k.pretransformed) {
        // Screen x, y, z and 1/w back to clip space: the renderer's viewport
        // takes them to the same pixels again.
        a.mad(pos, M_X | M_Y, in(V_POS), cst(C_SCREEN), cst(C_SCREEN).swizzled(0xee));
        a.mad(pos, M_Z, in(V_POS), cst(C_DEPTH).x(), cst(C_DEPTH).y());
        a.op(OP_MAX, tmp(6), M_X, {in(V_POS).w(), cst(C_LIT2).x()});
        a.op(OP_RCP, pos, M_W, {tmp(6).x()});
        a.mul(pos, M_XYZ, pos, pos.w());
        a.mov(opos, M_ALL, pos);
    } else {
        for (uint32_t j = 0; j < 4; ++j)
            a.op(OP_DP4, opos, 1u << j, {in(V_POS), cst(C_WVP + j)});
        if (k.lighting || k.fog || texgen) {
            for (uint32_t j = 0; j < 3; ++j)
                a.op(OP_DP4, pos, 1u << j, {in(V_POS), cst(C_WV + j)});
            a.mov(pos, M_W, one);
        }
        if (k.lighting || texgen) {
            if (k.has_normal) {
                for (uint32_t j = 0; j < 3; ++j)
                    a.op(OP_DP3, nrm, 1u << j, {in(V_NORMAL), cst(C_NORMAL + j)});
                a.mov(nrm, M_W, zero);
                if (k.normalize)
                    a.op(OP_NRM, nrm, M_ALL, {nrm});
            } else {
                a.mov(nrm, M_ALL, zero); // no normal: every N.L is zero
            }
        }
    }

    // Colours.
    const R od0 = reg(T_ATTROUT, 0), od1 = reg(T_ATTROUT, 1);
    if (k.lighting) {
        auto source = [&](uint8_t src, uint32_t material) {
            return src == 1 ? in(V_DIFFUSE) : src == 2 ? in(V_SPECULAR) : cst(material);
        };
        const R md = source(k.mat_src[0], C_MAT_D), ma = source(k.mat_src[1], C_MAT_A),
                ms = source(k.mat_src[2], C_MAT_S), me = source(k.mat_src[3], C_MAT_E);
        const R diffuse = tmp(2), specular = tmp(3), ambient = tmp(4), l = tmp(5), t6 = tmp(6),
                t7 = tmp(7), eye = tmp(8), h = tmp(9);
        a.mov(diffuse, M_ALL, zero);
        a.mov(specular, M_ALL, zero);
        a.mov(ambient, M_ALL, cst(C_AMBIENT));
        if (k.specular && k.local_viewer)
            a.op(OP_NRM, eye, M_ALL, {pos}); // from the eye to the vertex
        for (uint32_t i = 0; i < k.lights; ++i) {
            const uint32_t b = C_LIGHT + 8 * i;
            const bool directional = k.light_type[i] == 3;
            R light = l;
            R att = one;
            if (directional) {
                light = cst(b + 4);
            } else {
                a.add(l, M_ALL, cst(b + 3), -pos);
                a.op(OP_DP3, t6, M_X, {l, l});   // d^2
                a.op(OP_RSQ, t6, M_Y, {t6.x()}); // 1 / d
                a.mul(l, M_XYZ, l, t6.y());      // L
                a.mul(t6, M_Z, t6.x(), t6.y());  // d
                a.mad(t7, M_X, cst(b + 5).y(), t6.z(), cst(b + 5).x());
                a.mad(t7, M_X, cst(b + 5).z(), t6.x(), t7.x()); // a0 + a1 d + a2 d^2
                a.op(OP_RCP, t7, M_X, {t7.x()});
                a.op(OP_SGE, t7, M_Y, {cst(b + 5).w(), t6.z()}); // within range
                a.mul(t7, M_X, t7.x(), t7.y());
                if (k.light_type[i] == 2) {
                    // Spot: 1 inside theta, 0 outside phi, the falloff power between.
                    a.op(OP_DP3, t7, M_Z, {l, cst(b + 4)});
                    a.add(t7, M_Z, t7.z(), -cst(b + 6).y());
                    a.mul(t7, M_Z, t7.z(), cst(b + 6).w(), true);
                    a.op(OP_POW, t7, M_Z, {t7.z(), cst(b + 6).z()});
                    a.mul(t7, M_X, t7.x(), t7.z());
                }
                att = t7.x();
            }
            a.op(OP_DP3, t6, M_W, {nrm, light});
            a.op(OP_MAX, t6, M_W, {t6.w(), zero}); // N.L
            if (directional) {
                a.mad(diffuse, M_ALL, cst(b), t6.w(), diffuse);
                a.add(ambient, M_ALL, ambient, cst(b + 2));
            } else {
                a.mul(t7, M_W, t6.w(), att);
                a.mad(diffuse, M_ALL, cst(b), t7.w(), diffuse);
                a.mad(ambient, M_ALL, cst(b + 2), att, ambient);
            }
            if (k.specular) {
                // Blinn-Phong with the halfway vector, as Direct3D lights.
                if (k.local_viewer)
                    a.add(h, M_ALL, light, -eye);
                else
                    a.add(h, M_ALL, light, cst(C_VIEWDIR));
                a.mov(h, M_W, zero);
                a.op(OP_NRM, h, M_ALL, {h});
                a.op(OP_DP3, t6, M_X, {nrm, h});
                a.op(OP_MAX, t6, M_X, {t6.x(), zero});
                a.op(OP_POW, t6, M_X, {t6.x(), cst(C_POWER).x()});
                a.op(OP_SLT, t6, M_Y, {zero, t6.w()}); // only where the light reaches
                a.mul(t6, M_X, t6.x(), t6.y());
                a.mul(t6, M_X, t6.x(), cst(C_POWER).y());
                if (!directional)
                    a.mul(t6, M_X, t6.x(), att);
                a.mad(specular, M_ALL, cst(b + 1), t6.x(), specular);
            }
        }
        const R color = tmp(10);
        a.mul(color, M_ALL, ma, ambient);
        a.mad(color, M_ALL, md, diffuse, color);
        a.add(color, M_XYZ, color, me);
        a.mov(color, M_W, md);
        a.mov(od0, M_ALL, color);
        if (k.specular) {
            a.mul(tmp(11), M_XYZ, ms, specular);
            a.mov(tmp(11), M_W, ms);
            a.mov(od1, M_ALL, tmp(11));
        } else {
            a.mov(od1, M_ALL, zero);
        }
    } else {
        a.mov(od0, M_ALL, k.has_diffuse ? in(V_DIFFUSE) : one);
        a.mov(od1, M_ALL, k.has_specular ? in(V_SPECULAR) : zero);
    }

    // Fog.
    if (k.fog) {
        const R f = tmp(6);
        if (k.range_fog) {
            a.op(OP_DP3, f, M_X, {pos, pos});
            a.op(OP_RSQ, f, M_Y, {f.x()});
            a.mul(f, M_X, f.x(), f.y());
        } else {
            a.mov(f, M_X, pos.z());
        }
        if (k.fog == 1) {
            a.add(f, M_X, cst(C_FOG).y(), -f.x());
            a.mul(f, M_X, f.x(), cst(C_FOG).w());
        } else {
            a.mul(f, M_X, f.x(), cst(C_FOG).z());
            if (k.fog == 3)
                a.mul(f, M_X, f.x(), f.x());
            a.mul(f, M_X, f.x(), cst(C_LIT2).y());
            a.op(OP_EXP, f, M_X, {-f.x()});
        }
        a.mov(ofog, M_X, f.x());
    } else if (k.has_specular) {
        a.mov(ofog, M_X, in(V_SPECULAR).w()); // the fog factor travels in specular alpha
    }
    if (k.has_psize)
        a.mov(opts, M_X, in(V_PSIZE).x());

    // Texture coordinates, one set per stage.
    for (uint32_t s = 0; s < k.stages; ++s) {
        const R out = reg(T_TEXCRDOUT, s);
        R src;
        uint32_t n = 3;
        const uint32_t gen = k.tci_gen[s];
        if (gen == 0) {
            const uint32_t set = k.tci_index[s];
            if (!k.tex_dims[set]) {
                a.mov(out, M_ALL, zero);
                continue;
            }
            src = in(V_TEX + set);
            n = k.tex_dims[set];
        } else if (gen == 1) { // CAMERASPACENORMAL
            src = nrm;
        } else if (gen == 2) { // CAMERASPACEPOSITION
            src = pos;
        } else { // CAMERASPACEREFLECTIONVECTOR, SPHEREMAP
            const R e = tmp(13), r = tmp(14), t = tmp(12);
            a.op(OP_NRM, e, M_ALL, {pos});
            a.op(OP_DP3, t, M_X, {nrm, e});
            a.mul(t, M_X, t.x(), two);
            a.mad(r, M_ALL, nrm, -t.x(), e); // E - 2 (N.E) N
            a.mov(r, M_W, one);
            if (gen == 4) {
                const R m = tmp(15);
                a.mov(m, M_ALL, r);
                a.add(m, M_Z, r, one);
                a.op(OP_DP3, m, M_W, {m, m});
                a.op(OP_RSQ, m, M_W, {m.w()});
                a.mul(m, M_W, m.w(), half);
                a.mad(r, M_X | M_Y, r, m.w(), half);
                a.mov(r, M_Z, zero);
                n = 2;
            }
            src = r;
        }
        const uint32_t count = k.ttf_count[s];
        if (!count) {
            a.mov(out, M_ALL, src);
            continue;
        }
        // The transform reads the coordinate with a 1 after its last component,
        // so the row after the ones it uses is the translation.
        const R v = tmp(12), o = tmp(13);
        a.mov(v, M_ALL, zero);
        a.mov(v, (1u << n) - 1, src);
        if (n < 4)
            a.mov(v, 1u << n, one);
        for (uint32_t j = 0; j < 4; ++j)
            a.op(OP_DP4, o, 1u << j, {v, cst(C_TEXM + 4 * s + j)});
        if (k.ttf_projected[s] && count < 4)
            a.mov(o, M_W, o.c(count - 1)); // the pixel half divides by w
        a.mov(out, M_ALL, o);
    }
    return a.finish();
}

// D3DDECLTYPE components, for the texture coordinate sets a declaration holds.
uint8_t components(uint8_t type) {
    static const uint8_t n[17] = {1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4};
    return type < 17 ? n[type] : 0;
}

void fill_vertex_constants(const D9Pipeline &pl, const FixedState &fs, const VsKey &k,
                           const int32_t vp[4], const float depth[2], float (*c)[4]) {
    memset(c, 0, sizeof(float) * 4 * VS_CONSTANTS);
    const M4 world = transform(pl, fs, 24), view = transform(pl, fs, 2),
             proj = transform(pl, fs, 3);
    const M4 wv = mul(world, view);
    put_columns(c + C_WVP, mul(wv, proj), 4);
    put_columns(c + C_WV, wv, 4);
    // The normal matrix is the inverse transpose of world x view's 3x3; its
    // columns are the inverse's rows.
    const float *m = wv.m;
    const float a00 = m[0], a01 = m[1], a02 = m[2], a10 = m[4], a11 = m[5], a12 = m[6], a20 = m[8],
                a21 = m[9], a22 = m[10];
    const float c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22,
                c02 = a10 * a21 - a11 * a20;
    const float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (std::fabs(det) > 1e-20f) {
        const float inv = 1.0f / det;
        // Row i of the inverse is column i of the cofactor matrix over det.
        const float r0[3] = {c00 * inv, (a02 * a21 - a01 * a22) * inv,
                             (a01 * a12 - a02 * a11) * inv};
        const float r1[3] = {c01 * inv, (a00 * a22 - a02 * a20) * inv,
                             (a02 * a10 - a00 * a12) * inv};
        const float r2[3] = {c02 * inv, (a01 * a20 - a00 * a21) * inv,
                             (a00 * a11 - a01 * a10) * inv};
        // n' = n * inverse^T: component j is n . (row j of inverse^T) = n . (column j of inverse).
        for (int j = 0; j < 3; ++j)
            put4(c[C_NORMAL + j], r0[j], r1[j], r2[j], 0.0f);
    } else {
        for (int j = 0; j < 3; ++j)
            put4(c[C_NORMAL + j], m[j], m[4 + j], m[8 + j], 0.0f);
    }
    for (int i = 0; i < 4; ++i) {
        memcpy(c[C_MAT_D + i], fs.material + 4 * i, 16);
    }
    const float power = fs.material[16];
    put4(c[C_POWER], power > 0.0f ? power : 1.0f, power > 0.0f ? 1.0f : 0.0f, 0.0f, 0.0f);
    put_color(c[C_AMBIENT], pl.rs[RS_AMBIENT]);
    const float start = as_float(pl.rs[RS_FOGSTART]), end = as_float(pl.rs[RS_FOGEND]);
    put4(c[C_FOG], start, end, as_float(pl.rs[RS_FOGDENSITY]),
         end != start ? 1.0f / (end - start) : 0.0f);
    const float w = vp[2] ? (float)vp[2] : 1.0f, h = vp[3] ? (float)vp[3] : 1.0f;
    put4(c[C_SCREEN], 2.0f / w, -2.0f / h, -1.0f - 2.0f * vp[0] / w, 1.0f + 2.0f * vp[1] / h);
    const float range = depth[1] - depth[0];
    put4(c[C_DEPTH], range != 0.0f ? 1.0f / range : 0.0f, range != 0.0f ? -depth[0] / range : 0.0f,
         0.0f, 0.0f);
    for (uint32_t i = 0; i < k.lights; ++i) {
        auto it = fs.lights.find(fs.active_lights[i]);
        if (it == fs.lights.end())
            continue;
        const float *f = it->second.f;
        float (*l)[4] = c + C_LIGHT + 8 * i;
        memcpy(l[0], f + 1, 16); // diffuse
        memcpy(l[1], f + 5, 16); // specular
        memcpy(l[2], f + 9, 16); // ambient
        const float *p = f + 13, *d = f + 16;
        for (int j = 0; j < 3; ++j)
            l[3][j] =
                p[0] * view.m[j] + p[1] * view.m[4 + j] + p[2] * view.m[8 + j] + view.m[12 + j];
        l[3][3] = 1.0f;
        float dir[3];
        for (int j = 0; j < 3; ++j)
            dir[j] = -(d[0] * view.m[j] + d[1] * view.m[4 + j] + d[2] * view.m[8 + j]);
        const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int j = 0; j < 3; ++j)
            l[4][j] = len > 0.0f ? dir[j] / len : 0.0f; // towards the light
        put4(l[5], f[21], f[22], f[23], f[19]);         // attenuation, range
        const float ct = std::cos(f[24] * 0.5f), cp = std::cos(f[25] * 0.5f);
        put4(l[6], ct, cp, std::max(f[20], 1e-3f), ct > cp ? 1.0f / (ct - cp) : 1e6f);
    }
    for (uint32_t s = 0; s < 8; ++s)
        put_columns(c + C_TEXM + 4 * s, transform(pl, fs, 16 + s), 4);
}

// ---- The pixel half ----------------------------------------------------------
enum : uint32_t { P_TFACTOR = 0, P_STAGE = 1, P_LIT = 31 };

struct PsKey {
    uint8_t stages, specular, temp;
    uint8_t cop[8], carg[8][3], aop[8], aarg[8][3], result[8], texture[8], projected[8];
};

// Whether a stage's operation reads its texture.
bool reads_texture(uint8_t op, const uint8_t arg[3]) {
    if (op == TOP_BLENDTEXTUREALPHA || op == TOP_BLENDTEXTUREALPHAPM)
        return true;
    for (int i = 0; i < 3; ++i)
        if ((arg[i] & TA_SELECTMASK) == TA_TEXTURE)
            return true;
    return false;
}

struct PixelGen {
    const PsKey &k;
    Asm &a;
    R zero = cst(P_LIT).x(), half = cst(P_LIT).y(), one = cst(P_LIT).z(), two = cst(P_LIT).w();

    R texture(uint32_t s) const {
        return k.texture[s] ? tmp(16 + s) : one; // an unbound stage reads white
    }
    // One argument of stage `s`, with its modifiers; a complement goes
    // through `scratch`.
    R arg(uint8_t v, uint32_t s, uint32_t scratch) {
        R r;
        switch (v & TA_SELECTMASK) {
        case TA_DIFFUSE:
            r = in(0);
            break;
        case TA_CURRENT:
            r = tmp(0);
            break;
        case TA_TEXTURE:
            r = texture(s);
            break;
        case TA_TFACTOR:
            r = cst(P_TFACTOR);
            break;
        case TA_SPECULAR:
            r = in(1);
            break;
        case TA_TEMP:
            r = tmp(1);
            break;
        case TA_CONSTANT:
            r = cst(P_STAGE + s);
            break;
        default:
            r = zero;
            break;
        }
        if (v & TA_ALPHAREPLICATE)
            r = r.w();
        if (v & TA_COMPLEMENT) {
            a.add(tmp(scratch), M_ALL, one, -r);
            r = tmp(scratch);
        }
        return r;
    }
    // One D3DTOP into `d` (mask `m`); `color` is false for the alpha half.
    void top(uint32_t op, uint32_t s, const R &d, uint32_t m, const R &a0, const R &a1, const R &a2,
             bool color) {
        using namespace d9sh;
        const R t8 = tmp(8), t9 = tmp(9);
        switch (op) {
        case TOP_SELECTARG1:
            a.mov(d, m, a1, true);
            return;
        case TOP_SELECTARG2:
            a.mov(d, m, a2, true);
            return;
        case TOP_MODULATE:
            a.mul(d, m, a1, a2, true);
            return;
        case TOP_MODULATE2X:
            a.mul(t8, M_ALL, a1, a2);
            a.add(d, m, t8, t8, true);
            return;
        case TOP_MODULATE4X:
            a.mul(t8, M_ALL, a1, a2);
            a.add(t8, M_ALL, t8, t8);
            a.add(d, m, t8, t8, true);
            return;
        case TOP_ADD:
            a.add(d, m, a1, a2, true);
            return;
        case TOP_ADDSIGNED:
            a.add(t8, M_ALL, a1, a2);
            a.add(d, m, t8, -half, true);
            return;
        case TOP_ADDSIGNED2X:
            a.add(t8, M_ALL, a1, a2);
            a.add(t8, M_ALL, t8, -half);
            a.add(d, m, t8, t8, true);
            return;
        case TOP_SUBTRACT:
            a.add(d, m, a1, -a2, true);
            return;
        case TOP_ADDSMOOTH:
            a.add(t8, M_ALL, one, -a1);
            a.mad(d, m, a2, t8, a1, true);
            return;
        case TOP_BLENDDIFFUSEALPHA:
            a.lrp(d, m, in(0).w(), a1, a2, true);
            return;
        case TOP_BLENDTEXTUREALPHA:
            a.lrp(d, m, texture(s).w(), a1, a2, true);
            return;
        case TOP_BLENDFACTORALPHA:
            a.lrp(d, m, cst(P_TFACTOR).w(), a1, a2, true);
            return;
        case TOP_BLENDTEXTUREALPHAPM:
            a.add(t8, M_ALL, one, -texture(s).w());
            a.mad(d, m, a2, t8, a1, true);
            return;
        case TOP_BLENDCURRENTALPHA:
            a.lrp(d, m, tmp(0).w(), a1, a2, true);
            return;
        case TOP_MODULATEALPHA_ADDCOLOR:
            if (color) {
                a.mad(d, m, a1.w(), a2, a1, true);
                return;
            }
            break;
        case TOP_MODULATECOLOR_ADDALPHA:
            if (color) {
                a.mad(d, m, a1, a2, a1.w(), true);
                return;
            }
            break;
        case TOP_MODULATEINVALPHA_ADDCOLOR:
            if (color) {
                a.add(t8, M_ALL, one, -a1.w());
                a.mad(d, m, t8, a2, a1, true);
                return;
            }
            break;
        case TOP_MODULATEINVCOLOR_ADDALPHA:
            if (color) {
                a.add(t8, M_ALL, one, -a1);
                a.mad(d, m, t8, a2, a1.w(), true);
                return;
            }
            break;
        case TOP_BUMPENVMAP:
        case TOP_BUMPENVMAPLUMINANCE:
            // The perturbation feeds the next stage's lookup, which this
            // cascade does not model: the stage leaves the colour as it was.
            log_once(
                "d3d9.ffp.bumpenv",
                "d3d9: fixed-function bump mapping (D3DTOP_BUMPENVMAP) is drawn without the bump");
            a.mov(d, m, tmp(0), true);
            return;
        case TOP_DOTPRODUCT3:
            a.mad(t8, M_ALL, a1, two, -one);
            a.mad(t9, M_ALL, a2, two, -one);
            a.op(OP_DP3, d, color ? M_ALL : m, {t8, t9}, true);
            return;
        case TOP_MULTIPLYADD:
            a.mad(d, m, a1, a2, a0, true);
            return;
        case TOP_LERP:
            a.lrp(d, m, a0, a1, a2, true);
            return;
        default:
            break;
        }
        // PREMODULATE and anything unknown: modulate, the most common intent.
        a.mul(d, m, a1, a2, true);
    }
};

std::vector<uint8_t> build_pixel(const PsKey &k) {
    using namespace d9sh;
    Asm a(0xffff0200u); // ps_2_0
    a.def(P_LIT, 0.0f, 0.5f, 1.0f, 2.0f);
    a.dcl(0, 0, in(0));
    a.dcl(0, 0, in(1));
    for (uint32_t s = 0; s < k.stages; ++s)
        if (k.texture[s]) {
            a.dcl(0, 0, reg(T_TEXTURE, s));
            a.dcl_sampler(k.texture[s], s);
        }
    for (uint32_t s = 0; s < k.stages; ++s)
        if (k.texture[s])
            a.op(OP_TEX, tmp(16 + s), M_ALL, {reg(T_TEXTURE, s), reg(T_SAMPLER, s)}, false,
                 k.projected[s] ? 1u : 0u);
    PixelGen g{k, a};
    a.mov(tmp(0), M_ALL, in(0)); // CURRENT starts as the diffuse colour
    if (k.temp)
        a.mov(tmp(1), M_ALL, g.zero);
    for (uint32_t s = 0; s < k.stages; ++s) {
        const R d = k.result[s] == TA_TEMP ? tmp(1) : tmp(0);
        const R c0 = g.arg(k.carg[s][0], s, 2), c1 = g.arg(k.carg[s][1], s, 3),
                c2 = g.arg(k.carg[s][2], s, 4);
        // Both halves read the stage's inputs; the colour half writes only
        // rgb, so the alpha half still sees the alpha it started with.
        g.top(k.cop[s], s, d, M_XYZ, c0, c1, c2, true);
        if (k.cop[s] == TOP_DOTPRODUCT3 || k.aop[s] == TOP_DISABLE)
            continue; // DOTPRODUCT3 fills alpha too; a disabled alpha keeps it
        const R x0 = g.arg(k.aarg[s][0], s, 5), x1 = g.arg(k.aarg[s][1], s, 6),
                x2 = g.arg(k.aarg[s][2], s, 7);
        g.top(k.aop[s], s, d, M_W, x0, x1, x2, false);
    }
    if (k.specular)
        a.add(tmp(0), M_XYZ, tmp(0), in(1), true);
    a.mov(reg(T_COLOROUT, 0), M_ALL, tmp(0));
    return a.finish();
}

} // namespace

// ---- Public -------------------------------------------------------------------
bool fvf_declaration(uint32_t fvf, std::vector<uint8_t> *decl) {
    std::vector<uint8_t> d;
    uint32_t offset = 0;
    auto element = [&](uint8_t type, uint8_t usage, uint8_t index, uint32_t bytes) {
        const uint8_t e[8] = {0, 0, (uint8_t)offset, (uint8_t)(offset >> 8), type, 0, usage, index};
        d.insert(d.end(), e, e + 8);
        offset += bytes;
    };
    const uint32_t position = fvf & 0x400e;
    if (position == 0x002) { // XYZ
        element(2, U_POSITION, 0, 12);
    } else if (position == 0x004) { // XYZRHW
        element(3, U_POSITIONT, 0, 16);
    } else if (position == 0x4002) { // XYZW
        element(3, U_POSITION, 0, 16);
    } else if (position >= 0x006 && position <= 0x00e) { // XYZB1..XYZB5
        element(2, U_POSITION, 0, 12);
        uint32_t betas = (position - 0x006) / 2 + 1;
        const bool last_indices = (fvf & 0x9000) != 0; // LASTBETA_UBYTE4 or LASTBETA_D3DCOLOR
        const uint32_t weights = last_indices ? betas - 1 : betas;
        if (weights) // five weights do not fit one element; the vertex keeps its layout
            element((uint8_t)(std::min<uint32_t>(weights, 4) - 1), U_BLENDWEIGHT, 0, 4 * weights);
        if (last_indices)
            element((fvf & 0x8000) ? 4 : 5, U_BLENDINDICES, 0, 4);
    } else {
        return false;
    }
    if (fvf & 0x010)
        element(2, U_NORMAL, 0, 12);
    if (fvf & 0x020)
        element(0, U_PSIZE, 0, 4);
    if (fvf & 0x040)
        element(4, U_COLOR, 0, 4);
    if (fvf & 0x080)
        element(4, U_COLOR, 1, 4);
    const uint32_t sets = std::min<uint32_t>((fvf >> 8) & 0xf, 8);
    for (uint32_t i = 0; i < sets; ++i) {
        // D3DFVF_TEXCOORDSIZEn: 0 two floats, 1 three, 2 four, 3 one.
        static const uint8_t floats[4] = {2, 3, 4, 1};
        const uint8_t n = floats[(fvf >> (16 + 2 * i)) & 3];
        element((uint8_t)(n - 1), U_TEXCOORD, (uint8_t)i, 4u * n);
    }
    const uint8_t end[8] = {0xff, 0, 0, 0, 17, 0, 0, 0};
    d.insert(d.end(), end, end + 8);
    *decl = std::move(d);
    return true;
}

uint32_t active_stages(const D9Pipeline &pl) {
    uint32_t n = 0;
    while (n < 8 && pl.tss[n][TSS_COLOROP] != TOP_DISABLE)
        ++n;
    return n;
}

Program &vertex_program(const D9Pipeline &pl, const FixedState &fs, const uint8_t *decl,
                        uint32_t decl_size, const int32_t viewport[4], const float depth[2],
                        bool programmable_ps, float (*constants)[4]) {
    VsKey k;
    memset(&k, 0, sizeof k);
    bool blend = false;
    for (uint32_t i = 0; i + 8 <= decl_size; i += 8) {
        const uint8_t *e = decl + i;
        if (e[0] == 0xff && e[1] == 0)
            break;
        const uint8_t type = e[4], usage = e[6], index = e[7];
        switch (usage) {
        case U_POSITIONT:
            k.pretransformed = index == 0;
            break;
        case U_NORMAL:
            k.has_normal |= index == 0;
            break;
        case U_COLOR:
            if (index == 0)
                k.has_diffuse = 1;
            else if (index == 1)
                k.has_specular = 1;
            break;
        case U_PSIZE:
            k.has_psize = 1;
            break;
        case U_TEXCOORD:
            if (index < 8)
                k.tex_dims[index] = components(type);
            break;
        case U_BLENDWEIGHT:
        case U_BLENDINDICES:
            blend = true;
            break;
        default:
            break;
        }
    }
    if (blend && pl.rs[RS_VERTEXBLEND])
        log_once("d3d9.ffp.blend",
                 "d3d9: fixed-function vertex blending (D3DRS_VERTEXBLEND) is drawn with the world "
                 "matrix alone");
    k.lighting = !k.pretransformed && pl.rs[RS_LIGHTING] != 0;
    k.specular = pl.rs[RS_SPECULARENABLE] != 0;
    if (k.lighting) {
        k.local_viewer = pl.rs[RS_LOCALVIEWER] != 0;
        k.normalize = pl.rs[RS_NORMALIZENORMALS] != 0;
        const bool vertex_colors = pl.rs[RS_COLORVERTEX] != 0;
        const uint32_t states[4] = {RS_DIFFUSEMATERIALSOURCE, RS_AMBIENTMATERIALSOURCE,
                                    RS_SPECULARMATERIALSOURCE, RS_EMISSIVEMATERIALSOURCE};
        for (int i = 0; i < 4; ++i) {
            const uint32_t src = vertex_colors ? pl.rs[states[i]] : 0;
            k.mat_src[i] = (src == 1 && k.has_diffuse) ? 1 : (src == 2 && k.has_specular) ? 2 : 0;
        }
        k.lights = (uint8_t)std::min<uint32_t>(fs.active_light_count, 8);
        for (uint32_t i = 0; i < k.lights; ++i) {
            auto it = fs.lights.find(fs.active_lights[i]);
            const uint32_t type = it != fs.lights.end() ? it->second.type : 3;
            k.light_type[i] = (uint8_t)(type >= 1 && type <= 3 ? type : 3);
        }
    }
    if (!k.pretransformed && pl.rs[RS_FOGENABLE] && pl.rs[RS_FOGTABLEMODE] == 0) {
        // D3DFOGMODE: 1 exp, 2 exp2, 3 linear.
        const uint32_t mode = pl.rs[RS_FOGVERTEXMODE];
        k.fog = mode == 3 ? 1 : mode == 1 ? 2 : mode == 2 ? 3 : 0;
        k.range_fog = k.fog && pl.rs[RS_RANGEFOGENABLE] != 0;
    }
    k.stages = (uint8_t)(programmable_ps ? 8 : active_stages(pl));
    for (uint32_t s = 0; s < k.stages; ++s) {
        const uint32_t tci = pl.tss[s][TSS_TEXCOORDINDEX],
                       ttf = pl.tss[s][TSS_TEXTURETRANSFORMFLAGS];
        k.tci_index[s] = (uint8_t)(tci & 7);
        k.tci_gen[s] = k.pretransformed ? 0 : (uint8_t)std::min<uint32_t>((tci >> 16) & 0xf, 4);
        k.ttf_count[s] = k.pretransformed ? 0 : (uint8_t)std::min<uint32_t>(ttf & 0xff, 4);
        k.ttf_projected[s] = (ttf & 0x100) != 0;
    }
    fill_vertex_constants(pl, fs, k, viewport, depth, constants);
    return cached(k, build_vertex);
}

Program &pixel_program(const D9Pipeline &pl, const FixedState &fs, const Textures &textures,
                       float (*constants)[4]) {
    PsKey k;
    memset(&k, 0, sizeof k);
    k.stages = (uint8_t)active_stages(pl);
    k.specular = pl.rs[RS_SPECULARENABLE] != 0;
    for (uint32_t s = 0; s < k.stages; ++s) {
        k.cop[s] = (uint8_t)pl.tss[s][TSS_COLOROP];
        k.carg[s][0] = (uint8_t)stage_state(pl, fs, s, TSS_COLORARG0);
        k.carg[s][1] = (uint8_t)pl.tss[s][TSS_COLORARG1];
        k.carg[s][2] = (uint8_t)pl.tss[s][TSS_COLORARG2];
        k.aop[s] = (uint8_t)pl.tss[s][TSS_ALPHAOP];
        k.aarg[s][0] = (uint8_t)stage_state(pl, fs, s, TSS_ALPHAARG0);
        k.aarg[s][1] = (uint8_t)pl.tss[s][TSS_ALPHAARG1];
        k.aarg[s][2] = (uint8_t)pl.tss[s][TSS_ALPHAARG2];
        k.result[s] = stage_state(pl, fs, s, TSS_RESULTARG) == TA_TEMP ? TA_TEMP : TA_CURRENT;
        const bool sampled = reads_texture(k.cop[s], k.carg[s]) ||
                             (k.aop[s] != TOP_DISABLE && reads_texture(k.aop[s], k.aarg[s]));
        k.texture[s] = sampled ? textures.kind[s] : 0;
        k.projected[s] = k.texture[s] && (pl.tss[s][TSS_TEXTURETRANSFORMFLAGS] & 0x100) != 0;
        for (int i = 0; i < 3; ++i)
            if ((k.carg[s][i] & TA_SELECTMASK) == TA_TEMP ||
                (k.aarg[s][i] & TA_SELECTMASK) == TA_TEMP)
                k.temp = 1;
        if (k.result[s] == TA_TEMP)
            k.temp = 1;
    }
    memset(constants, 0, sizeof(float) * 4 * PS_CONSTANTS);
    put_color(constants[P_TFACTOR], pl.rs[RS_TEXTUREFACTOR]);
    for (uint32_t s = 0; s < 8; ++s)
        put_color(constants[P_STAGE + s], pl.tss[s][TSS_CONSTANT]);
    return cached(k, build_pixel);
}

} // namespace d9ffp
