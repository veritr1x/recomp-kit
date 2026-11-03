// vorbisfile.dll: Ogg Vorbis decoding on the kit's FFmpeg (the ogg demuxer
// and vorbis decoder it already builds).
//
// Unreal Engine 3 games ship Epic's build of libvorbisfile 1.2.0 and import
// seven of its entry points, all cdecl. Two differ from the public 1.2.0 API,
// as measured from a shipped DLL and its callers:
// - ov_open_callbacks(datasource, vf, initial, ibytes, callbacks, size): after
//   the by-value callbacks (read, seek, close, tell: arguments 5-8) comes a
//   ninth, sizeof(OggVorbis_File), which must be 0x2d8 or the call returns -139;
// - ov_read(vf, buffer, length, sgned, bitstream): five arguments, always
//   16-bit little-endian, signed unless sgned is 0 (then offset by 0x8000).
// Everything else follows the vorbisfile documentation: error codes, a
// stream without seek/tell is not seekable (no length, no seeking), ov_clear
// closes the datasource, a failed open leaves vf zeroed and does not.
//
// OggVorbis_File lives in guest memory and the caller owns it (often on its
// stack, never initialised before ov_open_callbacks). UE3 reads no field of
// it directly; it only hands its address back. So the decoder lives here,
// keyed by that address, and the struct gets what libvorbisfile would leave
// in its first fields: zeroed, the datasource at +0, seekable at +4, the link
// count at +0x34, the vorbis_info pointer at +0x48, the ready state at +0x58
// and the callbacks at +0x2c8. The vorbis_info (32 bytes: version, channels,
// rate, the three bitrates, the window, codec_setup) is guest heap, one per
// open file; callers read channels (+4) and rate (+8).
//
// The datasource is read through the caller's own callbacks, with guest_call,
// on the guest thread that called in: FFmpeg's I/O callbacks run inside the
// shim call, never on a host thread. Reads land in a guest buffer per file.
//
// The length a seekable stream reports is libvorbisfile's: the last page's
// granule position less the stream's start, as FFmpeg's ogg demuxer computes
// it. ov_read returns exactly that many frames: decoder output past it is
// cut and a shortfall is padded with silence, so a caller that sizes a
// buffer from ov_pcm_total and reads until it is full always finishes (UE3
// does exactly that). Either correction is counted for the tests.
//
// ov_pcm_seek_page reopens the stream and decodes forward to the position:
// exact, which is within "the page at or before pos" that the documentation
// allows, at a cost proportional to the position. UE3 seeks to 0 to loop.
//
// Chained (multi-link) files are not modelled: the shim always reports one
// link and bitstream 0, and a reopen refuses a stream whose channels or rate
// changed. Whether later links decode is up to FFmpeg's demuxer (untested).
#include "dx.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"

#ifdef RECOMP_HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string.h>
#include <vector>
#endif

namespace {
#ifdef RECOMP_HAVE_FFMPEG

constexpr int32_t OV_EREAD = -128, OV_EFAULT = -129, OV_EINVAL = -131, OV_ENOTVORBIS = -132,
                  OV_EBADHEADER = -133, OV_ENOSEEK = -138;
// Epic's DLL answers this when the caller's sizeof(OggVorbis_File) differs.
constexpr int32_t OV_EVFSIZE = -139;
constexpr uint32_t VF_SIZE = 0x2d8;
constexpr uint32_t VF_DATASOURCE = 0x00, VF_SEEKABLE = 0x04, VF_LINKS = 0x34, VF_VI = 0x48,
                   VF_READY_STATE = 0x58, VF_CALLBACKS = 0x2c8;
constexpr uint32_t READY_INITSET = 4;
constexpr uint32_t VI_SIZE = 32;
constexpr uint32_t CHUNK = 64 * 1024;
constexpr int SEEK_SET_ = 0, SEEK_END_ = 2;

struct File {
    X86 *c = nullptr; // the guest thread of the current call
    uint32_t vf = 0, ds = 0;
    uint32_t cb_read = 0, cb_seek = 0, cb_close = 0, cb_tell = 0;
    std::vector<uint8_t> initial; // bytes the caller read before opening
    bool seekable = false;
    int64_t size = -1;      // stream bytes, when seekable
    int64_t io_pos = 0;     // where FFmpeg's reads are
    int64_t guest_pos = -1; // where the datasource is; -1 when unknown
    uint32_t gbuf = 0, vi = 0;

