// Direct3D 8 declarations and fixed-function state expressed as the common
// renderer's vertex declaration and shader bytecode. No guest pointers live here.
#pragma once
#include "d3d9_pipeline.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace d8 {
struct Attribute {
    uint16_t stream = 0, offset = 0;
    uint8_t type = 0, usage = 0, usage_index = 0, reg = 0;
};
struct Layout {
    std::vector<Attribute> attributes;
    std::vector<uint8_t> declaration;
    std::array<uint32_t, 8> stride{};
    bool transformed = false;
};

// Decode an FVF or a bounded D3DVSD token list. Unsupported encodings fail
// atomically and report why, rather than manufacturing a plausible layout.
bool fvf_layout(uint32_t fvf, Layout *out, std::string *why);
bool token_layout(const std::vector<uint32_t> &tokens, Layout *out,
                  std::vector<uint32_t> *constant_defs, std::string *why);

// Add semantic declarations to a version 1.1 program whose register bindings
// were supplied separately by Direct3D 8's vertex declaration.
bool vertex_shader(const Layout &layout, const std::vector<uint8_t> &original,
                   const std::vector<uint32_t> &constant_defs, std::vector<uint8_t> *out,
                   std::string *why);
struct FixedPrograms {
    std::vector<uint8_t> vertex, pixel;
    float vertex_constants[6][4] = {};
    float pixel_constants[3][4] = {};
};

// Generate the unlit fixed-function path as common shader model 2 programs.
// Dynamic matrices and viewport values stay outside the shader cache key.
uint32_t texture_operation_caps();
bool fixed_programs(const Layout &layout, const D9Pipeline &state, uint32_t width, uint32_t height,
                    FixedPrograms *out, std::string *why);
} // namespace d8
