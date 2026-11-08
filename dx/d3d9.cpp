// d3d9.cpp - Direct3D 9, for a game that renders through shaders.
//
// The kit's other Direct3D module (d3d.cpp) models version 2: a fixed-function
// recorder reached by QueryInterface on a DirectDraw object. Version 9 shares
// none of that. It is its own DLL with its own factory function, its own
// object model, and a device whose drawing is programmable, so it lives here.
//
// The device records resources, shader objects and draw state for the CPU
// rasterizer or GPU host seam. Vtable order is the guest ABI. Remaining stubs
// report their method names so unsupported calls remain visible.
#include "com.h"
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include "game_config.h"
#include "host_api.h"
#include "d3d9_pipeline.h"
#include "d3d9_raster.h"
#include "d3d9_shader.h"
#include "d3d9_api.h"
#include "d3d9_ffp.h"
#include "d3d8_program.h"
#include "host_d9.h"
#include "../platform/ab_phase.h"
#include <deque>
#include <array>
#include <unordered_map>
#include <map>

#include <string.h>
#include <iterator>
#include <algorithm>
#include <initializer_list>

#define IID_BYTES(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                         \
    {(uint8_t)((a) & 0xff),                                                                        \
     (uint8_t)(((a) >> 8) & 0xff),                                                                 \
     (uint8_t)(((a) >> 16) & 0xff),                                                                \
     (uint8_t)(((a) >> 24) & 0xff),                                                                \
     (uint8_t)((b) & 0xff),                                                                        \
     (uint8_t)(((b) >> 8) & 0xff),                                                                 \
     (uint8_t)((c) & 0xff),                                                                        \
     (uint8_t)(((c) >> 8) & 0xff),                                                                 \
     d0,                                                                                           \
     d1,                                                                                           \
     d2,                                                                                           \
     d3,                                                                                           \
     d4,                                                                                           \
     d5,                                                                                           \
     d6,                                                                                           \
     d7}

// {81BDCBCA-64D4-426d-AE8D-AD0147F4275C} and {D0223B96-BF7A-43fd-92BD-A43B0D82B9EB}
static const uint8_t IID_IDirect3D9_[16] =
    IID_BYTES(0x81bdcbca, 0x64d4, 0x426d, 0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c);
static const uint8_t IID_IDirect3DDevice9_[16] =
    IID_BYTES(0xd0223b96, 0xbf7a, 0x43fd, 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb);

#define MAKE_D3DHRESULT(code) (0x88760000u | (uint32_t)(code))
static const uint32_t D3D_OK9 = 0u;
static const uint32_t D3DERR_NOTAVAILABLE = MAKE_D3DHRESULT(2154);
static const uint32_t D3DERR_INVALIDCALL = MAKE_D3DHRESULT(2156);
static const uint32_t D3DERR_NOTFOUND = MAKE_D3DHRESULT(2150);
// The draw limits the caps report (GetDeviceCaps) and the four draw calls
// enforce: one definition, so what the guest is told and what is enforced
// cannot drift apart.
static const uint32_t D9_MAX_PRIMITIVE_COUNT = 1048575;
static const uint32_t D9_MAX_VERTEX_INDEX = 1048575;

// Defined with the resources further down.
struct ComObj;
static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);
static void bind_color_target(ComObj *dev, uint32_t index, ComObj *s);
static void bind_depth_stencil(ComObj *dev, ComObj *s);
static bool gpu_on();
static HostD9Target gpu_target(ComObj *dev);
static void target_viewport(ComObj *dev, const HostD9Target &t, int32_t vp[4]);
static void gpu_target_drawn(ComObj *dev, bool color, bool depth);
static void gpu_sync(ComObj *o);
static std::unordered_map<uint32_t, uint32_t> &resource_usage();

// What only the fixed-function pipeline reads (d3d9_ffp.h): the material, the
// lights, the FVF and which transforms and stage states the game has set, per
// device id. Kept beside D9Pipeline rather than in it.
static std::unordered_map<uint32_t, d9ffp::FixedState> &fixed_states() {
    static auto *m = new std::unordered_map<uint32_t, d9ffp::FixedState>();
    return *m;
}
static d9ffp::FixedState &fixed_state(uint32_t device_id) {
    return fixed_states()[device_id];
}

// Every resource inherits its device's API version. The storage, revision and
// GPU contracts are shared, but each out-pointer names the matching vtable.
uint32_t d9_api_view(ComObj *o, ComIface iface) {
    if (o && o->d3d_version == 8) {
        switch (iface) {
        case IF_D3D9:
            iface = IF_D3D8;
            break;
        case IF_D3DDEVICE9:
            iface = IF_D3DDEVICE8;
            break;
        case IF_D3DTEXTURE9:
            iface = IF_D3DTEXTURE8;
            break;
        case IF_D3DCUBETEXTURE9:
            iface = IF_D3DCUBETEXTURE8;
            break;
        case IF_D3DSURFACE9:
            iface = IF_D3DSURFACE8;
            break;
        case IF_D3DVERTEXBUFFER9:
            iface = IF_D3DVERTEXBUFFER8;
            break;
        case IF_D3DINDEXBUFFER9:
            iface = IF_D3DINDEXBUFFER8;
            break;
        default:
            return 0;
        }
    }
    return com_view(o, iface);
}

static bool d9_view_allowed(const ComObj *o, ComIface iface) {
    const bool version8 = iface >= IF_D3D8 && iface <= IF_D3DINDEXBUFFER8;
    if (version8 != (o->d3d_version == 8))
        return false;
    if (version8 && o->kind == K_D3D9TEXTURE) {
        if (iface == IF_D3DTEXTURE8 && o->caps)
            return false;
        if (iface == IF_D3DCUBETEXTURE8 && !o->caps)
            return false;
    }
    return true;
}

// One scratch block for structures a method fills in for the guest.
static uint32_t g_scratch = 0, g_scratch_size = 0;
static uint32_t scratch9(uint32_t n) {
    if (g_scratch_size < n) {
        if (g_scratch)
            heap_free(g_scratch);
        g_scratch = heap_alloc(n, true, 16);
        g_scratch_size = g_scratch ? n : 0;
    }
    if (g_scratch)
        memset(gm_ptr(g_scratch), 0, g_scratch_size);
    return g_scratch;
}

static ComObj *this_d3d9(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9) ? o : nullptr;
}
static ComObj *this_device9(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9DEVICE) ? o : nullptr;
}

// ---------------------------------------------------------------------------
// IDirect3D9
// ---------------------------------------------------------------------------
// One adapter, the host window's display. A game that enumerates further gets
// an honest "there is only one".
void D9_RegisterSoftwareDevice(X86 *c) {
    com_ret(c, D3DERR_NOTAVAILABLE);
}
void D9_GetAdapterCount(X86 *c) {
    set_eax(c, 1);
}

// D3DADAPTER_IDENTIFIER9: two 512-byte strings, a 32-byte device name, the
// driver version, four ids, the identifier GUID and the WHQL level.
void D9_GetAdapterIdentifier(X86 *c) {
    uint32_t out = arg(c, 3);
    ComObj *factory = this_d3d9(c);
    const bool version8 = factory && factory->d3d_version == 8;
    if (arg(c, 1) || !out || !gm_fits(out, version8 ? 1068 : 1100)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(out), 0, version8 ? 1068 : 1100);
    // Games of this era pick their feature set by vendor and device id, and
    // turn off whatever they do not recognise (Most Wanted offers shadows
    // only on NVIDIA and ATI). The renderer covers what a GeForce 7800 GTX
    // offered, hardware shadow maps included, so that is the card it names.
    gm_put_str(out, "nv4_disp.dll", 512);
    gm_put_str(out + 512, RECOMP_ADAPTER_DESCRIPTION, 512);
    if (!version8)
        gm_put_str(out + 1024, "\\\\.\\DISPLAY1", 32);
    // Driver[512], Description[512], DeviceName[32], then:
    const uint32_t at = out + (version8 ? 1024 : 1056);
    wr32(at, 0x000a249b); // DriverVersion 6.14.10.9371 (low, high)
    wr32(at + 4, 0x0006000e);
    wr32(at + 8, RECOMP_ADAPTER_VENDOR_ID);
    wr32(at + 12, RECOMP_ADAPTER_DEVICE_ID);
    wr32(at + 20, 0xa1); // Revision
    wr32(at + 40, 1);    // WHQLLevel: certified
    com_ret(c, D3D_OK9);
}

