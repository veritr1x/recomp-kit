// Direct3D 8 ABI over the kit's shared device, storage and renderer. A slot
// is forwarded only when its argument layout and implemented behavior match;
// differing structures and unsupported operations are handled here explicitly.
#include "dx.h"
#include "d3d9_api.h"
#include "d3d8_program.h"
#include "d3d9_raster.h"
#include "d3d9_shader.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr uint32_t INVALID = 0x8876086cu, UNAVAILABLE = 0x8876086au;
constexpr uint32_t MOREDATA = 0x88760867u;

struct Shader8 {
    d8::Layout layout;
    d8::PreparedShader prepared;
    std::vector<uint8_t> original, declaration;
};
struct Device8 {
    uint32_t creation[4] = {};
    uint32_t fvf = 0, declaration = 0, base_vertex = 0;
    d8::Layout layout;
    std::array<uint32_t, 8> textures{}, streams{};
    uint32_t indices = 0, color = 0, depth = 0;
    uint32_t automatic_depth = 0;
    uint32_t vertex_handle = 0, pixel_handle = 0, next_shader = 0x10001;
    std::map<uint32_t, std::shared_ptr<Shader8>> vertex_shaders, pixel_shaders;
    std::shared_ptr<Shader8> vertex, pixel;
    float vconst[96][4] = {}, pconst[8][4] = {};
};
std::map<uint32_t, Device8> &devices() {
    static auto *records = new std::map<uint32_t, Device8>();
    return *records;
}

ComObj *device8(X86 *c) {
    ComObj *o = com_this_arg(c);
    return o && o->kind == K_D3D9DEVICE && o->d3d_version == 8 ? o : nullptr;
}
bool output(uint32_t p, uint32_t size) {
    return p && gm_valid(p, size);
}
void replace_reference(uint32_t &slot, ComObj *next) {
    if (slot == (next ? next->id : 0))
        return;
    if (next)
        com_addref(next);
    ComObj *previous = com_get(slot);
    slot = next ? next->id : 0;
    if (previous)
        com_release(previous);
}

void shared(X86 *c, ComIface iface, const char *name) {
    // Call sites pass permanent string literals. Cache their resolved methods
    // per interface without allocating strings on the draw/setter path.
    static std::map<const char *, const ComMethod *> cache[IF_COUNT];
    auto &methods = cache[iface];
    auto found = methods.find(name);
    const ComMethod *method = found == methods.end() ? d9_api_method(iface, name) : found->second;
    if (found == methods.end())
        methods.emplace(name, method);
    if (!method || !method->fn) {
        log_once(name, "d3d8: shared method %s is unavailable", name);
        com_ret(c, UNAVAILABLE);
        return;
    }
    method->fn(c);
}

// The frontend accepts the colour formats its shared renderer can draw, and
// the texture/depth formats supported by its storage. Volume resources and
// formats outside this list fail capability queries before device creation.
bool color_format(uint32_t format) {
    return format == 21 || format == 22;
}
bool depth_format(uint32_t format) {
    return format == 70 || format == 71 || format == 73 || format == 75 || format == 77 ||
           format == 79 || format == 80;
}
bool texture_format(uint32_t format) {
    switch (format) {
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 50:
    case 51:
    case 52:
    case 60:
    case 61:
    case 0x31545844u:
    case 0x33545844u:
    case 0x35545844u:
        return true;
    default:
        return false;
    }
}

bool presentation(uint32_t p) {
    return output(p, 52) && rd32(p) <= 4096 && rd32(p + 4) <= 4096 &&
           (!rd32(p + 8) || color_format(rd32(p + 8))) && rd32(p + 12) <= 1 &&
           (rd32(p + 16) == 0 || rd32(p + 16) == 2 || rd32(p + 16) == 4) &&
           (!rd32(p + 32) || depth_format(rd32(p + 36)));
}

void D8_Create(X86 *c) {
    ComObj *factory = com_new(K_D3D9);
    factory->d3d_version = 8;
    uint32_t view = d9_api_view(factory, IF_D3D9);
    if (!view)
        com_release(factory);
    set_eax(c, view);
}
void D8_CheckDeviceType(X86 *c) {
    com_ret(c, !arg(c, 1) && arg(c, 2) == 1 && color_format(arg(c, 3)) && color_format(arg(c, 4))
                   ? 0
                   : UNAVAILABLE);
}
void D8_CheckDeviceFormat(X86 *c) {
    const uint32_t usage = arg(c, 4), type = arg(c, 5), format = arg(c, 6);
    const bool allowed = !arg(c, 1) && arg(c, 2) == 1 && (type == 1 || type == 3 || type == 5) &&
                         ((usage & 2)   ? depth_format(format)
                          : (usage & 1) ? color_format(format)
                                        : texture_format(format));
    com_ret(c, allowed ? 0 : UNAVAILABLE);
}
void D8_CreateDevice(X86 *c) {
    const uint32_t p = arg(c, 5), out = arg(c, 6);
    if (!output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, 0);
    if (arg(c, 1) || arg(c, 2) != 1 || !presentation(p)) {
        com_ret(c, INVALID);
        return;
    }
    int32_t x = 0, y = 0, w = 640, h = 480;
    host_window_rect(rd32(p + 24) ? rd32(p + 24) : arg(c, 3), &x, &y, &w, &h);
    if (!rd32(p))
        wr32(p, uint32_t(std::max(w, 1)));
    if (!rd32(p + 4))
        wr32(p + 4, uint32_t(std::max(h, 1)));
    shared(c, IF_D3D9, "CreateDevice");
    if (c->r[R_EAX] != 0)
        return;
    ComObj *dev = com_this(rd32(out));
    if (!dev) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    state.automatic_depth = dev->zbuffer_obj;
    for (uint32_t i = 0; i < 4; ++i)
        state.creation[i] = arg(c, i + 1);
    D9Pipeline &pipeline = d9_pipeline(dev->id);
    pipeline = D9Pipeline{};
    for (uint32_t slot = 0; slot < 32; ++slot)
        for (uint32_t i = 0; i < 4; ++i)
            pipeline.transform[slot][i * 4 + i] = 1;
}
void D8_Reset(X86 *c) {
    ComObj *dev = device8(c);
    const uint32_t present = arg(c, 1);
    if (!dev || !presentation(present)) {
        com_ret(c, INVALID);
        return;
    }
    int32_t x = 0, y = 0, w = 640, h = 480;
    host_window_rect(dev->hwnd, &x, &y, &w, &h);
    if (!rd32(present))
        wr32(present, uint32_t(std::max(w, 1)));
    if (!rd32(present + 4))
        wr32(present + 4, uint32_t(std::max(h, 1)));
    Device8 &state = devices()[dev->id];
    replace_reference(state.color, nullptr);
    replace_reference(state.depth, nullptr);
    dev->zbuffer_obj = state.automatic_depth;
    shared(c, IF_D3DDEVICE9, "Reset");
    state.automatic_depth = dev->zbuffer_obj;
}
void D8_GetCreationParameters(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !output(out, 16)) {
        com_ret(c, INVALID);
        return;
    }
    for (uint32_t i = 0; i < 4; ++i)
        wr32(out + 4 * i, devices()[dev->id].creation[i]);
    com_ret(c, 0);
}
void D8_GetDisplayMode(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !output(out, 16)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, dev->width);
    wr32(out + 4, dev->height);
    wr32(out + 8, 60);
    wr32(out + 12, dev->d3d_format);
    com_ret(c, 0);
}

void create_surface8(X86 *c, uint32_t out_index, uint32_t usage, uint32_t pool) {
    ComObj *dev = device8(c);
    const uint32_t out = arg(c, out_index), w = arg(c, 1), h = arg(c, 2), fmt = arg(c, 3);
    const uint32_t samples = usage ? arg(c, 4) : 0;
    if (!dev || !output(out, 4) || !w || !h || w > 4096 || h > 4096 ||
        ((usage & 2)   ? !depth_format(fmt)
         : (usage & 1) ? !color_format(fmt)
                       : !texture_format(fmt)) ||
        (samples != 0 && samples != 2 && samples != 4)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, 0);
    ComObj *surface = d9_api_surface(dev, w, h, fmt, samples, usage, pool);
    if (!surface) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    uint32_t view = d9_api_view(surface, IF_D3DSURFACE9);
    if (!view)
        com_release(surface);
    wr32(out, view);
    com_ret(c, view ? 0 : E_OUTOFMEMORY);
}
void D8_CreateRenderTarget(X86 *c) {
    create_surface8(c, 6, 1, 0);
}
void D8_CreateDepthStencilSurface(X86 *c) {
    create_surface8(c, 5, 2, 0);
}
void D8_CreateImageSurface(X86 *c) {
    create_surface8(c, 4, 0, 2);
}

