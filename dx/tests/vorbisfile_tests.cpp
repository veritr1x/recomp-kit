// vorbisfile.dll imports through guest callbacks over a synthetic Ogg tone.
//
// Unreal Engine 3 games ship Epic's build of libvorbisfile 1.2.0:
// ov_open_callbacks takes a ninth argument, sizeof(OggVorbis_File) == 0x2d8,
// and ov_read is (vf, buffer, length, sgned, bitstream), always 16-bit
// little-endian. The datasource here is a guest record {data, size, pos}
// behind four cdecl callbacks, reached with guest_call as a game's would be.
//
// RECOMP_TEST_OGG=host-path[,offset,length] additionally decodes a private
// Ogg Vorbis stream, optionally stored raw inside a larger package file.
#include "../dx.h"
#include "../../runtime/imports.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"
#include "../../platform/os.h"
#include "guest_abi.h"
#include "fixtures/tone_ogg.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        uint64_t va = (uint64_t)(a), vb = (uint64_t)(b);                                           \
        ++g_checks;                                                                                \
        if (va != vb) {                                                                            \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "%d: %s == %s (%llu vs %llu)\n", __LINE__, #a, #b,                \
                         (unsigned long long)va, (unsigned long long)vb);                          \
        }                                                                                          \
    } while (0)

static const char *const kEntryPoints[] = {"ov_open_callbacks", "ov_read",       "ov_info",
                                           "ov_pcm_total",      "ov_time_total", "ov_pcm_seek_page",
                                           "ov_clear"};

#ifdef RECOMP_HAVE_FFMPEG
static const int32_t OV_EFAULT = -129, OV_EINVAL = -131, OV_ENOTVORBIS = -132, OV_ENOSEEK = -138,
                     OV_EVFSIZE = -139;
static const uint32_t VF_BYTES = 0x2d8;
static int g_closes = 0;

