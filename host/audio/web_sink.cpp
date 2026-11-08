// web_sink.cpp - the mixer's output in a browser: an AudioWorklet reading a ring buffer in
// the WebAssembly memory, filled by a thread of the sink's own.
//
// SDL's Emscripten backend plays through a ScriptProcessorNode, whose callback runs on the
// page's main thread and renders the mix there. That thread also encodes every Direct3D frame
// into WebGPU and presents it, so each of its busy turns was a pull that came late: a gap or a
// repeat in the sound, and mixing time taken from the renderer. Here the browser's audio
// thread only copies finished samples out of the ring, and the mix is rendered on a worker
// thread that may block freely. Ported from austynoconnor/recomp-kit 90eb38e (MIT), which
// measured 0 underruns against 375 late pulls of 1408 for SDL's path under the same load.
//
// make_sdl_sink() returns this in a browser unless RECOMP_AUDIO_SINK=sdl. When Web Audio
// cannot start, it plays through SDL's sink instead.
#ifdef __EMSCRIPTEN__
#include "sink.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../../platform/os.h"

// Opens the AudioContext and its worklet node on the main thread; returns the context's
// sample rate, or 0. The worklet module loads asynchronously: until it has, and until the
// page has had a user gesture, the ring is not read and the sink's thread waits.
// clang-format off: the body is JavaScript, which clang-format would split ("!==" into "!= =").
EM_JS(int, recomp_web_audio_open,
      (double rate, uint32_t header, uint32_t samples, uint32_t capacity,
       uint32_t prefill),
      {
          try {
              var ctx;
              try {
                  ctx = new AudioContext({sampleRate : rate, latencyHint : 'interactive'});
              } catch (e) {
                  ctx = new AudioContext({latencyHint : 'interactive'});
              }
              Module['recompAudio'] = ctx;
              // Through the player's locateFile, which stamps the build (web/player/player.js).
              var url = Module['locateFile'] ? Module['locateFile']('audio-worklet.js', "")
                                             : 'audio-worklet.js';
              ctx.audioWorklet.addModule(url)
                  .then(function() {
                      var node = new AudioWorkletNode(ctx, 'recomp-ring-player', {
                          numberOfInputs : 0,
                          numberOfOutputs : 1,
                          outputChannelCount : [2],
                          processorOptions : {
                              buffer : (typeof wasmMemory !== 'undefined' ? wasmMemory.buffer
                                                                           : HEAPU8.buffer),
                              header : header,
                              samples : samples,
                              capacity : capacity,
                              prefill : prefill
                          }
                      });
                      node.connect(ctx.destination);
                      Module['recompAudioNode'] = node;
                  })
                  .catch(function(e) {
                      console.error('audio worklet: ' + e);
                      // Tells the sink's thread to play through SDL's sink instead (RingHeader).
                      Atomics.store(HEAPU32, (header >>> 2) + 5, 1);
                  });
              // Autoplay: start on the first gesture the page sees, or as soon as it has
              // had one.
              var resume = function() {
                  if (ctx.state === 'suspended')
                      ctx.resume();
              };
              [ 'pointerdown', 'keydown', 'touchstart', 'gamepadconnected' ].forEach(
                  function(t) { window.addEventListener(t, resume, true); });
              Module['recompAudioTimer'] = setInterval(
                  function() {
                      if (ctx.state === 'running') {
                          clearInterval(Module['recompAudioTimer']);
                          return;
                      }
                      if (!navigator.userActivation || navigator.userActivation.hasBeenActive)
                          resume();
                  },
                  250);
              resume();
              return ctx.sampleRate | 0;
          } catch (e) {
              console.error('audio: ' + e);
              return 0;
          }
      });
EM_JS(void, recomp_web_audio_close, (), {
    var ctx = Module['recompAudio'];
    if (Module['recompAudioTimer'])
        clearInterval(Module['recompAudioTimer']);
    if (ctx)
        ctx.close();
    Module['recompAudio'] = Module['recompAudioNode'] = undefined;
});
// clang-format on

namespace {

// Runs `fn(arg)` on the page's main thread, where Web Audio objects belong, and waits.
void on_main(void (*fn)(void *), void *arg) {
    if (emscripten_is_main_runtime_thread())
        fn(arg);
    else
        emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                              emscripten_main_runtime_thread_id(), fn, arg);
}

// Read by the worklet through Uint32/Float32 views of the same memory: keep the field order
// in step with web/player/audio-worklet.js's indices (u32[hdr + 0..5]).
struct alignas(16) RingHeader {
    std::atomic<uint32_t> write{0};     // frames written, ever (wraps)
    std::atomic<uint32_t> read{0};      // frames the worklet has taken, ever
    std::atomic<uint32_t> underruns{0}; // render quanta the worklet had to pad
    std::atomic<uint32_t> paused{0};    // 1: play silence, count nothing
    std::atomic<uint32_t> quanta{0};    // render quanta played
    std::atomic<uint32_t> failed{0};    // 1: the worklet module did not load (JS sets it)
};

// The processor is web/player/audio-worklet.js, served beside the player.