void surface_desc(ComObj *surface, uint32_t out) {
    gm_zero(out, 32);
    wr32(out, surface->rmask);
    wr32(out + 4, 1);
    wr32(out + 8, surface->d3d_usage);
    wr32(out + 12, surface->d3d_pool);
    wr32(out + 16, uint32_t(surface->blob.size()));
    wr32(out + 20, surface->samples > 1 ? surface->samples : 0);
    wr32(out + 24, surface->width);
    wr32(out + 28, surface->height);
}
void D8_GetDesc(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t out = arg(c, 1);
    uint32_t size = o && o->kind == K_D3D9SURFACE ? 32 : o && o->kind == K_D3D9VB ? 24 : 20;
    if (!o || !output(out, size)) {
        com_ret(c, INVALID);
        return;
    }
    if (o->kind == K_D3D9SURFACE)
        surface_desc(o, out);
    else {
        gm_zero(out, size);
        wr32(out, o->kind == K_D3D9VB ? 100 : o->rmask == 4 ? 102 : 101);
        wr32(out + 4, o->kind == K_D3D9VB ? 6 : 7);
        wr32(out + 8, o->d3d_usage);
        wr32(out + 12, o->d3d_pool);
        wr32(out + 16, o->buf_bytes);
        if (size == 24)
            wr32(out + 20, o->d3d_fvf);
    }
    com_ret(c, 0);
}

// Only the same-version view may escape through GetContainer, and the
// requested IID must belong to the actual parent rather than a guessed type.
void D8_GetContainer(X86 *c) {
    ComObj *surface = com_this_arg(c);
    uint32_t iid = arg(c, 1), out = arg(c, 2);
    if (!surface || !output(iid, 16) || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, 0);
    ComObj *parent = com_get(surface->front_obj);
    if (!parent)
        parent = com_get(surface->dev_d3d);
    const ComIface want = com_iface_for_iid(iid);
    static const uint8_t unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    uint32_t view = 0;
    if (parent && !std::memcmp(gm_ptr(iid), unknown, 16))
        view = parent->identity;
    else if (parent && want >= IF_D3D8 && want <= IF_D3DINDEXBUFFER8 &&
             com_iface_binds(want, parent->kind))
        view = com_view(parent, want);
    if (view) {
        com_addref(parent);
        wr32(out, view);
    }
    com_ret(c, view ? 0 : E_NOINTERFACE);
}