    AVIOContext *io = nullptr;
    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    int stream = -1;
    int channels = 0, rate = 0;
    std::vector<int> plane; // FFmpeg plane of each channel, in Vorbis order
    bool flushed = false, done = false;

    std::vector<int16_t> pending; // decoded, interleaved, Vorbis channel order
    size_t pending_at = 0;        // in samples
    int64_t total = -1;           // frames; -1 when not seekable
    int64_t pos = 0;              // frames returned so far
    bool fixed_up = false;

    void close_decoder() {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        // Custom I/O survives close_input; FFmpeg may have swapped its buffer.
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        pending.clear();
        pending_at = 0;
        flushed = done = false;
        stream = -1;
    }
    ~File() {
        close_decoder();
    }
    size_t pending_frames() const {
        return channels ? (pending.size() - pending_at) / (size_t)channels : 0;
    }
};

std::map<uint32_t, std::unique_ptr<File>> g_files;
uint64_t g_fixups = 0;

File *find(uint32_t vf) {
    auto it = g_files.find(vf);
    return it == g_files.end() ? nullptr : it->second.get();
}

// The datasource, through the caller's callbacks.
bool guest_seek(File &f, int64_t offset, int whence) {
    if (!f.cb_seek)
        return false;
    uint32_t a[4] = {f.ds, (uint32_t)offset, (uint32_t)((uint64_t)offset >> 32), (uint32_t)whence};
    return (int32_t)guest_call(f.c, f.cb_seek, a, 4) != -1;
}

int io_read(void *opaque, uint8_t *buf, int size) {
    File &f = *static_cast<File *>(opaque);
    if (size <= 0)
        return 0;
    if (f.io_pos < (int64_t)f.initial.size()) {
        size_t n = std::min((size_t)size, f.initial.size() - (size_t)f.io_pos);
        memcpy(buf, f.initial.data() + f.io_pos, n);
        f.io_pos += (int64_t)n;
        return (int)n;
    }
    if (f.guest_pos != f.io_pos) {
        if (!guest_seek(f, f.io_pos, SEEK_SET_))
            return AVERROR(EIO);
        f.guest_pos = f.io_pos;
    }
    uint32_t want = std::min<uint32_t>((uint32_t)size, CHUNK);
    uint32_t a[4] = {f.gbuf, 1, want, f.ds};
    uint32_t got = guest_call(f.c, f.cb_read, a, 4);
    if (got > want) {
        f.guest_pos = -1;
        return AVERROR(EIO);
    }
    if (!got)
        return AVERROR_EOF;
    memcpy(buf, g_mem + f.gbuf, got);
    f.io_pos += got;
    f.guest_pos += got;
    return (int)got;
}

// Seeks are lazy: the datasource moves when the next read needs it to.
int64_t io_seek(void *opaque, int64_t offset, int whence) {
    File &f = *static_cast<File *>(opaque);
    if (whence & AVSEEK_SIZE)
        return f.size >= 0 ? f.size : AVERROR(ENOSYS);
    whence &= ~AVSEEK_FORCE;
    int64_t to = whence == SEEK_SET                  ? offset
                 : whence == SEEK_CUR                ? f.io_pos + offset
                 : whence == SEEK_END && f.size >= 0 ? f.size + offset
                                                     : -1;
    if (to < 0)
        return AVERROR(EINVAL);
    f.io_pos = to;
    return to;
}

// The identification header's own fields, from the codec's extradata (the
// three Vorbis headers): bitrates are not in FFmpeg's codec parameters.
void write_info(const File &f) {
    uint32_t vi = f.vi;
    for (uint32_t i = 0; i < VI_SIZE; i += 4)
        wr32(vi + i, 0);
    wr32(vi + 4, (uint32_t)f.channels);
    wr32(vi + 8, (uint32_t)f.rate);
    const AVCodecParameters *par = f.fmt->streams[f.stream]->codecpar;
    const uint8_t *x = par->extradata;
    int n = par->extradata_size;
    for (int i = 0; x && i + 30 <= n; ++i)
        if (x[i] == 1 && !memcmp(x + i + 1, "vorbis", 6)) {
            uint32_t version, br[3];
            memcpy(&version, x + i + 7, 4);
            memcpy(br, x + i + 16, 12);
            wr32(vi + 0, version);
            wr32(vi + 12, br[0]); // upper
            wr32(vi + 16, br[1]); // nominal
            wr32(vi + 20, br[2]); // lower
            return;
        }
    wr32(vi + 16, (uint32_t)par->bit_rate);
}

// Which FFmpeg plane holds each Vorbis channel. Vorbis fixes the order for
// one to eight channels (Vorbis I specification, 4.3.9); FFmpeg's decoder
// reorders to its own layout, so look each one up by name.
void map_channels(File &f) {
    static const AVChannel order[8][8] = {
        {AV_CHAN_FRONT_CENTER},
        {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT},
        {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_CENTER, AV_CHAN_FRONT_RIGHT},
        {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT},
        {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_CENTER, AV_CHAN_FRONT_RIGHT, AV_CHAN_BACK_LEFT,
         AV_CHAN_BACK_RIGHT},
        {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_CENTER, AV_CHAN_FRONT_RIGHT, AV_CHAN_BACK_LEFT,
         AV_CHAN_BACK_RIGHT, AV_CHAN_LOW_FREQUENCY},
        {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_CENTER, AV_CHAN_FRONT_RIGHT, AV_CHAN_SIDE_LEFT,
         AV_CHAN_SIDE_RIGHT, AV_CHAN_BACK_CENTER, AV_CHAN_LOW_FREQUENCY},
        {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_CENTER, AV_CHAN_FRONT_RIGHT, AV_CHAN_SIDE_LEFT,
         AV_CHAN_SIDE_RIGHT, AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT, AV_CHAN_LOW_FREQUENCY},
    };
    f.plane.resize((size_t)f.channels);
    for (int i = 0; i < f.channels; ++i)
        f.plane[(size_t)i] = i;
    if (f.channels < 3 || f.channels > 8)
        return;
    std::vector<int> mapped((size_t)f.channels);
    for (int i = 0; i < f.channels; ++i) {
        int p = av_channel_layout_index_from_channel(&f.dec->ch_layout, order[f.channels - 1][i]);
        if (p < 0 || p >= f.channels)
            return; // an unexpected layout: keep FFmpeg's order
        mapped[(size_t)i] = p;
    }
    f.plane = mapped;
}

// Opens (or reopens, from the start) the demuxer and decoder.
int32_t open_decoder(File &f) {
    f.close_decoder();
    f.io_pos = 0;
    if (f.seekable)
        f.guest_pos = -1; // reseek before the first read
    auto *buffer = static_cast<uint8_t *>(av_malloc(CHUNK));
    if (!buffer)
        return OV_EFAULT;
    f.io =
        avio_alloc_context(buffer, CHUNK, 0, &f, io_read, nullptr, f.seekable ? io_seek : nullptr);
    if (!f.io) {
        av_free(buffer);
        return OV_EFAULT;
    }
    f.fmt = avformat_alloc_context();
    if (!f.fmt)
        return OV_EFAULT;
    f.fmt->pb = f.io;
    f.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Ogg only: vorbisfile reads nothing else, and probing could take a
    // non-Ogg stream for something the kit's other demuxers know.
    if (avformat_open_input(&f.fmt, nullptr, av_find_input_format("ogg"), nullptr) < 0)
        return OV_ENOTVORBIS; // fmt is freed on failure
    for (unsigned i = 0; i < f.fmt->nb_streams; ++i) {
        if (f.stream < 0 && f.fmt->streams[i]->codecpar->codec_id == AV_CODEC_ID_VORBIS)
            f.stream = (int)i;
        else
            f.fmt->streams[i]->discard = AVDISCARD_ALL;
    }
    if (f.stream < 0)
        return OV_ENOTVORBIS;
    const AVCodecParameters *par = f.fmt->streams[f.stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_VORBIS);
    if (!codec || !(f.dec = avcodec_alloc_context3(codec)) ||
        avcodec_parameters_to_context(f.dec, par) < 0 || avcodec_open2(f.dec, codec, nullptr) < 0)
        return OV_EBADHEADER;
    int channels = f.dec->ch_layout.nb_channels, rate = f.dec->sample_rate;
    if (channels < 1 || channels > 255 || rate <= 0)
        return OV_EBADHEADER;
    if (f.channels && (channels != f.channels || rate != f.rate))
        return OV_EBADHEADER; // a reopen found another stream
    f.channels = channels;
    f.rate = rate;
    map_channels(f);
    f.pkt = av_packet_alloc();
    f.frame = av_frame_alloc();
    return f.pkt && f.frame ? 0 : OV_EFAULT;
}

void take_frame(File &f) {
    const AVFrame &fr = *f.frame;
    bool planar = fr.format == AV_SAMPLE_FMT_FLTP;
    if ((!planar && fr.format != AV_SAMPLE_FMT_FLT) || fr.ch_layout.nb_channels != f.channels) {
        log_once("vorbisfile-format", "vorbisfile: unexpected decoder output (format %d, %d ch)",
                 fr.format, fr.ch_layout.nb_channels);
        return;
    }
    if (f.pending_at == f.pending.size()) {
        f.pending.clear();
        f.pending_at = 0;
    }
    size_t at = f.pending.size();
    f.pending.resize(at + (size_t)fr.nb_samples * (size_t)f.channels);
    int16_t *out = f.pending.data() + at;
    for (int ch = 0; ch < f.channels; ++ch) {
        int p = f.plane[(size_t)ch];
        const float *src = reinterpret_cast<const float *>(fr.extended_data[planar ? p : 0]);
        size_t step = planar ? 1 : (size_t)f.channels, first = planar ? 0 : (size_t)p;
        for (int i = 0; i < fr.nb_samples; ++i) {
            // As libvorbisfile does: scale by 32768, truncate, clamp.
            float v = src[first + (size_t)i * step] * 32768.0f;
            int s = std::isnan(v) ? 0 : v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)v;
            out[(size_t)i * (size_t)f.channels + (size_t)ch] = (int16_t)s;
        }
    }
}

