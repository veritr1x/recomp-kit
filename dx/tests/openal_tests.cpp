// OpenAL imports against a captured mixer and an explicitly advanced clock.
#include "../dx.h"
#include "../host_api.h"
#include "../../runtime/imports.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"
#include "../../platform/os.h"
#include "guest_abi.h"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
#include <cmath>
#include <cstring>
#include <vector>
static uint32_t now_ms, plays, stops, queues;
static bool refuse_queue;
static HostAudioPlay last_play{};
static int32_t volume, pan;
extern "C" void host_audio_set_volume(int32_t, int32_t value) {
    volume = value;
}
extern "C" void host_audio_set_pan(int32_t, int32_t value) {
    pan = value;
}
static std::vector<uint8_t> pcm;
extern "C" void host_audio_play(const HostAudioPlay *p) {
    ++plays;
    last_play = *p;
    pcm.assign((const uint8_t *)p->pcm, (const uint8_t *)p->pcm + p->bytes);
}
extern "C" void host_audio_stop(int32_t) {
    ++stops;
}
extern "C" int32_t host_audio_stream(int32_t) {
    return 0;
}
extern "C" int32_t host_audio_queue(int32_t, const void *, uint32_t bytes) {
    ++queues;
    return refuse_queue ? 0 : int32_t(bytes);
}
static uint32_t clock_ms() {
    return now_ms;
}
static uint32_t string(const char *s) {
    uint32_t p = heap_alloc(uint32_t(std::strlen(s) + 1), true);
    std::memcpy(gm_ptr(p), s, std::strlen(s) + 1);
    return p;
}
static uint32_t f(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
static uint32_t api(const char *name, std::initializer_list<uint32_t> args = {}) {
    const uint32_t target = imports_resolve("OpenAL32.dll", name);
    CHECK(target);
    return target ? call_shim(target, args) : 0;
}
int main() {
    mem_init();
    imports_init();
    win32_init(".");
    dx_register_shims();
    dx_reset();
    g_stack_top = STACK_TOP - 4096;
    cpu_reset();
    host_set_time_source(clock_ms);
    const uint32_t device = api("alcOpenDevice", {0});
    CHECK(device);
    if (!device)
        return 1;
    const uint32_t context = api("alcCreateContext", {device, 0});
    CHECK(context && api("alcMakeContextCurrent", {context}));
    CHECK(api("alcGetContextsDevice", {context}) == device);
    CHECK(!std::strcmp((const char *)gm_ptr(api("alcGetString", {0, 0x1005})), "Generic Software"));
    const uint32_t ext = string("EAXSet");
    CHECK(!api("alGetProcAddress", {ext}));
    const uint32_t ids = heap_alloc(32, true), data = heap_alloc(4000, true),
                   out = heap_alloc(16, true);
    api("alGenBuffers", {3, ids});
    const uint32_t a = rd32(ids), b = rd32(ids + 4), different = rd32(ids + 8);
    CHECK(a && b && a != b);
    api("alBufferData", {a, 0x1101, data, 2000, 1000});
    api("alBufferData", {b, 0x1101, data, 2000, 1000});
    api("alBufferData", {different, 0x1101, data, 2000, 2000});
    CHECK(api("alGetError") == 0);
    api("alGenSources", {1, ids + 12});
    const uint32_t source = rd32(ids + 12);
    wr32(ids + 16, a);
    wr32(ids + 20, b);
    api("alSourceQueueBuffers", {source, 2, ids + 16});
    api("alSourcePlay", {source});
    CHECK(plays == 1);
    const uint32_t initially_stopped = stops;
    now_ms = 1100;
    api("alGetSourcei", {source, 0x1016, out});
    CHECK(rd32(out) == 1);
    api("alSourceUnqueueBuffers", {source, 1, out});
    CHECK(rd32(out) == a);
    api("alSourcef", {source, 0x1003, f(1.001f)});
    api("alSource3f", {source, 0x1004, f(.01f), 0, 0});
    CHECK(plays == 1 && stops == initially_stopped);
    api("alSourceQueueBuffers", {source, 1, ids + 8});
    CHECK(api("alGetError") == 0xa004);
    api("alSourceQueueBuffers", {source, 1, ids + 16});
    CHECK(queues == 1 && plays == 1);
    refuse_queue = true;
    api("alSourceQueueBuffers", {source, 1, ids + 16});
    CHECK(plays == 2 && stops > initially_stopped && last_play.start_offset > 0);
    refuse_queue = false;
    now_ms = 10000;
    const uint32_t before_finish = stops;
    api("alGetSourcei", {source, 0x1010, out});
    CHECK(rd32(out) == 0x1014 && stops == before_finish);
    api("alSourcePlay", {source});
    CHECK(stops > before_finish && plays == 3);
    api("alSourceStop", {source});
    api("alGetSourcei", {source, 0x1016, out});
    CHECK(rd32(out) == 3);
    api("alSourceUnqueueBuffers", {source, 3, out});
    CHECK(api("alGetError") == 0);
    api("alSourcei", {source, 0x1009, a});
    api("alSourcePlay", {source});
    api("alDeleteBuffers", {1, ids});
    CHECK(api("alGetError") == 0xa004);
    api("alSourceStop", {source});
    api("alSourcei", {source, 0x1009, 0});
    api("alBufferData", {a, 0x1101, 0, 0x7fffffff, 1000});
    CHECK(api("alGetError") == 0xa003);
    api("alBufferData", {a, 0x1101, 0, 20, 1000});
    CHECK(api("alGetError") == 0);
    for (uint32_t model = 0xd001; model <= 0xd006; ++model) {
        api("alDistanceModel", {model});
        CHECK(api("alGetError") == 0);
    }
    api("alDistanceModel", {0xdead});
    CHECK(api("alGetError") == 0xa003);
    api("alGetSourcei", {source, 0x1023, out});
    CHECK(rd32(out) == INT32_MAX);
    wr32(out, f(2));
    wr32(out + 4, 0);
    wr32(out + 8, 0);
    api("alSourcefv", {source, 0x1004, out});
    api("alGetSourcefv", {source, 0x1004, out});
    CHECK(rd32(out) == f(2) && api("alGetError") == 0);
    wr32(out, 0xdeadbeef);
    api("alGetListenerf", {0x1004, out});
    CHECK(rd32(out) == 0xdeadbeef && api("alGetError") == 0xa002);
    const uint32_t second_context = api("alcCreateContext", {device, 0});
    CHECK(api("alcMakeContextCurrent", {second_context}));
    CHECK(api("alIsBuffer", {a}) == 1 && api("alIsSource", {source}) == 0);
    api("alGenSources", {1, ids + 24});
    const uint32_t second_source = rd32(ids + 24);
    api("alSourcei", {second_source, 0x1009, a});
    CHECK(api("alcMakeContextCurrent", {context}));
    api("alDeleteBuffers", {1, ids});
    CHECK(api("alGetError") == 0xa004);
    api("alcDestroyContext", {second_context});
    CHECK(api("alIsBuffer", {a}) == 1);
    wr32(out, 0);
    api("alSourceQueueBuffers", {source, 1, out});
    api("alSourcePlay", {source});
    api("alGetSourcei", {source, 0x1016, out});
    CHECK(rd32(out) == 1);
    api("alSourceUnqueueBuffers", {source, 1, out});
    CHECK(rd32(out) == 0);
    api("alGetSourcei", {source, 0x1027, out});
    CHECK(rd32(out) == 0x1029);
    api("alBufferData", {a, 0x120b, data, 12, 1000});
    wr16(data, 1000);
    wr16(data + 2, uint16_t(-1000));
    wr16(data + 4, 2000);
    api("alBufferData", {a, 0x120b, data, 12, 1000});
    api("alSourcei", {source, 0x1009, a});
    api("alSourcePlay", {source});
    CHECK(last_play.channels == 2 && pcm.size() == 4);
    int16_t left, right;
    std::memcpy(&left, pcm.data(), 2);
    std::memcpy(&right, pcm.data() + 2, 2);
    CHECK(left == 2414 && right == 414);
    api("alSourcef", {source, 0x100d, f(.9f)});
    api("alSourcef", {source, 0x100e, f(.1f)});
    CHECK(api("alGetError") == 0);
    api("alGenBuffers", {2, GUEST_SIZE - 2});
    CHECK(api("alGetError") == 0xa003);
    api("alSourceStop", {source});
    api("alSourcei", {source, 0x1009, b});
    api("alSourcef", {source, 0x100d, 0});
    api("alSourcef", {source, 0x100e, f(1)});
    api("alSourcef", {source, 0x1023, f(10)});
    api("alSourcef", {source, 0x1021, f(2)});
    api("alSource3f", {source, 0x1004, f(2), 0, 0});
    api("alSourcePlay", {source});
    const int expected[] = {-954, -954, -218, -218, -1204, -1204};
    for (uint32_t i = 0; i < 6; ++i) {
        api("alDistanceModel", {0xd001 + i});
        CHECK(std::abs(volume - expected[i]) <= 1 && pan == 10000);
    }
    api("alSource3f", {source, 0x1004, f(20), 0, 0});
    const int outside[] = {-3182, -2557, -10000, -10000, -5204, -4000};
    for (uint32_t i = 0; i < 6; ++i) {
        api("alDistanceModel", {0xd001 + i});
        CHECK(std::abs(volume - outside[i]) <= 1);
    }
    api("alSourcef", {source, 0x1021, f(.5f)});
    api("alDistanceModel", {0xd003});
    CHECK(volume == -10000);
    api("alDistanceModel", {0xd004});
    CHECK(std::abs(volume + 602) <= 1);
    api("alSource3f", {source, 0x1004, f(-2), 0, 0});
    CHECK(pan == -10000);
    api("alListener3f", {0x1004, f(-2), 0, 0});
    CHECK(pan == 0);
    const uint32_t captured_stops = stops;
    CHECK(api("alcCloseDevice", {device}) == 1);
    CHECK(!api("alcMakeContextCurrent", {context}) && stops >= captured_stops);
    CHECK(api("alcGetError", {0}) == 0xa002);
    host_clear_time_source();
    dx_reset();
    mem_shutdown();
    std::printf("OpenAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