static constexpr uint32_t adapter_modes[][2] = {
    {640, 480},  {800, 600},   {1024, 768},  {1152, 864},  {1280, 720}, {1280, 768},
    {1280, 800}, {1280, 960},  {1280, 1024}, {1360, 768},  {1366, 768}, {1440, 900},
    {1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {2560, 1600}};
void D9_GetAdapterModeCount(X86 *c) {
    ComObj *factory = this_d3d9(c);
    const bool version8 = factory && factory->d3d_version == 8;
    set_eax(c, arg(c, 1) || (!version8 && arg(c, 2) != 22) ? 0
               : version8                                  ? 1
                                                           : uint32_t(std::size(adapter_modes)));
}

// D3DDISPLAYMODE: width, height, refresh rate, format. X8R8G8B8 is 22.
static void put_display_mode(uint32_t p, uint32_t w, uint32_t h) {
    wr32(p + 0, w);
    wr32(p + 4, h);
    wr32(p + 8, 60);
    wr32(p + 12, 22);
}
void D9_EnumAdapterModes(X86 *c) {
    ComObj *factory = this_d3d9(c);
    const bool version8 = factory && factory->d3d_version == 8;
    const uint32_t index = arg(c, version8 ? 2 : 3), out = arg(c, version8 ? 3 : 4);
    if (arg(c, 1) || !out || !gm_fits(out, 16) ||
        (version8 ? index != 0 : arg(c, 2) != 22 || index >= std::size(adapter_modes))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    put_display_mode(out, version8 ? 1280 : adapter_modes[index][0],
                     version8 ? 960 : adapter_modes[index][1]);
    com_ret(c, D3D_OK9);
}
void D9_GetAdapterDisplayMode(X86 *c) {
    const uint32_t out = arg(c, 2);
    if (arg(c, 1) || !out || !gm_fits(out, 16)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t w, h, bpp;
    win32_display_mode(&w, &h, &bpp);
    put_display_mode(out, w, h);
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceType(X86 *c) {
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceFormat(X86 *c) {
    com_ret(c, D3D_OK9);
}
// D3DMULTISAMPLE_TYPE and quality as a sample count: 2x and 4x, which every
// Apple GPU renders. NONMASKABLE (1) offers them as quality levels 0 and 1.
static uint32_t multisample_count(uint32_t type, uint32_t quality) {
    if (type == 1)
        return quality == 0 ? 2 : 4;
    if (type >= 2)
        return type >= 4 ? 4 : 2;
    return 0;
}
// (this, Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType,
// pQualityLevels)
void D9_CheckDeviceMultiSampleType(X86 *c) {
    ComObj *factory = this_d3d9(c);
    uint32_t type = arg(c, 5);
    uint32_t quality = factory && factory->d3d_version == 8 ? 0 : arg(c, 6);
    uint32_t levels = type == 0 ? 1 : type == 1 ? 2 : (type == 2 || type == 4) ? 1 : 0;
    if (!levels) {
        com_ret(c, 0x8876086au); // D3DERR_NOTAVAILABLE
        return;
    }
    if (quality && gm_valid(quality, 4))
        wr32(quality, levels);
    com_ret(c, D3D_OK9);
}
void D9_CheckDepthStencilMatch(X86 *c) {
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceFormatConversion(X86 *c) {
    com_ret(c, D3D_OK9);
}

// D3DCAPS9 (304 bytes), as a GeForce 7800 GTX reports it for what the
// renderer implements: shader model 3.0, trilinear and 16x anisotropic
// filtering, the usual blend, compare and stencil sets, non-power-of-two
// textures, four render targets.
static void put_caps9(uint32_t p, bool version8 = false) {
    if (!p)
        return;
    memset(gm_ptr(p), 0, version8 ? 212 : 304);
    auto f = [&](uint32_t off, float v) { wrf32(p + off, v); };
    wr32(p + 0, 1);            // DeviceType: D3DDEVTYPE_HAL
    wr32(p + 8, 0x00020000u);  // Caps: READ_SCANLINE
    wr32(p + 12, 0x00020000u); // Caps2: FULLSCREENGAMMA
    wr32(
        p + 16,
        0x000003a0u); // Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD, COPY_TO_VIDMEM, COPY_TO_SYSTEMMEM, LINEAR_TO_SRGB
    wr32(p + 20, 0x8000000fu); // PresentationIntervals: IMMEDIATE, ONE..FOUR
    wr32(p + 28, 0x001bbef0u); // DevCaps: HWTRANSFORMANDLIGHT, PUREDEVICE, HWRASTERIZATION, ...
    wr32(
        p + 32,
        0x0003ccf2u); // PrimitiveMiscCaps: masks, cull modes, blend op, separate alpha, independent write masks
    wr32(
        p + 36,
        0x0f732191u); // RasterCaps: dither, zbias, fog, anisotropy, scissor, slope-scale depth bias
    wr32(p + 40, 0x000000ffu); // ZCmpCaps: all
    wr32(p + 44, 0x00003fffu); // SrcBlendCaps: all, BLENDFACTOR
    wr32(p + 48, 0x00003fffu); // DestBlendCaps
    wr32(p + 52, 0x000000ffu); // AlphaCmpCaps
    wr32(p + 56, 0x00084208u); // ShadeCaps
    wr32(
        p + 60,
        0x0001ec45u); // TextureCaps: PERSPECTIVE, ALPHA, CUBEMAP, MIPMAP, MIPCUBEMAP, PROJECTED, VOLUMEMAP, ...
    wr32(
        p + 64,
        0x03030700u); // TextureFilterCaps: MIN point/linear/aniso, MIP point/linear, MAG point/linear
    wr32(p + 68, 0x03030300u);  // CubeTextureFilterCaps
    wr32(p + 72, 0x03030300u);  // VolumeTextureFilterCaps
    wr32(p + 76, 0x0000003fu);  // TextureAddressCaps
    wr32(p + 80, 0x0000003fu);  // VolumeTextureAddressCaps
    wr32(p + 84, 0x0000001fu);  // LineCaps
    wr32(p + 88, 4096);         // MaxTextureWidth
    wr32(p + 92, 4096);         // MaxTextureHeight
    wr32(p + 96, 512);          // MaxVolumeExtent
    wr32(p + 100, 8192);        // MaxTextureRepeat
    wr32(p + 104, 8192);        // MaxTextureAspectRatio
    wr32(p + 108, 16);          // MaxAnisotropy
    f(112, 1e10f);              // MaxVertexW
    f(116, -1e8f);              // GuardBandLeft
    f(120, -1e8f);              // GuardBandTop
    f(124, 1e8f);               // GuardBandRight
    f(128, 1e8f);               // GuardBandBottom
    wr32(p + 136, 0x000001ffu); // StencilCaps: all ops, TWOSIDED
    wr32(p + 140, 0x00080008u); // FVFCaps: 8 texture coordinates, PSIZE
    wr32(p + 144, 0x03feffffu); // TextureOpCaps
    wr32(p + 148, 8);           // MaxTextureBlendStages
    wr32(p + 152, 8);           // MaxSimultaneousTextures
    wr32(p + 156, 0x0000007bu); // VertexProcessingCaps
    wr32(p + 160, 8);           // MaxActiveLights
    wr32(p + 164, 6);           // MaxUserClipPlanes
    wr32(p + 168, 4);           // MaxVertexBlendMatrices
    f(176, 8192.0f);            // MaxPointSize
    wr32(p + 180, D9_MAX_PRIMITIVE_COUNT);
    wr32(p + 184, D9_MAX_VERTEX_INDEX);
    wr32(p + 188, 16);          // MaxStreams
    wr32(p + 192, 255);         // MaxStreamStride
    wr32(p + 196, 0xfffe0300u); // VertexShaderVersion vs_3_0
    wr32(p + 200, 256);         // MaxVertexShaderConst
    wr32(p + 204, 0xffff0300u); // PixelShaderVersion ps_3_0
    f(208, 3.4e38f);            // PixelShaderMaxValue
    if (version8) {
        // Only capabilities implemented by the version 8 frontend are
        // exposed. Lighting, generated coordinates, blending of matrices
        // and user clip planes must not lure a game into an unsupported path.
        wr32(p + 28, 0x00080000u); // HWRASTERIZATION; software vertex processing is available
        wr32(p + 36, rd32(p + 36) & ~0x00010180u); // no fixed-function fog
        wr32(p + 60, rd32(p + 60) & ~0x0004a000u); // no volume textures
        wr32(p + 72, 0);                           // VolumeTextureFilterCaps
        wr32(p + 80, 0);                           // VolumeTextureAddressCaps
        wr32(p + 96, 0);                           // MaxVolumeExtent
        wr32(p + 144, d8::texture_operation_caps());
        wr32(p + 156, 0); // VertexProcessingCaps
        wr32(p + 160, 0); // MaxActiveLights
        wr32(p + 164, 0); // MaxUserClipPlanes
        wr32(p + 168, 0); // MaxVertexBlendMatrices
        wr32(p + 188, 8); // MaxStreams
        wr32(p + 196, 0xfffe0101u);
        wr32(p + 200, 96);
        wr32(p + 204, 0xffff0101u);
        f(208, 8.0f);
        return;
    }
    wr32(
        p + 212,
        0x00000051u); // DevCaps2: STREAMOFFSET, CAN_STRETCHRECT_FROM_TEXTURES, VERTEXELEMENTSCANSHARESTREAMOFFSET
    wr32(p + 232, 1);           // NumberOfAdaptersInGroup
    wr32(p + 236, 0x0000030fu); // DeclTypes
    wr32(p + 240, 4);           // NumSimultaneousRTs
    wr32(p + 244, 0x03000300u); // StretchRectFilterCaps: point, linear
    wr32(p + 248, 1);           // VS20Caps.Caps: PREDICATION
    wr32(p + 252, 24);          // VS20Caps.DynamicFlowControlDepth
    wr32(p + 256, 32);          // VS20Caps.NumTemps
    wr32(p + 260, 4);           // VS20Caps.StaticFlowControlDepth
    wr32(p + 264, 0x1f);        // PS20Caps.Caps
    wr32(p + 268, 24);          // PS20Caps.DynamicFlowControlDepth
    wr32(p + 272, 32);          // PS20Caps.NumTemps
    wr32(p + 276, 4);           // PS20Caps.StaticFlowControlDepth
    wr32(p + 280, 512);         // PS20Caps.NumInstructionSlots
    wr32(p + 284, 0x00000300u); // VertexTextureFilterCaps
    wr32(p + 288, 0xffffffffu); // MaxVShaderInstructionsExecuted
    wr32(p + 292, 0xffffffffu); // MaxPShaderInstructionsExecuted
    wr32(p + 296, 32768);       // MaxVertexShader30InstructionSlots
    wr32(p + 300, 32768);       // MaxPixelShader30InstructionSlots
}
void D9_GetDeviceCaps(X86 *c) {
    ComObj *factory = this_d3d9(c);
    put_caps9(arg(c, 3), factory && factory->d3d_version == 8);
    com_ret(c, D3D_OK9);
}
void D9_GetAdapterMonitor(X86 *c) {
    set_eax(c, 1); // a non-null HMONITOR
}

void D9_CreateDevice(X86 *c) {
    ComObj *d3d = this_d3d9(c);
    uint32_t present = arg(c, 5), out = arg(c, 6);
    if (!d3d || !out || !gm_valid(out, 4) || !present ||
        !gm_valid(present, d3d->d3d_version == 8 ? 52 : 56)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_out_ptr(out, 0);
    // Direct3D resets the calling thread's FPU at device creation unless the
    // behaviour flags (arg 4) hold D3DCREATE_FPU_PRESERVE (2): every
    // floating-point exception is masked. Games unmask exceptions before
    // CreateDevice and rely on this (NFSMW at 006e69f0, right after
    // Direct3DCreate9, then flags 0x40; NFS Carbon the same), and a CRT maths
    // call with an invalid argument would otherwise trap. The real runtime
    // also selects single precision here; that is left as the guest set it on
    // purpose, because the translated x87 code was validated at the guest's
    // own precision and rounding to single would change every result.
    if (!(arg(c, 4) & 2u))
        c->fpu_cw = uint16_t(c->fpu_cw | 0x3fu);
    ComObj *dev = com_new(K_D3D9DEVICE);
    if (!dev) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    dev->dev_d3d = d3d->id;
    dev->d3d_version = d3d->d3d_version;
    for (uint32_t i = 0; i < 4; ++i)
        dev->d3d_creation[i] = arg(c, i + 1);
    d9_pipeline(dev->id) = D9Pipeline{};
    fixed_states().erase(dev->id);
    const bool version8 = dev->d3d_version == 8;
    // D3DPRESENT_PARAMETERS: back buffer size and format first, the device
    // window at 28 and Windowed at 32.
    if (present) {
        dev->width = rd32(present + 0);
        dev->height = rd32(present + 4);
        dev->bpp = 32;
        dev->hwnd = rd32(present + (version8 ? 24 : 28));
        dev->samples = multisample_count(rd32(present + 16), version8 ? 0 : rd32(present + 20));
        if (version8) {
            dev->d3d_format = rd32(present + 8) ? rd32(present + 8) : 22;
            dev->d3d_depth_format = rd32(present + 36);
            if (!dev->hwnd)
                dev->hwnd = arg(c, 3);
        }
    }
    uint32_t view = d9_api_view(dev, IF_D3DDEVICE9);
    if (!view) {
        com_release(dev);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    {
        ComObj *bb = device_backbuffer(dev);
        D9Pipeline &pl = d9_pipeline(dev->id);
        bind_color_target(dev, 0, bb); // the device holds its bound target
        pl.scissor[2] = (int32_t)dev->width;
        pl.scissor[3] = (int32_t)dev->height;
        if (present && rd32(present + (version8 ? 32 : 36))) {
            device_depthbuffer(dev);
            pl.rs[7] = 1; // ZENABLE defaults on with an automatic depth buffer
            pl.states_changed();
        }
    }
    LOGW("d3d9: CreateDevice %ux%u, window %08x, %s renderer", dev->width, dev->height, dev->hwnd,
         host_d9_active() ? "GPU" : "CPU");
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);

// ---------------------------------------------------------------------------
// IDirect3DDevice9
//
// Every slot is named and in its interface order, because a guest calls them
// by index. A stub reports itself once and returns D3D_OK: a game that is
// told "no" by a method it expects to work usually stops, and stopping tells
// us less than carrying on does.
// ---------------------------------------------------------------------------
#define D9_STUB(name, argc)                                                                        \
    void Dev_##name(X86 *c) {                                                                      \
        log_once("d3d9.dev." #name, "d3d9: IDirect3DDevice9::" #name " is not implemented");       \
        (void)argc;                                                                                \
        com_ret(c, D3D_OK9);                                                                       \
    }

D9_STUB(EvictManagedResources, 1)
D9_STUB(GetDisplayMode, 3)
D9_STUB(SetCursorProperties, 4)
D9_STUB(SetCursorPosition, 4)
D9_STUB(ShowCursor, 2)
D9_STUB(CreateAdditionalSwapChain, 3)
D9_STUB(GetSwapChain, 3)
D9_STUB(GetRasterStatus, 3)
D9_STUB(SetDialogBoxMode, 2)
D9_STUB(SetGammaRamp, 4)
D9_STUB(GetGammaRamp, 3)
D9_STUB(CreateVolumeTexture, 9)
D9_STUB(UpdateSurface, 5)
D9_STUB(UpdateTexture, 3)
D9_STUB(GetRenderTargetData, 3)
D9_STUB(GetFrontBufferData, 3)
D9_STUB(ColorFill, 4)
D9_STUB(GetTransform, 3)
D9_STUB(MultiplyTransform, 3)
D9_STUB(SetClipPlane, 3)
D9_STUB(GetClipPlane, 3)
D9_STUB(CreateStateBlock, 3)
D9_STUB(BeginStateBlock, 1)
D9_STUB(EndStateBlock, 2)
D9_STUB(SetClipStatus, 2)
D9_STUB(GetClipStatus, 2)
D9_STUB(ValidateDevice, 2)
D9_STUB(SetPaletteEntries, 3)
D9_STUB(GetPaletteEntries, 3)
D9_STUB(SetCurrentTexturePalette, 2)
D9_STUB(GetCurrentTexturePalette, 2)
D9_STUB(GetScissorRect, 2)
D9_STUB(SetSoftwareVertexProcessing, 2)
D9_STUB(GetSoftwareVertexProcessing, 1)
D9_STUB(SetNPatchMode, 2)
D9_STUB(GetNPatchMode, 1)
D9_STUB(ProcessVertices, 6)
D9_STUB(GetVertexDeclaration, 2)
D9_STUB(GetVertexShaderConstantF, 4)
D9_STUB(SetVertexShaderConstantI, 4)
D9_STUB(GetVertexShaderConstantI, 4)
D9_STUB(SetVertexShaderConstantB, 4)
D9_STUB(GetVertexShaderConstantB, 4)
D9_STUB(GetStreamSource, 5)
D9_STUB(GetIndices, 2)
D9_STUB(GetPixelShaderConstantF, 4)
D9_STUB(SetPixelShaderConstantI, 4)
D9_STUB(GetPixelShaderConstantI, 4)
D9_STUB(SetPixelShaderConstantB, 4)
D9_STUB(GetPixelShaderConstantB, 4)
D9_STUB(DrawRectPatch, 4)
D9_STUB(DrawTriPatch, 4)
D9_STUB(DeletePatch, 2)

// ---- Fixed-function state (drawn by the programs of d3d9_ffp.h) -----------
// (this, pMaterial): D3DMATERIAL9, 17 floats.
void Dev_SetMaterial(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t p = arg(c, 1);
    if (!dev || !p || !gm_fits(p, 68)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(fixed_state(dev->id).material, gm_ptr(p), 68);
    com_ret(c, D3D_OK9);
}
void Dev_GetMaterial(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t p = arg(c, 1);
    if (!dev || !p || !gm_fits(p, 68)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(gm_ptr(p), fixed_state(dev->id).material, 68);
    com_ret(c, D3D_OK9);
}
// (this, Index, pLight): D3DLIGHT9, 104 bytes. Any index; only enabled lights shine.
void Dev_SetLight(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1), p = arg(c, 2);
    if (!dev || !p || !gm_fits(p, 104)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    d9ffp::FixedState::Light &l = fixed_state(dev->id).lights[index];
    l.type = rd32(p);
    memcpy(l.f, gm_ptr(p), 104);
    com_ret(c, D3D_OK9);
}
void Dev_GetLight(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1), p = arg(c, 2);
    if (!dev || !p || !gm_fits(p, 104)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const auto &lights = fixed_state(dev->id).lights;
    auto it = lights.find(index);
    if (it == lights.end()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(gm_ptr(p), it->second.f, 104);
    com_ret(c, D3D_OK9);
}
// (this, Index, Enable). Enabling a light never set makes Direct3D's default
// one: white, directional, shining along +z. At most eight shine at once.
void Dev_LightEnable(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1);
    const bool enable = arg(c, 2) != 0;
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    d9ffp::FixedState &fs = fixed_state(dev->id);
    auto it = fs.lights.find(index);
    if (it == fs.lights.end()) {
        d9ffp::FixedState::Light l;
        l.f[1] = l.f[2] = l.f[3] = 1.0f; // diffuse
        l.f[18] = 1.0f;                  // direction
        memcpy(&l.f[0], &l.type, 4);
        it = fs.lights.emplace(index, l).first;
    }
    if (it->second.enabled == enable) {
        com_ret(c, D3D_OK9);
        return;
    }
    if (enable) {
        if (fs.active_light_count >= 8) {
            log_once("d3d9.ffp.lights", "d3d9: a ninth light enabled at once is refused");
            com_ret(c, D3DERR_INVALIDCALL);
            return;
        }
        fs.active_lights[fs.active_light_count++] = index;
    } else {
        uint32_t n = 0;
        for (uint32_t i = 0; i < fs.active_light_count; ++i)
            if (fs.active_lights[i] != index)
                fs.active_lights[n++] = fs.active_lights[i];
        fs.active_light_count = n;
    }
    it->second.enabled = enable;
    com_ret(c, D3D_OK9);
}
// (this, Index, pEnable): TRUE is any nonzero BOOL; Direct3D 9 writes 128.
void Dev_GetLightEnable(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const auto &lights = fixed_state(dev->id).lights;
    auto it = lights.find(index);
    if (it == lights.end()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, it->second.enabled ? 128 : 0);
    com_ret(c, D3D_OK9);
}
// (this, FVF): the declaration an FVF code describes, made once per code and
// device and kept, bound in place of the game's declaration. It was a stub, so
// an FVF draw had no declaration and was skipped.
void Dev_SetFVF(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t fvf = arg(c, 1);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    static auto *made = new std::map<std::pair<uint32_t, uint32_t>, uint32_t>();
    uint32_t &id = (*made)[{dev->id, fvf}];
    ComObj *d = com_get(id);
    // Ids start again after a reset (com_reset), so a remembered one must
    // still be this device's declaration.
    if (d && (d->kind != K_D3D9DECL || d->dev_d3d != dev->id))
        d = nullptr;
    if (!d) {
        std::vector<uint8_t> elements;
        if (!d9ffp::fvf_declaration(fvf, &elements)) {
            if (fvf)
                log_once("d3d9.fvf.noposition", "d3d9: SetFVF(0x%x) has no position; ignored",
                         fvf);
            com_ret(c, fvf ? D3DERR_INVALIDCALL : D3D_OK9);
            return;
        }
        d = com_new(K_D3D9DECL);
        if (!d) {
            com_ret(c, E_OUTOFMEMORY);
            return;
        }
        d->dev_d3d = dev->id;
        d->blob = std::move(elements);
        id = d->id;
    }
    dev->current_viewport = d->id; // the CPU renderer's declaration, as SetVertexDeclaration
    d9_pipeline(dev->id).declaration = d->id;
    fixed_state(dev->id).fvf = fvf;
    com_ret(c, D3D_OK9);
}
void Dev_GetFVF(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, fixed_state(dev->id).fvf);
    com_ret(c, D3D_OK9);
}

// The getters read back what the setters recorded, as a pure device does.
// The stubs returned D3D_OK without writing anything, so a game that saves a
// state, changes it and restores it restored whatever its stack held.
// (this, State, pValue)
void Dev_GetRenderState(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t state = arg(c, 1), out = arg(c, 2);
    if (!dev || state >= 256 || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).rs[state]);
    com_ret(c, D3D_OK9);
}
// (this, Sampler, Type, pValue)
void Dev_GetSamplerState(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t sampler = arg(c, 1), type = arg(c, 2), out = arg(c, 3);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    // Only what SetSamplerState keeps (samplers 0-15, types below 14).
    wr32(out, sampler < 16 && type < 14 ? d9_pipeline(dev->id).sampler_state[sampler][type] : 0);
    com_ret(c, D3D_OK9);
}
// (this, Stage, Type, pValue)
void Dev_GetTextureStageState(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stage = arg(c, 1), type = arg(c, 2), out = arg(c, 3);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, stage < 8 && type < 33 ? d9_pipeline(dev->id).tss[stage][type] : 0);
    com_ret(c, D3D_OK9);
}
// (this, Stage, ppTexture): the bound texture with a reference for the
// caller, or null.
void Dev_GetTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stage = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    ComObj *tex = stage < 16 ? com_get(d9_pipeline(dev->id).sampler_tex[stage]) : nullptr;
    if (tex && tex->kind == K_D3D9TEXTURE) {
        const uint32_t view = d9_api_view(tex, tex->caps ? IF_D3DCUBETEXTURE9 : IF_D3DTEXTURE9);
        if (view) {
            com_addref(tex);
            wr32(out, view);
        }
    }
    com_ret(c, D3D_OK9);
}
// (this, RenderTargetIndex, ppRenderTarget)
void Dev_GetRenderTarget(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1), out = arg(c, 2);
    if (!dev || index > 3 || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    ComObj *s = com_get(d9_pipeline(dev->id).color_target[index]);
    const uint32_t view = s ? d9_api_view(s, IF_D3DSURFACE9) : 0;
    if (!view) {
        com_ret(c, D3DERR_NOTFOUND);
        return;
    }
    com_addref(s);
    wr32(out, view);
    com_ret(c, D3D_OK9);
}

// (this, pRect)
void Dev_GetCreationParameters(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_valid(out, 16)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    for (uint32_t i = 0; i < 4; ++i)
        wr32(out + 4 * i, dev->d3d_creation[i]);
    com_ret(c, D3D_OK9);
}

// A shader owns one immutable copy; binding shares those bytes and retains
// the COM object until it is replaced, unbound, or its device is destroyed.
static void create_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c);
    const uint32_t code = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    if (!code || !gm_valid(code, 8)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t version = rd32(code), major = (version >> 8) & 255, minor = version & 255;
    size_t bytes = 0;
    if ((version >> 16) != (pixel ? 0xffffu : 0xfffeu) ||
        !((major == 1 && minor >= 1 && minor <= (pixel ? 4u : 1u)) ||
          ((major == 2 || major == 3) && !minor)) ||
        !d9sh::bytecode_extent(gm_ptr(code), std::min<size_t>(GUEST_SIZE - code, 1u << 20), &bytes,
                               nullptr)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *shader = com_new(pixel ? K_D3D9PS : K_D3D9VS);
    if (!shader) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    shader->dev_d3d = dev->id;
    shader->d9_shader_holds_device = true;
    com_addref(dev);
    shader->d9_shader =
        std::make_shared<const std::vector<uint8_t>>(gm_ptr(code), gm_ptr(code) + bytes);
    shader->d9_shader_key = d9sh::code_key(shader->d9_shader->data(), bytes);
    const uint32_t view = com_view(shader, pixel ? IF_D3DPIXELSHADER9 : IF_D3DVERTEXSHADER9);
    if (!view) {
        com_release(shader);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, view);
    if (host_d9_active()) {
        const auto &program =
            d9sh::program_for_key(shader->d9_shader_key, shader->d9_shader->data(), bytes);
        if (program.ok)
            host_d9_shader_created(shader->d9_shader->data(), uint32_t(bytes),
                                   shader->d9_shader_key, pixel);
    }
    com_ret(c, D3D_OK9);
}
static void set_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c), *shader = com_this(arg(c, 1));
    if (!dev || (arg(c, 1) && (!shader || shader->kind != (pixel ? K_D3D9PS : K_D3D9VS) ||
                               shader->dev_d3d != dev->id))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    d9_bind_shader(dev->id, pixel, D9ShaderBytes{shader ? shader->d9_shader : nullptr},
                   shader ? shader->d9_shader_key : 0, shader ? shader->id : 0);
    com_ret(c, D3D_OK9);
}
static void get_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c);
    const uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const D9Pipeline &pl = d9_pipeline(dev->id);
    ComObj *shader = com_get(pixel ? pl.ps_object : pl.vs_object);
    const uint32_t view =
        shader ? com_view(shader, pixel ? IF_D3DPIXELSHADER9 : IF_D3DVERTEXSHADER9) : 0;
    if (shader && !view) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    if (shader)
        com_addref(shader);
    wr32(out, view);
    com_ret(c, D3D_OK9);
}
void Dev_CreateVertexShader(X86 *c) {
    create_shader(c, false);
}
void Dev_CreatePixelShader(X86 *c) {
    create_shader(c, true);
}
void Dev_SetVertexShader(X86 *c) {
    set_shader(c, false);
}
void Dev_SetPixelShader(X86 *c) {
    set_shader(c, true);
}
void Dev_GetVertexShader(X86 *c) {
    get_shader(c, false);
}
void Dev_GetPixelShader(X86 *c) {
    get_shader(c, true);
}

static void Shader_GetFunction(X86 *c) {
    ComObj *shader = com_this_arg(c);
    const uint32_t data = arg(c, 1), size = arg(c, 2);
    if (!shader || !shader->d9_shader || !size || !gm_valid(size, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t bytes = (uint32_t)shader->d9_shader->size(), capacity = rd32(size);
    wr32(size, bytes);
    if (data && (capacity < bytes || !gm_valid(data, bytes))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (data)
        memcpy(gm_ptr(data), shader->d9_shader->data(), bytes);
    com_ret(c, D3D_OK9);
}
static void shader_references(ComObj *shader, int32_t, int32_t after) {
    // External references retain the device. A binding alone must not: the
    // device owns that binding, and retaining it back would create a cycle.
    const bool retain = after > int32_t(shader->d9_shader_bindings);
    if (retain == shader->d9_shader_holds_device)
        return;
    shader->d9_shader_holds_device = retain;
    if (ComObj *device = com_get(shader->dev_d3d)) {
        if (retain)
            com_addref(device);
        else
            com_release(device);
    }
}
static void shader_destroy(ComObj *shader) {
    shader->d9_shader.reset();
    shader_references(shader, 0, 0);
}

void Dev_SetScissorRect(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t r = arg(c, 1);
    if (dev && r)
        for (int i = 0; i < 4; ++i)
            d9_pipeline(dev->id).scissor[i] = (int32_t)rd32(r + 4u * (uint32_t)i);
    com_ret(c, dev && r ? D3D_OK9 : D3DERR_INVALIDCALL);
}

static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);
static void device_forget_buffers(ComObj *dev);

// (this, pPresentationParameters): new back buffer and depth buffer at the
// new size; every resource the game made itself survives, as with
// D3DPOOL_MANAGED.
void Dev_Reset(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t present = arg(c, 1);
    if (!dev || !present) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    dev->width = rd32(present + 0);
    dev->height = rd32(present + 4);
    const bool version8 = dev->d3d_version == 8;
    dev->samples = multisample_count(rd32(present + 16), version8 ? 0 : rd32(present + 20));
    if (version8) {
        dev->d3d_format = rd32(present + 8) ? rd32(present + 8) : 22;
        dev->d3d_depth_format = rd32(present + 36);
    }
    // Reset unbinds render targets 1-3 and puts the new back buffer on 0.
    for (uint32_t i = 1; i < 4; ++i)
        bind_color_target(dev, i, nullptr);
    device_forget_buffers(dev);
    ComObj *bb = device_backbuffer(dev);
    D9Pipeline &pl = d9_pipeline(dev->id);
    bind_color_target(dev, 0, bb);
    pl.viewport_set = false;
    pl.scissor[0] = pl.scissor[1] = 0;
    pl.scissor[2] = (int32_t)dev->width;
    pl.scissor[3] = (int32_t)dev->height;
    if (rd32(present + (version8 ? 32 : 36))) { // EnableAutoDepthStencil
        device_depthbuffer(dev);
        pl.rs[7] = 1;
        pl.states_changed();
    } else
        bind_depth_stencil(dev, nullptr);
    LOGW("d3d9: Reset to %ux%u", dev->width, dev->height);
    com_ret(c, D3D_OK9);
}

void Dev_TestCooperativeLevel(X86 *c) {
    com_ret(c, this_device9(c) ? D3D_OK9 : D3DERR_INVALIDCALL);
}
void Dev_GetAvailableTextureMem(X86 *c) {
    set_eax(c, std::max(256u, RECOMP_ADAPTER_VIDEO_MEMORY_MB) * 1024u * 1024u);
}
void Dev_GetNumberOfSwapChains(X86 *c) {
    set_eax(c, 1);
}
void Dev_GetDirect3D(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *d3d = dev ? com_get(dev->dev_d3d) : nullptr;
    uint32_t out = arg(c, 1);
    if (!d3d || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t view = d9_api_view(d3d, IF_D3D9);
    com_addref(d3d);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}
void Dev_GetDeviceCaps(X86 *c) {
    ComObj *dev = this_device9(c);
    put_caps9(arg(c, 1), dev && dev->d3d_version == 8);
    com_ret(c, D3D_OK9);
}
void Dev_BeginScene(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        dev->in_scene = true;
    com_ret(c, dev ? D3D_OK9 : D3DERR_INVALIDCALL);
}
void Dev_EndScene(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        dev->in_scene = false;
    com_ret(c, dev ? D3D_OK9 : D3DERR_INVALIDCALL);
}
// Defined with the resources below.
static ComObj *device_backbuffer(ComObj *dev);
static uint8_t *resource_bytes(ComObj *o);

// (this, Count, pRects, Flags, Color, Z, Stencil). Only the colour target is
// real so far: a 32-bit surface is filled with the colour, over the listed
// D3DRECTs or the whole surface.
void Dev_Clear(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t count = arg(c, 1), rects = arg(c, 2), flags = arg(c, 3), color = arg(c, 4);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        HostD9Target target = gpu_target(dev);
        int32_t vp[4];
        target_viewport(dev, target, vp);
        std::vector<int32_t> rs;
        if (count && rects && gm_fits(rects, count * 16u))
            for (uint32_t i = 0; i < count * 4; ++i)
                rs.push_back((int32_t)rd32(rects + 4 * i));
        float z;
        uint32_t zbits = arg(c, 5);
        memcpy(&z, &zbits, 4);
        host_d9_clear(&target, vp, (uint32_t)(rs.size() / 4), rs.empty() ? nullptr : rs.data(),
                      flags, color, z, arg(c, 6));
        gpu_target_drawn(dev, flags & 1u, flags & 6u);
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *s = com_get(dev->render_target);
    if (!s)
        s = device_backbuffer(dev);
    static uint32_t g_clears = 0;
    if (++g_clears <= 24)
        LOGW("d3d9: clear %u: flags %x colour %08x rects %u, target %u (%ux%u, %u bpp)%s", g_clears,
             flags, color, count, s ? s->id : 0, s ? s->width : 0, s ? s->height : 0,
             s ? s->bpp : 0, (s && s->id == dev->palette_obj) ? ", the back buffer" : "");
    float z;
    const uint32_t zbits = arg(c, 5);
    memcpy(&z, &zbits, 4);
    com_ret(c, d9_raster_clear(dev, s, count, rects, flags, color, z, arg(c, 6))
                   ? D3D_OK9
                   : D3DERR_INVALIDCALL);
}

// (this, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion). The back
// buffer goes to the host as a 32-bit frame: the first thing this module puts
// on screen.
void Dev_Present(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *bb = dev ? device_backbuffer(dev) : nullptr;
    if (bb && gpu_on()) {
        gpu_sync(bb);
        ddraw_external_present_begin();
        host_present(nullptr, (int)bb->width, (int)bb->height, 32, nullptr, (int)bb->pitch);
        host_d9_present(bb->id, bb->width, bb->height);
        ddraw_external_present_end();
        com_ret(c, D3D_OK9);
        return;
    }
    uint8_t *bbytes = resource_bytes(bb);
    if (bb && bbytes) {
        uint32_t lit = 0, n = bb->width * bb->height;
        for (uint32_t i = 0; i < n; i += 97) {
            uint32_t v;
            memcpy(&v, bbytes + (size_t)i * 4, 4);
            if (v & 0x00ffffffu)
                ++lit;
        }
        LOGW("d3d9: present back buffer %u (%ux%u, %u bpp), %u of %u sampled pixels lit, target "
             "now %u",
             bb->id, bb->width, bb->height, bb->bpp, lit, (n + 96) / 97, dev->render_target);
    }
    if (bb && bbytes && bb->bpp == 32) {
        ddraw_external_present_begin();
        host_present(bbytes, (int)bb->width, (int)bb->height, 32, nullptr, (int)bb->pitch);
        ddraw_external_present_end();
    }
    com_ret(c, D3D_OK9);
}

// ---------------------------------------------------------------------------
// Device resources
//
// A game creates textures, buffers and surfaces before it draws anything, and
// then writes its geometry and its images into them. None of that reaches a
// GPU yet, but the storage is real guest memory, so a Lock hands back a
// pointer the game can fill and the contents survive to be drawn later.
// ---------------------------------------------------------------------------
// A resource's contents live in host memory, as a driver's would: the guest
// heap is the game's, and a texture set would exhaust it. The guest sees the
// bytes only while the resource is locked, through a copy made on the first
// lock and written back on the last unlock.
static uint32_t resource_alloc(ComObj *o, uint32_t bytes) {
    if (!bytes)
        return 0;
    o->blob.assign(bytes, 0);
    o->pixels = 0;
    o->pixels_bytes = bytes;
    return 1;
}

// The bytes to read or write right now: the guest copy while locked.
static uint8_t *resource_bytes(ComObj *o) {
    if (!o)
        return nullptr;
    if (o->pixels)
        return gm_ptr(o->pixels);
    return o->blob.empty() ? nullptr : o->blob.data();
}

static void gpu_before_lock(ComObj *s);
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi);
static void gpu_forget(ComObj *o);

// A vertex or index buffer keeps its guest copy between locks (o->stage). It
// is equal to the host bytes whenever the buffer is unlocked, so a Lock hands
// it out without copying anything in, and Unlock compares only the bytes the
// open locks covered. Staging the whole buffer on every lock (allocate, copy
// in, compare, copy out, free) was paid once per draw for a dynamic buffer
// locked to append a few vertices. Only a D3DUSAGE_DYNAMIC buffer keeps its
// copy after the last Unlock: static geometry is written once, and a second
// copy of every track buffer exhausted the guest heap in another port.
static bool keeps_stage(const ComObj *o) {
    return (o->kind == K_D3D9VB || o->kind == K_D3D9IB) && !recomp_ab::old(recomp_ab::LOCKS);
}
static void drop_stage(ComObj *o) {
    if (o->stage) {
        if (o->pixels == o->stage)
            o->pixels = 0;
        heap_free(o->stage);
        o->stage = 0;
    }
}

// [lo, hi) is the byte range the lock hands out; nested locks widen it.
static uint32_t stage_lock(ComObj *o, uint32_t lo = 0, uint32_t hi = ~0u) {
    if (!o || o->blob.empty())
        return 0;
    const uint32_t size = (uint32_t)o->blob.size();
    hi = std::min(hi, size);
    lo = std::min(lo, hi);
    if (o->lock_count == 0)
        gpu_before_lock(o);
    if (o->lock_count++ == 0) {
        if (keeps_stage(o)) {
            if (!o->stage) {
                o->stage = heap_alloc(size, false, 16);
                if (!o->stage) {
                    o->lock_count = 0;
                    return 0;
                }
                memcpy(gm_ptr(o->stage), o->blob.data(), size);
            }
            o->pixels = o->stage;
        } else {
            // A copy kept while the A/B switch selected the persistent path
            // would go stale now that unlocks stop updating it.
            drop_stage(o);
            o->pixels = heap_alloc(size, false, 16);
            if (!o->pixels) {
                o->lock_count = 0;
                return 0;
            }
            memcpy(gm_ptr(o->pixels), o->blob.data(), size);
        }
        o->lock_lo = lo;
        o->lock_hi = hi;
    } else {
        o->lock_lo = std::min(o->lock_lo, lo);
        o->lock_hi = std::max(o->lock_hi, hi);
    }
    return o->pixels;
}

// The first and one past the last byte where a and b differ in [lo, hi), eight bytes a step.
static uint32_t first_difference(const uint8_t *a, const uint8_t *b, uint32_t lo, uint32_t hi) {
    for (; lo + 8 <= hi; lo += 8) {
        uint64_t x, y;
        memcpy(&x, a + lo, 8);
        memcpy(&y, b + lo, 8);
        if (x != y)
            break;
    }
    while (lo < hi && a[lo] == b[lo])
        ++lo;
    return lo;
}
static uint32_t end_of_difference(const uint8_t *a, const uint8_t *b, uint32_t lo, uint32_t hi) {
    for (; hi >= lo + 8; hi -= 8) {
        uint64_t x, y;
        memcpy(&x, a + hi - 8, 8);
        memcpy(&y, b + hi - 8, 8);
        if (x != y)
            break;
    }
    while (hi > lo && a[hi - 1] == b[hi - 1])
        --hi;
    return hi;
}

static void stage_unlock(ComObj *o) {
    if (!o || o->lock_count <= 0)
        return;
    if (--o->lock_count == 0 && o->pixels) {
        const uint8_t *staged = gm_ptr(o->pixels);
        uint32_t lo = 0, hi = (uint32_t)o->blob.size();
        const bool persistent = o->pixels == o->stage;
        if (persistent) {
            // Only the bytes the open locks covered can have changed (Direct3D
            // leaves writes outside a locked range undefined); the comparison
            // narrows that to what the game actually rewrote.
            lo = std::min(o->lock_lo, hi);
            hi = std::min(std::max(o->lock_hi, lo), hi);
        }
        if ((o->kind == K_D3D9VB || o->kind == K_D3D9IB) &&
            (persistent || !recomp_ab::old(recomp_ab::MIRRORS))) {
            // Only the bytes the game changed go back and on to the GPU. A dynamic buffer is
            // locked to append a few vertices at a time, and sending all of it each time has the
            // browser's main thread write megabytes of unchanged geometry to the GPU a frame. The
            // span is found by comparison inside the locked ranges (the whole buffer on the
            // earlier path of the LOCKS A/B switch, which also keeps a write outside them).
            lo = first_difference(staged, o->blob.data(), lo, hi);
            hi = end_of_difference(staged, o->blob.data(), lo, hi);
            if (hi > lo) { // whole 4-byte words: GPU buffer writes take nothing smaller
                lo &= ~3u;
                hi = std::min<uint32_t>((hi + 3) & ~3u, (uint32_t)o->blob.size());
            }
        }
        if (hi > lo)
            memcpy(o->blob.data() + lo, staged + lo, hi - lo);
        o->lock_lo = o->lock_hi = 0;
        if (persistent) {
            o->pixels = 0; // the copy stays in o->stage, equal to blob wherever locked
            if (!(o->d3d_usage & 0x200u)) // D3DUSAGE_DYNAMIC
                drop_stage(o);
        } else {
            heap_free(o->pixels);
            o->pixels = 0;
        }
        if (hi > lo) {
            d9_raster_invalidate(o->id);
            gpu_after_unlock(o, lo, hi);
        }
    }
}

static void d3d9_resource_destroy(ComObj *o) {
    // A 2D or cube texture takes its levels and faces with it, in either
    // version (d3d8.cpp moves them out first, so a level's release cannot
    // reach back to the texture). Version 9 used to leave them alive.
    d3d8_resource_destroy(o);
    // A buffer released while locked: its lock pointer may be the kept copy.
    if (o->pixels && o->pixels != o->stage)
        heap_free(o->pixels);
    o->pixels = 0;
    drop_stage(o);
    o->lock_count = 0;
    o->lock_lo = o->lock_hi = 0;
    o->pixels_bytes = 0;
    std::vector<uint8_t>().swap(o->blob);
    d9_raster_invalidate(o->id);
    gpu_forget(o);
}

// Bytes per pixel for the formats this game creates. D3DFMT_A8R8G8B8 is 21,
// X8R8G8B8 22, R5G6B5 23, A8 28, D24S8 75, and the DXT block formats carry
// their four characters.
static uint32_t format_bytes(uint32_t fmt) {
    switch (fmt) {
    case 27:
    case 28:
    case 41:
    case 50:
    case 52: // R3G3B2, A8, P8, L8, A4L4
        return 1;
    case 23:
    case 24:
    case 25:
    case 26:
    case 29:
    case 30:
    case 40:
    case 51:
    case 60:
    case 61:
    case 70:
    case 73:
    case 80:
    case 81:
    case 111:
        return 2;
    case 36:
    case 113: // A16B16G16R16, A16B16G16R16F
        return 8;
    case 116: // A32B32G32R32F
        return 16;
    default:
        return 4;
    }
}

// The DXT formats, stored as blocks of 4x4 pixels.
static uint32_t dxt_block_bytes(uint32_t fmt) {
    switch (fmt) {
    case 0x31545844u:
        return 8; // DXT1
    case 0x32545844u:
    case 0x33545844u:
    case 0x34545844u:
    case 0x35545844u:
        return 16;
    default:
        return 0;
    }
}

static ComObj *make_surface(ComObj *dev, uint32_t w, uint32_t h, uint32_t fmt) {
    ComObj *s = com_new(K_D3D9SURFACE);
    if (!s)
        return nullptr;
    s->dev_d3d = dev ? dev->id : 0;
    s->d3d_version = dev ? dev->d3d_version : 9;
    s->width = w ? w : 1;
    s->height = h ? h : 1;
    s->rmask = fmt; // the D3DFORMAT, which a Direct3D 9 surface has no other field for
    if (uint32_t block = dxt_block_bytes(fmt)) {
        // A compressed surface is rows of blocks; Pitch is the bytes in one.
        uint32_t bw = (s->width + 3) / 4, bh = (s->height + 3) / 4;
        s->bpp = 0;
        s->pitch = bw * block;
        resource_alloc(s, s->pitch * bh);
    } else {
        s->bpp = format_bytes(fmt) * 8;
        s->pitch = s->width * format_bytes(fmt);
        resource_alloc(s, s->pitch * s->height);
    }
    return s;
}

ComObj *d9_api_surface(ComObj *dev, uint32_t width, uint32_t height, uint32_t format,
                       uint32_t sample_type, uint32_t usage, uint32_t pool) {
    if (!dev || !width || !height)
        return nullptr;
    ComObj *surface = make_surface(dev, width, height, format);
    if (surface) {
        surface->samples = multisample_count(sample_type, 0);
        surface->d3d_usage = usage;
        surface->d3d_pool = pool;
        resource_usage()[surface->id] = usage;
    }
    return surface;
}

// ---- What the CPU renderer needs to know about surfaces and textures --------
bool d9_surface_info(uint32_t surface_id, D9SurfaceInfo *out) {
    ComObj *s = com_get(surface_id);
    if (!s || s->kind != K_D3D9SURFACE)
        return false;
    out->data = resource_bytes(s);
    out->size = s->blob.size();
    out->width = s->width;
    out->height = s->height;
    out->pitch = s->pitch;
    out->format = s->rmask;
    return true;
}

uint32_t d9_texture_level0(uint32_t texture_id) {
    ComObj *t = com_get(texture_id);
    if (!t)
        return 0;
    if (t->kind == K_D3D9SURFACE)
        return t->id;
    if (t->kind != K_D3D9TEXTURE)
        return 0;
    if (t->caps) // a cube: face zero, level zero, once it exists
        return t->surfaces.empty() ? 0 : t->surfaces[0];
    return t->back_obj;
}

// The surface behind a 2D texture's level, or null.
static ComObj *texture_level(ComObj *tex, uint32_t level) {
    if (!tex || tex->kind != K_D3D9TEXTURE || tex->caps)
        return nullptr;
    if (level < tex->surfaces.size())
        return com_get(tex->surfaces[level]);
    return level == 0 ? com_get(tex->back_obj) : nullptr;
}

// ---- The GPU's copies ------------------------------------------------------
// With a GPU renderer running, every texture, surface and buffer has a mirror
// on the host under the object's id; a surface that belongs to a texture is a
// slot of its container's mirror. The shim's bytes stay the reference for what
// the game wrote, the GPU's for what was drawn: a slot the game unlocked is
// uploaded before the next draw that reads it, and a slot the GPU drew into is
// read back before the game locks it.
static const uint32_t USAGE_RENDERTARGET = 1, USAGE_DEPTHSTENCIL = 2;

struct GpuMirror {
    bool defined = false;
    std::vector<uint8_t> cpu_dirty;      // by slot
    std::vector<uint8_t> gpu_newer;      // by slot
    uint32_t dirty_lo = 0, dirty_hi = 0; // buffers: the changed byte range
};
// Mirrors by COM id. Ids are small and dense, and draws look mirrors up many
// times a frame, so this is an indexed table rather than a hash map. A deque
// keeps every reference valid while the table grows, as the map did.
struct GpuMirrorTable {
    std::deque<GpuMirror> slots;
    std::vector<uint8_t> present;
    GpuMirror &operator[](uint32_t id) {
        while (slots.size() <= id) {
            slots.emplace_back();
            present.push_back(0);
        }
        present[id] = 1;
        return slots[id];
    }
    GpuMirror *find(uint32_t id) {
        return id < slots.size() && present[id] ? &slots[id] : nullptr;
    }
    void erase(uint32_t id) {
        if (id < slots.size()) {
            slots[id] = GpuMirror{};
            present[id] = 0;
        }
    }
};
static GpuMirrorTable &gpu_mirrors() {
    static auto *m = new GpuMirrorTable();
    return *m;
}
// A resource's mirror without indexing the table: the deque never moves a mirror, and gpu_forget
// clears the cached pointer before it erases. RECOMP_AB_SWITCHES=mirrors (test only) takes the
// table every time.
static GpuMirror &mirror_of(ComObj *o) {
    if (o->gpu_mirror && !recomp_ab::old(recomp_ab::MIRRORS))
        return *static_cast<GpuMirror *>(o->gpu_mirror);
    GpuMirror &m = gpu_mirrors()[o->id];
    o->gpu_mirror = &m;
    return m;
}
// D3DUSAGE of every texture and surface, by id.
static std::unordered_map<uint32_t, uint32_t> &resource_usage() {
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
static bool gpu_on() {
    return host_d9_active() != 0;
}
static bool is_depth_format(uint32_t fmt) {
    return (fmt >= 70 && fmt <= 82) || fmt == 0x5a544e49u /* INTZ */ ||
           fmt == 0x34324644u /* DF24 */ || fmt == 0x36314644u /* DF16 */;
}

// A surface's place in its mirror: the container's id and the slot, or the
// surface's own id and slot zero.
static uint32_t mirror_slot(ComObj *s, uint32_t *slot) {
    *slot = 0;
    if (!s)
        return 0;
    if (s->kind == K_D3D9SURFACE && s->front_obj) {
        ComObj *tex = com_get(s->front_obj);
        if (tex && tex->kind == K_D3D9TEXTURE) {
            for (size_t i = 0; i < tex->surfaces.size(); ++i)
                if (tex->surfaces[i] == s->id) {
                    *slot = (uint32_t)i;
                    return tex->id;
                }
        }
    }
    return s->id;
}
static HostD9Surface surface_ref(ComObj *s) {
    HostD9Surface r = {0, 0, 0};
    if (!s)
        return r;
    uint32_t slot;
    r.id = mirror_slot(s, &slot);
    ComObj *owner = com_get(r.id);
    if (owner && owner->kind == K_D3D9TEXTURE && owner->caps) {
        r.face = slot % 6;
        r.level = slot / 6;
    } else {
        r.level = slot;
    }
    return r;
}
static uint32_t slot_count(ComObj *o) {
    if (o->kind == K_D3D9TEXTURE)
        return (uint32_t)std::max<size_t>(o->surfaces.size(), 1);
    return 1;
}
static ComObj *slot_surface(ComObj *o, uint32_t slot) {
    if (o->kind != K_D3D9TEXTURE)
        return o;
    return slot < o->surfaces.size() ? com_get(o->surfaces[slot]) : nullptr;
}

// Creates the mirror of a texture or stand-alone surface on first use.
static GpuMirror &gpu_define(ComObj *o) {
    GpuMirror &m = mirror_of(o);
    if (m.defined)
        return m;
    HostD9TextureDesc d{};
    d.id = o->id;
    d.format = o->rmask ? o->rmask : 22;
    d.width = o->width ? o->width : 1;
    d.height = o->height ? o->height : 1;
    uint32_t usage = resource_usage()[o->id];
    if (is_depth_format(d.format))
        usage |= USAGE_DEPTHSTENCIL;
    d.usage = (usage & USAGE_RENDERTARGET ? HOST_D9_USAGE_RENDERTARGET : 0) |
              (usage & USAGE_DEPTHSTENCIL ? HOST_D9_USAGE_DEPTH : 0);
    d.samples = o->kind == K_D3D9SURFACE ? o->samples : 0;
    if (o->kind == K_D3D9TEXTURE && o->caps) {
        d.kind = HOST_D9_TEX_CUBE;
        d.levels = o->pal_flags ? o->pal_flags : 1;
    } else {
        d.kind = HOST_D9_TEX_2D;
        d.levels = o->kind == K_D3D9TEXTURE ? slot_count(o) : 1;
    }
    host_d9_texture_define(&d);
    m.defined = true;
    uint32_t n = o->kind == K_D3D9TEXTURE && o->caps ? 6 * d.levels : slot_count(o);
    // A target's first contents are whatever is drawn; a texture's are what
    // the game wrote, so every slot starts out needing an upload.
    m.cpu_dirty.assign(n, d.usage ? 0 : 1);
    m.gpu_newer.assign(n, 0);
    return m;
}

// Uploads every slot the game changed since the GPU last saw it.
static void gpu_sync(ComObj *o) {
    if (!o)
        return;
    GpuMirror &m = gpu_define(o);
    for (uint32_t slot = 0; slot < m.cpu_dirty.size(); ++slot) {
        if (!m.cpu_dirty[slot])
            continue;
        m.cpu_dirty[slot] = 0;
        ComObj *s = slot_surface(o, slot);
        if (!s || s->blob.empty())
            continue;
        HostD9Surface r = surface_ref(s);
        // Copied, not waited for: on the web the upload would otherwise wait
        // for the browser's main thread on every dirty surface.
        host_d9_texture_upload_copy(o->id, r.face, r.level, resource_bytes(s), s->pitch,
                                    (uint32_t)s->blob.size());
    }
}

// The surface is about to be read or written by the game.
static void gpu_before_lock(ComObj *s) {
    if (!s || s->kind != K_D3D9SURFACE)
        return;
    uint32_t slot;
    uint32_t id = mirror_slot(s, &slot);
    GpuMirror *it = gpu_mirrors().find(id);
    if (!it || slot >= it->gpu_newer.size() || !it->gpu_newer[slot])
        return;
    it->gpu_newer[slot] = 0;
    if (s->bpp == 32 && !s->blob.empty()) {
        HostD9Surface r = surface_ref(s);
        host_d9_texture_read(id, r.face, r.level, s->blob.data(), s->pitch);
    }
}
// The game wrote the resource.
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi) {
    if (!o)
        return;
    if (o->kind == K_D3D9VB || o->kind == K_D3D9IB) {
        GpuMirror &m = mirror_of(o);
        if (!m.defined) { // never uploaded: its first upload carries all of it
            lo = 0;
            hi = (uint32_t)o->blob.size();
        }
        if (m.dirty_hi <= m.dirty_lo) {
            m.dirty_lo = lo;
            m.dirty_hi = hi;
        } else {
            m.dirty_lo = std::min(m.dirty_lo, lo);
            m.dirty_hi = std::max(m.dirty_hi, hi);
        }
        m.defined = true;
        return;
    }
    uint32_t slot;
    uint32_t id = mirror_slot(o, &slot);
    GpuMirror *it = gpu_mirrors().find(id);
    if (!it)
        return; // not mirrored yet: its first use uploads everything
    if (slot < it->cpu_dirty.size())
        it->cpu_dirty[slot] = 1;
}
static void gpu_drawn_into(ComObj *s) {
    if (!s)
        return;
    uint32_t slot;
    uint32_t id = mirror_slot(s, &slot);
    ComObj *owner = com_get(id);
    if (!owner)
        return;
    GpuMirror &m = gpu_define(owner);
    if (slot < m.gpu_newer.size()) {
        m.gpu_newer[slot] = 1;
        m.cpu_dirty[slot] = 0;
    }
}
static void gpu_sync_buffer(ComObj *b) {
    if (!b)
        return;
    GpuMirror &m = mirror_of(b);
    if (!m.defined) {
        m.defined = true;
        m.dirty_lo = 0;
        m.dirty_hi = (uint32_t)b->blob.size();
    }
    if (m.dirty_hi > m.dirty_lo && !b->blob.empty()) {
        uint32_t hi = std::min<uint32_t>(m.dirty_hi, (uint32_t)b->blob.size());
        host_d9_buffer_upload(b->id, (uint32_t)b->blob.size(), m.dirty_lo,
                              b->blob.data() + m.dirty_lo, hi - m.dirty_lo);
    }
    m.dirty_lo = m.dirty_hi = 0;
}
static void gpu_forget(ComObj *o) {
    o->gpu_mirror = nullptr;
    if (GpuMirror *it = gpu_mirrors().find(o->id)) {
        if (o->kind == K_D3D9VB || o->kind == K_D3D9IB)
            host_d9_buffer_drop(o->id);
        else if (it->defined)
            host_d9_texture_drop(o->id);
        gpu_mirrors().erase(o->id);
    }
    resource_usage().erase(o->id);
}

// (this, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle)
void Dev_CreateVertexBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t bytes = arg(c, 1), out = arg(c, 5);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *vb = com_new(K_D3D9VB);
    if (!vb || !resource_alloc(vb, bytes)) {
        if (vb)
            com_release(vb);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    vb->dev_d3d = dev->id;
    vb->d3d_version = dev->d3d_version;
    vb->d3d_usage = arg(c, 2);
    vb->d3d_pool = arg(c, 4);
    vb->d3d_fvf = arg(c, 3);
    vb->buf_bytes = bytes;
    uint32_t view_ = d9_api_view(vb, IF_D3DVERTEXBUFFER9);
    if (!view_) {
        com_release(vb);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle)
void Dev_CreateIndexBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t bytes = arg(c, 1), out = arg(c, 5);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *ib = com_new(K_D3D9IB);
    if (!ib || !resource_alloc(ib, bytes)) {
        if (ib)
            com_release(ib);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    ib->dev_d3d = dev->id;
    ib->d3d_version = dev->d3d_version;
    ib->d3d_usage = arg(c, 2);
    ib->d3d_pool = arg(c, 4);
    ib->buf_bytes = bytes;
    ib->rmask = arg(c, 3) == 102 ? 4 : 2; // D3DFMT_INDEX32 or D3DFMT_INDEX16
    uint32_t view_ = d9_api_view(ib, IF_D3DINDEXBUFFER9);
    if (!view_) {
        com_release(ib);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
void Dev_CreateTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, 5), out = arg(c, 7);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    tex->dev_d3d = dev->id;
    tex->d3d_version = dev->d3d_version;
    tex->d3d_usage = arg(c, 4);
    tex->d3d_pool = arg(c, 6);
    tex->width = w;
    tex->height = h;
    tex->bpp = format_bytes(fmt) * 8;
    // Every level gets its own surface. Levels = 0 asks for the full chain;
    // a game uploads each level with its own LockRect.
    uint32_t levels = arg(c, 3);
    if (!levels) {
        levels = 1;
        for (uint32_t d = std::max(w, h); d > 1; d >>= 1)
            ++levels;
    }
    tex->rmask = fmt;
    uint32_t usage = arg(c, 4);
    resource_usage()[tex->id] = usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL);
    for (uint32_t l = 0; l < levels && l < 16; ++l) {
        ComObj *s = make_surface(dev, std::max(w >> l, 1u), std::max(h >> l, 1u), fmt);
        if (!s)
            break;
        s->front_obj = tex->id;
        s->d3d_usage = tex->d3d_usage;
        s->d3d_pool = tex->d3d_pool;
        tex->surfaces.push_back(s->id);
    }
    tex->back_obj = tex->surfaces.empty() ? 0 : tex->surfaces[0];
    uint32_t view_ = d9_api_view(tex, IF_D3DTEXTURE9);
    if (!view_) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)
void Dev_CreateCubeTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t edge = arg(c, 1), fmt = arg(c, 4), out = arg(c, 6);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    tex->dev_d3d = dev->id;
    tex->width = tex->height = edge;
    tex->bpp = format_bytes(fmt) * 8;
    tex->rmask = fmt;
    tex->caps = 1; // a cube map
    tex->d3d_version = dev->d3d_version;
    tex->d3d_usage = arg(c, 3);
    tex->d3d_pool = arg(c, 5);
    tex->pal_flags = arg(c, 2) ? arg(c, 2) : 1; // levels, reusing a field cubes do not use
    resource_usage()[tex->id] = arg(c, 3) & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL);
    uint32_t view = d9_api_view(tex, IF_D3DCUBETEXTURE9);
    if (!view) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    LOGV("d3d9: CreateCubeTexture %u -> %08x into %08x", edge, view, out);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

static void create_surface_method(X86 *c, uint32_t out_index, uint32_t fmt_index, uint32_t usage) {
    ComObj *dev = this_device9(c);
    uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, fmt_index), out = arg(c, out_index);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = make_surface(dev, w, h, fmt);
    if (!s) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    // CreateRenderTarget and CreateDepthStencilSurface: MultiSample, MultisampleQuality.
    if (out_index == 7)
        s->samples = multisample_count(arg(c, 4), arg(c, 5));
    resource_usage()[s->id] = usage;
    uint32_t view_ = d9_api_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
// (this, Width, Height, Format, MultiSample, MultisampleQuality, Discard,
//  ppSurface, pSharedHandle)
void Dev_CreateDepthStencilSurface(X86 *c) {
    create_surface_method(c, 7, 3, USAGE_DEPTHSTENCIL);
}
// (this, Width, Height, Format, MultiSample, MultisampleQuality, Lockable,
//  ppSurface, pSharedHandle)
void Dev_CreateRenderTarget(X86 *c) {
    create_surface_method(c, 7, 3, USAGE_RENDERTARGET);
}
// (this, Width, Height, Format, Pool, ppSurface, pSharedHandle)
void Dev_CreateOffscreenPlainSurface(X86 *c) {
    create_surface_method(c, 5, 3, 0);
}

// The device's own back buffer and depth buffer, made on first ask and kept.
// The back buffer lives in its own field. It used to share render_target,
// which SetRenderTarget overwrites, so after the game switched targets the
// "back buffer" was whatever it had last drawn into.
static ComObj *device_backbuffer(ComObj *dev) {
    ComObj *s = com_get(dev->palette_obj);
    if (!s) {
        s = make_surface(dev, dev->width ? dev->width : 640, dev->height ? dev->height : 480,
                         dev->d3d_format);
        dev->palette_obj = s ? s->id : 0;
        if (s)
            s->samples = dev->samples;
        if (s)
            resource_usage()[s->id] = USAGE_RENDERTARGET;
        if (s)
            s->d3d_usage = USAGE_RENDERTARGET;
        if (s && !com_get(dev->render_target))
            dev->render_target = s->id;
    }
    return s;
}

// (this, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter): a
// nearest-neighbour copy between 32-bit surfaces, rectangles honoured. This is
// how a game moves an off-screen target onto the back buffer.
void Dev_StretchRect(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 3));
    if (!src || !dst || src->kind != K_D3D9SURFACE || dst->kind != K_D3D9SURFACE) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        uint32_t slot;
        ComObj *sowner = com_get(mirror_slot(src, &slot));
        ComObj *downer = com_get(mirror_slot(dst, &slot));
        if (sowner)
            gpu_sync(sowner);
        if (downer)
            gpu_sync(downer);
        int32_t sr[4] = {0, 0, (int32_t)src->width, (int32_t)src->height};
        int32_t dr[4] = {0, 0, (int32_t)dst->width, (int32_t)dst->height};
        if (arg(c, 2))
            for (int i = 0; i < 4; ++i)
                sr[i] = (int32_t)rd32(arg(c, 2) + 4u * (uint32_t)i);
        if (arg(c, 4))
            for (int i = 0; i < 4; ++i)
                dr[i] = (int32_t)rd32(arg(c, 4) + 4u * (uint32_t)i);
        host_d9_stretch(surface_ref(src), sr, surface_ref(dst), dr, arg(c, 5));
        gpu_drawn_into(dst);
        com_ret(c, D3D_OK9);
        return;
    }
    uint8_t *sbytes = resource_bytes(src), *dbytes = resource_bytes(dst);
    if (!sbytes || !dbytes || src->bpp != 32 || dst->bpp != 32) {
        com_ret(c, D3D_OK9);
        return;
    }
    auto rect = [](uint32_t p, ComObj *s, int32_t r[4]) {
        r[0] = 0;
        r[1] = 0;
        r[2] = (int32_t)s->width;
        r[3] = (int32_t)s->height;
        if (p)
            for (int i = 0; i < 4; ++i)
                r[i] = (int32_t)rd32(p + 4u * (uint32_t)i);
        r[0] = std::max(r[0], 0);
        r[1] = std::max(r[1], 0);
        r[2] = std::min(r[2], (int32_t)s->width);
        r[3] = std::min(r[3], (int32_t)s->height);
    };
    ComObj *dev_ = com_get(dst->dev_d3d);
    static uint32_t g_stretches = 0;
    if (++g_stretches <= 24)
        LOGW("d3d9: StretchRect %u (%ux%u, %u bpp) -> %u (%ux%u, %u bpp)%s", src->id, src->width,
             src->height, src->bpp, dst->id, dst->width, dst->height, dst->bpp,
             (dev_ && dst->id == dev_->palette_obj) ? ", onto the back buffer" : "");
    int32_t sr[4], dr[4];
    rect(arg(c, 2), src, sr);
    rect(arg(c, 4), dst, dr);
    int32_t sw = sr[2] - sr[0], sh = sr[3] - sr[1], dw = dr[2] - dr[0], dh = dr[3] - dr[1];
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        com_ret(c, D3D_OK9);
        return;
    }
    for (int32_t y = 0; y < dh; ++y) {
        int32_t sy = sr[1] + y * sh / dh;
        uint8_t *drow = dbytes + (size_t)(dr[1] + y) * dst->pitch;
        const uint8_t *srow = sbytes + (size_t)sy * src->pitch;
        for (int32_t x = 0; x < dw; ++x)
            memcpy(drow + 4 * (dr[0] + x), srow + 4 * (sr[0] + x * sw / dw), 4);
    }
    d9_raster_invalidate(dst->id);
    com_ret(c, D3D_OK9);
}
// Bound surfaces are referenced as Direct3D 9 does it: SetRenderTarget and
// SetDepthStencilSurface AddRef the new surface and release the old one, so a
// target the game releases while it is bound stays valid until it is unbound.
// Before, a binding took no reference: a depth surface the game released while
// bound was destroyed at once, and Reset released the bound depth surface even
// when it was the game's own. A version 8 device keeps its own references
// (d3d8.cpp) and only records the binding here.
static void bind_color_target(ComObj *dev, uint32_t index, ComObj *s) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    const uint32_t id = s ? s->id : 0;
    ComObj *old = com_get(pl.color_target[index]);
    pl.color_target[index] = id;
    if (index == 0)
        dev->render_target = id;
    if (dev->d3d_version == 8 || old == s)
        return;
    if (s)
        com_addref(s);
    if (old)
        com_release(old);
}
static void bind_depth_stencil(ComObj *dev, ComObj *s) {
    ComObj *old = com_get(dev->zbuffer_obj);
    dev->zbuffer_obj = s ? s->id : 0;
    if (dev->d3d_version == 8 || old == s)
        return;
    if (s)
        com_addref(s);
    if (old)
        com_release(old);
}