// Decodes until there is PCM pending or the stream is done.
void fill(File &f) {
    while (!f.done && !f.pending_frames()) {
        int rc = avcodec_receive_frame(f.dec, f.frame);
        if (rc >= 0) {
            take_frame(f);
            av_frame_unref(f.frame);
            continue;
        }
        if (rc == AVERROR_EOF) {
            f.done = true;
            break;
        }
        if (rc != AVERROR(EAGAIN)) {
            log_once("vorbisfile-decode", "vorbisfile: decoding stopped (error %d)", rc);
            f.done = true;
            break;
        }
        if (f.flushed) {
            f.done = true;
            break;
        }
        rc = av_read_frame(f.fmt, f.pkt);
        if (rc < 0) {
            if (rc != AVERROR_EOF)
                log_once("vorbisfile-read", "vorbisfile: reading stopped (error %d)", rc);
            avcodec_send_packet(f.dec, nullptr);
            f.flushed = true;
            continue;
        }
        if (f.pkt->stream_index == f.stream && avcodec_send_packet(f.dec, f.pkt) < 0)
            log_once("vorbisfile-packet", "vorbisfile: a damaged packet was skipped");
        av_packet_unref(f.pkt);
    }
}

void count_fixup(File &f, bool longer, int64_t frames) {
    if (f.fixed_up)
        return;
    f.fixed_up = true;
    ++g_fixups;
    log_once(longer ? "vorbisfile-long" : "vorbisfile-short",
             "vorbisfile: decoded %lld frames %s than the granule length %lld; %s",
             (long long)frames, longer ? "more" : "fewer", (long long)f.total,
             longer ? "cut" : "padded with silence");
}