static uint32_t ds_avail(uint32_t ds) {
    uint32_t size = rd32(ds + 4), pos = rd32(ds + 8);
    return pos < size ? size - pos : 0;
}
// size_t read(void *ptr, size_t size, size_t nmemb, void *datasource)
static void cb_read(X86 *c) {
    uint32_t ptr = arg(c, 0), size = arg(c, 1), nmemb = arg(c, 2), ds = arg(c, 3);
    uint32_t items = size ? (uint32_t)std::min<uint64_t>(nmemb, ds_avail(ds) / size) : 0;
    uint32_t n = items * size;
    if (n)
        std::memmove(gm_ptr(ptr), gm_ptr(rd32(ds) + rd32(ds + 8)), n);
    wr32(ds + 8, rd32(ds + 8) + n);
    set_eax(c, items);
}
// int seek(void *datasource, ogg_int64_t offset, int whence)
static void cb_seek(X86 *c) {
    uint32_t ds = arg(c, 0), whence = arg(c, 3);
    int64_t off = (int64_t)((uint64_t)arg(c, 1) | (uint64_t)arg(c, 2) << 32);
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)rd32(ds + 8) : (int64_t)rd32(ds + 4);
    int64_t to = base + off;
    if (whence > 2 || to < 0 || to > (int64_t)rd32(ds + 4)) {
        set_eax(c, 0xffffffffu);
        return;
    }
    wr32(ds + 8, (uint32_t)to);
    set_eax(c, 0);
}
static void cb_close(X86 *c) {
    ++g_closes;
    set_eax(c, 0);
}
static void cb_tell(X86 *c) {
    set_eax(c, rd32(arg(c, 0) + 8));
}
struct Callbacks {
    uint32_t read, seek, close, tell;
};
static Callbacks callbacks(bool seekable = true) {
    static uint32_t r = imports_alloc_trampoline("TEST", "vf_read", cb_read, ARGC_CDECL);
    static uint32_t s = imports_alloc_trampoline("TEST", "vf_seek", cb_seek, ARGC_CDECL);
    static uint32_t c = imports_alloc_trampoline("TEST", "vf_close", cb_close, ARGC_CDECL);
    static uint32_t t = imports_alloc_trampoline("TEST", "vf_tell", cb_tell, ARGC_CDECL);
    return {r, seekable ? s : 0, c, seekable ? t : 0};
}
// A guest datasource over a copy of `bytes`, positioned at `pos`.
static uint32_t datasource(const uint8_t *bytes, uint32_t n, uint32_t pos = 0) {
    uint32_t ds = heap_alloc(12 + n, true, 16);
    if (!ds)
        return 0;
    std::memcpy(gm_ptr(ds + 12), bytes, n);
    wr32(ds, ds + 12);
    wr32(ds + 4, n);
    wr32(ds + 8, pos);
    return ds;
}
static uint32_t vf(const char *name, std::initializer_list<uint32_t> args) {
    uint32_t t = imports_resolve("vorbisfile.dll", name);
    if (!t) {
        std::fprintf(stderr, "vorbisfile.dll!%s has no shim\n", name);
        ++g_failures;
        return 0;
    }
    return call_shim(t, args);
}
static uint32_t open(uint32_t ds, uint32_t file, const Callbacks &cb, uint32_t initial = 0,
                     uint32_t ibytes = 0, uint32_t size = VF_BYTES) {
    return vf("ov_open_callbacks",
              {ds, file, initial, ibytes, cb.read, cb.seek, cb.close, cb.tell, size});
}
static int64_t pcm_total(uint32_t file, int32_t link = -1) {
    g_cpu.r[R_EDX] = 0xdeadbeefu;
    uint32_t lo = vf("ov_pcm_total", {file, (uint32_t)link});
    return (int64_t)((uint64_t)lo | (uint64_t)g_cpu.r[R_EDX] << 32);
}
static double time_total(uint32_t file, int32_t link = -1) {
    uint32_t top = g_cpu.fpu_top;
    vf("ov_time_total", {file, (uint32_t)link});
    if (g_cpu.fpu_top == top) {
        std::fprintf(stderr, "ov_time_total pushed nothing onto the x87 stack\n");
        ++g_failures;
        return 0;
    }
    return fpop(&g_cpu);
}
static bool all_zero(uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (rd8(a + i))
            return false;
    return true;
}
// Decodes a whole open file with ov_read calls of `chunk` bytes into `out`.
static void read_all(uint32_t file, uint32_t buf, uint32_t chunk, std::vector<int16_t> &out,
                     uint32_t frame_bytes) {
    for (int guard = 0; guard < 1000000; ++guard) {
        uint32_t got = vf("ov_read", {file, buf, chunk, 1, 0});
        if (got == 0)
            return;
        if ((int32_t)got < 0 || got > chunk || got % frame_bytes) {
            std::fprintf(stderr, "ov_read returned %d for %u bytes\n", (int32_t)got, chunk);
            ++g_failures;
            return;
        }
        size_t at = out.size();
        out.resize(at + got / 2);
        std::memcpy(out.data() + at, gm_ptr(buf), got);
    }
    std::fprintf(stderr, "ov_read never reached the end\n");
    ++g_failures;
}
// Ogg's page checksum: CRC-32, polynomial 0x04c11db7, MSB first, zero start,
// computed with the checksum field zeroed.
static void fix_page_crc(std::vector<uint8_t> &b, size_t at) {
    size_t n = 27 + b[at + 26];
    for (size_t k = 0; k < b[at + 26]; ++k)
        n += b[at + 27 + k];
    std::memset(&b[at + 22], 0, 4);
    uint32_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint32_t)b[at + i] << 24;
        for (int k = 0; k < 8; ++k)
            crc = crc & 0x80000000u ? crc << 1 ^ 0x04c11db7u : crc << 1;
    }
    std::memcpy(&b[at + 22], &crc, 4);
}
static uint32_t crossings(const std::vector<int16_t> &pcm, int channels, int ch) {
    uint32_t n = 0;
    for (size_t i = (size_t)(channels + ch); i < pcm.size(); i += (size_t)channels)
        if ((pcm[i - (size_t)channels] < 0) != (pcm[i] < 0))
            ++n;
    return n;
}