void D8_GetRenderTarget(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t out = arg(c, 1);
    ComObj *surface = dev ? com_get(dev->render_target) : nullptr;
    if (!surface || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    com_addref(surface);
    wr32(out, d9_api_view(surface, IF_D3DSURFACE9));
    com_ret(c, 0);
}
void D8_SetRenderTarget(X86 *c) {
    ComObj *dev = device8(c), *color = com_this(arg(c, 1)), *depth = com_this(arg(c, 2));
    if (!dev || (arg(c, 1) && !color) || (arg(c, 2) && !depth) ||
        (color && (color->kind != K_D3D9SURFACE || color->dev_d3d != dev->id)) ||
        (depth && (depth->kind != K_D3D9SURFACE || depth->dev_d3d != dev->id))) {
        com_ret(c, INVALID);
        return;
    }
    if (!color)
        color = com_get(dev->render_target);
    if (!color) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    replace_reference(state.color, color);
    replace_reference(state.depth, depth);
    dev->render_target = color->id;
    dev->zbuffer_obj = depth ? depth->id : 0;
    D9Pipeline &pipeline = d9_pipeline(dev->id);
    pipeline.color_target[0] = color->id;
    pipeline.viewport_set = false;
    pipeline.scissor[0] = pipeline.scissor[1] = 0;
    pipeline.scissor[2] = int32_t(color->width);
    pipeline.scissor[3] = int32_t(color->height);
    com_ret(c, 0);
}

void D8_SetStreamSource(X86 *c) {
    ComObj *dev = device8(c), *buffer = com_this(arg(c, 2));
    uint32_t stream = arg(c, 1), stride = arg(c, 3);
    if (!dev || stream >= 8 ||
        (arg(c, 2) && (!buffer || buffer->kind != K_D3D9VB || buffer->dev_d3d != dev->id)) ||
        stride > 255) {
        com_ret(c, INVALID);
        return;
    }
    replace_reference(devices()[dev->id].streams[stream], buffer);
    d9_api_stream(dev, stream, buffer, 0, stride);
    if (!stream && !buffer)
        dev->back_obj = dev->lock_off = dev->rate = 0;
    com_ret(c, 0);
}
void D8_GetStreamSource(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t stream = arg(c, 1), out = arg(c, 2), stride = arg(c, 3);
    if (!dev || stream >= 8 || !output(out, 4) || !output(stride, 4)) {
        com_ret(c, INVALID);
        return;
    }
    const auto &bound = d9_pipeline(dev->id).stream[stream];
    ComObj *buffer = com_get(bound.vb);
    wr32(out, buffer ? d9_api_view(buffer, IF_D3DVERTEXBUFFER9) : 0);
    wr32(stride, bound.stride);
    if (buffer)
        com_addref(buffer);
    com_ret(c, 0);
}
void D8_SetIndices(X86 *c) {
    ComObj *dev = device8(c), *buffer = com_this(arg(c, 1));
    if (!dev ||
        (arg(c, 1) && (!buffer || buffer->kind != K_D3D9IB || buffer->dev_d3d != dev->id))) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    replace_reference(state.indices, buffer);
    state.base_vertex = arg(c, 2);
    d9_api_indices(dev, buffer);
    com_ret(c, 0);
}
void D8_GetIndices(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t out = arg(c, 1), base = arg(c, 2);
    if (!dev || !output(out, 4) || !output(base, 4)) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    ComObj *buffer = com_get(state.indices);
    wr32(out, buffer ? d9_api_view(buffer, IF_D3DINDEXBUFFER9) : 0);
    wr32(base, state.base_vertex);
    if (buffer)
        com_addref(buffer);
    com_ret(c, 0);
}

void D8_SetTexture(X86 *c) {
    ComObj *dev = device8(c), *texture = com_this(arg(c, 2));
    uint32_t stage = arg(c, 1);
    if (!dev || stage >= 8 ||
        (arg(c, 2) &&
         (!texture || texture->kind != K_D3D9TEXTURE || texture->dev_d3d != dev->id))) {
        com_ret(c, INVALID);
        return;
    }
    replace_reference(devices()[dev->id].textures[stage], texture);
    shared(c, IF_D3DDEVICE9, "SetTexture");
}
void D8_GetTexture(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t stage = arg(c, 1), out = arg(c, 2);
    if (!dev || stage >= 8 || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    ComObj *texture = com_get(devices()[dev->id].textures[stage]);
    wr32(out,
         texture ? d9_api_view(texture, texture->caps ? IF_D3DCUBETEXTURE9 : IF_D3DTEXTURE9) : 0);
    if (texture)
        com_addref(texture);
    com_ret(c, 0);
}

uint32_t sampler_state8(uint32_t type) {
    static const uint8_t migration[] = {1, 2, 4, 5, 6, 7, 8, 9, 10};
    if (type >= 13 && type <= 21)
        return migration[type - 13];
    return type == 25 ? 3 : 0;
}
void D8_SetTextureStageState(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t stage = arg(c, 1), type = arg(c, 2), value = arg(c, 3);
    if (!dev || stage >= 8 || !type || type > 28 || type == 12) {
        com_ret(c, INVALID);
        return;
    }
    D9Pipeline &pipeline = d9_pipeline(dev->id);
    const uint32_t sampler = sampler_state8(type);
    if (sampler) {
        pipeline.sampler_state[stage][sampler] = value;
        pipeline.states_changed();
    } else
        pipeline.tss[stage][type] = value;
    com_ret(c, 0);
}
void D8_GetTextureStageState(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t stage = arg(c, 1), type = arg(c, 2), out = arg(c, 3);
    if (!dev || stage >= 8 || !type || type > 28 || type == 12 || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    const D9Pipeline &pipeline = d9_pipeline(dev->id);
    const uint32_t sampler = sampler_state8(type);
    wr32(out, sampler ? pipeline.sampler_state[stage][sampler] : pipeline.tss[stage][type]);
    com_ret(c, 0);
}
void D8_GetRenderState(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t state = arg(c, 1), out = arg(c, 2);
    if (!dev || state >= 256 || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, d9_pipeline(dev->id).rs[state]);
    com_ret(c, 0);
}

// A shader's extent is found by instructions, never by searching for END:
// a comment or floating point constant may contain exactly that bit pattern.
bool shader_bytes(uint32_t address, std::vector<uint8_t> *out) {
    if (!address || !gm_valid(address, 8))
        return false;
    const size_t available =
        size_t(std::min<uint64_t>(1024 * 1024, uint64_t(GUEST_SIZE) - address));
    size_t bytes = 0;
    std::string why;
    if (!d9sh::bytecode_extent(gm_ptr(address), available, &bytes, &why))
        return false;
    out->assign(gm_ptr(address), gm_ptr(address) + bytes);
    return true;
}
bool declaration_bytes(uint32_t address, std::vector<uint32_t> *tokens) {
    if (!address)
        return false;
    for (uint32_t at = 0; at < 4096;) {
        if (!gm_fits(address, uint64_t(at + 1) * 4))
            return false;
        const uint32_t token = rd32(address + at * 4);
        tokens->push_back(token);
        ++at;
        if (token == 0xffffffffu)
            return true;
        const uint32_t payload = (token >> 29) == 4 ? ((token >> 25) & 15) * 4 : 0;
        if (payload > 4096 - at || !gm_fits(address, uint64_t(at + payload) * 4))
            return false;
        for (uint32_t i = 0; i < payload; ++i, ++at)
            tokens->push_back(rd32(address + at * 4));
    }
    return false;
}

void D8_CreateVertexShader(X86 *c) {
    ComObj *dev = device8(c);
    const uint32_t out = arg(c, 3);
    if (!dev || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, 0);
    auto shader = std::make_shared<Shader8>();
    std::vector<uint32_t> tokens, defs;
    std::string why;
    if (!declaration_bytes(arg(c, 1), &tokens) ||
        !d8::token_layout(tokens, &shader->layout, &defs, &why) ||
        !shader_bytes(arg(c, 2), &shader->original) ||
        !d8::vertex_shader(shader->layout, shader->original, defs, &shader->prepared, &why)) {
        com_ret(c, INVALID);
        return;
    }
    shader->declaration.resize(tokens.size() * 4);
    std::memcpy(shader->declaration.data(), tokens.data(), tokens.size() * 4);
    Device8 &state = devices()[dev->id];
    const uint32_t handle = state.next_shader;
    state.next_shader += 2;
    state.vertex_shaders.emplace(handle, shader);
    wr32(out, handle);
    com_ret(c, 0);
}
void D8_CreatePixelShader(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t out = arg(c, 2);
    if (!dev || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, 0);
    auto shader = std::make_shared<Shader8>();
    std::string why;
    if (!shader_bytes(arg(c, 1), &shader->original) || shader->original[0] != 1 ||
        !d8::pixel_shader(shader->original, &shader->prepared, &why)) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    const uint32_t handle = state.next_shader;
    state.next_shader += 2;
    state.pixel_shaders.emplace(handle, shader);
    wr32(out, handle);
    com_ret(c, 0);
}

void bind_layout(ComObj *dev, Device8 &state, const d8::Layout &layout) {
    if (ComObj *previous = com_get(state.declaration))
        com_release(previous);
    ComObj *decl = com_new(K_D3D9DECL);
    decl->blob = layout.declaration;
    state.declaration = decl->id;
    state.layout = layout;
    dev->current_viewport = decl->id;
    d9_pipeline(dev->id).declaration = decl->id;
}
void D8_SetVertexShader(X86 *c) {
    ComObj *dev = device8(c);
    if (!dev) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    const uint32_t handle = arg(c, 1);
    if (!(handle & 1)) { // FVF bit zero is reserved; shader handles are always odd.
        d8::Layout layout;
        std::string why;
        if (!d8::fvf_layout(handle, &layout, &why)) {
            com_ret(c, INVALID);
            return;
        }
        bind_layout(dev, state, layout);
        state.vertex.reset();
        state.fvf = handle;
    } else {
        auto found = state.vertex_shaders.find(handle);
        if (found == state.vertex_shaders.end()) {
            com_ret(c, INVALID);
            return;
        }
        state.vertex = found->second;
        state.fvf = 0;
        bind_layout(dev, state, state.vertex->layout);
        for (const auto &constant : state.vertex->prepared.constants) {
            if (constant.first >= 96) {
                com_ret(c, INVALID);
                return;
            }
            std::copy(constant.second.begin(), constant.second.end(), state.vconst[constant.first]);
        }
    }
    state.vertex_handle = handle;
    com_ret(c, 0);
}
void D8_SetPixelShader(X86 *c) {
    ComObj *dev = device8(c);
    if (!dev) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    const uint32_t handle = arg(c, 1);
    auto found = state.pixel_shaders.find(handle);
    if (handle && found == state.pixel_shaders.end()) {
        com_ret(c, INVALID);
        return;
    }
    state.pixel = handle ? found->second : nullptr;
    if (state.pixel)
        for (const auto &constant : state.pixel->prepared.constants) {
            if (constant.first >= 8) {
                com_ret(c, INVALID);
                return;
            }
            std::copy(constant.second.begin(), constant.second.end(), state.pconst[constant.first]);
        }
    state.pixel_handle = handle;
    com_ret(c, 0);
}
void shader_handle(X86 *c, bool pixel, bool erase) {
    ComObj *dev = device8(c);
    if (!dev) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    auto &table = pixel ? state.pixel_shaders : state.vertex_shaders;
    if (erase) {
        // Bound objects retain their storage even after the public handle dies.
        com_ret(c, table.erase(arg(c, 1)) ? 0 : INVALID);
    } else if (output(arg(c, 1), 4)) {
        wr32(arg(c, 1), pixel ? state.pixel_handle : state.vertex_handle);
        com_ret(c, 0);
    } else
        com_ret(c, INVALID);
}
void D8_GetVertexShader(X86 *c) {
    shader_handle(c, false, false);
}
void D8_GetPixelShader(X86 *c) {
    shader_handle(c, true, false);
}
void D8_DeleteVertexShader(X86 *c) {
    shader_handle(c, false, true);
}
void D8_DeletePixelShader(X86 *c) {
    shader_handle(c, true, true);
}
void constants(X86 *c, bool pixel, bool get) {
    ComObj *dev = device8(c);
    const uint32_t first = arg(c, 1), data = arg(c, 2), count = arg(c, 3), limit = pixel ? 8 : 96;
    if (!dev || first > limit || count > limit - first || !data || !gm_fits_n(data, count, 16)) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    float (*table)[4] = pixel ? state.pconst : state.vconst;
    if (get)
        std::memcpy(gm_ptr(data), table + first, size_t(count) * 16);
    else
        std::memcpy(table + first, gm_ptr(data), size_t(count) * 16);
    com_ret(c, 0);
}
void D8_SetVertexShaderConstant(X86 *c) {
    constants(c, false, false);
}
void D8_GetVertexShaderConstant(X86 *c) {
    constants(c, false, true);
}
void D8_SetPixelShaderConstant(X86 *c) {
    constants(c, true, false);
}
void D8_GetPixelShaderConstant(X86 *c) {
    constants(c, true, true);
}
void shader_data(X86 *c, bool pixel, bool declaration) {
    ComObj *dev = device8(c);
    const uint32_t data = arg(c, 2), size = arg(c, 3);
    if (!dev || !output(size, 4)) {
        com_ret(c, INVALID);
        return;
    }
    Device8 &state = devices()[dev->id];
    auto &table = pixel ? state.pixel_shaders : state.vertex_shaders;
    auto found = table.find(arg(c, 1));
    if (found == table.end()) {
        com_ret(c, INVALID);
        return;
    }
    const auto &bytes = declaration ? found->second->declaration : found->second->original;
    const uint32_t capacity = rd32(size);
    wr32(size, uint32_t(bytes.size()));
    if (!data) {
        com_ret(c, 0);
        return;
    }
    if (capacity < bytes.size()) {
        com_ret(c, MOREDATA);
        return;
    }
    if (!output(data, uint32_t(bytes.size()))) {
        com_ret(c, INVALID);
        return;
    }
    std::memcpy(gm_ptr(data), bytes.data(), bytes.size());
    com_ret(c, 0);
}
void D8_GetVertexShaderDeclaration(X86 *c) {
    shader_data(c, false, true);
}
void D8_GetVertexShaderFunction(X86 *c) {
    shader_data(c, false, false);
}
void D8_GetPixelShaderFunction(X86 *c) {
    shader_data(c, true, false);
}

bool prepare_draw(ComObj *dev) {
    if (!dev)
        return false;
    Device8 &state = devices()[dev->id];
    if (!state.declaration)
        return false;
    D9Pipeline &pipeline = d9_pipeline(dev->id);
    std::memcpy(pipeline.vconst, state.vconst, sizeof state.vconst);
    std::memcpy(pipeline.pconst, state.pconst, sizeof state.pconst);
    d8::FixedPrograms fixed;
    if (!state.vertex || !state.pixel) {
        D9Pipeline generation = pipeline;
        // A custom half replaces the corresponding generated half entirely.
        if (state.vertex) {
            generation.rs[137] = generation.rs[28] = 0;
            for (uint32_t i = 0; i < 8; ++i) {
                generation.tss[i][11] = i;
                generation.tss[i][24] = 0;
            }
        }
        if (state.pixel) {
            generation.tss[0][1] = 1;
            generation.rs[29] = generation.rs[28] = 0;
        }
        std::string why;
        if (!d8::fixed_programs(state.layout, generation, dev->width, dev->height, &fixed, &why)) {
            log_once(why.c_str(), "d3d8: %s", why.c_str());
            return false;
        }
    }
    auto bind = [](D9ShaderBytes &slot, uint64_t &key, const std::vector<uint8_t> &bytes) {
        const uint64_t next = d9sh::code_key(bytes.data(), bytes.size());
        if (next != key || slot.vec() != bytes) {
            slot.bytes = std::make_shared<const std::vector<uint8_t>>(bytes);
            key = next;
        }
    };
    bind(pipeline.vs, pipeline.vs_key, state.vertex ? state.vertex->prepared.code : fixed.vertex);
    bind(pipeline.ps, pipeline.ps_key, state.pixel ? state.pixel->prepared.code : fixed.pixel);
    if (!state.vertex)
        std::memcpy(pipeline.vconst, fixed.vertex_constants, sizeof fixed.vertex_constants);
    if (!state.pixel)
        std::memcpy(pipeline.pconst, fixed.pixel_constants, sizeof fixed.pixel_constants);
    return true;
}
void D8_DrawPrimitive(X86 *c) {
    if (!prepare_draw(device8(c))) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, IF_D3DDEVICE9, "DrawPrimitive");
}
void D8_DrawIndexedPrimitive(X86 *c) {
    ComObj *dev = device8(c);
    if (!prepare_draw(dev)) {
        com_ret(c, INVALID);
        return;
    }
    d9_api_indexed_draw(dev, arg(c, 1), int32_t(devices()[dev->id].base_vertex), arg(c, 4),
                        arg(c, 5));
    com_ret(c, 0);
}
void D8_DrawPrimitiveUP(X86 *c) {
    ComObj *dev = device8(c);
    if (!prepare_draw(dev)) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, IF_D3DDEVICE9, "DrawPrimitiveUP");
    replace_reference(devices()[dev->id].streams[0], nullptr);
    d9_api_stream(dev, 0, nullptr, 0, 0);
    dev->back_obj = dev->lock_off = dev->rate = 0;
}
void D8_DrawIndexedPrimitiveUP(X86 *c) {
    ComObj *dev = device8(c);
    if (!prepare_draw(dev)) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, IF_D3DDEVICE9, "DrawIndexedPrimitiveUP");
    Device8 &state = devices()[dev->id];
    replace_reference(state.streams[0], nullptr);
    replace_reference(state.indices, nullptr);
    d9_api_stream(dev, 0, nullptr, 0, 0);
    d9_api_indices(dev, nullptr);
    dev->back_obj = dev->lock_off = dev->rate = 0;
}
void D8_ValidateDevice(X86 *c) {
    if (!output(arg(c, 1), 4) || !prepare_draw(device8(c))) {
        com_ret(c, UNAVAILABLE);
        return;
    }
    wr32(arg(c, 1), 1);
    com_ret(c, 0);
}