// Up to `want` frames into guest memory at `buf` (or nowhere when buf is 0),
// as 16-bit samples. Returns the frames produced; 0 at the end.
int64_t produce(File &f, uint32_t buf, int64_t want, bool sgned) {
    if (f.total >= 0 && f.pos >= f.total)
        return 0;
    fill(f);
    int64_t avail = (int64_t)f.pending_frames();
    bool pad = false;
    if (!avail) {
        if (f.total < 0 || f.pos >= f.total)
            return 0;
        count_fixup(f, false, f.total - f.pos);
        avail = f.total - f.pos;
        pad = true;
    }
    if (f.total >= 0)
        avail = std::min(avail, f.total - f.pos);
    int64_t n = std::min(avail, want);
    size_t samples = (size_t)n * (size_t)f.channels;
    if (buf) {
        uint16_t bias = sgned ? 0 : 0x8000;
        for (size_t i = 0; i < samples; ++i) {
            int16_t s = pad ? 0 : f.pending[f.pending_at + i];
            wr16(buf + 2 * (uint32_t)i, (uint16_t)((uint16_t)s + bias));
        }
    }
    if (!pad)
        f.pending_at += samples;
    f.pos += n;
    if (f.total >= 0 && f.pos == f.total && !f.fixed_up) {
        // Anything the decoder still has is past the granule length.
        int64_t extra = 0;
        for (fill(f); f.pending_frames(); fill(f)) {
            extra += (int64_t)f.pending_frames();
            f.pending.clear();
            f.pending_at = 0;
        }
        if (extra)
            count_fixup(f, true, extra);
    }
    return n;
}