static void test_errors() {
    cpu_reset();
    for (const char *name : kEntryPoints) {
        uint32_t t = imports_resolve("vorbisfile.dll", name);
        CHECK(t != 0 && imports_argc(t) == ARGC_CDECL);
        if (!t) {
            std::fprintf(stderr, "  vorbisfile.dll!%s is not a cdecl shim\n", name);
            return;
        }
    }
    // Only the decoding side is served; encoders and the test entry stay absent.
    CHECK(!imports_resolve("vorbisenc.dll", "vorbis_encode_init_vbr"));
    CHECK(!imports_resolve("vorbisfile.dll", "ov_test_callbacks"));

    Callbacks cb = callbacks();
    uint32_t file = heap_alloc(VF_BYTES, false, 16);
    CHECK(file != 0);
    static const uint8_t junk[] = "RIFF this is not an Ogg stream, nor Vorbis at all......";
    uint32_t ds = datasource(junk, sizeof junk);
    CHECK_EQ(open(ds, 0, cb), (uint32_t)OV_EFAULT);
    // Epic's size check comes first.
    CHECK_EQ(open(ds, file, cb, 0, 0, VF_BYTES - 4), (uint32_t)OV_EVFSIZE);
    std::memset(gm_ptr(file), 0xa5, VF_BYTES);
    g_closes = 0;
    CHECK_EQ(open(ds, file, cb), (uint32_t)OV_ENOTVORBIS);
    CHECK(all_zero(file, VF_BYTES));
    CHECK_EQ(g_closes, 0);
    // An Ogg stream that is not Vorbis: the first page with its codec magic
    // broken. The page checksum is redone, so only the codec is wrong.
    std::vector<uint8_t> broken(kToneOgg, kToneOgg + sizeof kToneOgg);
    fix_page_crc(broken, 0);
    CHECK(std::memcmp(broken.data(), kToneOgg, 58) == 0); // the checksum routine is Ogg's
    broken[29] = 'X';                                     // "\x01vorbis" -> "\x01Xorbis"
    fix_page_crc(broken, 0);
    uint32_t ds2 = datasource(broken.data(), (uint32_t)broken.size());
    CHECK_EQ(open(ds2, file, cb), (uint32_t)OV_ENOTVORBIS);
    // Everything else on a file that never opened.
    uint32_t buf = sc(0x1000);
    CHECK_EQ(vf("ov_read", {file, buf, 4096, 1, 0}), (uint32_t)OV_EINVAL);
    CHECK_EQ(vf("ov_read", {0, buf, 4096, 1, 0}), (uint32_t)OV_EINVAL);
    CHECK_EQ(pcm_total(file), OV_EINVAL);
    CHECK_EQ(pcm_total(0), OV_EINVAL);
    CHECK(time_total(file) == (double)OV_EINVAL);
    CHECK_EQ(vf("ov_pcm_seek_page", {file, 0, 0}), (uint32_t)OV_EINVAL);
    CHECK_EQ(vf("ov_info", {file, 0xffffffffu}), 0u);
    CHECK_EQ(vf("ov_clear", {0}), 0u);
    CHECK_EQ(vf("ov_clear", {file}), 0u);
    CHECK_EQ(g_closes, 0);
    heap_free(ds);
    heap_free(ds2);
    heap_free(file);
}