// The device's automatic depth buffer, made on first use and bound. Version 9
// keeps it apart from the binding, so Reset releases only this one and never a
// depth surface the game made; version 8 tracks it in d3d8.cpp.
static ComObj *device_depthbuffer(ComObj *dev) {
    const bool version8 = dev->d3d_version == 8;
    uint32_t &own = version8 ? dev->zbuffer_obj : dev->d9_auto_depth;
    ComObj *s = com_get(own);
    if (!s) {
        s = make_surface(dev, dev->width ? dev->width : 640, dev->height ? dev->height : 480,
                         dev->d3d_depth_format);
        own = s ? s->id : 0; // the device's own reference, make_surface's
        if (!s)
            return nullptr;
        s->samples = dev->samples;
        resource_usage()[s->id] = USAGE_DEPTHSTENCIL;
        s->d3d_usage = USAGE_DEPTHSTENCIL;
    }
    if (!version8 && dev->zbuffer_obj != s->id)
        bind_depth_stencil(dev, s);
    return s;
}

// Drops the device's own back buffer, its depth binding and its automatic
// depth buffer; the game's references keep those objects alive, and the device
// makes new ones on next use.
static void device_forget_buffers(ComObj *dev) {
    if (dev->d3d_version == 8) {
        for (uint32_t *field : {&dev->palette_obj, &dev->zbuffer_obj}) {
            ComObj *s = com_get(*field);
            if (s)
                com_release(s);
            *field = 0;
        }
        return;
    }
    if (ComObj *s = com_get(dev->palette_obj))
        com_release(s);
    dev->palette_obj = 0;
    bind_depth_stencil(dev, nullptr);
    if (ComObj *s = com_get(dev->d9_auto_depth))
        com_release(s);
    dev->d9_auto_depth = 0;
}

