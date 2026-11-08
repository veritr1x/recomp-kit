// audio-worklet.js - the processor host/audio/web_sink.cpp plays the mixer through: it copies
// finished samples out of a ring in the WebAssembly memory. Samples are interleaved stereo; the
// capacity is a power of two. It waits for `prefill` frames before it starts, and again after
// running dry. A file of the site rather than a blob: URL, which the page's
// Content-Security-Policy (script-src 'self') does not allow.
class RecompRingPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.u32 = new Uint32Array(o.buffer);
    this.f32 = new Float32Array(o.buffer);
    this.hdr = o.header >>> 2;
    this.smp = o.samples >>> 2;
    this.mask = o.capacity - 1;
    this.prefill = o.prefill;
    this.primed = false;
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const left = out[0], right = out[1] || out[0], n = left.length;
    const u32 = this.u32, h = this.hdr;
    Atomics.add(u32, h + 4, 1);
    const w = Atomics.load(u32, h), r = Atomics.load(u32, h + 1);
    const avail = (w - r) >>> 0;
    if (Atomics.load(u32, h + 3) || (!this.primed && avail < this.prefill)) {
      left.fill(0); right.fill(0);
      return true;
    }
    this.primed = true;
    const take = Math.min(avail, n), f32 = this.f32, base = this.smp, mask = this.mask;
    let at = r & mask;
    for (let i = 0; i < take; ++i) {
      left[i] = f32[base + 2 * at];
      right[i] = f32[base + 2 * at + 1];
      at = (at + 1) & mask;
    }
    if (take < n) {
      left.fill(0, take); right.fill(0, take);
      Atomics.add(u32, h + 2, 1);
      this.primed = false;
    }
    Atomics.store(u32, h + 1, (r + take) >>> 0);
    return true;
  }
}
registerProcessor('recomp-ring-player', RecompRingPlayer);