static void test_decode() {
    cpu_reset();
    Callbacks cb = callbacks();
    uint32_t file = heap_alloc(VF_BYTES, false, 16);
    uint32_t ds = datasource(kToneOgg, sizeof kToneOgg);
    uint32_t buf = heap_alloc(0x10000, true, 16);
    CHECK(file && ds && buf);
    if (!file || !ds || !buf)
        return;
    uint64_t fixups = vorbisfile_fixups_for_test();
    std::memset(gm_ptr(file), 0xa5, VF_BYTES);
    CHECK_EQ(open(ds, file, cb), 0u);
    CHECK_EQ(rd32(file), ds);     // the datasource, where libvorbisfile keeps it
    CHECK_EQ(rd32(file + 4), 1u); // seekable
    CHECK_EQ(rd32(file + 0x34), 1u);
    CHECK_EQ(rd32(file + 0x58), 4u); // ready state INITSET
    CHECK_EQ(rd32(file + 0x2c8), cb.read);
    CHECK_EQ(rd32(file + 0x2d4), cb.tell);
    uint32_t vi = vf("ov_info", {file, 0xffffffffu});
    CHECK(vi != 0);
    if (!vi)
        return;
    CHECK_EQ(rd32(file + 0x48), vi);
    CHECK_EQ(vf("ov_info", {file, 0}), vi);
    CHECK_EQ(vf("ov_info", {file, 1}), 0u); // one link only
    CHECK_EQ(rd32(vi + 0), 0u);             // version
    CHECK_EQ(rd32(vi + 4), 2u);             // channels
    CHECK_EQ(rd32(vi + 8), 44100u);         // rate
    // The bitrates are the identification header's (this encoder writes 0s).
    uint32_t header_bitrates[3];
    std::memcpy(header_bitrates, kToneOgg + 28 + 16, 12);
    CHECK_EQ(rd32(vi + 12), header_bitrates[0]);
    CHECK_EQ(rd32(vi + 16), header_bitrates[1]);
    CHECK_EQ(rd32(vi + 20), header_bitrates[2]);
    CHECK_EQ(pcm_total(file), 13248); // the last page's granule position
    CHECK_EQ(pcm_total(file, 0), 13248);
    CHECK_EQ(pcm_total(file, 1), OV_EINVAL);
    CHECK(std::fabs(time_total(file) - 13248.0 / 44100.0) < 1e-9);
    CHECK(time_total(file, 1) == (double)OV_EINVAL);
    // A buffer smaller than one frame is an error; an odd length is rounded
    // down to whole frames.
    CHECK_EQ(vf("ov_read", {file, buf, 3, 1, 0}), (uint32_t)OV_EINVAL);
    wr32(sc(0x40), 0xdeadbeefu);
    uint32_t first = vf("ov_read", {file, buf, 4099, 1, sc(0x40)});
    CHECK(first > 0 && first <= 4096 && first % 4 == 0);
    CHECK_EQ(rd32(sc(0x40)), 0u); // the bitstream (link) it came from
    std::vector<uint8_t> head(gm_ptr(buf), gm_ptr(buf) + first);
    std::vector<int16_t> pcm(first / 2);
    std::memcpy(pcm.data(), head.data(), first);
    read_all(file, buf, 4099, pcm, 4);
    CHECK_EQ(pcm.size(), 13248u * 2);
    CHECK_EQ(vf("ov_read", {file, buf, 4096, 1, 0}), 0u);
    CHECK_EQ(vf("ov_read", {file, buf, 4096, 1, 0}), 0u);
    int peak_l = 0, peak_r = 0;
    for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
        peak_l = std::max(peak_l, std::abs((int)pcm[i]));
        peak_r = std::max(peak_r, std::abs((int)pcm[i + 1]));
    }
    CHECK(peak_l > 1000 && peak_r > 1000);
    // 440 Hz left and 660 Hz right over 0.3 s: about 264 and 396 sign changes.
    uint32_t xl = crossings(pcm, 2, 0), xr = crossings(pcm, 2, 1);
    CHECK(xl > 240 && xl < 290);
    CHECK(xr > 370 && xr < 420);
    if (!(xl > 240 && xl < 290) || !(xr > 370 && xr < 420))
        std::fprintf(stderr, "  crossings: left %u right %u\n", xl, xr);
    // Back to the start: the same samples again.
    CHECK_EQ(vf("ov_pcm_seek_page", {file, 0, 0}), 0u);
    uint32_t again = vf("ov_read", {file, buf, first, 1, 0});
    CHECK_EQ(again, first);
    CHECK(std::memcmp(gm_ptr(buf), head.data(), first) == 0);
    std::vector<int16_t> rest(again / 2);
    std::memcpy(rest.data(), gm_ptr(buf), again);
    read_all(file, buf, 8192, rest, 4);
    CHECK(rest == pcm);
    // Unsigned output is the signed samples offset by 0x8000.
    CHECK_EQ(vf("ov_pcm_seek_page", {file, 0, 0}), 0u);
    uint32_t u = vf("ov_read", {file, buf, first, 0, 0});
    CHECK_EQ(u, first);
    bool offset_ok = u == first;
    for (uint32_t i = 0; offset_ok && i < first / 2; ++i)
        offset_ok = rd16(buf + 2 * i) == (uint16_t)(pcm[i] + 0x8000);
    CHECK(offset_ok);
    // A seek into the middle lands at or before the position asked for and
    // decodes the rest of the stream from there.
    CHECK_EQ(vf("ov_pcm_seek_page", {file, 6000, 0}), 0u);
    std::vector<int16_t> tail;
    read_all(file, buf, 4096, tail, 4);
    CHECK(tail.size() >= (13248u - 6000u) * 2 && tail.size() <= pcm.size());
    CHECK(tail.size() <= pcm.size() &&
          std::equal(tail.end() - std::min(tail.size(), (size_t)1024), tail.end(),
                     pcm.end() - std::min(tail.size(), (size_t)1024)));
    CHECK_EQ(vf("ov_pcm_seek_page", {file, 0xffffffffu, 0xffffffffu}), (uint32_t)OV_EINVAL);
    CHECK_EQ(vf("ov_pcm_seek_page", {file, 13249, 0}), (uint32_t)OV_EINVAL);
    // FFmpeg's encoder ends this stream 64 samples into its last packet
    // (granule 13248, that packet starting at 13312), which FFmpeg's demuxer
    // does not trim; vorbisfile stops at the granule, and so does the shim.
    CHECK(vorbisfile_fixups_for_test() > fixups);
    // ov_clear closes the datasource once and forgets the file.
    g_closes = 0;
    CHECK_EQ(vf("ov_clear", {file}), 0u);
    CHECK_EQ(g_closes, 1);
    CHECK(all_zero(file, VF_BYTES));
    CHECK_EQ(vf("ov_read", {file, buf, 4096, 1, 0}), (uint32_t)OV_EINVAL);
    CHECK_EQ(vf("ov_clear", {file}), 0u);
    CHECK_EQ(g_closes, 1);

    // Bytes the caller already read go first (initial/ibytes); the datasource
    // stands just past them.
    uint32_t ds2 = datasource(kToneOgg, sizeof kToneOgg, 100);
    uint32_t initial = heap_alloc(100, false, 16);
    std::memcpy(gm_ptr(initial), kToneOgg, 100);
    CHECK_EQ(open(ds2, file, cb, initial, 100), 0u);
    std::vector<int16_t> pcm2;
    read_all(file, buf, 4096, pcm2, 4);
    CHECK(pcm2 == pcm);
    CHECK_EQ(vf("ov_clear", {file}), 0u);

    // Without seek and tell the stream is not seekable: it still decodes, but
    // its length is unknown and it cannot seek (libvorbisfile's answers).
    uint32_t ds3 = datasource(kToneOgg, sizeof kToneOgg);
    CHECK_EQ(open(ds3, file, callbacks(false)), 0u);
    CHECK_EQ(rd32(file + 4), 0u);
    CHECK_EQ(pcm_total(file), OV_EINVAL);
    CHECK_EQ(vf("ov_pcm_seek_page", {file, 0, 0}), (uint32_t)OV_ENOSEEK);
    uint32_t vi3 = vf("ov_info", {file, 0xffffffffu});
    CHECK(vi3 != 0 && rd32(vi3 + 4) == 2u);
    std::vector<int16_t> pcm3;
    read_all(file, buf, 4096, pcm3, 4);
    // Unseekable, there is no granule length to cut to: FFmpeg's output as is.
    CHECK(pcm3.size() >= pcm.size() && std::equal(pcm.begin(), pcm.end(), pcm3.begin()));
    CHECK_EQ(vf("ov_clear", {file}), 0u);

    // Opening over a file that was never cleared replaces it.
    CHECK_EQ(open(ds, file, cb), 0u);
    CHECK_EQ(open(ds, file, cb), 0u);
    CHECK_EQ(pcm_total(file), 13248);
    CHECK_EQ(vf("ov_clear", {file}), 0u);
    heap_free(initial);
    heap_free(ds);
    heap_free(ds2);
    heap_free(ds3);
    heap_free(buf);
    heap_free(file);
}