// (this, iSwapChain, iBackBuffer, Type, ppBackBuffer)
void Dev_GetBackBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, dev && dev->d3d_version == 8 ? 3 : 4);
    ComObj *s = dev ? device_backbuffer(dev) : nullptr;
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(s);
    uint32_t view_ = d9_api_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// The bound depth-stencil surface (the automatic one until the game sets
// another). With none bound Direct3D 9 answers D3DERR_NOTFOUND; making a new
// device-sized buffer instead, which the game never released, leaked one per
// call in NFS Carbon.
void Dev_GetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = com_get(dev->zbuffer_obj);
    if (!s) {
        wr32(out, 0);
        com_ret(c, D3DERR_NOTFOUND);
        return;
    }
    com_addref(s);
    uint32_t view_ = d9_api_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

void Dev_SetRenderTarget(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *s = com_this(arg(c, 2));
    uint32_t index = arg(c, 1);
    if (!dev || index > 3) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (index == 0 && !(s && s->kind == K_D3D9SURFACE)) {
        com_ret(c, D3DERR_INVALIDCALL); // target 0 cannot be unset
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    bind_color_target(dev, index, (s && s->kind == K_D3D9SURFACE) ? s : nullptr);
    if (index == 0) {
        // Setting render target 0 resets the viewport and scissor to the target.
        pl.viewport_set = false;
        pl.scissor[0] = pl.scissor[1] = 0;
        pl.scissor[2] = (int32_t)s->width;
        pl.scissor[3] = (int32_t)s->height;
    }
    com_ret(c, D3D_OK9);
}

void Dev_SetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *s = com_this(arg(c, 1));
    if (dev)
        bind_depth_stencil(dev, (s && s->kind == K_D3D9SURFACE) ? s : nullptr);
    com_ret(c, D3D_OK9);
}

