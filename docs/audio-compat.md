# OpenAL, Bink and vorbisfile compatibility

`OpenAL32.dll` is served by the runtime's DirectX layer. One playback device,
`Generic Software`, supports contexts, a listener, shared device buffers,
sources and queued streaming. All guest API state stays on the scheduler
baton holder; submitted PCM is copied by the host mixer.

Playback position, source state and processed-buffer counts use `host_millis`,
so they advance on a silent host and follow a pinned guest clock. Finishing
does not cut the host's remaining latency tail. Explicit replay stops the
previous voice before scheduling; pitch and spatial controls update that
voice without rescheduling. A refused stream append rebuilds the remaining
queue at the clock's current source offset. That offset can lead the audible
position by the output latency.

The six distance models and listener-relative stereo pan approximate spatial
audio. Cones, Doppler processing and HRTF are absent; their scalar API values
are retained for queries. Mono/stereo unsigned 8-bit and signed 16-bit PCM
are supported; quad/5.1/6.1/7.1 signed 16-bit PCM is folded to stereo. Queue
entries require matching rates and original formats. Each upload and joined
queue is limited to 64 MiB, including null-data uploads. Sources per context
and buffers per device are limited to 4096 each; devices and contexts to 64.
Capture devices are unavailable. Advertised extensions are
ALC_ENUMERATION_EXT, ALC_ENUMERATE_ALL_EXT and AL_EXT_OFFSET; EAX/EFX functions
do not resolve. See the [OpenAL specification](https://www.openal.org/documentation/openal-1.1-specification.pdf)
for the underlying buffer/context and queue contracts.

The Bink front end still defaults to its existing duplicated count/counter
record layout. Ports needing the SDK fields opt in at build time:

```toml
[bink]
record = "sdk"
```

The SDK view uses width/height at +0/+4, frame count/current at +8/+12,
last decoded frame at +16 and rate/divisor at +20/+24. The frame-buffer
record is 120 guest bytes: six header words and two sets of four 12-byte
planes, ordered Y, cR, cB, alpha. Registration validates every complete span
and snapshots the destinations. FFmpeg's U/V order is converted to the SDK's
cB/cR order. Visible pixels and deterministic padded rows are copied into
the selected registered set. The 1.8r SDK header is the layout reference;
other private SDK variants have not been established.

Memory-resident Bink input reads through a checked guest-address window whose
length comes from the header's file-size field plus eight. It never becomes
a native pointer stored in a guest record. The caller retains that memory
until close. File-handle playback keeps its independent host descriptor.
Seeking rewinds the demuxer and decodes forward without displaying the
intermediate frames, discarding intermediate PCM while preserving decoder
history. This costs time proportional to the target frame. Movies longer
than the 32-bit millisecond clock's range are refused.

Summary and realtime outputs cover their full 124/56-byte SDK records;
unmeasured native timing counters stay zero. Without FFmpeg, the existing
finished-record fallback remains available. Smacker is still unavailable.

## vorbisfile

Unreal Engine 3 games ship Epic's build of libvorbisfile 1.2.0. With FFmpeg
enabled, `vorbisfile.dll` serves its seven decoding entry points, all cdecl:
`ov_open_callbacks`, `ov_read`, `ov_info`, `ov_pcm_total`, `ov_time_total`,
`ov_pcm_seek_page` and `ov_clear`. Two differ from the public 1.2.0 API, as
measured from a shipped DLL and its callers:

- `ov_open_callbacks(datasource, vf, initial, ibytes, callbacks, sizeof_vf)`
  takes a ninth stack argument after the four by-value callbacks. It must be
  `0x2d8`, `sizeof(OggVorbis_File)`, or the call returns -139.
- `ov_read(vf, buffer, length, sgned, bitstream)` takes five arguments, not the
  [seven of Xiph's signature](https://xiph.org/vorbis/doc/vorbisfile/ov_read.html).
  Output is always 16-bit little-endian, scaled by 32768, truncated and
  clamped; `sgned == 0` offsets each sample by 0x8000. Only whole frames are
  returned; a length under one frame is `OV_EINVAL`.

The guest owns the 0x2d8-byte `OggVorbis_File` and its callers only pass its
address back, so decoder state stays on the host, keyed by that address. The
record is zeroed and receives libvorbisfile's fields: datasource +0, seekable
+4, link count +0x34, `vorbis_info` pointer +0x48, ready state +0x58 and the
four callbacks at +0x2c8. The 32-byte `vorbis_info` (version, channels, rate,
three bitrates, window, codec_setup) is guest heap, one per open file.

The datasource is read only through the guest's callbacks, on the guest
thread that made the call. A stream is seekable when seek and tell exist and
`seek(0, SEEK_CUR)` succeeds, libvorbisfile's own test. Its length is the
last page's granule position; `ov_read` returns exactly that many frames,
cutting decoder output past it and padding a shortfall with silence, because
UE3 sizes its buffer from `ov_pcm_total` and reads until it is full.
Unseekable streams report no length and decode whatever FFmpeg produces.
`ov_pcm_seek_page` reopens the stream and decodes forward to the exact
frame, at a cost proportional to the position. Chained (multi-link) files are
not modelled: one link and bitstream 0 are reported. Without FFmpeg the
imports stay unresolved. The encoder DLLs (`vorbisenc.dll`, `vorbis.dll`,
`ogg.dll`) are not served.