// Back to the start, then forward to `to` frames.
int32_t seek_to(File &f, int64_t to) {
    if (int32_t rc = open_decoder(f)) {
        f.close_decoder();
        f.done = true;
        return rc == OV_ENOTVORBIS ? OV_EREAD : rc;
    }
    f.pos = 0;
    f.fixed_up = false;
    while (f.pos < to)
        if (!produce(f, 0, to - f.pos, true))
            break;
    return 0;
}

void drop(uint32_t vf, bool free_guest) {
    auto it = g_files.find(vf);
    if (it == g_files.end())
        return;
    if (free_guest) {
        if (it->second->gbuf)
            heap_free(it->second->gbuf);
        if (it->second->vi)
            heap_free(it->second->vi);
    }
    g_files.erase(it);
}

// int ov_open_callbacks(void *datasource, OggVorbis_File *vf, const char *initial,
//                       long ibytes, ov_callbacks callbacks, size_t sizeof_vf)
void ov_open_callbacks(X86 *c) {
    uint32_t ds = arg(c, 0), vf = arg(c, 1), initial = arg(c, 2);
    int32_t ibytes = (int32_t)arg(c, 3);
    if (arg(c, 8) != VF_SIZE)
        return set_eax(c, (uint32_t)OV_EVFSIZE);
    if (!vf || !gm_valid(vf, VF_SIZE))
        return set_eax(c, (uint32_t)OV_EFAULT);
    drop(vf, true); // libvorbisfile would overwrite it too
    memset(g_mem + vf, 0, VF_SIZE);
    auto owned = std::make_unique<File>();
    File &f = *owned;
    f.c = c;
    f.vf = vf;
    f.ds = ds;
    f.cb_read = arg(c, 4);
    f.cb_seek = arg(c, 5);
    f.cb_close = arg(c, 6);
    f.cb_tell = arg(c, 7);
    if (!f.cb_read)
        return set_eax(c, (uint32_t)OV_EFAULT);
    if (initial && ibytes > 0) {
        if (!gm_valid(initial, (uint32_t)ibytes))
            return set_eax(c, (uint32_t)OV_EFAULT);
        f.initial.assign(g_mem + initial, g_mem + initial + ibytes);
    }
    f.guest_pos = (int64_t)f.initial.size();
    f.gbuf = heap_alloc(CHUNK, false, 16);
    f.vi = heap_alloc(VI_SIZE, true, 16);
    auto fail = [&](int32_t rc) {
        if (f.gbuf)
            heap_free(f.gbuf);
        if (f.vi)
            heap_free(f.vi);
        memset(g_mem + vf, 0, VF_SIZE);
        set_eax(c, (uint32_t)rc);
    };
    if (!f.gbuf || !f.vi)
        return fail(OV_EFAULT);
    // Seekable as libvorbisfile decides it: seek(0, SEEK_CUR) works. Its
    // length is where SEEK_END lands, by tell.
    if (f.cb_seek && f.cb_tell && guest_seek(f, 0, 1)) {
        f.seekable = true;
        if (guest_seek(f, 0, SEEK_END_)) {
            uint32_t a[1] = {ds};
            int32_t end = (int32_t)guest_call(c, f.cb_tell, a, 1);
            f.size = end >= 0 ? end : -1;
        }
        f.guest_pos = -1;
    }
    if (int32_t rc = open_decoder(f)) {
        log_once("vorbisfile-open", "vorbisfile: ov_open_callbacks failed (%d)", rc);
        return fail(rc);
    }
    if (f.seekable) {
        const AVStream *st = f.fmt->streams[f.stream];
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
            f.total = av_rescale_q(st->duration, st->time_base, AVRational{1, f.rate});
        if (f.total < 0) {
            // No granule length: count by decoding, then start again.
            f.total = -1;
            int64_t n = 0;
            while (int64_t got = produce(f, 0, INT32_MAX, true))
                n += got;
            f.total = n;
            if (int32_t rc = seek_to(f, 0))
                return fail(rc);
        }
    }
    write_info(f);
    wr32(vf + VF_DATASOURCE, ds);
    wr32(vf + VF_SEEKABLE, f.seekable ? 1 : 0);
    wr32(vf + VF_LINKS, 1);
    wr32(vf + VF_VI, f.vi);
    wr32(vf + VF_READY_STATE, READY_INITSET);
    wr32(vf + VF_CALLBACKS + 0, f.cb_read);
    wr32(vf + VF_CALLBACKS + 4, f.cb_seek);
    wr32(vf + VF_CALLBACKS + 8, f.cb_close);
    wr32(vf + VF_CALLBACKS + 12, f.cb_tell);
    g_files[vf] = std::move(owned);
    set_eax(c, 0);
}