// (this, pVertexElements, ppDecl): the element array is kept verbatim, since
// only the eventual renderer can interpret it.
void Dev_CreateVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t elems = arg(c, 1), out = arg(c, 2);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *d = com_new(K_D3D9DECL);
    if (!d) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    d->dev_d3d = dev->id;
    // D3DVERTEXELEMENT9 is 8 bytes and the array ends with D3DDECL_END, whose
    // Stream is 0xff. Copy until that, so the declaration survives the call.
    for (uint32_t i = 0; i < 64 && elems; ++i) {
        uint32_t e = elems + i * 8u;
        uint16_t stream = rd16(e);
        for (int b = 0; b < 8; ++b)
            d->blob.push_back(gm_ptr(e)[b]);
        if (stream == 0xff)
            break;
    }
    uint32_t view_ = d9_api_view(d, IF_D3DVERTEXDECL9);
    if (!view_) {
        com_release(d);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

void Dev_SetVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *d = com_this(arg(c, 1));
    if (dev) {
        dev->current_viewport = (d && d->kind == K_D3D9DECL) ? d->id : 0;
        d9_pipeline(dev->id).declaration = dev->current_viewport;
        fixed_state(dev->id).fvf = 0; // a declaration replaces the FVF
    }
    com_ret(c, D3D_OK9);
}

// (this, Type, ppQuery). A query with no GPU behind it answers immediately.
void Dev_CreateQuery(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t type = arg(c, 1), out = arg(c, 2);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (!out) {
        com_ret(c, D3D_OK9); // a null out pointer only asks whether the type exists
        return;
    }
    ComObj *q = com_new(K_D3D9QUERY);
    if (!q) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    q->dev_d3d = dev ? dev->id : 0;
    q->dev_type = type;
    uint32_t view_ = d9_api_view(q, IF_D3DQUERY9);
    if (!view_) {
        com_release(q);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, StreamNumber, pStreamData, OffsetInBytes, Stride)
void d9_api_stream(ComObj *dev, uint32_t stream, ComObj *vb, uint32_t offset, uint32_t stride) {
    if (dev && stream < 8) {
        D9Pipeline::Stream &st = d9_pipeline(dev->id).stream[stream];
        st.vb = (vb && vb->kind == K_D3D9VB) ? vb->id : 0;
        st.offset = offset;
        st.stride = stride;
    }
    if (dev && stream == 0 && vb && vb->kind == K_D3D9VB) {
        dev->back_obj = vb->id;
        dev->lock_off = offset;
        dev->rate = stride; // the stride, reusing a spare field
    }
}

void Dev_SetStreamSource(X86 *c) {
    d9_api_stream(this_device9(c), arg(c, 1), com_this(arg(c, 2)), arg(c, 3), arg(c, 4));
    com_ret(c, D3D_OK9);
}

void Dev_SetStreamSourceFreq(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stream = arg(c, 1), frequency = arg(c, 2),
                   flags = frequency & ~HOST_D9_STREAM_DIVIDER;
    if (!dev || stream >= 8 || !(frequency & HOST_D9_STREAM_DIVIDER) ||
        flags == (HOST_D9_STREAM_INDEXED | HOST_D9_STREAM_INSTANCE) ||
        (stream && (flags & HOST_D9_STREAM_INDEXED)) ||
        (!stream && (flags & HOST_D9_STREAM_INSTANCE))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    d9_pipeline(dev->id).stream[stream].frequency = frequency;
    com_ret(c, D3D_OK9);
}
void Dev_GetStreamSourceFreq(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stream = arg(c, 1), out = arg(c, 2);
    if (!dev || stream >= 8 || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).stream[stream].frequency);
    com_ret(c, D3D_OK9);
}

void d9_api_indices(ComObj *dev, ComObj *ib) {
    if (dev) {
        dev->front_obj = (ib && ib->kind == K_D3D9IB) ? ib->id : 0;
        d9_pipeline(dev->id).index_buffer = dev->front_obj;
    }
}

void Dev_SetIndices(X86 *c) {
    d9_api_indices(this_device9(c), com_this(arg(c, 1)));
    com_ret(c, D3D_OK9);
}

// (this, Stage, pTexture): the texture behind a sampler, for the renderer.
// The textures each version 9 device holds a reference to, by stage (Dev_SetTexture).
static std::unordered_map<uint32_t, std::array<uint32_t, 16>> &set_textures() {
    static auto *m = new std::unordered_map<uint32_t, std::array<uint32_t, 16>>();
    return *m;
}
void Dev_SetTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *tex = com_this(arg(c, 2));
    uint32_t stage = arg(c, 1);
    if (dev) {
        dev->texture_handle = tex ? tex->id : 0;
        if (stage < 16) {
            d9_pipeline(dev->id).sampler_tex[stage] = tex ? tex->id : 0;
            // Direct3D 9 keeps a reference to each texture set on a stage, so one the
            // game releases while it is still set stays alive until it is replaced.
            // Kept apart from sampler_tex, which D3DX effects also write without
            // references (a version 8 device keeps its own, d3d8.cpp D8_SetTexture).
            if (dev->d3d_version != 8) {
                uint32_t &held = set_textures()[dev->id][stage];
                if (held != (tex ? tex->id : 0)) {
                    if (tex)
                        com_addref(tex);
                    if (ComObj *old = com_get(held))
                        com_release(old);
                    held = tex ? tex->id : 0;
                }
            }
        }
    }
    com_ret(c, D3D_OK9);
}

// The draws. Nothing is rasterized yet, so each one reports what it was asked
// to draw: the geometry a renderer would need is all in these arguments, and
// a run that names them is the specification for the work that comes next.
// The first few are reported in full and then every 256th, because a game at
// speed issues thousands.
static uint32_t g_draws = 0;

static const char *prim_name(uint32_t type) {
    switch (type) {
    case 1:
        return "point list";
    case 2:
        return "line list";
    case 3:
        return "line strip";
    case 4:
        return "triangle list";
    case 5:
        return "triangle strip";
    case 6:
        return "triangle fan";
    default:
        return "unknown primitive";
    }
}

// The first few draws, then (with verbose logging) every 256th: a race makes
// thousands a frame, and writing even one in 256 cost a percent of the frame.
static bool draw_worth_reporting() {
    ++g_draws;
    return g_draws <= 8 || ((g_draws % 256) == 0 && log_level() >= 2);
}

// In 64 bits: a garbage primitive count must not wrap to a size that fits.
static uint64_t primitive_vertices(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1:
        return count; // points
    case 2:
        return uint64_t(count) * 2; // lines
    case 3:
        return uint64_t(count) + 1; // line strip
    case 4:
        return uint64_t(count) * 3; // triangles
    case 5:
    case 6:
        return uint64_t(count) + 2;
    default:
        return 0;
    }
}

// Direct3D 9 refuses a draw above the device's MaxPrimitiveCount, or one
// naming more vertices than MaxVertexIndex allows, with D3DERR_INVALIDCALL.
// Checked before any size arithmetic: in NFS Carbon a garbage count of
// 0x80000000 + n once wrapped a 32-bit vertex size to one that fitted the
// arena and grew the process by hundreds of gigabytes.
static bool draw_refused(const char *what, uint32_t prim, uint32_t count, uint64_t vertices) {
    if (count <= D9_MAX_PRIMITIVE_COUNT && vertices <= uint64_t(D9_MAX_VERTEX_INDEX) + 1)
        return false;
    static uint32_t refused = 0;
    if (++refused <= 8 || (refused & (refused - 1)) == 0)
        LOGW("d3d9: %s refused (D3DERR_INVALIDCALL): %u primitives of type %u, %llu vertices "
             "(%u refused so far)",
             what, count, prim, (unsigned long long)vertices, refused);
    return true;
}

// The render target and depth buffer a draw or clear goes to, mirrored.
static HostD9Target gpu_target(ComObj *dev) {
    HostD9Target t{};
    D9Pipeline &pl = d9_pipeline(dev->id);
    for (int i = 0; i < 4; ++i) {
        ComObj *s = com_get(pl.color_target[i]);
        if (i == 0 && !s)
            s = com_get(dev->render_target) ? com_get(dev->render_target) : device_backbuffer(dev);
        if (!s)
            continue;
        uint32_t slot;
        ComObj *owner = com_get(mirror_slot(s, &slot));
        if (owner)
            gpu_sync(owner);
        t.color[i] = surface_ref(s);
    }
    if (ComObj *z = com_get(dev->zbuffer_obj)) {
        uint32_t slot;
        ComObj *owner = com_get(mirror_slot(z, &slot));
        if (owner)
            gpu_sync(owner);
        t.depth = surface_ref(z);
    }
    return t;
}
static void gpu_target_drawn(ComObj *dev, bool color, bool depth) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (color)
        for (int i = 0; i < 4; ++i) {
            ComObj *s = com_get(pl.color_target[i]);
            if (i == 0 && !s)
                s = com_get(dev->render_target);
            gpu_drawn_into(s);
        }
    if (depth)
        gpu_drawn_into(com_get(dev->zbuffer_obj));
}
static void target_viewport(ComObj *dev, const HostD9Target &t, int32_t vp[4]) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (pl.viewport_set) {
        for (int i = 0; i < 4; ++i)
            vp[i] = (int32_t)pl.viewport[i];
        return;
    }
    ComObj *rt =
        com_get(pl.color_target[0]) ? com_get(pl.color_target[0]) : com_get(dev->render_target);
    (void)t;
    vp[0] = vp[1] = 0;
    vp[2] = rt ? (int32_t)rt->width : (int32_t)dev->width;
    vp[3] = rt ? (int32_t)rt->height : (int32_t)dev->height;
}

// ---- Draws with no shader bound --------------------------------------------
// The fixed-function programs' constant registers for the draw being built.
// The host reads a draw's constants while host_d9_draw runs, as it must for
// the device's own registers, so one set serves every draw.
static float g_ffp_vconst[d9ffp::VS_CONSTANTS][4], g_ffp_pconst[d9ffp::PS_CONSTANTS][4];

// What each of the first eight samplers has bound, as the cascade declares it.
static d9ffp::Textures ffp_textures(const D9Pipeline &pl) {
    d9ffp::Textures t;
    for (int s = 0; s < 8; ++s) {
        ComObj *tex = com_get(pl.sampler_tex[s]);
        if (!tex)
            continue;
        if (tex->kind == K_D3D9TEXTURE)
            t.kind[s] = tex->caps ? d9sh::S_CUBE : d9sh::S_2D;
        else if (tex->kind == K_D3D9SURFACE)
            t.kind[s] = d9sh::S_2D;
    }
    return t;
}
// A generated program, told to the GPU host once, as CreateVertexShader and
// CreatePixelShader tell it about the game's.
static void ffp_announce(d9ffp::Program &p, bool pixel) {
    if (p.announced || !host_d9_active())
        return;
    p.announced = true;
    host_d9_shader_created(p.bytes.data(), (uint32_t)p.bytes.size(), p.key, pixel ? 1 : 0);
}