void D8_GetType(X86 *c) {
    ComObj *o = com_this_arg(c);
    set_eax(c, !o                         ? 0
               : o->kind == K_D3D9SURFACE ? 1
               : o->kind == K_D3D9TEXTURE ? (o->caps ? 5 : 3)
               : o->kind == K_D3D9VB      ? 6
               : o->kind == K_D3D9IB      ? 7
                                          : 0);
}
void D8_GetLevelDesc(X86 *c) {
    ComObj *tex = com_this_arg(c);
    const uint32_t level = arg(c, 1), out = arg(c, 2);
    const uint32_t levels = tex ? (tex->caps ? tex->pal_flags : uint32_t(tex->surfaces.size())) : 0;
    if (!tex || level >= levels || !output(out, 32)) {
        com_ret(c, INVALID);
        return;
    }
    ComObj desc = *tex;
    desc.width = std::max(tex->width >> level, 1u);
    desc.height = std::max(tex->height >> level, 1u);
    if (!tex->caps) {
        ComObj *surface = com_get(tex->surfaces[level]);
        if (!surface) {
            com_ret(c, INVALID);
            return;
        }
        desc.blob = surface->blob;
    }
    surface_desc(&desc, out);
    if (tex->caps) {
        const uint32_t block = tex->rmask == 0x31545844u                                  ? 8
                               : (tex->rmask == 0x33545844u || tex->rmask == 0x35545844u) ? 16
                                                                                          : 0;
        wr32(out + 16, block ? ((desc.width + 3) / 4) * ((desc.height + 3) / 4) * block
                             : desc.width * desc.height * (tex->bpp / 8));
    }
    com_ret(c, 0);
}
void D8_GetTransform(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t type = arg(c, 1), out = arg(c, 2);
    const uint32_t slot = type >= 256 && type < 260 ? 24 + type - 256 : type;
    if (!dev || slot >= 32 || !output(out, 64)) {
        com_ret(c, INVALID);
        return;
    }
    std::memcpy(gm_ptr(out), d9_pipeline(dev->id).transform[slot], 64);
    com_ret(c, 0);
}

void create_buffer(X86 *c, bool index) {
    uint32_t length = arg(c, 1), out = arg(c, 5);
    if (!device8(c) || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, 0);
    if (!length || length > 64 * 1024 * 1024 || arg(c, 4) > 3 ||
        (index && arg(c, 3) != 101 && arg(c, 3) != 102)) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, IF_D3DDEVICE9, index ? "CreateIndexBuffer" : "CreateVertexBuffer");
}
void D8_CreateVertexBuffer(X86 *c) {
    create_buffer(c, false);
}
void D8_CreateIndexBuffer(X86 *c) {
    create_buffer(c, true);
}
void create_texture(X86 *c, bool cube) {
    uint32_t w = arg(c, 1), h = cube ? w : arg(c, 2), level = arg(c, cube ? 2 : 3);
    uint32_t format = arg(c, cube ? 4 : 5), pool = arg(c, cube ? 5 : 6), out = arg(c, cube ? 6 : 7);
    if (!device8(c) || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    wr32(out, 0);
    uint32_t max_levels = 1;
    for (uint32_t size = std::max(w, h); size > 1; size >>= 1)
        ++max_levels;
    if (!w || !h || w > 4096 || h > 4096 || level > max_levels || pool > 3 ||
        !texture_format(format)) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, IF_D3DDEVICE9, cube ? "CreateCubeTexture" : "CreateTexture");
    if (cube && !c->r[R_EAX] && !level)
        if (ComObj *texture = com_this(rd32(out)))
            texture->pal_flags = max_levels;
}
void D8_CreateTexture(X86 *c) {
    create_texture(c, false);
}
void D8_CreateCubeTexture(X86 *c) {
    create_texture(c, true);
}

void D8_GetDepthStencilSurface(X86 *c) {
    ComObj *dev = device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !output(out, 4)) {
        com_ret(c, INVALID);
        return;
    }
    ComObj *surface = com_get(dev->zbuffer_obj);
    wr32(out, surface ? d9_api_view(surface, IF_D3DSURFACE9) : 0);
    if (surface)
        com_addref(surface);
    com_ret(c, surface ? 0 : 0x88760866u);
}

void D8_GetDeviceCaps(X86 *c) {
    ComObj *o = com_this_arg(c);
    const bool factory = o && o->kind == K_D3D9;
    uint32_t out = arg(c, factory ? 3 : 1);
    if (!o || !output(out, 212) || (factory && (arg(c, 1) || arg(c, 2) != 1))) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, factory ? IF_D3D9 : IF_D3DDEVICE9, "GetDeviceCaps");
}
void D8_GetAdapterIdentifier(X86 *c) {
    if (arg(c, 1) || !output(arg(c, 3), 1068)) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, IF_D3D9, "GetAdapterIdentifier");
}
void D8_CheckDepthStencilMatch(X86 *c) {
    com_ret(c, !arg(c, 1) && arg(c, 2) == 1 && color_format(arg(c, 3)) && color_format(arg(c, 4)) &&
                       depth_format(arg(c, 5))
                   ? 0
                   : UNAVAILABLE);
}
void D8_GetBackBuffer(X86 *c) {
    if (!device8(c) || arg(c, 1) || arg(c, 2) || !output(arg(c, 3), 4)) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, IF_D3DDEVICE9, "GetBackBuffer");
}
void D8_GetDevice(X86 *c) {
    if (!output(arg(c, 1), 4)) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, IF_D3DVERTEXBUFFER9, "GetDevice");
}
void D8_GetLevelCount(X86 *c) {
    ComObj *o = com_this_arg(c);
    set_eax(c, !o ? 0 : o->caps ? o->pal_flags : uint32_t(o->surfaces.size()));
}

void buffer_lock8(X86 *c) {
    ComObj *o = com_this_arg(c);
    const uint32_t offset = arg(c, 1), size = arg(c, 2), out = arg(c, 3);
    if (!o || !output(out, 4) || offset >= o->buf_bytes || size > o->buf_bytes - offset) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, o->kind == K_D3D9VB ? IF_D3DVERTEXBUFFER9 : IF_D3DINDEXBUFFER9, "Lock");
}