// A private stream, read-only: RECOMP_TEST_OGG=host-path[,offset,length].
// Offset and length select an Ogg stream stored raw inside a larger file.
static void test_private_stream(const char *spec) {
    std::string s = spec, path = s;
    unsigned long long off = 0, len = 0;
    size_t c2 = s.rfind(','),
           c1 = c2 == std::string::npos || !c2 ? std::string::npos : s.rfind(',', c2 - 1);
    std::ifstream in;
    if (c1 != std::string::npos) {
        path = s.substr(0, c1);
        char *end1 = nullptr, *end2 = nullptr;
        off = std::strtoull(s.c_str() + c1 + 1, &end1, 10);
        len = std::strtoull(s.c_str() + c2 + 1, &end2, 10);
        CHECK(*end1 == ',' && !*end2);
    }
    in.open(path, std::ios::binary);
    CHECK(in.good());
    if (!in.good())
        return;
    if (!len) {
        in.seekg(0, std::ios::end);
        len = (unsigned long long)in.tellg();
    }
    CHECK(len > 0 && len < (64u << 20));
    if (!len || len >= (64u << 20))
        return;
    std::vector<uint8_t> bytes((size_t)len);
    in.seekg((std::streamoff)off, std::ios::beg);
    bool read_ok = (bool)in.read(reinterpret_cast<char *>(bytes.data()), (std::streamsize)len);
    CHECK(read_ok);
    if (!read_ok)
        return;
    // The expected length the way libvorbisfile measures it: the last page's
    // granule position (the stream starts at 0).
    int64_t last_granule = -1;
    for (size_t j = 0; j + 27 <= bytes.size() && !std::memcmp(&bytes[j], "OggS", 4);) {
        int64_t g;
        std::memcpy(&g, &bytes[j + 6], 8);
        if (g >= 0)
            last_granule = g;
        size_t n = 27 + bytes[j + 26];
        for (size_t k = 0; k < bytes[j + 26] && j + 27 + k < bytes.size(); ++k)
            n += bytes[j + 27 + k];
        j += n;
    }
    cpu_reset();
    uint32_t file = heap_alloc(VF_BYTES, false, 16);
    uint32_t ds = datasource(bytes.data(), (uint32_t)bytes.size());
    uint32_t buf = heap_alloc(0x10000, true, 16);
    CHECK(file && ds && buf);
    if (!file || !ds || !buf)
        return;
    uint64_t fixups = vorbisfile_fixups_for_test();
    CHECK_EQ(open(ds, file, callbacks()), 0u);
    uint32_t vi = vf("ov_info", {file, 0xffffffffu});
    uint32_t channels = vi ? rd32(vi + 4) : 0, rate = vi ? rd32(vi + 8) : 0;
    CHECK(channels >= 1 && channels <= 8 && rate >= 8000 && rate <= 192000);
    int64_t total = pcm_total(file);
    CHECK_EQ(total, last_granule);
    std::vector<int16_t> pcm;
    if (channels)
        read_all(file, buf, 0x8000, pcm, 2 * channels);
    CHECK_EQ(pcm.size(), (uint64_t)total * channels);
    CHECK(std::any_of(pcm.begin(), pcm.end(), [](int16_t v) { return v != 0; }));
    // Whether FFmpeg's own count agreed (a stream whose only audio page is
    // also its last can come out short and is padded): reported, not checked,
    // since the length the caller sees is right either way.
    std::printf("vorbisfile private stream: %u ch, %u Hz, %lld frames, %.2f s, %s\n", channels,
                rate, (long long)total, channels && rate ? (double)total / rate : 0.0,
                vorbisfile_fixups_for_test() == fixups ? "decoded length exact"
                                                       : "length corrected");
    CHECK_EQ(vf("ov_clear", {file}), 0u);
    heap_free(ds);
    heap_free(buf);
    heap_free(file);
}
#endif // RECOMP_HAVE_FFMPEG

int main() {
    mem_init();
    imports_init();
    win32_init(".");
    dx_register_shims();
    dx_reset();
    g_stack_top = STACK_TOP - 4096;
    cpu_reset();
    g_scratch = heap_alloc(0x4000, true, 16);
    if (!g_scratch) {
        std::fprintf(stderr, "cannot allocate scratch\n");
        return 1;
    }
#ifdef RECOMP_HAVE_FFMPEG
    if (const char *spec = recomp_env("TEST_OGG"); spec && *spec)
        test_private_stream(spec);
    else
        std::printf("vorbisfile private stream: RECOMP_TEST_OGG unset, skipped\n");
    test_errors();
    test_decode();
#else
    // Without FFmpeg nothing registers: the imports must stay unresolved.
    for (const char *name : kEntryPoints)
        CHECK(!imports_resolve("vorbisfile.dll", name));
#endif
    dx_reset();
    mem_shutdown();
    std::printf("vorbisfile: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