// A draw with no vertex or pixel shader bound, drawn with the fixed-function
// programs its state describes (d3d9_ffp.h) for the half that has none. It
// was skipped before: NFSMW's highlighted menu labels are drawn this way. Kept
// apart from gpu_draw so a draw with both shaders pays nothing for it; the
// rest follows gpu_draw, and the constants carry no serial (read every time).
static void gpu_draw_fixed(ComObj *dev, HostD9Draw &d) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    ComObj *decl = com_get(pl.declaration);
    if (!decl || decl->kind != K_D3D9DECL) {
        log_once("d3d9.gpu.nodecl.ffp",
                 "d3d9: a fixed-function draw with no vertex declaration or FVF is skipped");
        return;
    }
    const d9ffp::FixedState &fs = fixed_state(dev->id);
    d.target = gpu_target(dev);
    target_viewport(dev, d.target, d.viewport);
    d.depth_range[0] = pl.viewport_z[0];
    d.depth_range[1] = pl.viewport_z[1];
    for (int i = 0; i < 4; ++i)
        d.scissor[i] = pl.scissor[i];
    if (!pl.vs.empty()) {
        if (!pl.vs_key)
            pl.vs_key = d9sh::code_key(pl.vs.data(), pl.vs.size());
        const d9sh::Program &vs = d9sh::program_for_key(pl.vs_key, pl.vs.data(), pl.vs.size());
        d.vs_key = pl.vs_key;
        d.vs = pl.vs.data();
        d.vs_size = (uint32_t)pl.vs.size();
        d.vconst = &pl.vconst[0][0];
        d.vconst_count = std::min<uint32_t>(vs.max_const, 256);
    } else {
        d9ffp::Program &ffp = d9ffp::vertex_program(
            pl, fs, decl->blob.data(), (uint32_t)decl->blob.size(), d.viewport, d.depth_range,
            !pl.ps.empty(), g_ffp_vconst);
        ffp_announce(ffp, false);
        d.vs_key = ffp.key;
        d.vs = ffp.bytes.data();
        d.vs_size = (uint32_t)ffp.bytes.size();
        d.vconst = &g_ffp_vconst[0][0];
        d.vconst_count = d9ffp::VS_CONSTANTS;
    }
    if (!pl.ps.empty()) {
        if (!pl.ps_key)
            pl.ps_key = d9sh::code_key(pl.ps.data(), pl.ps.size());
        const d9sh::Program &ps = d9sh::program_for_key(pl.ps_key, pl.ps.data(), pl.ps.size());
        d.ps_key = pl.ps_key;
        d.ps = pl.ps.data();
        d.ps_size = (uint32_t)pl.ps.size();
        d.pconst = &pl.pconst[0][0];
        d.pconst_count = std::min<uint32_t>(ps.max_const, 32);
    } else {
        d9ffp::Program &ffp = d9ffp::pixel_program(pl, fs, ffp_textures(pl), g_ffp_pconst);
        ffp_announce(ffp, true);
        d.ps_key = ffp.key;
        d.ps = ffp.bytes.data();
        d.ps_size = (uint32_t)ffp.bytes.size();
        d.pconst = &g_ffp_pconst[0][0];
        d.pconst_count = d9ffp::PS_CONSTANTS;
    }
    static uint64_t ffp_draws = 0;
    if (++ffp_draws == 1 || (ffp_draws & (ffp_draws - 1)) == 0)
        LOGW("d3d9: %llu draws through the fixed-function pipeline so far (this one: %s vertex, "
             "%s pixel)",
             (unsigned long long)ffp_draws, pl.vs.empty() ? "fixed" : "shader",
             pl.ps.empty() ? "fixed" : "shader");
    d.decl = decl->blob.data();
    d.decl_size = (uint32_t)decl->blob.size();
    d.decl_id = decl->id;
    for (int i = 0; i < 8; ++i) {
        d.stream[i].frequency = pl.stream[i].frequency;
        if (d.stream[i].stride && !d.stream[i].buffer)
            continue; // the draw's own inline vertices
        ComObj *vb = com_get(pl.stream[i].vb);
        if (!vb)
            continue;
        gpu_sync_buffer(vb);
        d.stream[i].buffer = vb->id;
        d.stream[i].offset = pl.stream[i].offset;
        d.stream[i].stride = pl.stream[i].stride;
    }
    for (int s = 0; s < 16; ++s) {
        ComObj *tex = com_get(pl.sampler_tex[s]);
        if (!tex || (tex->kind != K_D3D9TEXTURE && tex->kind != K_D3D9SURFACE))
            continue;
        gpu_sync(tex);
        d.sampler_texture[s] = tex->id;
    }
    d.sampler_state = &pl.sampler_state[0][0];
    d.render_state = pl.rs;
    d.render_state_set = (const uint8_t *)pl.rs_set;
    d.state_version = pl.state_version;
    // A generated pixel program projects with texldp itself; only a game's
    // ps_1_x reads the projected stages from the draw.
    if (!pl.ps.empty())
        for (int st = 0; st < 8; ++st)
            if (pl.tss[st][24] & 0x100) // D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_PROJECTED
                d.projected_mask |= 1u << st;
    d.label = pl.label;
    d.instance_count = (pl.stream[0].frequency & HOST_D9_STREAM_INDEXED)
                           ? pl.stream[0].frequency & HOST_D9_STREAM_DIVIDER
                           : 1;
    host_d9_draw(&d);
    bool color_write = !pl.rs_set[168] || (pl.rs[168] & 0xf) != 0;
    bool z_write = (!pl.rs_set[14] || pl.rs[14]) && (!pl.rs_set[7] || pl.rs[7]);
    gpu_target_drawn(dev, color_write, z_write);
}

// One draw, handed to the GPU renderer with everything the device has bound.
static void gpu_draw(ComObj *dev, HostD9Draw &d) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (pl.vs.empty() || pl.ps.empty()) {
        if (dev->d3d_version != 8) {
            gpu_draw_fixed(dev, d);
            return;
        }
        log_once("d3d9.gpu.ffp", "d3d9: a version 8 draw with no programs bound is skipped");
        return;
    }
    d.target = gpu_target(dev);
    target_viewport(dev, d.target, d.viewport);
    d.depth_range[0] = pl.viewport_z[0];
    d.depth_range[1] = pl.viewport_z[1];
    for (int i = 0; i < 4; ++i)
        d.scissor[i] = pl.scissor[i];
    if (!pl.vs_key)
        pl.vs_key = d9sh::code_key(pl.vs.data(), pl.vs.size());
    if (!pl.ps_key)
        pl.ps_key = d9sh::code_key(pl.ps.data(), pl.ps.size());
    // The decoded programs of the last keys asked for: program_for_key takes a lock and a hash
    // lookup, and consecutive draws mostly keep their shaders. Its cache never erases, so the
    // pointers stay valid.
    thread_local uint64_t last_vs_key = 0, last_ps_key = 0;
    thread_local const d9sh::Program *last_vs = nullptr, *last_ps = nullptr;
    const bool lookup_always = recomp_ab::old(recomp_ab::MIRRORS);
    if (!last_vs || last_vs_key != pl.vs_key || lookup_always) {
        last_vs = &d9sh::program_for_key(pl.vs_key, pl.vs.data(), pl.vs.size());
        last_vs_key = pl.vs_key;
    }
    if (!last_ps || last_ps_key != pl.ps_key || lookup_always) {
        last_ps = &d9sh::program_for_key(pl.ps_key, pl.ps.data(), pl.ps.size());
        last_ps_key = pl.ps_key;
    }
    const d9sh::Program &vs = *last_vs;
    const d9sh::Program &ps = *last_ps;
    d.vs_key = pl.vs_key;
    d.ps_key = pl.ps_key;
    d.vs = pl.vs.data();
    d.vs_size = (uint32_t)pl.vs.size();
    d.ps = pl.ps.data();
    d.ps_size = (uint32_t)pl.ps.size();
    d.vconst = &pl.vconst[0][0];
    d.vconst_count = std::min<uint32_t>(vs.max_const, 256);
    d.pconst = &pl.pconst[0][0];
    d.pconst_count = std::min<uint32_t>(ps.max_const, 32);
    d.vconst_serial = pl.vconst_serial; // 0 (unknown) for writers that keep no serial
    d.pconst_serial = pl.pconst_serial;
    ComObj *decl = com_get(pl.declaration);
    if (!decl || decl->kind != K_D3D9DECL) {
        log_once("d3d9.gpu.nodecl", "d3d9: a draw with no vertex declaration is skipped");
        return;
    }
    d.decl = decl->blob.data();
    d.decl_size = (uint32_t)decl->blob.size();
    d.decl_id = decl->id;
    for (int i = 0; i < 8; ++i) {
        d.stream[i].frequency = pl.stream[i].frequency;
        if (d.stream[i].stride && !d.stream[i].buffer)
            continue; // the draw's own inline vertices
        ComObj *vb = com_get(pl.stream[i].vb);
        if (!vb)
            continue;
        gpu_sync_buffer(vb);
        d.stream[i].buffer = vb->id;
        d.stream[i].offset = pl.stream[i].offset;
        d.stream[i].stride = pl.stream[i].stride;
    }
    for (int s = 0; s < 16; ++s) {
        ComObj *tex = com_get(pl.sampler_tex[s]);
        if (!tex || (tex->kind != K_D3D9TEXTURE && tex->kind != K_D3D9SURFACE))
            continue;
        gpu_sync(tex);
        d.sampler_texture[s] = tex->id;
    }
    d.sampler_state = &pl.sampler_state[0][0];
    d.render_state = pl.rs;
    d.render_state_set = (const uint8_t *)pl.rs_set;
    d.state_version = pl.state_version;
    for (int st = 0; st < 8; ++st)
        if (pl.tss[st][24] & 0x100) // D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_PROJECTED
            d.projected_mask |= 1u << st;
    d.label = pl.label;
    d.instance_count = (pl.stream[0].frequency & HOST_D9_STREAM_INDEXED)
                           ? pl.stream[0].frequency & HOST_D9_STREAM_DIVIDER
                           : 1;
    host_d9_draw(&d);
    bool color_write = !pl.rs_set[168] || (pl.rs[168] & 0xf) != 0;
    bool z_write = (!pl.rs_set[14] || pl.rs[14]) && (!pl.rs_set[7] || pl.rs[7]);
    gpu_target_drawn(dev, color_write, z_write);
}

// Hands one draw to the CPU renderer with what the device has bound.
static void raster(ComObj *dev, const D9DrawCall &call) {
    if (!dev)
        return;
    ComObj *target = com_get(dev->render_target);
    if (!target)
        target = device_backbuffer(dev);
    ComObj *decl = com_get(dev->current_viewport); // SetVertexDeclaration keeps it here
    static const std::vector<uint8_t> none;
    d9_raster_draw(dev, target, (decl && decl->kind == K_D3D9DECL) ? decl->blob : none, call);
}

// (this, PrimitiveType, StartVertex, PrimitiveCount)
void Dev_DrawPrimitive(X86 *c) {
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives from vertex %u, from the bound stream", g_draws,
             prim_name(arg(c, 1)), arg(c, 3), arg(c, 2));
    if (draw_refused("DrawPrimitive", arg(c, 1), arg(c, 3), 0)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *dev = this_device9(c);
    if (dev && gpu_on()) {
        HostD9Draw d{};
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 3);
        d.start = arg(c, 2);
        gpu_draw(dev, d);
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *vb = dev ? com_get(dev->back_obj) : nullptr;
    if (vb && resource_bytes(vb) && dev->lock_off < vb->blob.size()) {
        D9DrawCall call;
        call.prim = arg(c, 1);
        call.prim_count = arg(c, 3);
        call.vertex_data = resource_bytes(vb) + dev->lock_off;
        call.vertex_bytes = vb->blob.size() - dev->lock_off;
        call.stride = dev->rate;
        call.first = arg(c, 2);
        raster(this_device9(c), call);
    }
    com_ret(c, D3D_OK9);
}

// (this, Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
void Dev_DrawIndexedPrimitive(X86 *c) {
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives, %u vertices from index %u, indexed", g_draws,
             prim_name(arg(c, 1)), arg(c, 6), arg(c, 4), arg(c, 5));
    if (draw_refused("DrawIndexedPrimitive", arg(c, 1), arg(c, 6), arg(c, 4))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    d9_api_indexed_draw(this_device9(c), arg(c, 1), (int32_t)arg(c, 2), arg(c, 5), arg(c, 6));
    com_ret(c, D3D_OK9);
}

// BaseVertexIndex is explicit here: version 8 binds it with SetIndices,
// whereas version 9 supplies it with each draw. Neither frontend rewrites
// the guest's argument frame to call the other ABI.
void d9_api_indexed_draw(ComObj *dev, uint32_t primitive, int32_t base_vertex, uint32_t first_index,
                         uint32_t primitive_count) {
    if (dev && gpu_on()) {
        ComObj *ibo = com_get(d9_pipeline(dev->id).index_buffer);
        if (ibo) {
            gpu_sync_buffer(ibo);
            HostD9Draw d{};
            d.primitive = primitive;
            d.primitive_count = primitive_count;
            d.start = first_index;
            d.base_vertex = base_vertex;
            d.index_buffer = ibo->id;
            d.index_size = ibo->rmask ? ibo->rmask : 2;
            gpu_draw(dev, d);
        }
        return;
    }
    ComObj *vb = dev ? com_get(dev->back_obj) : nullptr;
    ComObj *ib = dev ? com_get(dev->front_obj) : nullptr;
    if (vb && resource_bytes(vb) && ib && resource_bytes(ib) && dev->lock_off < vb->blob.size()) {
        D9DrawCall call;
        call.prim = primitive;
        call.prim_count = primitive_count;
        call.vertex_data = resource_bytes(vb) + dev->lock_off;
        call.vertex_bytes = vb->blob.size() - dev->lock_off;
        call.stride = dev->rate;
        call.index_data = resource_bytes(ib);
        call.index_bytes = ib->blob.size();
        call.index_size = ib->rmask ? ib->rmask : 2;
        call.base_vertex = base_vertex;
        call.first = first_index;
        raster(dev, call);
    }
}

// (this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, Stride): the
// vertices travel with the call rather than in a buffer, which is how this
// game draws.
// What a rasterizer will need from a draw, spelled out for the first few: the
// bound vertex declaration decoded, the raw vertices as floats, the bound
// texture, and the surface being drawn into.
static void describe_draw_inputs(X86 *c, uint32_t vertices, uint32_t stride, uint32_t nverts) {
    ComObj *dev = this_device9(c);
    if (!dev)
        return;
    ComObj *decl = com_get(dev->current_viewport); // SetVertexDeclaration keeps it here
    if (decl && decl->kind == K_D3D9DECL) {
        static const char *types[] = {"FLOAT1",    "FLOAT2",   "FLOAT3",   "FLOAT4",  "D3DCOLOR",
                                      "UBYTE4",    "SHORT2",   "SHORT4",   "UBYTE4N", "SHORT2N",
                                      "SHORT4N",   "USHORT2N", "USHORT4N", "UDEC3",   "DEC3N",
                                      "FLOAT16_2", "FLOAT16_4"};
        static const char *usages[] = {
            "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
            "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",   "DEPTH",    "SAMPLE"};
        char line[512];
        size_t used = 0;
        for (size_t i = 0; i + 8 <= decl->blob.size(); i += 8) {
            uint16_t stream = (uint16_t)(decl->blob[i] | (decl->blob[i + 1] << 8));
            if (stream == 0xff)
                break;
            uint16_t off = (uint16_t)(decl->blob[i + 2] | (decl->blob[i + 3] << 8));
            uint8_t type = decl->blob[i + 4], usage = decl->blob[i + 6], index = decl->blob[i + 7];
            used += (size_t)snprintf(line + used, sizeof line - used, " %s%u@%u:%s",
                                     usage < 14 ? usages[usage] : "?", index, off,
                                     type < 17 ? types[type] : "?");
            if (used >= sizeof line)
                break;
        }
        LOGW("d3d9:   declaration %u:%s", decl->id, line);
    } else {
        LOGW("d3d9:   no vertex declaration bound (FVF path)");
    }
    for (uint32_t v = 0; v < nverts && v < 4; ++v) {
        uint32_t base = vertices + v * stride;
        char line[256];
        size_t used = 0;
        for (uint32_t o = 0; o + 4 <= stride && o < 40; o += 4)
            used += (size_t)snprintf(
                line + used, sizeof line - used, " %g", (double)[&] {
                    float f;
                    uint32_t bits = rd32(base + o);
                    memcpy(&f, &bits, 4);
                    return f;
                }());
        LOGW("d3d9:   vertex %u:%s", v, line);
    }
    ComObj *tex = com_get(dev->texture_handle);
    ComObj *rt = com_get(dev->render_target);
    LOGW("d3d9:   texture %u (%ux%u), target %u (%ux%u)%s", tex ? tex->id : 0, tex ? tex->width : 0,
         tex ? tex->height : 0, rt ? rt->id : 0, rt ? rt->width : 0, rt ? rt->height : 0,
         (rt && rt->id == dev->palette_obj) ? ", the back buffer" : "");
}