// The shared lock stages a whole level. Version 8 returns a pointer at the
// requested rectangle's origin while retaining the full-level row pitch.
void lock_rect8(X86 *c, ComIface iface) {
    ComObj *o = com_this_arg(c);
    const bool cube = iface == IF_D3DCUBETEXTURE9, surface = iface == IF_D3DSURFACE9;
    const uint32_t level = surface ? 0 : arg(c, cube ? 2 : 1);
    const uint32_t out = arg(c, surface ? 1
                                : cube  ? 3
                                        : 2),
                   rect = arg(c, surface ? 2
                                 : cube  ? 4
                                         : 3);
    if (!o || !output(out, 8) ||
        (!surface && level >= (cube ? o->pal_flags : o->surfaces.size())) ||
        (cube && arg(c, 1) > 5) || (rect && !output(rect, 16))) {
        com_ret(c, INVALID);
        return;
    }
    const uint32_t w = std::max(o->width >> level, 1u), h = std::max(o->height >> level, 1u);
    const uint32_t x = rect ? rd32(rect) : 0, y = rect ? rd32(rect + 4) : 0;
    const uint32_t right = rect ? rd32(rect + 8) : w, bottom = rect ? rd32(rect + 12) : h;
    const uint32_t block = o->rmask == 0x31545844u                                ? 8
                           : (o->rmask == 0x33545844u || o->rmask == 0x35545844u) ? 16
                                                                                  : 0;
    if (x >= right || y >= bottom || right > w || bottom > h ||
        (block && ((x | y) & 3 || (right != w && (right & 3)) || (bottom != h && (bottom & 3))))) {
        com_ret(c, INVALID);
        return;
    }
    shared(c, iface, "LockRect");
    if (!c->r[R_EAX])
        wr32(out + 4, rd32(out + 4) + (block ? (y / 4) * rd32(out) + (x / 4) * block
                                             : y * rd32(out) + x * (o->bpp / 8)));
}

void device_destroy(ComObj *dev) {
    d3d9_device_destroy(dev);
    auto found = devices().find(dev->id);
    if (found == devices().end())
        return;
    Device8 &state = found->second;
    for (auto &slot : state.textures)
        replace_reference(slot, nullptr);
    for (auto &slot : state.streams)
        replace_reference(slot, nullptr);
    replace_reference(state.indices, nullptr);
    replace_reference(state.color, nullptr);
    replace_reference(state.depth, nullptr);
    if (ComObj *decl = com_get(state.declaration))
        com_release(decl);
    if (ComObj *color = com_get(dev->palette_obj))
        com_release(color);
    if (ComObj *depth = com_get(state.automatic_depth))
        com_release(depth);
    devices().erase(found);
}

// A texture owns one reference to each cached level. The first external
// reference retains the texture; the last external release drops that hold.
// This avoids both a permanent parent/child cycle and dangling containers.
void surface_references(ComObj *surface, int32_t before, int32_t after) {
    if (surface->d3d_version != 8)
        return;
    ComObj *parent = com_get(surface->front_obj);
    if (!parent || parent->kind != K_D3D9TEXTURE)
        return;
    if (before == 1 && after == 2)
        com_addref(parent);
    else if (before == 2 && after == 1)
        com_release(parent);
}

// Each table follows the published version 8 ABI, including surface objects
// whose IUnknown prefix differs from the resource hierarchy in version 9.
DX_STUB(IF_D3D8_RegisterSoftwareDevice, UNAVAILABLE)
void IF_D3D8_GetAdapterCount(X86 *c) {
    shared(c, IF_D3D9, "GetAdapterCount");
}
void IF_D3D8_GetAdapterModeCount(X86 *c) {
    shared(c, IF_D3D9, "GetAdapterModeCount");
}
void IF_D3D8_EnumAdapterModes(X86 *c) {
    shared(c, IF_D3D9, "EnumAdapterModes");
}
void IF_D3D8_GetAdapterDisplayMode(X86 *c) {
    shared(c, IF_D3D9, "GetAdapterDisplayMode");
}
void IF_D3D8_CheckDeviceMultiSampleType(X86 *c) {
    shared(c, IF_D3D9, "CheckDeviceMultiSampleType");
}
void IF_D3D8_GetAdapterMonitor(X86 *c) {
    shared(c, IF_D3D9, "GetAdapterMonitor");
}
void IF_D3DDEVICE8_TestCooperativeLevel(X86 *c) {
    shared(c, IF_D3DDEVICE9, "TestCooperativeLevel");
}
void IF_D3DDEVICE8_GetAvailableTextureMem(X86 *c) {
    shared(c, IF_D3DDEVICE9, "GetAvailableTextureMem");
}
DX_STUB(IF_D3DDEVICE8_ResourceManagerDiscardBytes, UNAVAILABLE)
void IF_D3DDEVICE8_GetDirect3D(X86 *c) {
    shared(c, IF_D3DDEVICE9, "GetDirect3D");
}
DX_STUB(IF_D3DDEVICE8_SetCursorProperties, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_SetCursorPosition, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_ShowCursor, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_CreateAdditionalSwapChain, UNAVAILABLE)
void IF_D3DDEVICE8_Present(X86 *c) {
    shared(c, IF_D3DDEVICE9, "Present");
}
DX_STUB(IF_D3DDEVICE8_GetRasterStatus, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_SetGammaRamp, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetGammaRamp, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_CreateVolumeTexture, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_CopyRects, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_UpdateTexture, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetFrontBuffer, UNAVAILABLE)
void IF_D3DDEVICE8_BeginScene(X86 *c) {
    shared(c, IF_D3DDEVICE9, "BeginScene");
}
void IF_D3DDEVICE8_EndScene(X86 *c) {
    shared(c, IF_D3DDEVICE9, "EndScene");
}
void IF_D3DDEVICE8_Clear(X86 *c) {
    shared(c, IF_D3DDEVICE9, "Clear");
}
void IF_D3DDEVICE8_SetTransform(X86 *c) {
    shared(c, IF_D3DDEVICE9, "SetTransform");
}
DX_STUB(IF_D3DDEVICE8_MultiplyTransform, UNAVAILABLE)
void IF_D3DDEVICE8_SetViewport(X86 *c) {
    shared(c, IF_D3DDEVICE9, "SetViewport");
}
void IF_D3DDEVICE8_GetViewport(X86 *c) {
    shared(c, IF_D3DDEVICE9, "GetViewport");
}
DX_STUB(IF_D3DDEVICE8_SetMaterial, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetMaterial, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_SetLight, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetLight, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_LightEnable, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetLightEnable, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_SetClipPlane, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetClipPlane, UNAVAILABLE)
void IF_D3DDEVICE8_SetRenderState(X86 *c) {
    shared(c, IF_D3DDEVICE9, "SetRenderState");
}
DX_STUB(IF_D3DDEVICE8_BeginStateBlock, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_EndStateBlock, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_ApplyStateBlock, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_CaptureStateBlock, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_DeleteStateBlock, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_CreateStateBlock, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_SetClipStatus, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetClipStatus, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetInfo, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_SetPaletteEntries, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetPaletteEntries, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_SetCurrentTexturePalette, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_GetCurrentTexturePalette, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_ProcessVertices, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_DrawRectPatch, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_DrawTriPatch, UNAVAILABLE)
DX_STUB(IF_D3DDEVICE8_DeletePatch, UNAVAILABLE)

DX_STUB(IF_D3DRESOURCE8_SetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DRESOURCE8_GetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DRESOURCE8_FreePrivateData, UNAVAILABLE)
DX_STUB(IF_D3DRESOURCE8_SetPriority, UNAVAILABLE)
DX_STUB(IF_D3DRESOURCE8_GetPriority, UNAVAILABLE)
DX_STUB(IF_D3DRESOURCE8_PreLoad, UNAVAILABLE)

DX_STUB(IF_D3DBASETEXTURE8_SetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DBASETEXTURE8_GetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DBASETEXTURE8_FreePrivateData, UNAVAILABLE)
DX_STUB(IF_D3DBASETEXTURE8_SetPriority, UNAVAILABLE)
DX_STUB(IF_D3DBASETEXTURE8_GetPriority, UNAVAILABLE)
DX_STUB(IF_D3DBASETEXTURE8_PreLoad, UNAVAILABLE)
DX_STUB(IF_D3DBASETEXTURE8_SetLOD, UNAVAILABLE)
DX_STUB(IF_D3DBASETEXTURE8_GetLOD, UNAVAILABLE)