class WebSink final : public AudioSink {
  public:
    explicit WebSink(std::unique_ptr<AudioSink> fallback) : fallback_(std::move(fallback)) {}
    ~WebSink() override {
        stop();
    }
    bool start(double rate, std::function<void(float *, float *, uint32_t)> render) override {
        if (running_)
            return true;
        if (on_fallback_)
            return fallback_->start(rate, std::move(render));
        render_ = std::move(render);
        // The backlog kept rendered ahead of the worklet, in frames: RECOMP_AUDIO_AHEAD
        // overrides 4096 (85 ms at 48 kHz). A browser tab's workers can be descheduled for
        // tens of milliseconds at a time.
        ahead_ = 4096;
        if (const char *ahead = recomp_env("AUDIO_AHEAD"); ahead && *ahead)
            ahead_ = std::max<uint32_t>(512, uint32_t(strtoul(ahead, nullptr, 10)));
        capacity_ = 1;
        while (capacity_ < ahead_ * 2)
            capacity_ <<= 1;
        samples_.assign(size_t(capacity_) * 2, 0.0f);
        rate_ = rate;
        int opened = 0;
        struct Open {
            WebSink *self;
            int *out;
        } open{this, &opened};
        on_main(
            [](void *p) {
                auto *o = static_cast<Open *>(p);
                WebSink *s = o->self;
                *o->out = recomp_web_audio_open(
                    s->rate_, uint32_t(uintptr_t(&s->header_)),
                    uint32_t(uintptr_t(s->samples_.data())), s->capacity_, s->ahead_ / 2);
            },
            &open);
        if (opened <= 0) {
            fprintf(stderr, "[host] audio: no Web Audio output; trying SDL's\n");
            on_fallback_ = fallback_ != nullptr;
            return on_fallback_ && fallback_->start(rate, std::move(render_));
        }
        if (opened != int(rate))
            fprintf(stderr,
                    "[host] audio: the browser runs Web Audio at %d Hz, not %.0f Hz; the mix "
                    "will play at the wrong pitch\n",
                    opened, rate);
        printf("[host] audio device running: mixer %.0f Hz stereo, Web Audio worklet, %u "
               "frames ahead\n",
               rate, ahead_);
        fflush(stdout);
        running_ = true;
        producer_ = std::thread([this] { produce(); });
        return true;
    }
    void pause(bool paused) override {
        if (on_fallback_) {
            fallback_->pause(paused);
            return;
        }
        header_.paused.store(paused ? 1 : 0, std::memory_order_release);
    }
    void stop() override {
        if (on_fallback_) {
            running_ = false;
            if (producer_.joinable())
                producer_.join();
            fallback_->stop();
            return;
        }
        if (!running_)
            return;
        running_ = false;
        if (producer_.joinable())
            producer_.join();
        on_main([](void *) { recomp_web_audio_close(); }, nullptr);
        report();
    }
    bool running() const override {
        return on_fallback_ ? fallback_->running() : running_.load();
    }

  private:
    // Evidence for audio reports: underruns are worklet quanta (128 frames) that found the
    // ring short; a slow render is the mix itself.
    void report() {
        printf("[host] audio sink: %u quanta played, %u underruns, %llu renders, max render "
               "%.1f ms, max queued %u / min queued %u frames, ahead %u\n",
               header_.quanta.load(), header_.underruns.load(), (unsigned long long)renders_,
               max_render_ms_, max_queued_, min_queued_ == ~0u ? 0 : min_queued_, ahead_);
        fflush(stdout);
        max_queued_ = 0;
        min_queued_ = ~0u;
    }
    void produce() {
        std::vector<float> left, right;
        const uint32_t mask = capacity_ - 1;
        auto last_report = std::chrono::steady_clock::now();
        bool seen_read = false;
        while (running_) {
            if (header_.failed.load(std::memory_order_acquire)) {
                // The worklet never loaded: nothing reads the ring. Play through SDL's sink.
                fprintf(stderr,
                        "[host] audio: the Web Audio worklet did not load; trying SDL's\n");
                if (fallback_ && fallback_->start(rate_, render_))
                    on_fallback_ = true;
                return;
            }
            const uint32_t w = header_.write.load(std::memory_order_relaxed);
            const uint32_t r = header_.read.load(std::memory_order_acquire);
            const uint32_t queued = w - r;
            if (r)
                seen_read = true;
            if (seen_read) {
                max_queued_ = std::max(max_queued_, queued);
                min_queued_ = std::min(min_queued_, queued);
            }
            // A line every 30 s as well as at exit, so a player's console shows the numbers.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds(30)) {
                last_report = now;
                report();
            }
            if (header_.paused.load(std::memory_order_acquire) || queued >= ahead_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            // Blocks of at most 512 frames top the ring up steadily rather than in long
            // bursts behind the mixer's lock.
            const uint32_t frames = std::min<uint32_t>(ahead_ - queued, 512);
            left.assign(frames, 0.0f);
            right.assign(frames, 0.0f);
            const auto t0 = std::chrono::steady_clock::now();
            render_(left.data(), right.data(), frames);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            max_render_ms_ = std::max(max_render_ms_, ms);
            ++renders_;
            for (uint32_t i = 0; i < frames; ++i) {
                const uint32_t at = (w + i) & mask;
                samples_[size_t(at) * 2] = left[i];
                samples_[size_t(at) * 2 + 1] = right[i];
            }
            header_.write.store(w + frames, std::memory_order_release);
        }
    }

    std::unique_ptr<AudioSink> fallback_;
    std::atomic<bool> on_fallback_{false};
    RingHeader header_;
    std::vector<float> samples_;
    uint32_t capacity_ = 0, ahead_ = 4096;
    double rate_ = 0;
    std::atomic<bool> running_{false};
    std::thread producer_;
    std::function<void(float *, float *, uint32_t)> render_;
    uint64_t renders_ = 0;
    double max_render_ms_ = 0;
    uint32_t max_queued_ = 0, min_queued_ = ~0u;
};
} // namespace

std::unique_ptr<AudioSink> make_web_sink(std::unique_ptr<AudioSink> fallback) {
    return std::make_unique<WebSink>(std::move(fallback));
}
#endif