void Dev_DrawPrimitiveUP(X86 *c) {
    if (draw_worth_reporting()) {
        LOGW("d3d9: draw %u: %s, %u primitives, %u-byte vertices inline at %08x", g_draws,
             prim_name(arg(c, 1)), arg(c, 2), arg(c, 4), arg(c, 3));
        // A fan of n primitives has n + 2 vertices.
        if (g_draws <= 8)
            describe_draw_inputs(c, arg(c, 3), arg(c, 4), arg(c, 2) + 2);
    }
    if (draw_refused("DrawPrimitiveUP", arg(c, 1), arg(c, 2), 0)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (ComObj *dev = this_device9(c); dev && gpu_on()) {
        HostD9Draw d{};
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 2);
        const uint64_t bytes = primitive_vertices(d.primitive, d.primitive_count) * arg(c, 4);
        if (bytes && gm_fits(arg(c, 3), bytes)) {
            d.stream[0].stride = arg(c, 4);
            d.inline_vertices = gm_ptr(arg(c, 3));
            d.inline_bytes = (uint32_t)bytes;
            gpu_draw(dev, d);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    D9DrawCall call;
    call.prim = arg(c, 1);
    call.prim_count = arg(c, 2);
    call.vertices = arg(c, 3);
    call.stride = arg(c, 4);
    raster(this_device9(c), call);
    com_ret(c, D3D_OK9);
}

// (this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount,
//  pIndexData, IndexDataFormat, pVertexStreamZeroData, Stride)
void Dev_DrawIndexedPrimitiveUP(X86 *c) {
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives, %u vertices of %u bytes inline at %08x, "
             "indices at %08x",
             g_draws, prim_name(arg(c, 1)), arg(c, 4), arg(c, 3), arg(c, 8), arg(c, 7), arg(c, 5));
    if (draw_refused("DrawIndexedPrimitiveUP", arg(c, 1), arg(c, 4),
                     uint64_t(arg(c, 2)) + arg(c, 3))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (ComObj *dev = this_device9(c); dev && gpu_on()) {
        HostD9Draw d{};
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 4);
        const uint32_t isize = arg(c, 6) == 102 ? 4 : 2;
        const uint64_t ibytes = primitive_vertices(d.primitive, d.primitive_count) * isize;
        const uint64_t vbytes = (uint64_t(arg(c, 2)) + arg(c, 3)) * arg(c, 8);
        if (ibytes && vbytes && gm_fits(arg(c, 5), ibytes) && gm_fits(arg(c, 7), vbytes)) {
            d.stream[0].stride = arg(c, 8);
            d.inline_vertices = gm_ptr(arg(c, 7));
            d.inline_bytes = (uint32_t)vbytes;
            d.inline_indices = gm_ptr(arg(c, 5));
            d.inline_index_bytes = (uint32_t)ibytes;
            d.index_size = isize;
            gpu_draw(dev, d);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    D9DrawCall call;
    call.prim = arg(c, 1);
    call.prim_count = arg(c, 4);
    call.vertices = arg(c, 7);
    call.stride = arg(c, 8);
    call.indices = arg(c, 5);
    call.index_size = arg(c, 6) == 102 ? 4 : 2;
    raster(this_device9(c), call);
    com_ret(c, D3D_OK9);
}

// ---------------------------------------------------------------------------
// The resource interfaces themselves
// ---------------------------------------------------------------------------
#define RES_STUB(iface, name)                                                                      \
    void iface##_##name(X86 *c) {                                                                  \
        log_once("d3d9." #iface "." #name, "d3d9: " #iface "::" #name " is not implemented");      \
        com_ret(c, D3D_OK9);                                                                       \
    }

RES_STUB(Res, SetPrivateData)
RES_STUB(Res, GetPrivateData)
RES_STUB(Res, FreePrivateData)
RES_STUB(Res, SetPriority)
RES_STUB(Res, GetPriority)
RES_STUB(Res, PreLoad)
RES_STUB(Res, GetType)
RES_STUB(Tex, SetLOD)
RES_STUB(Tex, GetLOD)
RES_STUB(Tex, SetAutoGenFilterType)
RES_STUB(Tex, GetAutoGenFilterType)
RES_STUB(Tex, GenerateMipSubLevels)
RES_STUB(Tex, AddDirtyRect)
RES_STUB(Tex, AddDirtyRectCube)
RES_STUB(Surf, GetDC)
RES_STUB(Surf, ReleaseDC)
RES_STUB(Decl, GetDeclaration)

// (this, riid, ppContainer): the texture a level or face belongs to, or the
// device when the surface stands alone.
void Surf_GetContainer(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t out = arg(c, 2);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *parent = com_get(s->front_obj);
    if (parent) {
        com_addref(parent);
        com_out_ptr(out, d9_api_view(parent, parent->caps ? IF_D3DCUBETEXTURE9 : IF_D3DTEXTURE9));
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *dev = com_get(s->dev_d3d);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(dev);
    com_out_ptr(out, d9_api_view(dev, IF_D3DDEVICE9));
    com_ret(c, D3D_OK9);
}

void Res_GetDevice(X86 *c) {
    ComObj *o = com_this_arg(c);
    ComObj *dev = o ? com_get(o->dev_d3d) : nullptr;
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(dev);
    com_out_ptr(out, d9_api_view(dev, IF_D3DDEVICE9));
    com_ret(c, D3D_OK9);
}

// Lock hands the guest the buffer's staged copy (stage_lock) at the offset
// asked for; Unlock writes back what changed inside the locked range.
static void buffer_lock(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t offset = arg(c, 1), size = arg(c, 2), out = arg(c, 3);
    if (!o || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    // SizeToLock 0 locks to the end of the buffer.
    const uint32_t total = (uint32_t)o->blob.size();
    const uint32_t hi = size && offset < total && size <= total - offset ? offset + size : total;
    uint32_t staged = offset < total ? stage_lock(o, offset, hi) : 0;
    com_out_ptr(out, staged ? staged + offset : 0);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void VB_Lock(X86 *c) {
    buffer_lock(c);
}
void IB_Lock(X86 *c) {
    buffer_lock(c);
}
void Buf_Unlock(X86 *c) {
    stage_unlock(com_this_arg(c));
    com_ret(c, D3D_OK9);
}
// D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size.
void Buf_GetDesc(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t d = arg(c, 1);
    if (!o || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 20);
    wr32(d + 16, o->buf_bytes);
    com_ret(c, D3D_OK9);
}

// D3DLOCKED_RECT is { INT Pitch; void *pBits; }.
void Surf_LockRect(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t out = arg(c, 1);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void Surf_UnlockRect(X86 *c) {
    stage_unlock(com_this_arg(c));
    com_ret(c, D3D_OK9);
}
// D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, Quality,
// Width, Height.
void Surf_GetDesc(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t d = arg(c, 1);
    if (!s || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 32);
    wr32(d + 0, s->rmask ? s->rmask : 22);
    wr32(d + 24, s->width);
    wr32(d + 28, s->height);
    com_ret(c, D3D_OK9);
}

// A texture level is the surface made with the texture.
// A cube face, made on first ask and kept, so the game gets the same surface
// back every time. Returning success without writing the pointer - which is
// what the stub did - had the game store nothing and throw the cube away.
// The surface for one face and level of a cube, made on first ask and kept on
// the texture, indexed face + 6 * level.
static ComObj *cube_face(ComObj *tex, uint32_t face, uint32_t level) {
    if (!tex || tex->kind != K_D3D9TEXTURE || !tex->caps || face > 5 || level > 15)
        return nullptr;
    size_t slot = face + 6u * level;
    if (tex->surfaces.size() <= slot)
        tex->surfaces.resize(slot + 1, 0);
    ComObj *s = com_get(tex->surfaces[slot]);
    if (!s) {
        uint32_t edge = std::max(tex->width >> level, 1u);
        s = make_surface(com_get(tex->dev_d3d), edge, edge, tex->rmask ? tex->rmask : 22);
        if (!s)
            return nullptr;
        s->front_obj = tex->id;
        s->d3d_usage = tex->d3d_usage;
        s->d3d_pool = tex->d3d_pool;
        tex->surfaces[slot] = s->id;
    }
    return s;
}

// (this, FaceType, Level, ppCubeMapSurface). A face is a view into the
// texture, so handing one out references the texture too: the game takes
// face zero, releases the cube, and goes on using it for the other five.
void Tex_GetCubeMapSurface(X86 *c) {
    ComObj *tex = com_this_arg(c);
    uint32_t out = arg(c, 3);
    ComObj *s = cube_face(tex, arg(c, 1), arg(c, 2));
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t view = d9_api_view(s, IF_D3DSURFACE9);
    if (!view) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_addref(s); // takes the texture reference with the first outside one
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

// Returns a count, not an HRESULT. The stub returned zero, which told the
// game its textures had no levels at all.
// (this, FaceType, Level, pLockedRect, pRect, Flags)
void Tex_LockRectCube(X86 *c) {
    ComObj *s = cube_face(com_this_arg(c), arg(c, 1), arg(c, 2));
    uint32_t out = arg(c, 3);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
// (this, FaceType, Level)
void Tex_UnlockRectCube(X86 *c) {
    stage_unlock(cube_face(com_this_arg(c), arg(c, 1), arg(c, 2)));
    com_ret(c, D3D_OK9);
}

void Tex_GetLevelCount(X86 *c) {
    ComObj *tex = com_this_arg(c);
    set_eax(c, (tex && !tex->caps && !tex->surfaces.empty()) ? (uint32_t)tex->surfaces.size() : 1);
}

// (this, Level, pDesc): the same D3DSURFACE_DESC a surface describes itself
// with, for the level the texture was created at.
void Tex_GetLevelDesc(X86 *c) {
    ComObj *tex = com_this_arg(c);
    ComObj *s = texture_level(tex, arg(c, 1));
    uint32_t d = arg(c, 2);
    if (!tex || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 32);
    wr32(d + 0, tex->rmask ? tex->rmask : 22);
    wr32(d + 24, s ? s->width : tex->width);
    wr32(d + 28, s ? s->height : tex->height);
    com_ret(c, D3D_OK9);
}

void Tex_GetSurfaceLevel(X86 *c) {
    ComObj *tex = com_this_arg(c);
    uint32_t out = arg(c, 2);
    ComObj *s = texture_level(tex, arg(c, 1));
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    // A level is a view into the texture: while anything outside holds it, it
    // holds one texture reference (surface_references in d3d8.cpp takes it
    // with the first outside reference and gives it back with the last).
    // Taking one per call, never given back, kept every texture whose level
    // was ever asked for alive for the rest of the run.
    s->front_obj = tex->id;
    com_addref(s);
    uint32_t view_ = d9_api_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
void Tex_LockRect(X86 *c) {
    ComObj *s = texture_level(com_this_arg(c), arg(c, 1));
    uint32_t out = arg(c, 2);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void Tex_UnlockRect(X86 *c) {
    stage_unlock(texture_level(com_this_arg(c), arg(c, 1)));
    com_ret(c, D3D_OK9);
}

// D3DQUERYTYPE: 8 is EVENT, 9 OCCLUSION. An occlusion query with a GPU
// renderer counts real samples; without one, or for any other type, the
// query is finished at once, and an occlusion count is reported as one
// sample so that nothing the game gates on visibility disappears.
static const uint32_t D3DQUERYTYPE_EVENT = 8, D3DQUERYTYPE_OCCLUSION = 9;
static const uint32_t S_FALSE9 = 1;
static std::unordered_map<uint32_t, uint32_t> &query_state() { // id -> 1 begun, 2 ended
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
static void query_destroy(ComObj *o) {
    if (query_state().erase(o->id))
        host_d9_query_drop(o->id);
}
void Query_GetType9(X86 *c) {
    ComObj *q = com_this_arg(c);
    set_eax(c, q ? q->dev_type : 0);
}
void Query_GetDataSize(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t type = q ? q->dev_type : 0;
    set_eax(c, (type == D3DQUERYTYPE_OCCLUSION || type == D3DQUERYTYPE_EVENT) ? 4 : 0);
}
// (this, dwIssueFlags): D3DISSUE_END 1, D3DISSUE_BEGIN 2.
void Query_Issue(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t flags = arg(c, 1);
    if (q && q->dev_type == D3DQUERYTYPE_OCCLUSION && gpu_on()) {
        if (flags & 2) {
            host_d9_query_begin(q->id);
            query_state()[q->id] = 1;
        } else if (flags & 1) {
            host_d9_query_end(q->id);
            query_state()[q->id] = 2;
        }
    }
    com_ret(c, q ? D3D_OK9 : D3DERR_INVALIDCALL);
}
// (this, pData, dwSize, dwGetDataFlags)
void Query_GetData(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t data = arg(c, 1), size = arg(c, 2);
    if (!q) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t value = 1;
    if (q->dev_type == D3DQUERYTYPE_OCCLUSION && gpu_on()) {
        auto it = query_state().find(q->id);
        if (it != query_state().end()) {
            uint32_t count = 0;
            int r = it->second == 2 ? host_d9_query_result(q->id, &count) : 0;
            if (r == 0) {
                com_ret(c, S_FALSE9); // not yet
                return;
            }
            value = r > 0 ? count : 1;
        }
    }
    if (data && size >= 4)
        wr32(data, value);
    com_ret(c, D3D_OK9);
}

#define RESOURCE_HEAD                                                                              \
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef},                          \
        {"Release", 1, com_Release}, {"GetDevice", 2, Res_GetDevice},                              \
        {"SetPrivateData", 5, Res_SetPrivateData}, {"GetPrivateData", 4, Res_GetPrivateData},      \
        {"FreePrivateData", 2, Res_FreePrivateData}, {"SetPriority", 2, Res_SetPriority},          \
        {"GetPriority", 1, Res_GetPriority}, {"PreLoad", 1, Res_PreLoad}, {                        \
        "GetType", 1, Res_GetType                                                                  \
    }

static const ComMethod g_vb9[] = {
    RESOURCE_HEAD,
    {"Lock", 5, VB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, Buf_GetDesc},
};

static const ComMethod g_ib9[] = {
    RESOURCE_HEAD,
    {"Lock", 5, IB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, Buf_GetDesc},
};

static const ComMethod g_surface9[] = {
    RESOURCE_HEAD,
    {"GetContainer", 3, Surf_GetContainer},
    {"GetDesc", 2, Surf_GetDesc},
    {"LockRect", 4, Surf_LockRect},
    {"UnlockRect", 1, Surf_UnlockRect},
    {"GetDC", 2, Surf_GetDC},
    {"ReleaseDC", 2, Surf_ReleaseDC},
};

static const ComMethod g_texture9[] = {
    RESOURCE_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Tex_SetAutoGenFilterType},
    {"GetAutoGenFilterType", 1, Tex_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, Tex_GenerateMipSubLevels},
    {"GetLevelDesc", 3, Tex_GetLevelDesc},
    {"GetSurfaceLevel", 3, Tex_GetSurfaceLevel},
    {"LockRect", 5, Tex_LockRect},
    {"UnlockRect", 2, Tex_UnlockRect},
    {"AddDirtyRect", 2, Tex_AddDirtyRect},
};

static const ComMethod g_cubetexture9[] = {
    RESOURCE_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Tex_SetAutoGenFilterType},
    {"GetAutoGenFilterType", 1, Tex_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, Tex_GenerateMipSubLevels},
    {"GetLevelDesc", 3, Tex_GetLevelDesc},
    {"GetCubeMapSurface", 4, Tex_GetCubeMapSurface},
    {"LockRect", 6, Tex_LockRectCube},
    {"UnlockRect", 3, Tex_UnlockRectCube},
    {"AddDirtyRect", 3, Tex_AddDirtyRectCube},
};

static const ComMethod g_decl9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetDeclaration", 3, Decl_GetDeclaration},
};

static const ComMethod g_query9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetType", 1, Query_GetType9},
    {"GetDataSize", 1, Query_GetDataSize},
    {"Issue", 2, Query_Issue},
    {"GetData", 4, Query_GetData},
};

static const ComMethod g_shader9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetFunction", 3, Shader_GetFunction},
};

// ---------------------------------------------------------------------------
// The pipeline record, and the device setters that write it
// ---------------------------------------------------------------------------
static std::map<uint32_t, D9Pipeline> &pipeline_records() {
    static auto *records = new std::map<uint32_t, D9Pipeline>();
    return *records;
}
D9Pipeline &d9_pipeline(uint32_t device_id) {
    // Records are never erased and map nodes do not move, so the last one
    // asked for can be handed out again without a lookup.
    static thread_local uint32_t last_id = 0;
    static thread_local D9Pipeline *last = nullptr;
    if (last && last_id == device_id)
        return *last;
    last = &pipeline_records()[device_id];
    last_id = device_id;
    return *last;
}

void d9_bind_shader(uint32_t device_id, bool pixel, D9ShaderBytes bytes, uint64_t key,
                    uint32_t object) {
    D9Pipeline &pl = d9_pipeline(device_id);
    uint32_t &bound = pixel ? pl.ps_object : pl.vs_object;
    if (bound != object) {
        if (ComObj *next = com_get(object)) {
            ++next->d9_shader_bindings;
            com_addref(next);
        }
        ComObj *previous = com_get(bound);
        bound = object;
        if (previous) {
            --previous->d9_shader_bindings;
            com_release(previous);
        }
    }
    (pixel ? pl.ps : pl.vs) = std::move(bytes);
    (pixel ? pl.ps_key : pl.vs_key) = key;
    pl.label = nullptr;
}

void d3d9_device_destroy(ComObj *device) {
    if (device->d3d_version != 8) {
        // The bound targets, the depth binding, the automatic depth buffer and
        // the back buffer are the device's references (d3d8.cpp drops a
        // version 8 device's own).
        for (uint32_t i = 0; i < 4; ++i)
            bind_color_target(device, i, nullptr);
        device_forget_buffers(device);
        // And the textures set on its stages (Dev_SetTexture).
        auto held = set_textures().find(device->id);
        if (held != set_textures().end()) {
            const std::array<uint32_t, 16> ids = held->second;
            set_textures().erase(held);
            for (uint32_t id : ids)
                if (ComObj *t = com_get(id))
                    com_release(t);
        }
    }
    d9_bind_shader(device->id, false, {}, 0);
    d9_bind_shader(device->id, true, {}, 0);
    d9_pipeline(device->id) = D9Pipeline{};
    fixed_states().erase(device->id);
}

// (this, State, Value)
// (this, pViewport): D3DVIEWPORT9 is X, Y, Width, Height, MinZ, MaxZ.
void Dev_SetViewport(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t v = arg(c, 1);
    if (dev && v) {
        D9Pipeline &p = d9_pipeline(dev->id);
        for (int i = 0; i < 4; ++i)
            p.viewport[i] = rd32(v + 4u * (uint32_t)i);
        uint32_t b0 = rd32(v + 16), b1 = rd32(v + 20);
        memcpy(&p.viewport_z[0], &b0, 4);
        memcpy(&p.viewport_z[1], &b1, 4);
        p.viewport_set = true;
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetViewport(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t v = arg(c, 1);
    if (dev && v) {
        D9Pipeline &p = d9_pipeline(dev->id);
        ComObj *rt = com_get(dev->render_target);
        uint32_t whole[4] = {0, 0, rt ? rt->width : 640, rt ? rt->height : 480};
        for (int i = 0; i < 4; ++i)
            wr32(v + 4u * (uint32_t)i, p.viewport_set ? p.viewport[i] : whole[i]);
        float z0 = p.viewport_set ? p.viewport_z[0] : 0.0f,
              z1 = p.viewport_set ? p.viewport_z[1] : 1.0f;
        uint32_t b0, b1;
        memcpy(&b0, &z0, 4);
        memcpy(&b1, &z1, 4);
        wr32(v + 16, b0);
        wr32(v + 20, b1);
    }
    com_ret(c, D3D_OK9);
}

// (this, State, pMatrix): world, view, projection and texture transforms,
// kept for the fixed-function path.
void Dev_SetTransform(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1), m = arg(c, 2);
    if (dev && m) {
        uint32_t slot =
            state < 256 ? state : (state >= 256 && state < 260 ? state - 256 + 24 : 0xffffffffu);
        if (slot < 32) {
            for (int i = 0; i < 16; ++i) {
                uint32_t b = rd32(m + 4u * (uint32_t)i);
                memcpy(&d9_pipeline(dev->id).transform[slot][i], &b, 4);
            }
            fixed_state(dev->id).transform_set |= 1u << slot;
        }
    }
    com_ret(c, D3D_OK9);
}

// (this, Stage, Type, Value)
void Dev_SetTextureStageState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t stage = arg(c, 1), type = arg(c, 2);
    if (dev && stage < 8 && type < 33) {
        d9_pipeline(dev->id).tss[stage][type] = arg(c, 3);
        fixed_state(dev->id).tss_set[stage] |= uint64_t(1) << type;
    }
    com_ret(c, D3D_OK9);
}

void Dev_SetRenderState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1);
    if (dev && state < 256) {
        D9Pipeline &p = d9_pipeline(dev->id);
        if (p.rs[state] != arg(c, 2) || !p.rs_set[state]) {
            p.rs[state] = arg(c, 2);
            p.rs_set[state] = true;
            p.states_changed();
        }
    }
    com_ret(c, D3D_OK9);
}

// (this, Sampler, Type, Value)
void Dev_SetSamplerState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t sampler = arg(c, 1), type = arg(c, 2);
    if (dev && sampler < 16 && type < 14) {
        D9Pipeline &p = d9_pipeline(dev->id);
        if (p.sampler_state[sampler][type] != arg(c, 3)) {
            p.sampler_state[sampler][type] = arg(c, 3);
            p.states_changed();
        }
    }
    com_ret(c, D3D_OK9);
}

// Copies Count four-float vectors from guest memory into a register table,
// clipped to the table.
static void write_registers(float (*table)[4], uint32_t limit, uint32_t start, uint32_t data,
                            uint32_t count) {
    for (uint32_t i = 0; i < count && start + i < limit && data; ++i)
        for (uint32_t k = 0; k < 4; ++k) {
            uint32_t bits = rd32(data + (i * 4 + k) * 4);
            memcpy(&table[start + i][k], &bits, 4);
        }
}

// (this, StartRegister, pConstantData, Vector4fCount)
void Dev_SetVertexShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev) {
        D9Pipeline &pl = d9_pipeline(dev->id);
        write_registers(pl.vconst, 256, arg(c, 1), arg(c, 2), arg(c, 3));
        pl.vconst_serial = d9_const_serial();
    }
    com_ret(c, D3D_OK9);
}
void Dev_SetPixelShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev) {
        D9Pipeline &pl = d9_pipeline(dev->id);
        write_registers(pl.pconst, 32, arg(c, 1), arg(c, 2), arg(c, 3));
        pl.pconst_serial = d9_const_serial();
    }
    com_ret(c, D3D_OK9);
}