DX_STUB(IF_D3DTEXTURE8_SetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DTEXTURE8_GetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DTEXTURE8_FreePrivateData, UNAVAILABLE)
DX_STUB(IF_D3DTEXTURE8_SetPriority, UNAVAILABLE)
DX_STUB(IF_D3DTEXTURE8_GetPriority, UNAVAILABLE)
DX_STUB(IF_D3DTEXTURE8_PreLoad, UNAVAILABLE)
DX_STUB(IF_D3DTEXTURE8_SetLOD, UNAVAILABLE)
DX_STUB(IF_D3DTEXTURE8_GetLOD, UNAVAILABLE)
void IF_D3DTEXTURE8_GetSurfaceLevel(X86 *c) {
    shared(c, IF_D3DTEXTURE9, "GetSurfaceLevel");
}
void IF_D3DTEXTURE8_LockRect(X86 *c) {
    lock_rect8(c, IF_D3DTEXTURE9);
}
void IF_D3DTEXTURE8_UnlockRect(X86 *c) {
    shared(c, IF_D3DTEXTURE9, "UnlockRect");
}
DX_STUB(IF_D3DTEXTURE8_AddDirtyRect, UNAVAILABLE)

DX_STUB(IF_D3DCUBETEXTURE8_SetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DCUBETEXTURE8_GetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DCUBETEXTURE8_FreePrivateData, UNAVAILABLE)
DX_STUB(IF_D3DCUBETEXTURE8_SetPriority, UNAVAILABLE)
DX_STUB(IF_D3DCUBETEXTURE8_GetPriority, UNAVAILABLE)
DX_STUB(IF_D3DCUBETEXTURE8_PreLoad, UNAVAILABLE)
DX_STUB(IF_D3DCUBETEXTURE8_SetLOD, UNAVAILABLE)
DX_STUB(IF_D3DCUBETEXTURE8_GetLOD, UNAVAILABLE)
void IF_D3DCUBETEXTURE8_GetCubeMapSurface(X86 *c) {
    shared(c, IF_D3DCUBETEXTURE9, "GetCubeMapSurface");
}
void IF_D3DCUBETEXTURE8_LockRect(X86 *c) {
    lock_rect8(c, IF_D3DCUBETEXTURE9);
}
void IF_D3DCUBETEXTURE8_UnlockRect(X86 *c) {
    shared(c, IF_D3DCUBETEXTURE9, "UnlockRect");
}
DX_STUB(IF_D3DCUBETEXTURE8_AddDirtyRect, UNAVAILABLE)

DX_STUB(IF_D3DSURFACE8_SetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DSURFACE8_GetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DSURFACE8_FreePrivateData, UNAVAILABLE)
void IF_D3DSURFACE8_LockRect(X86 *c) {
    lock_rect8(c, IF_D3DSURFACE9);
}
void IF_D3DSURFACE8_UnlockRect(X86 *c) {
    shared(c, IF_D3DSURFACE9, "UnlockRect");
}

DX_STUB(IF_D3DVERTEXBUFFER8_SetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DVERTEXBUFFER8_GetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DVERTEXBUFFER8_FreePrivateData, UNAVAILABLE)
DX_STUB(IF_D3DVERTEXBUFFER8_SetPriority, UNAVAILABLE)
DX_STUB(IF_D3DVERTEXBUFFER8_GetPriority, UNAVAILABLE)
DX_STUB(IF_D3DVERTEXBUFFER8_PreLoad, UNAVAILABLE)

void IF_D3DVERTEXBUFFER8_Unlock(X86 *c) {
    shared(c, IF_D3DVERTEXBUFFER9, "Unlock");
}

DX_STUB(IF_D3DINDEXBUFFER8_SetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DINDEXBUFFER8_GetPrivateData, UNAVAILABLE)
DX_STUB(IF_D3DINDEXBUFFER8_FreePrivateData, UNAVAILABLE)
DX_STUB(IF_D3DINDEXBUFFER8_SetPriority, UNAVAILABLE)
DX_STUB(IF_D3DINDEXBUFFER8_GetPriority, UNAVAILABLE)
DX_STUB(IF_D3DINDEXBUFFER8_PreLoad, UNAVAILABLE)

void IF_D3DINDEXBUFFER8_Unlock(X86 *c) {
    shared(c, IF_D3DINDEXBUFFER9, "Unlock");
}

static const ComMethod IF_D3D8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"RegisterSoftwareDevice", 2, IF_D3D8_RegisterSoftwareDevice},
    {"GetAdapterCount", 1, IF_D3D8_GetAdapterCount},
    {"GetAdapterIdentifier", 4, D8_GetAdapterIdentifier},
    {"GetAdapterModeCount", 2, IF_D3D8_GetAdapterModeCount},
    {"EnumAdapterModes", 4, IF_D3D8_EnumAdapterModes},
    {"GetAdapterDisplayMode", 3, IF_D3D8_GetAdapterDisplayMode},
    {"CheckDeviceType", 6, D8_CheckDeviceType},
    {"CheckDeviceFormat", 7, D8_CheckDeviceFormat},
    {"CheckDeviceMultiSampleType", 6, IF_D3D8_CheckDeviceMultiSampleType},
    {"CheckDepthStencilMatch", 6, D8_CheckDepthStencilMatch},
    {"GetDeviceCaps", 4, D8_GetDeviceCaps},
    {"GetAdapterMonitor", 2, IF_D3D8_GetAdapterMonitor},
    {"CreateDevice", 7, D8_CreateDevice},
};

