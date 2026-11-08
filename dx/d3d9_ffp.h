// d3d9_ffp.h - Direct3D 9's fixed-function pipeline, as shader bytecode.
//
// A draw with no vertex or pixel shader bound is drawn by what the device's
// state describes instead: the world, view and projection transforms, the
// material and the enabled lights, vertex fog, texture coordinate generation
// and transforms, and the texture stage cascade. Each half becomes a small
// vs_2_0 or ps_2_0 program, generated once per distinct state and then cached,
// so the GPU backends see an ordinary shader. Nothing downstream knows the
// program was not the game's.
//
// The state a program depends on is its key; what changes from draw to draw
// (matrices, colours, light positions) goes into constant registers, filled
// for every draw by the same calls.
//
// Adapted from the fixed-function generator of the HommaRe port's kit (MIT).
// The material, lights and which transforms and stage states the game has set
// live in FixedState here, beside D9Pipeline rather than inside it.
#pragma once
#include "d3d9_pipeline.h"

#include <cstdint>
#include <map>
#include <vector>

namespace d9ffp {

// Device state only the fixed-function pipeline reads.
struct FixedState {
    // D3DMATERIAL9: diffuse, ambient, specular, emissive (four floats each), power.
    float material[17] = {};
    // Every D3DLIGHT9 the game set, as its 26 dwords read as floats; the type
    // (the first field) is kept apart. active_lights lists the enabled ones in
    // the order LightEnable turned them on.
    struct Light {
        float f[26] = {};
        uint32_t type = 3; // D3DLIGHT_DIRECTIONAL
        bool enabled = false;
    };
    std::map<uint32_t, Light> lights;
    uint32_t active_lights[8] = {};
    uint32_t active_light_count = 0;
    uint32_t fvf = 0; // SetFVF's code; 0 once a declaration replaces it
    // D9Pipeline::transform slots the game has set; the others are the
    // identity, as Direct3D starts them.
    uint32_t transform_set = 0;
    // Per stage, the D3DTEXTURESTAGESTATETYPEs the game has set. COLORARG0,
    // ALPHAARG0 and RESULTARG start as D3DTA_CURRENT, which D9Pipeline does not
    // record.
    uint64_t tss_set[8] = {};
};

// The D3DVERTEXELEMENT9 records an FVF code describes, ending with D3DDECL_END,
// in the order Direct3D 9 lays the vertex out. False, with nothing written, for
// a code with no position.
bool fvf_declaration(uint32_t fvf, std::vector<uint8_t> *decl);

// One generated program: its bytecode (kept for the life of the process, so a
// draw may point at it) and d9sh::code_key of those bytes. `announced` is the
// caller's: whether the GPU host has been told about it.
struct Program {
    std::vector<uint8_t> bytes;
    uint64_t key = 0;
    bool announced = false;
};

// Constant registers each generated program reads.
constexpr uint32_t VS_CONSTANTS = 128, PS_CONSTANTS = 32;

// D3DSAMPLER_TEXTURE_TYPE of what is bound to a stage: 0 nothing, 2 a 2D
// texture, 3 a cube map.
struct Textures {
    uint8_t kind[8] = {};
};

// The vertex half for a draw through `decl` (D3DVERTEXELEMENT9 records) into
// `viewport` (x, y, width, height) and `depth` (MinZ, MaxZ). `programmable_ps`
// says a pixel shader is bound, which then reads every texture coordinate set;
// otherwise only the stages the cascade uses are written. Fills `constants`.
Program &vertex_program(const D9Pipeline &pl, const FixedState &fs, const uint8_t *decl,
                        uint32_t decl_size, const int32_t viewport[4], const float depth[2],
                        bool programmable_ps, float (*constants)[4]);

// The pixel half: the texture stage cascade over the bound textures, then the
// specular colour. Alpha test and fog are the renderer's, as for any ps_2_0
// program. Fills `constants`.
Program &pixel_program(const D9Pipeline &pl, const FixedState &fs, const Textures &textures,
                       float (*constants)[4]);

// How many texture stages the cascade uses: those before the first whose
// COLOROP is D3DTOP_DISABLE.
uint32_t active_stages(const D9Pipeline &pl);

} // namespace d9ffp