static const ComMethod g_d3d9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"RegisterSoftwareDevice", 2, D9_RegisterSoftwareDevice},
    {"GetAdapterCount", 1, D9_GetAdapterCount},
    {"GetAdapterIdentifier", 4, D9_GetAdapterIdentifier},
    {"GetAdapterModeCount", 3, D9_GetAdapterModeCount},
    {"EnumAdapterModes", 5, D9_EnumAdapterModes},
    {"GetAdapterDisplayMode", 3, D9_GetAdapterDisplayMode},
    {"CheckDeviceType", 6, D9_CheckDeviceType},
    {"CheckDeviceFormat", 7, D9_CheckDeviceFormat},
    {"CheckDeviceMultiSampleType", 7, D9_CheckDeviceMultiSampleType},
    {"CheckDepthStencilMatch", 6, D9_CheckDepthStencilMatch},
    {"CheckDeviceFormatConversion", 5, D9_CheckDeviceFormatConversion},
    {"GetDeviceCaps", 4, D9_GetDeviceCaps},
    {"GetAdapterMonitor", 2, D9_GetAdapterMonitor},
    {"CreateDevice", 7, D9_CreateDevice},
};

static const ComMethod g_device9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"TestCooperativeLevel", 1, Dev_TestCooperativeLevel},
    {"GetAvailableTextureMem", 1, Dev_GetAvailableTextureMem},
    {"EvictManagedResources", 1, Dev_EvictManagedResources},
    {"GetDirect3D", 2, Dev_GetDirect3D},
    {"GetDeviceCaps", 2, Dev_GetDeviceCaps},
    {"GetDisplayMode", 3, Dev_GetDisplayMode},
    {"GetCreationParameters", 2, Dev_GetCreationParameters},
    {"SetCursorProperties", 4, Dev_SetCursorProperties},
    {"SetCursorPosition", 4, Dev_SetCursorPosition},
    {"ShowCursor", 2, Dev_ShowCursor},
    {"CreateAdditionalSwapChain", 3, Dev_CreateAdditionalSwapChain},
    {"GetSwapChain", 3, Dev_GetSwapChain},
    {"GetNumberOfSwapChains", 1, Dev_GetNumberOfSwapChains},
    {"Reset", 2, Dev_Reset},
    {"Present", 5, Dev_Present},
    {"GetBackBuffer", 5, Dev_GetBackBuffer},
    {"GetRasterStatus", 3, Dev_GetRasterStatus},
    {"SetDialogBoxMode", 2, Dev_SetDialogBoxMode},
    {"SetGammaRamp", 4, Dev_SetGammaRamp},
    {"GetGammaRamp", 3, Dev_GetGammaRamp},
    {"CreateTexture", 9, Dev_CreateTexture},
    {"CreateVolumeTexture", 10, Dev_CreateVolumeTexture},
    {"CreateCubeTexture", 8, Dev_CreateCubeTexture},
    {"CreateVertexBuffer", 7, Dev_CreateVertexBuffer},
    {"CreateIndexBuffer", 7, Dev_CreateIndexBuffer},
    {"CreateRenderTarget", 9, Dev_CreateRenderTarget},
    {"CreateDepthStencilSurface", 9, Dev_CreateDepthStencilSurface},
    {"UpdateSurface", 5, Dev_UpdateSurface},
    {"UpdateTexture", 3, Dev_UpdateTexture},
    {"GetRenderTargetData", 3, Dev_GetRenderTargetData},
    {"GetFrontBufferData", 3, Dev_GetFrontBufferData},
    {"StretchRect", 6, Dev_StretchRect},
    {"ColorFill", 4, Dev_ColorFill},
    {"CreateOffscreenPlainSurface", 7, Dev_CreateOffscreenPlainSurface},
    {"SetRenderTarget", 3, Dev_SetRenderTarget},
    {"GetRenderTarget", 3, Dev_GetRenderTarget},
    {"SetDepthStencilSurface", 2, Dev_SetDepthStencilSurface},
    {"GetDepthStencilSurface", 2, Dev_GetDepthStencilSurface},
    {"BeginScene", 1, Dev_BeginScene},
    {"EndScene", 1, Dev_EndScene},
    {"Clear", 7, Dev_Clear},
    {"SetTransform", 3, Dev_SetTransform},
    {"GetTransform", 3, Dev_GetTransform},
    {"MultiplyTransform", 3, Dev_MultiplyTransform},
    {"SetViewport", 2, Dev_SetViewport},
    {"GetViewport", 2, Dev_GetViewport},
    {"SetMaterial", 2, Dev_SetMaterial},
    {"GetMaterial", 2, Dev_GetMaterial},
    {"SetLight", 3, Dev_SetLight},
    {"GetLight", 3, Dev_GetLight},
    {"LightEnable", 3, Dev_LightEnable},
    {"GetLightEnable", 3, Dev_GetLightEnable},
    {"SetClipPlane", 3, Dev_SetClipPlane},
    {"GetClipPlane", 3, Dev_GetClipPlane},
    {"SetRenderState", 3, Dev_SetRenderState},
    {"GetRenderState", 3, Dev_GetRenderState},
    {"CreateStateBlock", 3, Dev_CreateStateBlock},
    {"BeginStateBlock", 1, Dev_BeginStateBlock},
    {"EndStateBlock", 2, Dev_EndStateBlock},
    {"SetClipStatus", 2, Dev_SetClipStatus},
    {"GetClipStatus", 2, Dev_GetClipStatus},
    {"GetTexture", 3, Dev_GetTexture},
    {"SetTexture", 3, Dev_SetTexture},
    {"GetTextureStageState", 4, Dev_GetTextureStageState},
    {"SetTextureStageState", 4, Dev_SetTextureStageState},
    {"GetSamplerState", 4, Dev_GetSamplerState},
    {"SetSamplerState", 4, Dev_SetSamplerState},
    {"ValidateDevice", 2, Dev_ValidateDevice},
    {"SetPaletteEntries", 3, Dev_SetPaletteEntries},
    {"GetPaletteEntries", 3, Dev_GetPaletteEntries},
    {"SetCurrentTexturePalette", 2, Dev_SetCurrentTexturePalette},
    {"GetCurrentTexturePalette", 2, Dev_GetCurrentTexturePalette},
    {"SetScissorRect", 2, Dev_SetScissorRect},
    {"GetScissorRect", 2, Dev_GetScissorRect},
    {"SetSoftwareVertexProcessing", 2, Dev_SetSoftwareVertexProcessing},
    {"GetSoftwareVertexProcessing", 1, Dev_GetSoftwareVertexProcessing},
    {"SetNPatchMode", 2, Dev_SetNPatchMode},
    {"GetNPatchMode", 1, Dev_GetNPatchMode},
    {"DrawPrimitive", 4, Dev_DrawPrimitive},
    {"DrawIndexedPrimitive", 7, Dev_DrawIndexedPrimitive},
    {"DrawPrimitiveUP", 5, Dev_DrawPrimitiveUP},
    {"DrawIndexedPrimitiveUP", 9, Dev_DrawIndexedPrimitiveUP},
    {"ProcessVertices", 7, Dev_ProcessVertices},
    {"CreateVertexDeclaration", 3, Dev_CreateVertexDeclaration},
    {"SetVertexDeclaration", 2, Dev_SetVertexDeclaration},
    {"GetVertexDeclaration", 2, Dev_GetVertexDeclaration},
    {"SetFVF", 2, Dev_SetFVF},
    {"GetFVF", 2, Dev_GetFVF},
    {"CreateVertexShader", 3, Dev_CreateVertexShader},
    {"SetVertexShader", 2, Dev_SetVertexShader},
    {"GetVertexShader", 2, Dev_GetVertexShader},
    {"SetVertexShaderConstantF", 4, Dev_SetVertexShaderConstantF},
    {"GetVertexShaderConstantF", 4, Dev_GetVertexShaderConstantF},
    {"SetVertexShaderConstantI", 4, Dev_SetVertexShaderConstantI},
    {"GetVertexShaderConstantI", 4, Dev_GetVertexShaderConstantI},
    {"SetVertexShaderConstantB", 4, Dev_SetVertexShaderConstantB},
    {"GetVertexShaderConstantB", 4, Dev_GetVertexShaderConstantB},
    {"SetStreamSource", 5, Dev_SetStreamSource},
    {"GetStreamSource", 5, Dev_GetStreamSource},
    {"SetStreamSourceFreq", 3, Dev_SetStreamSourceFreq},
    {"GetStreamSourceFreq", 3, Dev_GetStreamSourceFreq},
    {"SetIndices", 2, Dev_SetIndices},
    {"GetIndices", 2, Dev_GetIndices},
    {"CreatePixelShader", 3, Dev_CreatePixelShader},
    {"SetPixelShader", 2, Dev_SetPixelShader},
    {"GetPixelShader", 2, Dev_GetPixelShader},
    {"SetPixelShaderConstantF", 4, Dev_SetPixelShaderConstantF},
    {"GetPixelShaderConstantF", 4, Dev_GetPixelShaderConstantF},
    {"SetPixelShaderConstantI", 4, Dev_SetPixelShaderConstantI},
    {"GetPixelShaderConstantI", 4, Dev_GetPixelShaderConstantI},
    {"SetPixelShaderConstantB", 4, Dev_SetPixelShaderConstantB},
    {"GetPixelShaderConstantB", 4, Dev_GetPixelShaderConstantB},
    {"DrawRectPatch", 4, Dev_DrawRectPatch},
    {"DrawTriPatch", 4, Dev_DrawTriPatch},
    {"DeletePatch", 2, Dev_DeletePatch},
    {"CreateQuery", 3, Dev_CreateQuery},
};

// Version 8 explicitly selects only shared, implemented methods with the
// same argument layout. Its different slots and unsupported methods remain
// in the version 8 frontend; no vtable is reused by position.
const ComMethod *d9_api_method(ComIface iface, const char *name) {
    if (!name)
        return nullptr;
#define D9_METHODS(which, table)                                                                   \
    case which:                                                                                    \
        for (const auto &method : table)                                                           \
            if (!strcmp(method.name, name))                                                        \
                return &method;                                                                    \
        break
    switch (iface) {
        D9_METHODS(IF_D3D9, g_d3d9);
        D9_METHODS(IF_D3DDEVICE9, g_device9);
        D9_METHODS(IF_D3DTEXTURE9, g_texture9);
        D9_METHODS(IF_D3DCUBETEXTURE9, g_cubetexture9);
        D9_METHODS(IF_D3DSURFACE9, g_surface9);
        D9_METHODS(IF_D3DVERTEXBUFFER9, g_vb9);
        D9_METHODS(IF_D3DINDEXBUFFER9, g_ib9);
    default:
        break;
    }
#undef D9_METHODS
    return nullptr;
}

// The DLL's one export. Not a COM method: the guest calls it directly.
void d3d9_Direct3DCreate9(X86 *c) {
    ComObj *o = com_new(K_D3D9);
    uint32_t view = o ? d9_api_view(o, IF_D3D9) : 0;
    if (!view) {
        if (o)
            com_release(o);
        set_eax(c, 0);
        return;
    }
    LOGW("d3d9: Direct3DCreate9(SDK %u) -> %08x", arg(c, 0), view);
    set_eax(c, view);
}

static const ImportShim g_d3d9_exports[] = {
    {"d3d9.dll", "Direct3DCreate9", 1, d3d9_Direct3DCreate9},
};

void d3d9_reset() {
    g_scratch = 0;
    g_scratch_size = 0;
    // The following com_reset discards all objects; drop shared program bytes
    // here without invoking destructors against the remapped guest heap.
    for (auto &[id, pipeline] : pipeline_records())
        pipeline = D9Pipeline{};
    fixed_states().clear();
}

void d3d9_register() {
    static bool done = false;
    if (done)
        return;
    done = true;

    for (ComKind kind : {K_D3D9, K_D3D9DEVICE, K_D3D9TEXTURE, K_D3D9SURFACE, K_D3D9VB, K_D3D9IB})
        com_set_view_filter(kind, d9_view_allowed);
    com_define(IF_D3D9, "d3d9.dll", "IDirect3D9", g_d3d9, std::size(g_d3d9));
    com_define(IF_D3DDEVICE9, "d3d9.dll", "IDirect3DDevice9", g_device9, std::size(g_device9));
    com_bind(IF_D3D9, K_D3D9);
    com_bind(IF_D3DDEVICE9, K_D3D9DEVICE);
    com_register_iid(IF_D3D9, IID_IDirect3D9_);
    com_register_iid(IF_D3DDEVICE9, IID_IDirect3DDevice9_);
    com_define(IF_D3DTEXTURE9, "d3d9.dll", "IDirect3DTexture9", g_texture9, std::size(g_texture9));
    com_define(IF_D3DCUBETEXTURE9, "d3d9.dll", "IDirect3DCubeTexture9", g_cubetexture9,
               std::size(g_cubetexture9));
    com_define(IF_D3DSURFACE9, "d3d9.dll", "IDirect3DSurface9", g_surface9, std::size(g_surface9));
    com_define(IF_D3DVERTEXBUFFER9, "d3d9.dll", "IDirect3DVertexBuffer9", g_vb9, std::size(g_vb9));
    com_define(IF_D3DINDEXBUFFER9, "d3d9.dll", "IDirect3DIndexBuffer9", g_ib9, std::size(g_ib9));
    com_define(IF_D3DVERTEXDECL9, "d3d9.dll", "IDirect3DVertexDeclaration9", g_decl9,
               std::size(g_decl9));
    com_define(IF_D3DQUERY9, "d3d9.dll", "IDirect3DQuery9", g_query9, std::size(g_query9));
    com_bind(IF_D3DTEXTURE9, K_D3D9TEXTURE);
    com_bind(IF_D3DCUBETEXTURE9, K_D3D9TEXTURE);
    com_bind(IF_D3DSURFACE9, K_D3D9SURFACE);
    com_bind(IF_D3DVERTEXBUFFER9, K_D3D9VB);
    com_bind(IF_D3DINDEXBUFFER9, K_D3D9IB);
    com_bind(IF_D3DVERTEXDECL9, K_D3D9DECL);
    com_bind(IF_D3DQUERY9, K_D3D9QUERY);
    com_define(IF_D3DVERTEXSHADER9, "d3d9.dll", "IDirect3DVertexShader9", g_shader9,
               std::size(g_shader9));
    com_define(IF_D3DPIXELSHADER9, "d3d9.dll", "IDirect3DPixelShader9", g_shader9,
               std::size(g_shader9));
    com_bind(IF_D3DVERTEXSHADER9, K_D3D9VS);
    com_bind(IF_D3DPIXELSHADER9, K_D3D9PS);
    static const uint8_t vs_iid[16] =
        IID_BYTES(0xefc5557e, 0x6265, 0x4613, 0x8a, 0x94, 0x43, 0x85, 0x78, 0x89, 0xeb, 0x36);
    static const uint8_t ps_iid[16] =
        IID_BYTES(0x6d3bdbdc, 0x5b02, 0x4415, 0xb8, 0x52, 0xce, 0x5e, 0x8b, 0xcc, 0xb2, 0x89);
    com_register_iid(IF_D3DVERTEXSHADER9, vs_iid);
    com_register_iid(IF_D3DPIXELSHADER9, ps_iid);
    com_set_destructor(K_D3D9VS, shader_destroy);
    com_set_destructor(K_D3D9PS, shader_destroy);
    com_set_ref_hook(K_D3D9VS, shader_references);
    com_set_ref_hook(K_D3D9PS, shader_references);
    com_set_destructor(K_D3D9DEVICE, d3d9_device_destroy);
    for (ComKind k : {K_D3D9TEXTURE, K_D3D9SURFACE, K_D3D9VB, K_D3D9IB})
        com_set_destructor(k, d3d9_resource_destroy);
    com_set_destructor(K_D3D9QUERY, query_destroy);
    imports_register(g_d3d9_exports, std::size(g_d3d9_exports));
}
