// OpenAL 1.1 over the shared host mixer. Only the guest baton holder touches
// these maps; the mixer copies submitted PCM before returning. The guest clock
// owns state/progress, including when the host has no audio output at all.
#include "dx.h"
#include "host_api.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace {
constexpr uint32_t INVALID_NAME = 0xa001, INVALID_ENUM = 0xa002, INVALID_VALUE = 0xa003,
                   INVALID_OPERATION = 0xa004, OUT_OF_MEMORY = 0xa005;
constexpr uint32_t INITIAL = 0x1011, PLAYING = 0x1012, PAUSED = 0x1013, STOPPED = 0x1014;
constexpr uint32_t MAX_OBJECTS = 4096, MAX_PCM = 64u << 20;
struct Buffer {
    uint32_t rate = 0, bits = 0, channels = 0, frames = 0, bytes = 0;
    std::vector<int16_t> pcm;
};
struct Source {
    uint32_t state = INITIAL, stamp = 0;
    double cursor = 0;
    float pitch = 1, gain = 1, min_gain = 0, max_gain = 1, reference = 1, maximum = 3.402823466e38f,
          rolloff = 1;
    std::array<float, 3> position{}, velocity{}, direction{};
    float cone_inner = 360, cone_outer = 360, cone_gain = 0;
    bool looping = false, relative = false, static_buffer = false, streaming = false;
    std::vector<uint32_t> queue;
    int32_t channel = -1;
};
struct Context {
    uint32_t device = 0, error = 0, distance = 0xd002;
    float gain = 1, doppler = 1, doppler_velocity = 1, speed = 343.3f;
    std::array<float, 3> position{}, velocity{};
    std::array<float, 6> orientation = {0, 0, -1, 0, 1, 0};
    bool suspended = false;
    std::map<uint32_t, Source> sources;
};
struct Device {
    uint32_t error = 0;
    std::map<uint32_t, Buffer> buffers;
};
std::map<uint32_t, Device> devices;
std::map<uint32_t, Context> contexts;
uint32_t current = 0, no_context_error = 0, no_device_error = 0, next_object = 1;
uint64_t generation = 0;
std::map<std::string, uint32_t> strings;
auto &buffers(Context &ctx) {
    return devices.at(ctx.device).buffers;
}
const auto &buffers(const Context &ctx) {
    return devices.at(ctx.device).buffers;
}
void stop(Source &s) {
    if (s.channel >= 0)
        host_audio_stop(s.channel);
}
void release(Source &s) {
    stop(s);
    if (s.channel >= 0)
        dx_free_audio_channel(s.channel);
    s.channel = -1;
}
void clear_context(Context &ctx) {
    for (auto &[id, s] : ctx.sources)
        release(s);
}
void reset() {
    for (auto &[id, ctx] : contexts)
        clear_context(ctx);
    contexts.clear();
    devices.clear();
    strings.clear();
    current = no_context_error = no_device_error = 0;
    next_object = 1;
    generation = mem_generation();
}
void ensure() {
    if (generation != mem_generation())
        reset();
}
Context *context() {
    ensure();
    auto it = contexts.find(current);
    return it == contexts.end() ? nullptr : &it->second;
}
void error(uint32_t value) {
    Context *ctx = context();
    uint32_t &out = ctx ? ctx->error : no_context_error;
    if (!out)
        out = value;
}
void alc_error(uint32_t device, uint32_t value) {
    ensure();
    auto it = devices.find(device);
    uint32_t &out = it == devices.end() ? no_device_error : it->second.error;
    if (!out)
        out = value;
}
bool memory(uint32_t p, uint64_t size) {
    if (size && (!p || size > UINT32_MAX || !gm_valid(p, uint32_t(size)))) {
        error(INVALID_VALUE);
        return false;
    }
    return true;
}
uint32_t string(const std::string &value, bool list = false) {
    ensure();
    const auto key = value + char(list ? 1 : 0);
    auto it = strings.find(key);
    if (it != strings.end())
        return it->second;
    const uint32_t p = heap_alloc(uint32_t(value.size() + 1 + (list ? 1 : 0)), true);
    if (!p) {
        error(OUT_OF_MEMORY);
        return 0;
    }
    std::memcpy(gm_ptr(p), value.data(), value.size());
    strings[key] = p;
    return p;
}
bool text(uint32_t p, std::string &out) {
    out.clear();
    for (uint32_t i = 0; i < 256; ++i) {
        if (uint64_t(p) + i > UINT32_MAX || !memory(p + i, 1))
            return false;
        const char ch = char(rd8(p + i));
        if (!ch)
            return true;
        out += ch;
    }
    error(INVALID_VALUE);
    return false;
}
float floating(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
uint32_t bits(float value) {
    uint32_t out;
    std::memcpy(&out, &value, 4);
    return out;
}
Context *required() {
    auto ctx = context();
    if (!ctx)
        error(INVALID_OPERATION);
    return ctx;
}
Source *source(Context &ctx, uint32_t id) {
    auto it = ctx.sources.find(id);
    if (it == ctx.sources.end()) {
        error(INVALID_NAME);
        return nullptr;
    }
    return &it->second;
}
const Buffer &buffer(const Context &ctx, uint32_t id) {
    static const Buffer empty;
    return id ? buffers(ctx).at(id) : empty;
}
const Buffer &first_buffer(const Context &ctx, const Source &s) {
    for (auto id : s.queue)
        if (id && buffer(ctx, id).rate)
            return buffer(ctx, id);
    static const Buffer empty;
    return empty;
}
uint32_t rate(const Context &ctx, const Source &s) {
    return first_buffer(ctx, s).rate;
}
uint32_t channels(const Context &ctx, const Source &s) {
    return first_buffer(ctx, s).channels <= 1 ? 1 : 2;
}
uint64_t frames(const Context &ctx, const Source &s) {
    uint64_t n = 0;
    for (auto id : s.queue)
        n += buffer(ctx, id).frames;
    return n;
}
void update(Context &ctx, Source &s) {
    const uint32_t now = host_millis(), elapsed = now - s.stamp;
    s.stamp = now;
    if (s.state != PLAYING || ctx.suspended)
        return;
    const auto total = frames(ctx, s);
    if (!total) {
        s.state = STOPPED;
        s.cursor = 0;
        return;
    }
    s.cursor += double(elapsed) * rate(ctx, s) * s.pitch / 1000.0;
    if (s.looping)
        s.cursor = std::fmod(s.cursor, double(total));
    else if (s.cursor >= double(total)) {
        s.cursor = double(total);
        s.state = STOPPED; /* Keep the host's latency tail sounding. */
    }
}
uint32_t processed(const Context &ctx, const Source &s) {
    if (s.static_buffer || s.looping || s.state == INITIAL)
        return 0;
    if (s.state == STOPPED)
        return uint32_t(s.queue.size());
    double at = s.cursor;
    uint32_t n = 0;
    for (auto id : s.queue) {
        const auto size = buffer(ctx, id).frames;
        if (at < double(size))
            break;
        at -= size;
        ++n;
    }
    return n;
}
void spatial(Context &ctx, Source &s) {
    if (s.channel < 0)
        return;
    float attenuation = 1, pan = 0;
    if (first_buffer(ctx, s).channels == 1) {
        std::array<double, 3> delta{};
        double distance2 = 0;
        for (unsigned i = 0; i < 3; ++i) {
            delta[i] = s.position[i] - (s.relative ? 0 : ctx.position[i]);
            distance2 += delta[i] * delta[i];
        }
        double d = std::sqrt(distance2), ref = s.reference, max = s.maximum, r = s.rolloff;
        if (ctx.distance == 0xd002 || ctx.distance == 0xd004 || ctx.distance == 0xd006)
            d = std::clamp(d, ref, std::max(ref, max));
        if (ctx.distance == 0xd001 || ctx.distance == 0xd002) {
            const double denom = ref + r * (d - ref);
            if (denom > 0)
                attenuation = float(ref / denom);
        } else if (ctx.distance == 0xd003 || ctx.distance == 0xd004) {
            if (max != ref)
                attenuation = float(1 - r * (d - ref) / (max - ref));
        } else if (ctx.distance == 0xd005 || ctx.distance == 0xd006) {
            if (ref > 0 && d > 0)
                attenuation = float(std::pow(d / ref, -r));
        }
        const auto &v = ctx.orientation;
        const std::array<double, 3> right = {double(v[1]) * v[5] - double(v[2]) * v[4],
                                             double(v[2]) * v[3] - double(v[0]) * v[5],
                                             double(v[0]) * v[4] - double(v[1]) * v[3]};
        double norm = 0, dot = 0;
        for (unsigned i = 0; i < 3; ++i) {
            norm += right[i] * right[i];
            dot += right[i] * delta[i];
        }
        const double distance = std::sqrt(distance2);
        if (norm > 0 && distance > 0)
            pan = float(std::clamp(dot / (std::sqrt(norm) * distance), -1.0, 1.0));
    }
    const float gain =
        std::min(s.max_gain, std::max(s.min_gain, std::max(0.0f, attenuation) * s.gain)) * ctx.gain;
    const int32_t volume =
        gain <= 0 ? -10000 : int32_t(std::clamp(2000.0 * std::log10(double(gain)), -10000.0, 0.0));
    host_audio_set_volume(s.channel, volume);
    host_audio_set_pan(s.channel, int32_t(pan * 10000));
    host_audio_set_frequency(
        s.channel, uint32_t(std::clamp(double(rate(ctx, s)) * s.pitch, 1.0, double(INT32_MAX))));
}
// Rebuild only for explicit play/seek or refused append, never pitch/pan jitter.
// Stop first; a naturally finished source retains its host tail until here.
void start(Context &ctx, Source &s) {
    const uint64_t total = frames(ctx, s);
    if (!total) {
        stop(s);
        return;
    }
    const uint32_t ch = channels(ctx, s);
    if (total > MAX_PCM / (ch * 2)) {
        error(OUT_OF_MEMORY);
        return;
    }
    std::vector<int16_t> data;
    data.reserve(size_t(total) * ch);
    for (auto id : s.queue) {
        const auto &b = buffer(ctx, id);
        data.insert(data.end(), b.pcm.begin(), b.pcm.end());
    }
    stop(s);
    if (s.channel < 0)
        s.channel = dx_alloc_audio_channel();
    if (s.channel < 0)
        return;
    HostAudioPlay p{};
    p.channel = s.channel;
    p.pcm = data.data();
    p.bytes = uint32_t(data.size() * 2);
    p.sample_rate = int32_t(std::clamp(double(rate(ctx, s)) * s.pitch, 1.0, double(INT32_MAX)));
    p.channels = int(ch);
    p.bits = 16;
    p.loop = s.looping;
    p.start_offset = uint32_t(std::min(s.cursor, double(total))) * ch * 2;
    host_audio_play(&p);
    if (!s.static_buffer && !s.looping)
        host_audio_stream(s.channel);
    spatial(ctx, s);
}
void open_device(X86 *c) {
    ensure();
    if (devices.size() >= 64) {
        alc_error(0, 0xa005);
        set_eax(c, 0);
        return;
    }
    if (arg(c, 0)) {
        std::string name;
        if (!text(arg(c, 0), name) || (!name.empty() && name != "Generic Software")) {
            alc_error(0, 0xa004);
            set_eax(c, 0);
            return;
        }
    }
    const uint32_t p = heap_alloc(4, true);
    if (p)
        devices.emplace(p, Device{});
    else
        alc_error(0, 0xa005);
    set_eax(c, p);
}
void close_device(X86 *c) {
    ensure();
    const uint32_t id = arg(c, 0);
    if (!devices.count(id)) {
        alc_error(0, 0xa001);
        set_eax(c, 0);
        return;
    }
    for (auto it = contexts.begin(); it != contexts.end();)
        if (it->second.device == id) {
            clear_context(it->second);
            if (current == it->first)
                current = 0;
            heap_free(it->first);
            it = contexts.erase(it);
        } else
            ++it;
    devices.erase(id);
    heap_free(id);
    set_eax(c, 1);
}
void create_context(X86 *c) {
    ensure();
    const uint32_t device = arg(c, 0);
    if (!devices.count(device)) {
        alc_error(0, 0xa001);
        set_eax(c, 0);
        return;
    }
    const uint32_t attrs = arg(c, 1);
    if (attrs) {
        bool end = false;
        for (uint32_t i = 0; i < 128; i += 2) {
            const uint64_t at = uint64_t(attrs) + i * 4;
            if (at > UINT32_MAX || !memory(uint32_t(at), 4))
                break;
            const uint32_t key = rd32(uint32_t(at));
            if (!key) {
                end = true;
                break;
            }
            if (!memory(uint32_t(at), 8) || key < 0x1007 || key > 0x1011 ||
                (key > 0x1009 && key < 0x1010)) {
                alc_error(device, 0xa004);
                set_eax(c, 0);
                return;
            }
        }
        if (!end) {
            alc_error(device, 0xa004);
            set_eax(c, 0);
            return;
        }
    }
    if (contexts.size() >= 64) {
        alc_error(device, 0xa005);
        set_eax(c, 0);
        return;
    }
    const uint32_t p = heap_alloc(4, true);
    if (p)
        contexts.try_emplace(p).first->second.device = device;
    else
        alc_error(device, 0xa005);
    set_eax(c, p);
}
void current_context(X86 *c) {
    ensure();
    const uint32_t id = arg(c, 0);
    if (id && !contexts.count(id)) {
        alc_error(0, 0xa002);
        set_eax(c, 0);
        return;
    }
    current = id;
    set_eax(c, 1);
}
void get_context(X86 *c) {
    ensure();
    set_eax(c, current);
}
void context_device(X86 *c) {
    ensure();
    auto it = contexts.find(arg(c, 0));
    if (it == contexts.end()) {
        alc_error(0, 0xa002);
        set_eax(c, 0);
        return;
    }
    set_eax(c, it->second.device);
}
void destroy_context(X86 *c) {
    ensure();
    auto it = contexts.find(arg(c, 0));
    if (it == contexts.end()) {
        alc_error(0, 0xa002);
        return;
    }
    clear_context(it->second);
    if (current == it->first)
        current = 0;
    heap_free(it->first);
    contexts.erase(it);
}
template <bool Suspend> void process_context(X86 *c) {
    ensure();
    auto it = contexts.find(arg(c, 0));
    if (it == contexts.end()) {
        alc_error(0, 0xa002);
        return;
    }
    auto &ctx = it->second;
    if (ctx.suspended == Suspend)
        return;
    for (auto &[id, s] : ctx.sources)
        update(ctx, s);
    ctx.suspended = Suspend;
    for (auto &[id, s] : ctx.sources)
        if (s.state == PLAYING) {
            if (Suspend)
                stop(s);
            else
                start(ctx, s);
        }
}
void al_error(X86 *c) {
    auto ctx = context();
    uint32_t &value = ctx ? ctx->error : no_context_error;
    set_eax(c, value);
    value = 0;
}
void alc_get_error(X86 *c) {
    ensure();
    auto it = devices.find(arg(c, 0));
    uint32_t &value = it == devices.end() ? no_device_error : it->second.error;
    set_eax(c, value);
    value = 0;
}
void al_string(X86 *c) {
    const char *out = nullptr;
    switch (arg(c, 0)) {
    case 0xb001:
        out = "recomp-kit";
        break;
    case 0xb002:
        out = "1.1";
        break;
    case 0xb003:
        out = "Generic Software";
        break;
    case 0xb004:
        out = "AL_EXT_OFFSET";
        break;
    case INVALID_NAME:
        out = "Invalid name";
        break;
    case INVALID_ENUM:
        out = "Invalid enum";
        break;
    case INVALID_VALUE:
        out = "Invalid value";
        break;
    case INVALID_OPERATION:
        out = "Invalid operation";
        break;
    case OUT_OF_MEMORY:
        out = "Out of memory";
        break;
    default:
        error(INVALID_ENUM);
    }
    set_eax(c, out ? string(out) : 0);
}
void alc_string(X86 *c) {
    ensure();
    const uint32_t device = arg(c, 0), key = arg(c, 1);
    if (device && !devices.count(device)) {
        alc_error(0, 0xa001);
        set_eax(c, 0);
        return;
    }
    if (key == 0x1004 || key == 0x1005 || key == 0x1012 || key == 0x1013)
        set_eax(c, string("Generic Software", !device && (key == 0x1005 || key == 0x1013)));
    else if (key == 0x1006)
        set_eax(c, string("ALC_ENUMERATION_EXT ALC_ENUMERATE_ALL_EXT"));
    else {
        alc_error(device, 0xa003);
        set_eax(c, 0);
    }
}
template <bool Alc = false> void extension(X86 *c) {
    std::string name;
    if (!text(arg(c, Alc ? 1 : 0), name)) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, Alc ? (name == "ALC_ENUMERATION_EXT" || name == "ALC_ENUMERATE_ALL_EXT")
                   : name == "AL_EXT_OFFSET");
}
template <bool Alc = false> void proc(X86 *c) {
    std::string name;
    if (!text(arg(c, Alc ? 1 : 0), name)) {
        set_eax(c, 0);
        return;
    }
    if (name.rfind(Alc ? "alc" : "al", 0) != 0) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, imports_resolve("OpenAL32.dll", name.c_str()));
}
struct Enum {
    const char *name;
    uint32_t value;
};
constexpr Enum enums[] = {{"AL_NONE", 0},
                          {"AL_FALSE", 0},
                          {"AL_TRUE", 1},
                          {"AL_PITCH", 0x1003},
                          {"AL_POSITION", 0x1004},
                          {"AL_DIRECTION", 0x1005},
                          {"AL_VELOCITY", 0x1006},
                          {"AL_LOOPING", 0x1007},
                          {"AL_BUFFER", 0x1009},
                          {"AL_GAIN", 0x100a},
                          {"AL_MIN_GAIN", 0x100d},
                          {"AL_MAX_GAIN", 0x100e},
                          {"AL_ORIENTATION", 0x100f},
                          {"AL_SOURCE_RELATIVE", 0x202},
                          {"AL_SOURCE_STATE", 0x1010},
                          {"AL_INITIAL", INITIAL},
                          {"AL_PLAYING", PLAYING},
                          {"AL_PAUSED", PAUSED},
                          {"AL_STOPPED", STOPPED},
                          {"AL_BUFFERS_QUEUED", 0x1015},
                          {"AL_BUFFERS_PROCESSED", 0x1016},
                          {"AL_REFERENCE_DISTANCE", 0x1020},
                          {"AL_ROLLOFF_FACTOR", 0x1021},
                          {"AL_MAX_DISTANCE", 0x1023},
                          {"AL_SEC_OFFSET", 0x1024},
                          {"AL_SAMPLE_OFFSET", 0x1025},
                          {"AL_BYTE_OFFSET", 0x1026},
                          {"AL_SOURCE_TYPE", 0x1027},
                          {"AL_STATIC", 0x1028},
                          {"AL_STREAMING", 0x1029},
                          {"AL_UNDETERMINED", 0x1030},
                          {"AL_FORMAT_MONO8", 0x1100},
                          {"AL_FORMAT_MONO16", 0x1101},
                          {"AL_FORMAT_STEREO8", 0x1102},
                          {"AL_FORMAT_STEREO16", 0x1103},
                          {"AL_FORMAT_QUAD16", 0x1205},
                          {"AL_FORMAT_51CHN16", 0x120b},
                          {"AL_FORMAT_61CHN16", 0x120e},
                          {"AL_FORMAT_71CHN16", 0x1211},
                          {"AL_INVERSE_DISTANCE", 0xd001},
                          {"AL_INVERSE_DISTANCE_CLAMPED", 0xd002},
                          {"AL_LINEAR_DISTANCE", 0xd003},
                          {"AL_LINEAR_DISTANCE_CLAMPED", 0xd004},
                          {"AL_EXPONENT_DISTANCE", 0xd005},
                          {"AL_EXPONENT_DISTANCE_CLAMPED", 0xd006},
                          {"ALC_FREQUENCY", 0x1007},
                          {"ALC_REFRESH", 0x1008},
                          {"ALC_SYNC", 0x1009},
                          {"ALC_MONO_SOURCES", 0x1010},
                          {"ALC_STEREO_SOURCES", 0x1011},
                          {"ALC_DEFAULT_DEVICE_SPECIFIER", 0x1004},
                          {"ALC_DEVICE_SPECIFIER", 0x1005},
                          {"ALC_EXTENSIONS", 0x1006},
                          {"ALC_DEFAULT_ALL_DEVICES_SPECIFIER", 0x1012},
                          {"ALC_ALL_DEVICES_SPECIFIER", 0x1013}};