// long ov_read(OggVorbis_File *vf, char *buffer, int length, int sgned, int *bitstream)
void ov_read(X86 *c) {
    File *f = find(arg(c, 0));
    if (!f)
        return set_eax(c, (uint32_t)OV_EINVAL);
    f->c = c;
    uint32_t buf = arg(c, 1), bitstream = arg(c, 4);
    int32_t length = (int32_t)arg(c, 2);
    if (f->total >= 0 && f->pos >= f->total)
        return set_eax(c, 0);
    fill(*f);
    if (!f->pending_frames() && (f->total < 0 || f->pos >= f->total))
        return set_eax(c, 0);
    int64_t want = length > 0 ? length / (2 * f->channels) : 0;
    if (want <= 0 || !gm_valid(buf, (uint32_t)(want * 2 * f->channels)))
        return set_eax(c, (uint32_t)OV_EINVAL);
    int64_t n = produce(*f, buf, want, arg(c, 3) != 0);
    if (bitstream && gm_valid(bitstream, 4))
        wr32(bitstream, 0); // the only link
    set_eax(c, (uint32_t)(n * 2 * f->channels));
}

// vorbis_info *ov_info(OggVorbis_File *vf, int link)
void ov_info(X86 *c) {
    File *f = find(arg(c, 0));
    int32_t link = (int32_t)arg(c, 1);
    set_eax(c, !f || (link > 0 && f->seekable) ? 0 : f->vi);
}