static const ComMethod IF_D3DDEVICE8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"TestCooperativeLevel", 1, IF_D3DDEVICE8_TestCooperativeLevel},
    {"GetAvailableTextureMem", 1, IF_D3DDEVICE8_GetAvailableTextureMem},
    {"ResourceManagerDiscardBytes", 2, IF_D3DDEVICE8_ResourceManagerDiscardBytes},
    {"GetDirect3D", 2, IF_D3DDEVICE8_GetDirect3D},
    {"GetDeviceCaps", 2, D8_GetDeviceCaps},
    {"GetDisplayMode", 2, D8_GetDisplayMode},
    {"GetCreationParameters", 2, D8_GetCreationParameters},
    {"SetCursorProperties", 4, IF_D3DDEVICE8_SetCursorProperties},
    {"SetCursorPosition", 4, IF_D3DDEVICE8_SetCursorPosition},
    {"ShowCursor", 2, IF_D3DDEVICE8_ShowCursor},
    {"CreateAdditionalSwapChain", 3, IF_D3DDEVICE8_CreateAdditionalSwapChain},
    {"Reset", 2, D8_Reset},
    {"Present", 5, IF_D3DDEVICE8_Present},
    {"GetBackBuffer", 4, D8_GetBackBuffer},
    {"GetRasterStatus", 2, IF_D3DDEVICE8_GetRasterStatus},
    {"SetGammaRamp", 3, IF_D3DDEVICE8_SetGammaRamp},
    {"GetGammaRamp", 2, IF_D3DDEVICE8_GetGammaRamp},
    {"CreateTexture", 8, D8_CreateTexture},
    {"CreateVolumeTexture", 9, IF_D3DDEVICE8_CreateVolumeTexture},
    {"CreateCubeTexture", 7, D8_CreateCubeTexture},
    {"CreateVertexBuffer", 6, D8_CreateVertexBuffer},
    {"CreateIndexBuffer", 6, D8_CreateIndexBuffer},
    {"CreateRenderTarget", 7, D8_CreateRenderTarget},
    {"CreateDepthStencilSurface", 6, D8_CreateDepthStencilSurface},
    {"CreateImageSurface", 5, D8_CreateImageSurface},
    {"CopyRects", 6, IF_D3DDEVICE8_CopyRects},
    {"UpdateTexture", 3, IF_D3DDEVICE8_UpdateTexture},
    {"GetFrontBuffer", 2, IF_D3DDEVICE8_GetFrontBuffer},
    {"SetRenderTarget", 3, D8_SetRenderTarget},
    {"GetRenderTarget", 2, D8_GetRenderTarget},
    {"GetDepthStencilSurface", 2, D8_GetDepthStencilSurface},
    {"BeginScene", 1, IF_D3DDEVICE8_BeginScene},
    {"EndScene", 1, IF_D3DDEVICE8_EndScene},
    {"Clear", 7, IF_D3DDEVICE8_Clear},
    {"SetTransform", 3, IF_D3DDEVICE8_SetTransform},
    {"GetTransform", 3, D8_GetTransform},
    {"MultiplyTransform", 3, IF_D3DDEVICE8_MultiplyTransform},
    {"SetViewport", 2, IF_D3DDEVICE8_SetViewport},
    {"GetViewport", 2, IF_D3DDEVICE8_GetViewport},
    {"SetMaterial", 2, IF_D3DDEVICE8_SetMaterial},
    {"GetMaterial", 2, IF_D3DDEVICE8_GetMaterial},
    {"SetLight", 3, IF_D3DDEVICE8_SetLight},
    {"GetLight", 3, IF_D3DDEVICE8_GetLight},
    {"LightEnable", 3, IF_D3DDEVICE8_LightEnable},
    {"GetLightEnable", 3, IF_D3DDEVICE8_GetLightEnable},
    {"SetClipPlane", 3, IF_D3DDEVICE8_SetClipPlane},
    {"GetClipPlane", 3, IF_D3DDEVICE8_GetClipPlane},
    {"SetRenderState", 3, IF_D3DDEVICE8_SetRenderState},
    {"GetRenderState", 3, D8_GetRenderState},
    {"BeginStateBlock", 1, IF_D3DDEVICE8_BeginStateBlock},
    {"EndStateBlock", 2, IF_D3DDEVICE8_EndStateBlock},
    {"ApplyStateBlock", 2, IF_D3DDEVICE8_ApplyStateBlock},
    {"CaptureStateBlock", 2, IF_D3DDEVICE8_CaptureStateBlock},
    {"DeleteStateBlock", 2, IF_D3DDEVICE8_DeleteStateBlock},
    {"CreateStateBlock", 3, IF_D3DDEVICE8_CreateStateBlock},
    {"SetClipStatus", 2, IF_D3DDEVICE8_SetClipStatus},
    {"GetClipStatus", 2, IF_D3DDEVICE8_GetClipStatus},
    {"GetTexture", 3, D8_GetTexture},
    {"SetTexture", 3, D8_SetTexture},
    {"GetTextureStageState", 4, D8_GetTextureStageState},
    {"SetTextureStageState", 4, D8_SetTextureStageState},
    {"ValidateDevice", 2, D8_ValidateDevice},
    {"GetInfo", 4, IF_D3DDEVICE8_GetInfo},
    {"SetPaletteEntries", 3, IF_D3DDEVICE8_SetPaletteEntries},
    {"GetPaletteEntries", 3, IF_D3DDEVICE8_GetPaletteEntries},
    {"SetCurrentTexturePalette", 2, IF_D3DDEVICE8_SetCurrentTexturePalette},
    {"GetCurrentTexturePalette", 2, IF_D3DDEVICE8_GetCurrentTexturePalette},
    {"DrawPrimitive", 4, D8_DrawPrimitive},
    {"DrawIndexedPrimitive", 6, D8_DrawIndexedPrimitive},
    {"DrawPrimitiveUP", 5, D8_DrawPrimitiveUP},
    {"DrawIndexedPrimitiveUP", 9, D8_DrawIndexedPrimitiveUP},
    {"ProcessVertices", 6, IF_D3DDEVICE8_ProcessVertices},
    {"CreateVertexShader", 5, D8_CreateVertexShader},
    {"SetVertexShader", 2, D8_SetVertexShader},
    {"GetVertexShader", 2, D8_GetVertexShader},
    {"DeleteVertexShader", 2, D8_DeleteVertexShader},
    {"SetVertexShaderConstant", 4, D8_SetVertexShaderConstant},
    {"GetVertexShaderConstant", 4, D8_GetVertexShaderConstant},
    {"GetVertexShaderDeclaration", 4, D8_GetVertexShaderDeclaration},
    {"GetVertexShaderFunction", 4, D8_GetVertexShaderFunction},
    {"SetStreamSource", 4, D8_SetStreamSource},
    {"GetStreamSource", 4, D8_GetStreamSource},
    {"SetIndices", 3, D8_SetIndices},
    {"GetIndices", 3, D8_GetIndices},
    {"CreatePixelShader", 3, D8_CreatePixelShader},
    {"SetPixelShader", 2, D8_SetPixelShader},
    {"GetPixelShader", 2, D8_GetPixelShader},
    {"DeletePixelShader", 2, D8_DeletePixelShader},
    {"SetPixelShaderConstant", 4, D8_SetPixelShaderConstant},
    {"GetPixelShaderConstant", 4, D8_GetPixelShaderConstant},
    {"GetPixelShaderFunction", 4, D8_GetPixelShaderFunction},
    {"DrawRectPatch", 4, IF_D3DDEVICE8_DrawRectPatch},
    {"DrawTriPatch", 4, IF_D3DDEVICE8_DrawTriPatch},
    {"DeletePatch", 2, IF_D3DDEVICE8_DeletePatch},
};

static const ComMethod IF_D3DRESOURCE8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, D8_GetDevice},
    {"SetPrivateData", 5, IF_D3DRESOURCE8_SetPrivateData},
    {"GetPrivateData", 4, IF_D3DRESOURCE8_GetPrivateData},
    {"FreePrivateData", 2, IF_D3DRESOURCE8_FreePrivateData},
    {"SetPriority", 2, IF_D3DRESOURCE8_SetPriority},
    {"GetPriority", 1, IF_D3DRESOURCE8_GetPriority},
    {"PreLoad", 1, IF_D3DRESOURCE8_PreLoad},
    {"GetType", 1, D8_GetType},
};

static const ComMethod IF_D3DBASETEXTURE8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, D8_GetDevice},
    {"SetPrivateData", 5, IF_D3DBASETEXTURE8_SetPrivateData},
    {"GetPrivateData", 4, IF_D3DBASETEXTURE8_GetPrivateData},
    {"FreePrivateData", 2, IF_D3DBASETEXTURE8_FreePrivateData},
    {"SetPriority", 2, IF_D3DBASETEXTURE8_SetPriority},
    {"GetPriority", 1, IF_D3DBASETEXTURE8_GetPriority},
    {"PreLoad", 1, IF_D3DBASETEXTURE8_PreLoad},
    {"GetType", 1, D8_GetType},
    {"SetLOD", 2, IF_D3DBASETEXTURE8_SetLOD},
    {"GetLOD", 1, IF_D3DBASETEXTURE8_GetLOD},
    {"GetLevelCount", 1, D8_GetLevelCount},
};

static const ComMethod IF_D3DTEXTURE8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, D8_GetDevice},
    {"SetPrivateData", 5, IF_D3DTEXTURE8_SetPrivateData},
    {"GetPrivateData", 4, IF_D3DTEXTURE8_GetPrivateData},
    {"FreePrivateData", 2, IF_D3DTEXTURE8_FreePrivateData},
    {"SetPriority", 2, IF_D3DTEXTURE8_SetPriority},
    {"GetPriority", 1, IF_D3DTEXTURE8_GetPriority},
    {"PreLoad", 1, IF_D3DTEXTURE8_PreLoad},
    {"GetType", 1, D8_GetType},
    {"SetLOD", 2, IF_D3DTEXTURE8_SetLOD},
    {"GetLOD", 1, IF_D3DTEXTURE8_GetLOD},
    {"GetLevelCount", 1, D8_GetLevelCount},
    {"GetLevelDesc", 3, D8_GetLevelDesc},
    {"GetSurfaceLevel", 3, IF_D3DTEXTURE8_GetSurfaceLevel},
    {"LockRect", 5, IF_D3DTEXTURE8_LockRect},
    {"UnlockRect", 2, IF_D3DTEXTURE8_UnlockRect},
    {"AddDirtyRect", 2, IF_D3DTEXTURE8_AddDirtyRect},
};

static const ComMethod IF_D3DCUBETEXTURE8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, D8_GetDevice},
    {"SetPrivateData", 5, IF_D3DCUBETEXTURE8_SetPrivateData},
    {"GetPrivateData", 4, IF_D3DCUBETEXTURE8_GetPrivateData},
    {"FreePrivateData", 2, IF_D3DCUBETEXTURE8_FreePrivateData},
    {"SetPriority", 2, IF_D3DCUBETEXTURE8_SetPriority},
    {"GetPriority", 1, IF_D3DCUBETEXTURE8_GetPriority},
    {"PreLoad", 1, IF_D3DCUBETEXTURE8_PreLoad},
    {"GetType", 1, D8_GetType},
    {"SetLOD", 2, IF_D3DCUBETEXTURE8_SetLOD},
    {"GetLOD", 1, IF_D3DCUBETEXTURE8_GetLOD},
    {"GetLevelCount", 1, D8_GetLevelCount},
    {"GetLevelDesc", 3, D8_GetLevelDesc},
    {"GetCubeMapSurface", 4, IF_D3DCUBETEXTURE8_GetCubeMapSurface},
    {"LockRect", 6, IF_D3DCUBETEXTURE8_LockRect},
    {"UnlockRect", 3, IF_D3DCUBETEXTURE8_UnlockRect},
    {"AddDirtyRect", 3, IF_D3DCUBETEXTURE8_AddDirtyRect},
};

static const ComMethod IF_D3DSURFACE8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, D8_GetDevice},
    {"SetPrivateData", 5, IF_D3DSURFACE8_SetPrivateData},
    {"GetPrivateData", 4, IF_D3DSURFACE8_GetPrivateData},
    {"FreePrivateData", 2, IF_D3DSURFACE8_FreePrivateData},
    {"GetContainer", 3, D8_GetContainer},
    {"GetDesc", 2, D8_GetDesc},
    {"LockRect", 4, IF_D3DSURFACE8_LockRect},
    {"UnlockRect", 1, IF_D3DSURFACE8_UnlockRect},
};