template <bool Alc = false> void enum_value(X86 *c) {
    std::string name;
    if (text(arg(c, Alc ? 1 : 0), name))
        for (const auto &entry : enums)
            if (name == entry.name) {
                set_eax(c, entry.value);
                return;
            }
    set_eax(c, 0);
}
template <bool Sources> void generate(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    const uint32_t n = arg(c, 0), out = arg(c, 1);
    if (n > MAX_OBJECTS || !memory(out, uint64_t(n) * 4)) {
        error(INVALID_VALUE);
        return;
    }
    const size_t existing = Sources ? ctx->sources.size() : buffers(*ctx).size();
    if (existing + n > MAX_OBJECTS || uint64_t(next_object) + n > UINT32_MAX) {
        error(OUT_OF_MEMORY);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = next_object++;
        if (Sources)
            ctx->sources.try_emplace(id);
        else
            buffers(*ctx).try_emplace(id);
        wr32(out + i * 4, id);
    }
}
bool in_use(const Context &ctx, uint32_t id) {
    for (const auto &[handle, other] : contexts)
        if (other.device == ctx.device)
            for (const auto &[key, s] : other.sources)
                if (std::find(s.queue.begin(), s.queue.end(), id) != s.queue.end())
                    return true;
    return false;
}
template <bool Sources> void erase(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    const uint32_t n = arg(c, 0), ids = arg(c, 1);
    if (n > MAX_OBJECTS || !memory(ids, uint64_t(n) * 4)) {
        error(INVALID_VALUE);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = rd32(ids + i * 4);
        if (!Sources && !id)
            continue;
        if (!(Sources ? ctx->sources.count(id) : buffers(*ctx).count(id))) {
            error(INVALID_NAME);
            return;
        }
        if (!Sources && in_use(*ctx, id)) {
            error(INVALID_OPERATION);
            return;
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = rd32(ids + i * 4);
        if (Sources) {
            auto it = ctx->sources.find(id);
            if (it != ctx->sources.end()) {
                release(it->second);
                ctx->sources.erase(it);
            }
        } else
            buffers(*ctx).erase(id);
    }
}
template <bool Sources> void is_object(X86 *c) {
    auto ctx = context();
    set_eax(c, ctx && (Sources ? ctx->sources.count(arg(c, 0)) : buffers(*ctx).count(arg(c, 0))));
}
void buffer_data(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    auto it = buffers(*ctx).find(arg(c, 0));
    if (it == buffers(*ctx).end()) {
        error(INVALID_NAME);
        return;
    }
    if (in_use(*ctx, it->first)) {
        error(INVALID_OPERATION);
        return;
    }
    const uint32_t format = arg(c, 1), p = arg(c, 2), bytes = arg(c, 3), hz = arg(c, 4);
    uint32_t ch = 0, bps = 16;
    if (format >= 0x1100 && format <= 0x1103) {
        ch = format >= 0x1102 ? 2 : 1;
        bps = (format & 1) ? 16 : 8;
    } else if (format == 0x1205)
        ch = 4;
    else if (format == 0x120b)
        ch = 6;
    else if (format == 0x120e)
        ch = 7;
    else if (format == 0x1211)
        ch = 8;
    else {
        error(INVALID_ENUM);
        return;
    }
    if (bytes > MAX_PCM || hz == 0 || hz > 384000 || bytes % (ch * (bps / 8)) ||
        (p && !memory(p, bytes))) {
        error(INVALID_VALUE);
        return;
    }
    Buffer b;
    b.rate = hz;
    b.channels = ch;
    b.bits = bps;
    b.frames = bytes / (ch * (bps / 8));
    b.bytes = bytes;
    const uint32_t folded = ch == 1 ? 1 : 2;
    if (uint64_t(b.frames) * folded * 2 > MAX_PCM) {
        error(OUT_OF_MEMORY);
        return;
    }
    b.pcm.resize(size_t(b.frames) * folded);
    for (uint32_t i = 0; i < b.frames; ++i) {
        std::array<int32_t, 8> sample{};
        for (uint32_t j = 0; j < ch; ++j)
            sample[j] = !p         ? 0
                        : bps == 8 ? (int(rd8(p + i * ch + j)) - 128) * 256
                                   : int16_t(rd16(p + (i * ch + j) * 2));
        if (ch <= 2) {
            for (uint32_t j = 0; j < ch; ++j)
                b.pcm[i * folded + j] = int16_t(sample[j]);
        } else {
            double left = sample[0], right = sample[1];
            if (ch == 4) {
                left += sample[2] * .70710678;
                right += sample[3] * .70710678;
            } else {
                left += sample[2] * .70710678 + sample[3] * .5;
                right += sample[2] * .70710678 + sample[3] * .5;
                for (uint32_t j = 4; j < ch; ++j) {
                    if (j == 6 && ch == 7) {
                        left += sample[j] * .5;
                        right += sample[j] * .5;
                    } else if (j % 2)
                        right += sample[j] * .70710678;
                    else
                        left += sample[j] * .70710678;
                }
            }
            b.pcm[i * 2] = int16_t(std::clamp(left, -32768.0, 32767.0));
            b.pcm[i * 2 + 1] = int16_t(std::clamp(right, -32768.0, 32767.0));
        }
    }
    it->second = std::move(b);
}
template <bool Float = false> void get_buffer(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    auto it = buffers(*ctx).find(arg(c, 0));
    if (it == buffers(*ctx).end()) {
        error(INVALID_NAME);
        return;
    }
    if (!memory(arg(c, 2), 4))
        return;
    const auto &b = it->second;
    uint32_t value = 0;
    switch (arg(c, 1)) {
    case 0x2001:
        value = b.rate;
        break;
    case 0x2002:
        value = b.bits;
        break;
    case 0x2003:
        value = b.channels;
        break;
    case 0x2004:
        value = b.bytes;
        break;
    default:
        error(INVALID_ENUM);
        return;
    }
    wr32(arg(c, 2), Float ? bits(float(value)) : value);
}
template <unsigned Operation, bool Vector = false> void control(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    const uint32_t n = Vector ? arg(c, 0) : 1, ids = Vector ? arg(c, 1) : 0;
    if (n > MAX_OBJECTS || (Vector && !memory(ids, uint64_t(n) * 4))) {
        error(INVALID_VALUE);
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (!ctx->sources.count(Vector ? rd32(ids + i * 4) : arg(c, 0))) {
            error(INVALID_NAME);
            return;
        }
    for (uint32_t i = 0; i < n; ++i) {
        auto &s = ctx->sources.at(Vector ? rd32(ids + i * 4) : arg(c, 0));
        update(*ctx, s);
        if (Operation == 0) {
            if (s.state != PAUSED)
                s.cursor = 0;
            s.state = PLAYING;
            s.stamp = host_millis();
            if (frames(*ctx, s))
                start(*ctx, s);
            else
                s.state = STOPPED;
        } else if (Operation == 1) {
            if (s.state == PLAYING) {
                s.state = PAUSED;
                stop(s);
            }
        } else {
            s.state = Operation == 2 ? STOPPED : INITIAL;
            s.cursor = Operation == 2 ? double(frames(*ctx, s)) : 0;
            stop(s);
        }
    }
}
void source_value(Context &ctx, Source &s, uint32_t key, float value, int32_t integer,
                  bool integral) {
    update(ctx, s);
    if (!std::isfinite(value)) {
        error(INVALID_VALUE);
        return;
    }
    switch (key) {
    case 0x1003:
        if (value <= 0) {
            error(INVALID_VALUE);
            return;
        }
        s.pitch = value;
        break;
    case 0x100a:
        if (value < 0) {
            error(INVALID_VALUE);
            return;
        }
        s.gain = value;
        break;
    case 0x100d:
    case 0x100e:
        if (value < 0 || value > 1) {
            error(INVALID_VALUE);
            return;
        }
        (key == 0x100d ? s.min_gain : s.max_gain) = value;
        break;
    case 0x1020:
    case 0x1021:
    case 0x1023:
        if (value < 0) {
            error(INVALID_VALUE);
            return;
        }
        (key == 0x1020 ? s.reference : key == 0x1021 ? s.rolloff : s.maximum) = value;
        break;
    case 0x1001:
    case 0x1002:
        if (value < 0 || value > 360) {
            error(INVALID_VALUE);
            return;
        }
        (key == 0x1001 ? s.cone_inner : s.cone_outer) = value;
        break;
    case 0x1022:
        if (value < 0 || value > 1) {
            error(INVALID_VALUE);
            return;
        }
        s.cone_gain = value;
        break;
    case 0x1007:
    case 0x202:
        if (!integral || (integer != 0 && integer != 1)) {
            error(INVALID_VALUE);
            return;
        }
        (key == 0x1007 ? s.looping : s.relative) = integer != 0;
        if (key == 0x1007 && s.state == PLAYING)
            start(ctx, s);
        return;
    case 0x1009: {
        if (!integral) {
            error(INVALID_ENUM);
            return;
        }
        if (s.state == PLAYING || s.state == PAUSED) {
            error(INVALID_OPERATION);
            return;
        }
        const uint32_t id = uint32_t(integer);
        if (id && !buffers(ctx).count(id)) {
            error(INVALID_VALUE);
            return;
        }
        stop(s);
        s.queue.clear();
        s.static_buffer = id != 0;
        s.streaming = false;
        if (id)
            s.queue.push_back(id);
        s.cursor = 0;
        s.state = INITIAL;
        return;
    }
    case 0x1024:
    case 0x1025:
    case 0x1026: {
        if (value < 0 || !frames(ctx, s)) {
            error(INVALID_VALUE);
            return;
        }
        double offset = value;
        if (key == 0x1024)
            offset *= rate(ctx, s);
        else if (key == 0x1026 && !s.queue.empty()) {
            const auto &b = first_buffer(ctx, s);
            offset /= b.channels * (b.bits / 8);
        }
        if (offset >= double(frames(ctx, s))) {
            error(INVALID_VALUE);
            return;
        }
        s.cursor = std::floor(offset);
        if (s.state == PLAYING)
            start(ctx, s);
        return;
    }
    default:
        error(INVALID_ENUM);
        return;
    }
    spatial(ctx, s);
}
template <bool Float = false, bool Pointer = false> void set_source(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    auto s = source(*ctx, arg(c, 0));
    if (!s)
        return;
    uint32_t raw = arg(c, 2);
    if (Pointer) {
        if (!memory(raw, 4))
            return;
        raw = rd32(raw);
    }
    source_value(*ctx, *s, arg(c, 1), Float ? floating(raw) : float(int32_t(raw)), int32_t(raw),
                 !Float);
}
template <bool Float = true, bool Pointer = false> void source_vector(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    auto s = source(*ctx, arg(c, 0));
    if (!s)
        return;
    const uint32_t key = arg(c, 1);
    if (key != 0x1004 && key != 0x1005 && key != 0x1006) {
        error(INVALID_ENUM);
        return;
    }
    if (Pointer && !memory(arg(c, 2), 12))
        return;
    std::array<float, 3> values{};
    for (unsigned i = 0; i < 3; ++i) {
        const auto raw = Pointer ? rd32(arg(c, 2) + i * 4) : arg(c, 2 + i);
        values[i] = Float ? floating(raw) : float(int32_t(raw));
        if (!std::isfinite(values[i])) {
            error(INVALID_VALUE);
            return;
        }
    }
    (key == 0x1004 ? s->position : key == 0x1005 ? s->direction : s->velocity) = values;
    spatial(*ctx, *s);
}
template <bool Float> void source_pointer(X86 *c) {
    const uint32_t key = arg(c, 1);
    if (key == 0x1004 || key == 0x1005 || key == 0x1006)
        source_vector<Float, true>(c);
    else
        set_source<Float, true>(c);
}
uint32_t integer(float value) {
    return uint32_t(int32_t(std::clamp(double(value), double(INT32_MIN), double(INT32_MAX))));
}
float get_value(Context &ctx, Source &s, uint32_t key, bool &ok) {
    update(ctx, s);
    switch (key) {
    case 0x1001:
        return s.cone_inner;
    case 0x1002:
        return s.cone_outer;
    case 0x1003:
        return s.pitch;
    case 0x1007:
        return s.looping;
    case 0x202:
        return s.relative;
    case 0x1009:
        return s.static_buffer && !s.queue.empty() ? float(s.queue.front()) : 0;
    case 0x100a:
        return s.gain;
    case 0x100d:
        return s.min_gain;
    case 0x100e:
        return s.max_gain;
    case 0x1010:
        return float(s.state);
    case 0x1015:
        return float(s.queue.size());
    case 0x1016:
        return float(processed(ctx, s));
    case 0x1020:
        return s.reference;
    case 0x1021:
        return s.rolloff;
    case 0x1022:
        return s.cone_gain;
    case 0x1023:
        return s.maximum;
    case 0x1024:
        return rate(ctx, s) ? float(s.cursor / rate(ctx, s)) : 0;
    case 0x1025:
        return float(s.cursor);
    case 0x1026:
        if (s.queue.empty())
            return 0;
        else {
            const auto &b = first_buffer(ctx, s);
            return float(s.cursor * b.channels * (b.bits / 8));
        }
    case 0x1027:
        return s.static_buffer ? 0x1028 : s.streaming ? 0x1029 : 0x1030;
    default:
        ok = false;
        error(INVALID_ENUM);
        return 0;
    }
}
template <bool Float = false, bool Vector = false, bool Triple = false> void get_source(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    auto s = source(*ctx, arg(c, 0));
    if (!s)
        return;
    const uint32_t key = arg(c, 1);
    const bool spatial_key = key == 0x1004 || key == 0x1005 || key == 0x1006;
    const unsigned n = spatial_key && (Vector || Triple) ? 3 : 1;
    if ((spatial_key && n == 1) || (Triple && !spatial_key)) {
        error(INVALID_ENUM);
        return;
    }
    for (unsigned i = 0; i < n; ++i)
        if (!memory(Triple ? arg(c, 2 + i) : arg(c, 2) + i * 4, 4))
            return;
    bool ok = true;
    std::array<float, 3> values{};
    if (spatial_key)
        values = key == 0x1004 ? s->position : key == 0x1005 ? s->direction : s->velocity;
    else
        values[0] = get_value(*ctx, *s, key, ok);
    if (ok)
        for (unsigned i = 0; i < n; ++i)
            wr32(Triple ? arg(c, 2 + i) : arg(c, 2) + i * 4,
                 Float                                                    ? bits(values[i])
                 : key == 0x1009 && s->static_buffer && !s->queue.empty() ? s->queue.front()
                                                                          : integer(values[i]));
}
template <bool Remove = false> void queue_buffers(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    auto s = source(*ctx, arg(c, 0));
    if (!s)
        return;
    const uint32_t n = arg(c, 1), ids = arg(c, 2);
    if (n > MAX_OBJECTS || !memory(ids, uint64_t(n) * 4)) {
        error(INVALID_VALUE);
        return;
    }
    update(*ctx, *s);
    if (s->static_buffer) {
        error(INVALID_OPERATION);
        return;
    }
    if (Remove) {
        if (n > processed(*ctx, *s)) {
            error(INVALID_VALUE);
            return;
        }
        uint64_t removed = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t id = s->queue[i];
            removed += buffer(*ctx, id).frames;
            wr32(ids + i * 4, id);
        }
        s->queue.erase(s->queue.begin(), s->queue.begin() + n);
        s->cursor = std::max(0.0, s->cursor - double(removed));
        return;
    }
    if (s->queue.size() + n > MAX_OBJECTS) {
        error(OUT_OF_MEMORY);
        return;
    }
    uint32_t hz = rate(*ctx, *s), ch = channels(*ctx, *s),
             original_channels = first_buffer(*ctx, *s).channels,
             original_bits = first_buffer(*ctx, *s).bits;
    uint64_t total = frames(*ctx, *s) * ch * 2;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = rd32(ids + i * 4);
        if (!id)
            continue;
        auto it = buffers(*ctx).find(id);
        if (it == buffers(*ctx).end()) {
            error(INVALID_NAME);
            return;
        }
        const auto &b = it->second;
        if (!hz) {
            hz = b.rate;
            ch = b.channels == 1 ? 1 : 2;
            original_channels = b.channels;
            original_bits = b.bits;
        }
        if (!b.rate || b.rate != hz || b.channels != original_channels || b.bits != original_bits) {
            error(INVALID_OPERATION);
            return;
        }
        total += b.pcm.size() * 2;
        if (total > MAX_PCM) {
            error(OUT_OF_MEMORY);
            return;
        }
    }
    const size_t first = s->queue.size();
    s->queue.reserve(first + n);
    s->streaming = true;
    for (uint32_t i = 0; i < n; ++i)
        s->queue.push_back(rd32(ids + i * 4));
    if (s->state == PLAYING && !ctx->suspended) {
        bool restart = s->looping || s->channel < 0;
        for (size_t i = first; i < s->queue.size() && !restart; ++i) {
            const auto &b = buffer(*ctx, s->queue[i]);
            if (!b.pcm.empty() &&
                host_audio_queue(s->channel, b.pcm.data(), uint32_t(b.pcm.size() * 2)) !=
                    int32_t(b.pcm.size() * 2))
                restart = true;
        }
        if (restart)
            start(*ctx, *s);
    }
}
template <bool Float = true, bool Pointer = false, bool Triple = false> void listener(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    const uint32_t key = arg(c, 0), n = key == 0x100f ? 6 : key == 0x1004 || key == 0x1006 ? 3 : 1;
    if (Triple && n != 3) {
        error(INVALID_ENUM);
        return;
    }
    if (n == 6 && !Pointer) {
        error(INVALID_ENUM);
        return;
    }
    if (n == 3 && !Pointer && !Triple) {
        error(INVALID_ENUM);
        return;
    }
    if (Pointer && !memory(arg(c, 1), uint64_t(n) * 4))
        return;
    std::array<float, 6> v{};
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t raw = Pointer ? rd32(arg(c, 1) + i * 4) : arg(c, 1 + i);
        v[i] = Float ? floating(raw) : float(int32_t(raw));
        if (!std::isfinite(v[i])) {
            error(INVALID_VALUE);
            return;
        }
    }
    if (key == 0x100a) {
        if (v[0] < 0) {
            error(INVALID_VALUE);
            return;
        }
        ctx->gain = v[0];
    } else if (key == 0x1004)
        std::copy_n(v.begin(), 3, ctx->position.begin());
    else if (key == 0x1006)
        std::copy_n(v.begin(), 3, ctx->velocity.begin());
    else if (key == 0x100f)
        ctx->orientation = v;
    else {
        error(INVALID_ENUM);
        return;
    }
    for (auto &[id, s] : ctx->sources)
        spatial(*ctx, s);
}
template <bool Float = true, bool Triple = false, bool Vector = false> void get_listener(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    const uint32_t key = arg(c, 0), n = key == 0x100f ? 6 : key == 0x1004 || key == 0x1006 ? 3 : 1;
    if ((Triple && n != 3) || (!Triple && !Vector && n != 1)) {
        error(INVALID_ENUM);
        return;
    }
    std::array<float, 6> values{};
    if (key == 0x100a)
        values[0] = ctx->gain;
    else if (key == 0x1004)
        std::copy(ctx->position.begin(), ctx->position.end(), values.begin());
    else if (key == 0x1006)
        std::copy(ctx->velocity.begin(), ctx->velocity.end(), values.begin());
    else if (key == 0x100f)
        values = ctx->orientation;
    else {
        error(INVALID_ENUM);
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (!memory(Triple ? arg(c, 1 + i) : arg(c, 1) + i * 4, 4))
            return;
    for (uint32_t i = 0; i < n; ++i)
        wr32(Triple ? arg(c, 1 + i) : arg(c, 1) + i * 4,
             Float ? bits(values[i]) : integer(values[i]));
}
void distance(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    const uint32_t model = arg(c, 0);
    if (model && (model < 0xd001 || model > 0xd006)) {
        error(INVALID_VALUE);
        return;
    }
    ctx->distance = model;
    for (auto &[id, s] : ctx->sources)
        spatial(*ctx, s);
}
template <unsigned Kind> void global_set(X86 *c) {
    auto ctx = required();
    if (!ctx)
        return;
    const float value = floating(arg(c, 0));
    if (!std::isfinite(value) || value < 0 || (Kind && value == 0)) {
        error(INVALID_VALUE);
        return;
    }
    (Kind == 0 ? ctx->doppler : Kind == 1 ? ctx->doppler_velocity : ctx->speed) = value;
}
double global_value(uint32_t key) {
    auto ctx = required();
    if (!ctx)
        return 0;
    switch (key) {
    case 0xc000:
        return ctx->doppler;
    case 0xc001:
        return ctx->doppler_velocity;
    case 0xc003:
        return ctx->speed;
    case 0xd000:
        return ctx->distance;
    default:
        error(INVALID_ENUM);
        return 0;
    }
}
template <unsigned Kind, bool Vector = false> void get_global(X86 *c) {
    const double value = global_value(arg(c, 0));
    if (Vector) {
        const uint32_t out = arg(c, 1);
        if (!memory(out, Kind == 3 ? 8 : Kind == 0 ? 1 : 4))
            return;
        if (Kind == 0)
            wr8(out, value != 0);
        else if (Kind == 1)
            wr32(out, integer(float(value)));
        else if (Kind == 2)
            wr32(out, bits(float(value)));
        else {
            uint64_t raw;
            std::memcpy(&raw, &value, 8);
            wr64(out, raw);
        }
    } else if (Kind == 0)
        set_eax(c, value != 0);
    else if (Kind == 1)
        set_eax(c, integer(float(value)));
    else
        fpush(c, Kind == 2 ? float(value) : value);
}
void capability(X86 *c) {
    error(INVALID_ENUM);
    set_eax(c, 0);
}
void alc_integers(X86 *c) {
    ensure();
    const uint32_t dev = arg(c, 0), key = arg(c, 1), n = arg(c, 2), out = arg(c, 3);
    if (dev && !devices.count(dev)) {
        alc_error(0, 0xa001);
        return;
    }
    std::vector<uint32_t> values;
    switch (key) {
    case 0x1000:
    case 0x1001:
        values = {1};
        break;
    case 0x1002:
        values = {7};
        break;
    case 0x1003:
        values = {0x1007, 44100, 0x1008, 60, 0x1009, 0, 0};
        break;
    case 0x1007:
        values = {44100};
        break;
    case 0x1008:
        values = {60};
        break;
    case 0x1009:
        values = {0};
        break;
    case 0x1010:
    case 0x1011:
        values = {MAX_OBJECTS};
        break;
    default:
        alc_error(dev, 0xa003);
        return;
    }
    if (n < values.size() || !n || !memory(out, uint64_t(n) * 4)) {
        alc_error(dev, 0xa004);
        return;
    }
    for (uint32_t i = 0; i < values.size(); ++i)
        wr32(out + i * 4, values[i]);
}
void buffer_set(X86 *) {
    error(INVALID_ENUM);
}
void capture_open(X86 *c) {
    alc_error(0, 0xa004);
    set_eax(c, 0);
}
void capture_unavailable(X86 *c) {
    alc_error(arg(c, 0), 0xa001);
    set_eax(c, 0);
}
template <void (*Function)(X86 *)> void checked(X86 *c) {
    try {
        Function(c);
    } catch (const std::bad_alloc &) {
        const char *name = imports_current_description();
        if (name && std::strstr(name, "!alc"))
            alc_error(arg(c, 0), 0xa005);
        else
            error(OUT_OF_MEMORY);
        set_eax(c, 0);
    }
}
} // namespace
void openal_reset() {
    reset();
}
void openal_register() {
#define E(name, ...)                                                                               \
    {                                                                                              \
        "OpenAL32.dll", name, ARGC_CDECL, checked<__VA_ARGS__>                                     \
    }
    const ImportShim entries[] = {E("alcOpenDevice", open_device),
                                  E("alcCaptureOpenDevice", capture_open),
                                  E("alcCaptureCloseDevice", capture_unavailable),
                                  E("alcCaptureStart", capture_unavailable),
                                  E("alcCaptureStop", capture_unavailable),
                                  E("alcCaptureSamples", capture_unavailable),
                                  E("alcCloseDevice", close_device),
                                  E("alcCreateContext", create_context),
                                  E("alcDestroyContext", destroy_context),
                                  E("alcMakeContextCurrent", current_context),
                                  E("alcGetCurrentContext", get_context),
                                  E("alcGetContextsDevice", context_device),
                                  E("alcSuspendContext", process_context<true>),
                                  E("alcProcessContext", process_context<false>),
                                  E("alcGetError", alc_get_error),
                                  E("alcGetString", alc_string),
                                  E("alcGetIntegerv", alc_integers),
                                  E("alcIsExtensionPresent", extension<true>),
                                  E("alcGetProcAddress", proc<true>),
                                  E("alcGetEnumValue", enum_value<true>),
                                  E("alGetError", al_error),
                                  E("alGetString", al_string),
                                  E("alIsExtensionPresent", extension<>),
                                  E("alGetProcAddress", proc<>),
                                  E("alGetEnumValue", enum_value<>),
                                  E("alEnable", capability),
                                  E("alDisable", capability),
                                  E("alIsEnabled", capability),
                                  E("alGenBuffers", generate<false>),
                                  E("alDeleteBuffers", erase<false>),
                                  E("alIsBuffer", is_object<false>),
                                  E("alBufferData", buffer_data),
                                  E("alGetBufferi", get_buffer<>),
                                  E("alGetBufferiv", get_buffer<>),
                                  E("alGetBufferf", get_buffer<true>),
                                  E("alGetBufferfv", get_buffer<true>),
                                  E("alBufferi", buffer_set),
                                  E("alBufferiv", buffer_set),
                                  E("alBufferf", buffer_set),
                                  E("alBufferfv", buffer_set),
                                  E("alBuffer3f", buffer_set),
                                  E("alBuffer3i", buffer_set),
                                  E("alGetBuffer3f", buffer_set),
                                  E("alGetBuffer3i", buffer_set),
                                  E("alGenSources", generate<true>),
                                  E("alDeleteSources", erase<true>),
                                  E("alIsSource", is_object<true>),
                                  E("alSourcei", set_source<>),
                                  E("alSourceiv", source_pointer<false>),
                                  E("alSourcef", set_source<true>),
                                  E("alSourcefv", source_pointer<true>),
                                  E("alSource3f", source_vector<>),
                                  E("alSource3i", source_vector<false>),
                                  E("alGetSourcei", get_source<>),
                                  E("alGetSourceiv", get_source<false, true>),
                                  E("alGetSourcef", get_source<true>),
                                  E("alGetSourcefv", get_source<true, true>),
                                  E("alGetSource3i", get_source<false, false, true>),
                                  E("alGetSource3f", get_source<true, false, true>),
                                  E("alSourcePlay", control<0>),
                                  E("alSourcePause", control<1>),
                                  E("alSourceStop", control<2>),
                                  E("alSourceRewind", control<3>),
                                  E("alSourcePlayv", control<0, true>),
                                  E("alSourcePausev", control<1, true>),
                                  E("alSourceStopv", control<2, true>),
                                  E("alSourceRewindv", control<3, true>),
                                  E("alSourceQueueBuffers", queue_buffers<>),
                                  E("alSourceUnqueueBuffers", queue_buffers<true>),
                                  E("alListenerf", listener<>),
                                  E("alListenerfv", listener<true, true>),
                                  E("alListener3f", listener<true, false, true>),
                                  E("alListeneri", listener<false>),
                                  E("alListeneriv", listener<false, true>),
                                  E("alListener3i", listener<false, false, true>),
                                  E("alGetListenerf", get_listener<>),
                                  E("alGetListenerfv", get_listener<true, false, true>),
                                  E("alGetListener3f", get_listener<true, true>),
                                  E("alGetListeneri", get_listener<false>),
                                  E("alGetListeneriv", get_listener<false, false, true>),
                                  E("alGetListener3i", get_listener<false, true>),
                                  E("alDistanceModel", distance),
                                  E("alDopplerFactor", global_set<0>),
                                  E("alDopplerVelocity", global_set<1>),
                                  E("alSpeedOfSound", global_set<2>),
                                  E("alGetBoolean", get_global<0>),
                                  E("alGetInteger", get_global<1>),
                                  E("alGetFloat", get_global<2>),
                                  E("alGetDouble", get_global<3>),
                                  E("alGetBooleanv", get_global<0, true>),
                                  E("alGetIntegerv", get_global<1, true>),
                                  E("alGetFloatv", get_global<2, true>),
                                  E("alGetDoublev", get_global<3, true>)};
#undef E
    imports_register(entries, sizeof entries / sizeof entries[0]);
}