// ogg_int64_t ov_pcm_total(OggVorbis_File *vf, int i)
void ov_pcm_total(X86 *c) {
    File *f = find(arg(c, 0));
    int32_t link = (int32_t)arg(c, 1);
    if (!f || !f->seekable || link > 0)
        return set_eax64(c, (uint64_t)(int64_t)OV_EINVAL);
    set_eax64(c, (uint64_t)f->total);
}

// double ov_time_total(OggVorbis_File *vf, int i): in ST(0)
void ov_time_total(X86 *c) {
    File *f = find(arg(c, 0));
    int32_t link = (int32_t)arg(c, 1);
    if (!f || !f->seekable || link > 0)
        return fpush(c, (double)OV_EINVAL);
    fpush(c, (double)f->total / (double)f->rate);
}

// int ov_pcm_seek_page(OggVorbis_File *vf, ogg_int64_t pos)
void ov_pcm_seek_page(X86 *c) {
    File *f = find(arg(c, 0));
    if (!f)
        return set_eax(c, (uint32_t)OV_EINVAL);
    f->c = c;
    int64_t pos = (int64_t)((uint64_t)arg(c, 1) | (uint64_t)arg(c, 2) << 32);
    if (!f->seekable)
        return set_eax(c, (uint32_t)OV_ENOSEEK);
    if (pos < 0 || pos > f->total)
        return set_eax(c, (uint32_t)OV_EINVAL);
    set_eax(c, (uint32_t)seek_to(*f, pos));
}

// int ov_clear(OggVorbis_File *vf): closes the datasource and zeroes vf.
void ov_clear(X86 *c) {
    uint32_t vf = arg(c, 0);
    auto it = g_files.find(vf);
    if (it == g_files.end())
        return set_eax(c, 0);
    std::unique_ptr<File> f = std::move(it->second);
    g_files.erase(it);
    f->close_decoder();
    heap_free(f->gbuf);
    heap_free(f->vi);
    if (f->cb_close && f->ds) {
        uint32_t a[1] = {f->ds};
        guest_call(c, f->cb_close, a, 1);
    }
    memset(g_mem + vf, 0, VF_SIZE);
    set_eax(c, 0);
}

const ImportShim g_shims[] = {
    {"vorbisfile.dll", "ov_open_callbacks", ARGC_CDECL, ov_open_callbacks},
    {"vorbisfile.dll", "ov_read", ARGC_CDECL, ov_read},
    {"vorbisfile.dll", "ov_info", ARGC_CDECL, ov_info},
    {"vorbisfile.dll", "ov_pcm_total", ARGC_CDECL, ov_pcm_total},
    {"vorbisfile.dll", "ov_time_total", ARGC_CDECL, ov_time_total},
    {"vorbisfile.dll", "ov_pcm_seek_page", ARGC_CDECL, ov_pcm_seek_page},
    {"vorbisfile.dll", "ov_clear", ARGC_CDECL, ov_clear},
};

#endif // RECOMP_HAVE_FFMPEG
} // namespace

uint64_t vorbisfile_fixups_for_test() {
#ifdef RECOMP_HAVE_FFMPEG
    return g_fixups;
#else
    return 0;
#endif
}

void vorbisfile_reset() {
#ifdef RECOMP_HAVE_FFMPEG
    // mem_init already discarded the guest heap these records pointed into.
    g_files.clear();
#endif
}

void vorbisfile_register() {
#ifdef RECOMP_HAVE_FFMPEG
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_shims, sizeof g_shims / sizeof g_shims[0]);
#endif
}