static const ComMethod IF_D3DVERTEXBUFFER8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, D8_GetDevice},
    {"SetPrivateData", 5, IF_D3DVERTEXBUFFER8_SetPrivateData},
    {"GetPrivateData", 4, IF_D3DVERTEXBUFFER8_GetPrivateData},
    {"FreePrivateData", 2, IF_D3DVERTEXBUFFER8_FreePrivateData},
    {"SetPriority", 2, IF_D3DVERTEXBUFFER8_SetPriority},
    {"GetPriority", 1, IF_D3DVERTEXBUFFER8_GetPriority},
    {"PreLoad", 1, IF_D3DVERTEXBUFFER8_PreLoad},
    {"GetType", 1, D8_GetType},
    {"Lock", 5, buffer_lock8},
    {"Unlock", 1, IF_D3DVERTEXBUFFER8_Unlock},
    {"GetDesc", 2, D8_GetDesc},
};

static const ComMethod IF_D3DINDEXBUFFER8_methods[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, D8_GetDevice},
    {"SetPrivateData", 5, IF_D3DINDEXBUFFER8_SetPrivateData},
    {"GetPrivateData", 4, IF_D3DINDEXBUFFER8_GetPrivateData},
    {"FreePrivateData", 2, IF_D3DINDEXBUFFER8_FreePrivateData},
    {"SetPriority", 2, IF_D3DINDEXBUFFER8_SetPriority},
    {"GetPriority", 1, IF_D3DINDEXBUFFER8_GetPriority},
    {"PreLoad", 1, IF_D3DINDEXBUFFER8_PreLoad},
    {"GetType", 1, D8_GetType},
    {"Lock", 5, buffer_lock8},
    {"Unlock", 1, IF_D3DINDEXBUFFER8_Unlock},
    {"GetDesc", 2, D8_GetDesc},
};
} // namespace

void d3d8_reset() {
    devices().clear();
}

void d3d8_resource_destroy(ComObj *object) {
    if (object->kind == K_D3D9TEXTURE) {
        auto levels = std::move(object->surfaces);
        for (uint32_t id : levels)
            if (ComObj *level = com_get(id))
                com_release(level);
    }
}

void d3d8_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    com_define(IF_D3D8, "d3d8.dll", "IDirect3D8", IF_D3D8_methods, std::size(IF_D3D8_methods));
    static const uint8_t IF_D3D8_iid[16] = {0xda, 0xe8, 0xd9, 0x1d, 0x77, 0x1c, 0x40, 0x4d,
                                            0xb0, 0xcf, 0x98, 0xfe, 0xfd, 0xff, 0x95, 0x12};
    com_register_iid(IF_D3D8, IF_D3D8_iid);
    com_bind(IF_D3D8, K_D3D9);
    com_define(IF_D3DDEVICE8, "d3d8.dll", "IDirect3DDevice8", IF_D3DDEVICE8_methods,
               std::size(IF_D3DDEVICE8_methods));
    static const uint8_t IF_D3DDEVICE8_iid[16] = {0xdf, 0xe5, 0x85, 0x73, 0xe8, 0x8f, 0xd5, 0x41,
                                                  0x86, 0xb6, 0xd7, 0xb4, 0x85, 0x47, 0xb6, 0xcf};
    com_register_iid(IF_D3DDEVICE8, IF_D3DDEVICE8_iid);
    com_bind(IF_D3DDEVICE8, K_D3D9DEVICE);
    com_define(IF_D3DRESOURCE8, "d3d8.dll", "IDirect3DResource8", IF_D3DRESOURCE8_methods,
               std::size(IF_D3DRESOURCE8_methods));
    static const uint8_t IF_D3DRESOURCE8_iid[16] = {0x7b, 0xbb, 0x36, 0x1b, 0xb7, 0x09, 0x0a, 0x41,
                                                    0xb4, 0x45, 0x7d, 0x14, 0x30, 0xd7, 0xb3, 0x3f};
    com_register_iid(IF_D3DRESOURCE8, IF_D3DRESOURCE8_iid);
    com_bind(IF_D3DRESOURCE8, K_D3D9TEXTURE);
    com_bind(IF_D3DRESOURCE8, K_D3D9VB);
    com_bind(IF_D3DRESOURCE8, K_D3D9IB);
    com_define(IF_D3DBASETEXTURE8, "d3d8.dll", "IDirect3DBaseTexture8", IF_D3DBASETEXTURE8_methods,
               std::size(IF_D3DBASETEXTURE8_methods));
    static const uint8_t IF_D3DBASETEXTURE8_iid[16] = {0xfa, 0x1c, 0x21, 0xb4, 0xb9, 0x51,
                                                       0x9f, 0x4a, 0xab, 0x78, 0xdb, 0x99,
                                                       0xb2, 0xbb, 0x67, 0x8e};
    com_register_iid(IF_D3DBASETEXTURE8, IF_D3DBASETEXTURE8_iid);
    com_bind(IF_D3DBASETEXTURE8, K_D3D9TEXTURE);
    com_define(IF_D3DTEXTURE8, "d3d8.dll", "IDirect3DTexture8", IF_D3DTEXTURE8_methods,
               std::size(IF_D3DTEXTURE8_methods));
    static const uint8_t IF_D3DTEXTURE8_iid[16] = {0x75, 0xd5, 0xcd, 0xe4, 0x66, 0x28, 0x01, 0x4f,
                                                   0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58};
    com_register_iid(IF_D3DTEXTURE8, IF_D3DTEXTURE8_iid);
    com_bind(IF_D3DTEXTURE8, K_D3D9TEXTURE);
    com_define(IF_D3DCUBETEXTURE8, "d3d8.dll", "IDirect3DCubeTexture8", IF_D3DCUBETEXTURE8_methods,
               std::size(IF_D3DCUBETEXTURE8_methods));
    static const uint8_t IF_D3DCUBETEXTURE8_iid[16] = {0x68, 0xb9, 0xe5, 0x3e, 0xca, 0x2a,
                                                       0x34, 0x4c, 0x8b, 0xb5, 0x7e, 0x0c,
                                                       0x3d, 0x19, 0xb7, 0x50};
    com_register_iid(IF_D3DCUBETEXTURE8, IF_D3DCUBETEXTURE8_iid);
    com_bind(IF_D3DCUBETEXTURE8, K_D3D9TEXTURE);
    com_define(IF_D3DSURFACE8, "d3d8.dll", "IDirect3DSurface8", IF_D3DSURFACE8_methods,
               std::size(IF_D3DSURFACE8_methods));
    static const uint8_t IF_D3DSURFACE8_iid[16] = {0xca, 0xeb, 0x6e, 0xb9, 0x26, 0xb3, 0xa5, 0x4e,
                                                   0x88, 0x2f, 0x2f, 0xf5, 0xba, 0xe0, 0x21, 0xdd};
    com_register_iid(IF_D3DSURFACE8, IF_D3DSURFACE8_iid);
    com_bind(IF_D3DSURFACE8, K_D3D9SURFACE);
    com_define(IF_D3DVERTEXBUFFER8, "d3d8.dll", "IDirect3DVertexBuffer8",
               IF_D3DVERTEXBUFFER8_methods, std::size(IF_D3DVERTEXBUFFER8_methods));
    static const uint8_t IF_D3DVERTEXBUFFER8_iid[16] = {0xc7, 0xea, 0xee, 0x8a, 0xf9, 0x05,
                                                        0xd4, 0x44, 0xb5, 0x91, 0x00, 0x0b,
                                                        0x0d, 0xf1, 0xcb, 0x95};
    com_register_iid(IF_D3DVERTEXBUFFER8, IF_D3DVERTEXBUFFER8_iid);
    com_bind(IF_D3DVERTEXBUFFER8, K_D3D9VB);
    com_define(IF_D3DINDEXBUFFER8, "d3d8.dll", "IDirect3DIndexBuffer8", IF_D3DINDEXBUFFER8_methods,
               std::size(IF_D3DINDEXBUFFER8_methods));
    static const uint8_t IF_D3DINDEXBUFFER8_iid[16] = {0x9a, 0x9c, 0x68, 0x0e, 0x3d, 0x05,
                                                       0xa0, 0x44, 0x9d, 0x92, 0xdb, 0x0e,
                                                       0x3d, 0x75, 0x0f, 0x86};
    com_register_iid(IF_D3DINDEXBUFFER8, IF_D3DINDEXBUFFER8_iid);
    com_bind(IF_D3DINDEXBUFFER8, K_D3D9IB);
    com_set_destructor(K_D3D9DEVICE, device_destroy);
    com_set_ref_hook(K_D3D9SURFACE, surface_references);
    static const ImportShim exports[] = {{"d3d8.dll", "Direct3DCreate8", 1, D8_Create}};
    imports_register(exports, std::size(exports));
}
