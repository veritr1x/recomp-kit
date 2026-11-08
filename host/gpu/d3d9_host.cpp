// d3d9_host.cpp - the Direct3D 9 device's host entry points (dx/host_d9.h),
// over whichever GPU backend the host's device belongs to (d3d9_backend.h).
//
// Draws, clears, blits and buffer writes are encoded on a thread of their own
// while the game carries on; everything that answers the game (a read, a query
// result) or hands a frame on (present) first lets that thread finish, then
// runs here. The draw's variable data is copied into the batch, because the
// device state it points at changes as soon as the call returns.
// RECOMP_D3D9_THREAD=0 encodes on the game thread instead.
//
// On the web the GPU belongs to the browser's main thread. Each hand-over asks
// that thread to drain the queue as soon as it is free (and its animation-frame
// loop drains it too, host_d9_pump); the game runs on a worker and waits there
// for anything it needs answered.
#include "d3d9_backend.h"

#include "../../dx/d3d9_shader.h"
#include "../../platform/ab_phase.h"
#include "../../platform/os.h"
#include "../d3d_render.h"
#include "gpu.h"

// host/present_thread.cpp; absent from builds without the presenter (weak).
__attribute__((weak)) void *host_present_stage_defer();
__attribute__((weak)) bool host_present_stage_into(void *handle, gpu::Texture src, int w, int h,
                                                   int guest_w, int guest_h, gpu::CommandBuffer cb);

namespace {
// The presenter frame the Op::Present being executed stages into (main thread only):
// set around the backend's present and taken by it (host_d9_take_stage).
void *g_stage = nullptr;
} // namespace
void *host_d9_take_stage() {
    void *h = g_stage;
    g_stage = nullptr;
    return h;
}

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace {

D9Backend *g_test_backend = nullptr;

// The backend for the host's device: the first factory that takes it.
D9Backend *make_backend(gpu::Device *device) {
    if (!device)
        return nullptr;
#ifdef __APPLE__
    if (D9Backend *b = d9_metal_create(device))
        return b;
#endif
#ifdef __EMSCRIPTEN__
    if (D9Backend *b = d9_webgpu_create(device))
        return b;
#endif
#ifdef RECOMP_D9_VULKAN
    if (D9Backend *b = d9_vulkan_create(device))
        return b;
#endif
    return nullptr;
}

#ifdef __EMSCRIPTEN__
std::atomic<D9Backend *> g_web_backend{nullptr};
#endif

D9Backend *backend() {
    if (g_test_backend)
        return g_test_backend;
#ifdef __EMSCRIPTEN__
    return g_web_backend.load(); // made on the main thread by host_d9_web_start
#endif
    static D9Backend *b = nullptr;
    static bool tried = false;
    if (tried)
        return b;
    D3DRenderer *shared = D3DRenderer::shared();
    if (!shared)
        return nullptr; // the host has not made its device yet; ask again later
    tried = true;
    if (recomp_env("D3D9_CPU"))
        return nullptr;
    b = make_backend(shared->device());
    if (b)
        fprintf(stderr, "d3d9: rendering on %s\n", b->name());
    else
        fprintf(stderr, "d3d9: no GPU renderer for this device; drawing on the CPU\n");
    return b;
}

enum class Op : uint8_t {
    Publish, // nothing to encode: lets the render thread publish query results
    ShaderCreated,
    Draw,
    Clear,
    Stretch,
    Define,
    Drop,
    BufferUpload,
    BufferDrop,
    TextureUpload,
    QueryBegin,
    QueryEnd,
    QueryDrop,
    Present, // a = back buffer, b = width, c = height (the pumped queue)
    Probe,
    Call
};

struct Command {
    Op op;
    uint64_t shader_key = 0;
    const d9sh::Program *program = nullptr; // immutable process-lifetime decode cache
    uint32_t a = 0, b = 0, c = 0, d = 0;
    size_t at = 0;       // payload offset in the batch's bytes
    size_t state_at = 0; // a draw's sampler and render states, in the bytes
    // A draw's constant registers and label in the bytes: the constants may be an earlier
    // draw's copy, and the label follows state tables only some draws append.
    size_t vconst_at = 0, pconst_at = 0, label_at = 0;
    HostD9Draw draw{};
    HostD9TextureDesc desc{};
    HostD9Target target{};
    HostD9Surface s0{}, s1{};
    int32_t r0[4] = {}, r1[4] = {};
    float z = 0;
    std::function<void()> *call = nullptr; // Op::Call: run on the render thread
    void *stage = nullptr; // Op::Present: the presenter frame it stages (host_present_stage_defer)
};

// A batch's payload: appended to for every draw (its constants and state
// tables), so it is a plain growing buffer - no zero fill, no iterator
// machinery - kept across batches by the pool.
struct ByteBuf {
    std::unique_ptr<uint8_t[]> buf;
    size_t used = 0, cap = 0;
    uint8_t *data() {
        return buf.get();
    }
    const uint8_t *data() const {
        return buf.get();
    }
    size_t size() const {
        return used;
    }
    void clear() {
        used = 0;
    }
    size_t append(const void *p, size_t n) {
        const size_t at = used;
        if (used + n > cap) {
            size_t grown = cap ? cap * 2 : 64 * 1024;
            while (grown < used + n)
                grown *= 2;
            std::unique_ptr<uint8_t[]> next(new uint8_t[grown]);
            if (used)
                memcpy(next.get(), buf.get(), used);
            buf = std::move(next);
            cap = grown;
        }
        memcpy(buf.get() + used, p, n);
        used += n;
        return at;
    }
};

struct Batch {
    std::vector<Command> commands;
    ByteBuf bytes;
    // The last draw's state tables in `bytes`, reused while the version holds.
    uint64_t state_version = 0;
    size_t state_at = 0;
    // The last copy of each constant register file in `bytes` (constants()).
    struct Consts {
        uint64_t serial = 0; // the registers' serial when copied; 0: not known to match
        size_t at = 0;
        uint32_t count = 0; // registers copied; 0: none
    } vconst, pconst;
    bool ends_present = false; // its last command is a pipelined Op::Present
    void clear() {
        commands.clear();
        bytes.clear();
        state_version = 0;
        vconst = pconst = Consts{};
        ends_present = false;
    }
    size_t put(const void *p, size_t n) {
        return n ? bytes.append(p, n) : bytes.size();
    }
};

class RenderThread {
  public:
    explicit RenderThread(D9Backend *r) : r_(r) {
        const char *env = recomp_env("D3D9_THREAD");
        threaded_ = !(env && !strcmp(env, "0"));
#ifdef __EMSCRIPTEN__
        threaded_ = true;
        pumped_ = true; // the browser's main loop drains the queue
#endif
        // Presents the pumped queue may hold before the next one waits: 2, or
        // RECOMP_D3D9_FRAMES_AHEAD (0-4; 0 waits for every frame's present).
        if (const char *ahead = recomp_env("D3D9_FRAMES_AHEAD"); ahead && *ahead)
            frames_ahead_ = (uint32_t)std::clamp(atoi(ahead), 0, 4);
        if (threaded_ && !pumped_)
            worker_ = std::thread([this] { run(); });
    }

    // ---- the game's side ----
    Batch &batch() {
        return *filling_;
    }
    void submitted() {
        if (!threaded_) {
            execute(*filling_);
            filling_->clear();
            return;
        }
        // Hand work over often enough that the worker keeps pace with the game.
        if (filling_->commands.size() >= 48 || filling_->bytes.size() >= (1u << 20))
            hand_over();
    }
    // Everything queued has been encoded; the caller may use the backend.
    template <class F> auto sync(F &&f) -> decltype(f()) {
        if (!threaded_) {
            execute(*filling_);
            filling_->clear();
            return f();
        }
        if (pumped_)
            return sync_on_pump(std::forward<F>(f));
        hand_over();
        const uint64_t t0 = os_monotonic_ns();
        std::unique_lock<std::mutex> lock(m_);
        idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
        ++stats_.syncs;
        stats_.sync_ns += os_monotonic_ns() - t0;
        return f(); // the worker waits on m_ for new work, so it cannot run now
    }

    // The pumped queue's present: the game waits only while frames_ahead_ presents are
    // still queued, not for this one, so its next frame runs while the browser's main thread
    // encodes and presents this one. Waiting for its own present (the earlier behaviour, a
    // full drain) held the game for every main-thread turn. Anything that needs an answer
    // (a read, a query in step, a test's switch of device) still drains through sync().
    void present_pipelined(uint32_t backbuffer, uint32_t w, uint32_t h) {
        {
            std::unique_lock<std::mutex> lock(m_);
            idle_cv_.wait(lock, [this] { return presents_sent_ - presents_done_ < frames_ahead_; });
            ++presents_sent_;
        }
        Command c{Op::Present};
        c.a = backbuffer;
        c.b = w;
        c.c = h;
        // The game seals this frame as soon as Present returns, before the main thread has
        // staged its image: mark it as waiting so the presenter shows no frame without its
        // picture (the screen flashed black, or showed an older image).
        c.stage = host_present_stage_defer ? host_present_stage_defer() : nullptr;
        filling_->commands.push_back(c);
        filling_->ends_present = true;
        hand_over();
    }
    // The game's side of a present, either way: RECOMP_D3D9_STATS reports where the game
    // thread waited for the renderer and what the constant bank saved, every 300 frames.
    void count_present(uint64_t wait_ns) {
        stats_.present_ns += wait_ns;
        if (++stats_.frames < 300)
            return;
        static const bool on = recomp_env("D3D9_STATS") != nullptr;
        if (on) {
            const double f = (double)stats_.frames;
            const uint64_t copies = stats_.const_copies + stats_.const_reuses;
            fprintf(stderr,
                    "d3d9 queue: per frame %.2f syncs (%.2f ms), %.2f ms waiting to present "
                    "(%u frames ahead%s), constants %.0f KiB copied, %.0f%% of draws reused "
                    "a copy\n",
                    stats_.syncs / f, stats_.sync_ns / f / 1e6, stats_.present_ns / f / 1e6,
                    pumped_ ? frames_ahead_ : 0u, pumped_ ? "" : ", in step",
                    stats_.const_bytes / f / 1024.0,
                    copies ? 100.0 * stats_.const_reuses / copies : 0.0);
        }
        stats_ = Stats{};
    }
    // Constants of one draw (host_d9_draw): their offset in the batch's bytes. The batch's
    // last copy serves again when it holds at least these registers and they have not
    // changed: the same serial, or, as the effect framework rebinds before every draw and
    // takes a new serial each time, the same bytes. Each copy was up to 4 KiB a draw.
    size_t constants(Batch::Consts &last, const float *regs, uint32_t count, uint64_t serial,
                     bool bank) {
        if (!regs || !count)
            return 0;
        Batch &b = *filling_;
        const size_t bytes = (size_t)count * 16;
        if (bank && last.count >= count) {
            if (serial && serial == last.serial) {
                ++stats_.const_reuses;
                return last.at;
            }
            if (memcmp(b.bytes.data() + last.at, regs, bytes) == 0) {
                // Only a whole match may take the serial: registers past `count` may differ.
                if (count == last.count)
                    last.serial = serial;
                ++stats_.const_reuses;
                return last.at;
            }
        }
        last.at = b.put(regs, bytes);
        last.count = count;
        last.serial = serial;
        ++stats_.const_copies;
        stats_.const_bytes += bytes;
        return last.at;
    }

    // The pumped queue: `f` runs on the pumping thread, after everything
    // queued before it, while the caller waits.
    template <class F> auto sync_on_pump(F &&f) -> decltype(f()) {
        using R = decltype(f());
        if constexpr (std::is_void<R>::value) {
            std::function<void()> job = [&] { f(); };
            run_on_pump(&job);
        } else {
            R result{};
            std::function<void()> job = [&] { result = f(); };
            run_on_pump(&job);
            return result;
        }
    }
    void run_on_pump(std::function<void()> *job) {
        Command c{Op::Call};
        c.call = job;
        filling_->commands.push_back(c);
        hand_over();
        const uint64_t t0 = os_monotonic_ns();
        std::unique_lock<std::mutex> lock(m_);
        idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
        ++stats_.syncs;
        stats_.sync_ns += os_monotonic_ns() - t0;
    }

#ifdef __EMSCRIPTEN__
    // The main thread: inside its frame or a drain, where locks may be held.
    void frame(bool inside) {
        main_depth_ += inside ? 1 : -1;
        if (!main_depth_ && pump_missed_) {
            pump_missed_ = false;
            pump();
        }
    }
    // Drains again from a later turn of the event loop, `ms` from now.
    void resume_pump(int ms) {
        if (pump_asked_.exchange(true))
            return;
        emscripten_async_call([](void *s) { ((RenderThread *)s)->asked_pump(); }, this, ms);
    }
    void asked_pump() {
        pump_asked_.store(false);
        if (main_depth_) {
            pump_missed_ = true; // drained when the frame ends
            return;
        }
        pump();
    }
#endif

    // Runs what is queued on the calling thread (pumped mode).
    void pump() {
#ifdef __EMSCRIPTEN__
        ++main_depth_;
        struct Leave {
            int &depth;
            ~Leave() {
                --depth;
            }
        } leave{main_depth_};
#endif
        std::unique_lock<std::mutex> lock(m_);
        bool presented = false;
        while (!queue_.empty()) {
#ifdef __EMSCRIPTEN__
            // At most one pipelined frame a turn: presenter targets come back with GPU
            // completions, which arrive only between turns of this thread's event loop, and
            // staging queued frames back to back starved the pool (the presenter dropped
            // about half of them). The rest waits for the next turn (resume_pump()).
            if (queue_.front()->ends_present && presented) {
                lock.unlock();
                resume_pump(0);
                lock.lock();
                break;
            }
#endif
            std::unique_ptr<Batch> b = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            const bool was_present = b->ends_present;
            execute(*b);
            b->clear();
            lock.lock();
            busy_ = false;
            presented = presented || was_present;
            if (spare_.size() < 8)
                spare_.push_back(std::move(b));
        }
        idle_cv_.notify_all();
    }

  private:
    void hand_over() {
        if (filling_->commands.empty())
            return;
        std::unique_ptr<Batch> next;
        {
            std::lock_guard<std::mutex> lock(m_);
            queue_.push_back(std::move(filling_));
            if (!spare_.empty()) {
                next = std::move(spare_.back());
                spare_.pop_back();
            }
        }
        work_cv_.notify_one();
        filling_ = next ? std::move(next) : std::make_unique<Batch>();
#ifdef __EMSCRIPTEN__
        if (pumped_ && !pump_asked_.exchange(true))
            // The proxied call only asks for a timeout. The main thread runs
            // proxied calls wherever it is blocked, including inside a browser
            // lock that drawing would take again (a queue's completion callback
            // waiting on the allocator, say); a timeout runs from the event loop
            // instead, where nothing of ours is part-way through.
            emscripten_proxy_async(
                emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                [](void *self) {
                    emscripten_async_call([](void *s) { ((RenderThread *)s)->asked_pump(); }, self,
                                          0);
                },
                this);
#endif
    }

    void run() {
        os_thread_prefer_performance();
#if defined(__APPLE__)
        pthread_setname_np("d3d9 render");
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), "d3d9 render");
#endif
        std::unique_lock<std::mutex> lock(m_);
        for (;;) {
            work_cv_.wait(lock, [this] { return !queue_.empty(); });
            std::unique_ptr<Batch> b = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            execute_pooled(*b);
            b->clear();
            lock.lock();
            busy_ = false;
            if (spare_.size() < 8)
                spare_.push_back(std::move(b));
            if (queue_.empty())
                idle_cv_.notify_all();
        }
    }
    void execute_pooled(Batch &b);

  public:
    void execute(Batch &b) {
        for (Command &cmd : b.commands) {
            const uint8_t *base = b.bytes.data();
            switch (cmd.op) {
            case Op::Publish:
                break;
            case Op::ShaderCreated:
                r_->shader_created(cmd.shader_key, *cmd.program);
                break;
            case Op::Draw: {
                HostD9Draw &d = cmd.draw;
                size_t at = cmd.at;
                auto take = [&](size_t n) -> const uint8_t * {
                    const uint8_t *p = n ? base + at : nullptr;
                    at += n;
                    return p;
                };
                d.decl = take(d.decl_size);
                d.inline_vertices = take(d.inline_bytes);
                d.inline_indices = take(d.inline_index_bytes);
                d.vconst = d.vconst_count ? (const float *)(base + cmd.vconst_at) : nullptr;
                d.pconst = d.pconst_count ? (const float *)(base + cmd.pconst_at) : nullptr;
                d.sampler_state = (const uint32_t *)(base + cmd.state_at);
                d.render_state = (const uint32_t *)(base + cmd.state_at + 16 * 14 * 4);
                d.render_state_set = base + cmd.state_at + 16 * 14 * 4 + 256 * 4;
                d.label = cmd.a ? (const char *)(base + cmd.label_at) : nullptr;
                r_->draw(d);
                break;
            }
            case Op::Clear:
                r_->clear(cmd.target, cmd.r0, cmd.a,
                          cmd.a ? (const int32_t *)(base + cmd.at) : nullptr, cmd.b, cmd.c, cmd.z,
                          cmd.d);
                break;
            case Op::Stretch:
                r_->stretch(cmd.s0, cmd.r0, cmd.s1, cmd.r1, cmd.a);
                break;
            case Op::Define:
                r_->define(cmd.desc);
                break;
            case Op::Drop:
                r_->drop(cmd.a);
                break;
            case Op::BufferUpload:
                r_->buffer_upload(cmd.a, cmd.b, cmd.c, base + cmd.at, cmd.d);
                break;
            case Op::BufferDrop:
                r_->buffer_drop(cmd.a);
                break;
            case Op::TextureUpload:
                r_->upload(cmd.a, cmd.b, cmd.c, base + cmd.at, cmd.d);
                break;
            case Op::QueryBegin:
                r_->query_begin(cmd.a);
                break;
            case Op::QueryEnd:
                r_->query_end(cmd.a);
                ending_[cmd.a] = cmd.b;
                break;
            case Op::QueryDrop:
                r_->query_drop(cmd.a);
                ending_.erase(cmd.a);
                break;
            case Op::Present: {
                g_stage = cmd.stage;
                r_->present(cmd.a, cmd.b, cmd.c);
                // A backend that staged nothing (no back buffer yet) drops the frame.
                if (void *left = host_d9_take_stage(); left && host_present_stage_into)
                    host_present_stage_into(left, {}, 0, 0, 0, 0, {});
                std::lock_guard<std::mutex> lock(m_);
                ++presents_done_;
                idle_cv_.notify_all();
                break;
            }
            case Op::Probe:
                r_->probe_next(cmd.a ? (const char *)(base + cmd.at) : nullptr);
                break;
            case Op::Call:
                (*cmd.call)();
                break;
            }
        }
        publish_queries();
    }

    // ---- occlusion queries without a round trip ----
    // A query's result used to be asked for with sync(): the game's thread
    // waited for the render thread - in a browser, the main thread, part-way
    // through its frame - on every GetData. Results are published here as the
    // backend learns them instead, each tagged with the Issue(END) it answers,
    // and GetData reads the table: an answer to an older Issue is not ready.
    uint32_t frames_ahead() const {
        return frames_ahead_;
    }
    bool pumped() const {
        return pumped_;
    }
    uint32_t query_issued(uint32_t id) {
        std::lock_guard<std::mutex> lock(qm_);
        return ++issued_[id];
    }
    int query_peek(uint32_t id, uint32_t *count) {
        {
            std::lock_guard<std::mutex> lock(qm_);
            auto i = issued_.find(id);
            if (i == issued_.end())
                return -1;
            auto p = published_.find(id);
            if (p != published_.end() && p->second.gen == i->second) {
                *count = p->second.count;
                return p->second.result;
            }
        }
        // Not ready: make sure the END is on its way rather than waiting for
        // the game's next draws to fill a batch.
        if (threaded_) {
            if (filling_->commands.empty())
                filling_->commands.push_back(Command{Op::Publish});
            hand_over();
        } else {
            publish_queries();
        }
        return 0;
    }
    void query_forget(uint32_t id) {
        std::lock_guard<std::mutex> lock(qm_);
        issued_.erase(id);
        published_.erase(id);
    }

  private:
    // The render thread's side: the ENDs encoded and not yet answered.
    void publish_queries() {
        for (auto it = ending_.begin(); it != ending_.end();) {
            uint32_t count = 0;
            const int r = r_->query_result(it->first, &count);
            if (r == 0) {
                ++it;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(qm_);
                published_[it->first] = Published{it->second, r, r > 0 ? count : 1u};
            }
            it = ending_.erase(it);
        }
    }
    struct Published {
        uint32_t gen = 0;
        int result = 0;
        uint32_t count = 0;
    };
    std::unordered_map<uint32_t, uint32_t> ending_; // render thread only
    std::mutex qm_;
    std::unordered_map<uint32_t, uint32_t> issued_;     // under qm_
    std::unordered_map<uint32_t, Published> published_; // under qm_
    D9Backend *r_;
    bool threaded_ = false;
    bool pumped_ = false;
    std::atomic<bool> pump_asked_{false}; // a main-thread pump is on its way
    int main_depth_ = 0;                  // main thread only
    bool pump_missed_ = false;            // main thread only
    std::thread worker_;
    std::mutex m_;
    std::condition_variable work_cv_, idle_cv_;
    std::deque<std::unique_ptr<Batch>> queue_;
    std::vector<std::unique_ptr<Batch>> spare_;
    std::unique_ptr<Batch> filling_ = std::make_unique<Batch>();
    bool busy_ = false;
    uint64_t presents_sent_ = 0, presents_done_ = 0; // pipelined presents, under m_
    uint32_t frames_ahead_ = 2;
    // The game's side (count_present): waits for the renderer and constant copies.
    struct Stats {
        uint32_t frames = 0;
        uint64_t syncs = 0, sync_ns = 0, present_ns = 0;
        uint64_t const_copies = 0, const_reuses = 0, const_bytes = 0;
    } stats_;
};

RenderThread *render_thread() {
    static RenderThread *t = nullptr;
    static D9Backend *for_backend = nullptr;
    D9Backend *r = backend();
    if (!r)
        return nullptr;
    if (r != for_backend) {
        // A test swapped the backend: the old thread (if any) is idle; leave it.
        for_backend = r;
        t = new RenderThread(r);
    }
    return t;
}

// The d9 caches decode a program once for everyone; decoding it here means the
// render thread only ever looks it up, so the bytes need not travel.
// Fills in any key the caller left out.
void predecode(HostD9Draw &d) {
    if (!d.vs_key)
        d.vs_key = d9sh::code_key(d.vs, d.vs ? d.vs_size : 0);
    if (!d.ps_key)
        d.ps_key = d9sh::code_key(d.ps, d.ps ? d.ps_size : 0);
    d9sh::program_for_key(d.vs_key, d.vs, d.vs ? d.vs_size : 0);
    d9sh::program_for_key(d.ps_key, d.ps, d.ps ? d.ps_size : 0);
}

void push(Op op, uint32_t a) {
    if (RenderThread *t = render_thread()) {
        Command c{op};
        c.a = a;
        t->batch().commands.push_back(c);
        t->submitted();
    }
}

} // namespace

#ifdef __APPLE__
// Objective-C objects a batch creates are released as it finishes.
void d9_autorelease_run(void (*fn)(void *), void *arg);
void RenderThread::execute_pooled(Batch &b) {
    std::pair<RenderThread *, Batch *> job(this, &b);
    d9_autorelease_run(
        [](void *p) {
            auto *j = static_cast<std::pair<RenderThread *, Batch *> *>(p);
            j->first->execute(*j->second);
        },
        &job);
}
#else
void RenderThread::execute_pooled(Batch &b) {
    execute(b);
}
#endif

int host_d9_active(void) {
    return backend() != nullptr;
}
void host_d9_shader_created(const uint8_t *bytes, uint32_t size, uint64_t key, int pixel) {
    RenderThread *t = render_thread();
    if (!t || !bytes || !size)
        return;
    const auto &program = d9sh::program_for_key(key, bytes, size);
    if (!program.ok || program.pixel != bool(pixel))
        return;
    Command c{Op::ShaderCreated};
    c.shader_key = key;
    c.program = &program;
    t->batch().commands.push_back(c);
    t->submitted();
}
void host_d9_texture_define(const HostD9TextureDesc *desc) {
    RenderThread *t = render_thread();
    if (!t || !desc)
        return;
    Command c{Op::Define};
    c.desc = *desc;
    t->batch().commands.push_back(c);
    t->submitted();
}
void host_d9_texture_drop(uint32_t id) {
    push(Op::Drop, id);
}
void host_d9_texture_upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                            uint32_t pitch) {
    // The size is the backend's to know, so this one runs in step.
    if (RenderThread *t = render_thread())
        t->sync([&] { backend()->upload(id, face, level, bytes, pitch); });
}
void host_d9_texture_upload_copy(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                                 uint32_t pitch, uint32_t size) {
    RenderThread *t = render_thread();
    if (!t || !bytes)
        return;
    Batch &b = t->batch();
    Command c{Op::TextureUpload};
    c.a = id;
    c.b = face;
    c.c = level;
    c.d = pitch;
    c.at = b.put(bytes, size);
    b.commands.push_back(c);
    t->submitted();
}
int host_d9_texture_read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes,
                         uint32_t pitch) {
    RenderThread *t = render_thread();
    return t && t->sync([&] { return backend()->read(id, face, level, bytes, pitch); }) ? 1 : 0;
}
void host_d9_buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                           uint32_t size) {
    RenderThread *t = render_thread();
    if (!t || !bytes)
        return;
    Batch &b = t->batch();
    Command c{Op::BufferUpload};
    c.a = id;
    c.b = total;
    c.c = offset;
    c.d = size;
    c.at = b.put(bytes, size);
    b.commands.push_back(c);
    t->submitted();
}
void host_d9_buffer_drop(uint32_t id) {
    push(Op::BufferDrop, id);
}
void host_d9_draw(const HostD9Draw *draw) {
    RenderThread *t = render_thread();
    if (!t || !draw)
        return;
    Batch &b = t->batch();
    Command &c = b.commands.emplace_back();
    c.op = Op::Draw;
    c.draw = *draw;
    predecode(c.draw);
    c.draw.vconst_count = draw->vconst ? draw->vconst_count : 0;
    c.draw.pconst_count = draw->pconst ? draw->pconst_count : 0;
    // TEST ONLY (RECOMP_AB_PERIOD): the earlier phase copies every draw's constants. The
    // phase is set on this (the game's) thread.
    const bool bank = !recomp_ab::old(recomp_ab::CONSTBANK);
    c.vconst_at =
        t->constants(b.vconst, draw->vconst, c.draw.vconst_count, draw->vconst_serial, bank);
    c.pconst_at =
        t->constants(b.pconst, draw->pconst, c.draw.pconst_count, draw->pconst_serial, bank);
    // The declaration and inline data stay together: the render thread takes them in turn.
    c.at = b.put(draw->decl, draw->decl ? draw->decl_size : 0);
    c.draw.decl_size = draw->decl ? draw->decl_size : 0;
    b.put(draw->inline_vertices, draw->inline_vertices ? draw->inline_bytes : 0);
    c.draw.inline_bytes = draw->inline_vertices ? draw->inline_bytes : 0;
    b.put(draw->inline_indices, draw->inline_indices ? draw->inline_index_bytes : 0);
    c.draw.inline_index_bytes = draw->inline_indices ? draw->inline_index_bytes : 0;
    static const uint32_t zero_samplers[16 * 14] = {};
    static const uint32_t zero_states[256] = {};
    static const uint8_t zero_set[256] = {};
    if (!draw->state_version || draw->state_version != b.state_version) {
        b.state_at = b.put(draw->sampler_state ? draw->sampler_state : zero_samplers, 16 * 14 * 4);
        b.put(draw->render_state ? draw->render_state : zero_states, 256 * 4);
        b.put(draw->render_state_set ? draw->render_state_set : zero_set, 256);
        b.state_version = draw->state_version;
    }
    c.state_at = b.state_at;
    if (draw->label) {
        size_t n = strlen(draw->label) + 1;
        c.label_at = b.put(draw->label, n);
        c.a = (uint32_t)n;
    }
    // Programs are found by key on the render thread.
    c.draw.vs = nullptr;
    c.draw.ps = nullptr;
    t->submitted();
}
void host_d9_clear(const HostD9Target *target, const int32_t viewport[4], uint32_t count,
                   const int32_t *rects, uint32_t flags, uint32_t color, float z,
                   uint32_t stencil) {
    RenderThread *t = render_thread();
    if (!t || !target)
        return;
    Batch &b = t->batch();
    Command c{Op::Clear};
    c.target = *target;
    if (viewport)
        memcpy(c.r0, viewport, sizeof c.r0);
    c.a = rects ? count : 0;
    c.at = b.put(rects, rects ? count * 16u : 0);
    c.b = flags;
    c.c = color;
    c.z = z;
    c.d = stencil;
    b.commands.push_back(c);
    t->submitted();
}
void host_d9_stretch(HostD9Surface src, const int32_t src_rect[4], HostD9Surface dst,
                     const int32_t dst_rect[4], uint32_t filter) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    Command c{Op::Stretch};
    c.s0 = src;
    c.s1 = dst;
    if (src_rect)
        memcpy(c.r0, src_rect, sizeof c.r0);
    if (dst_rect)
        memcpy(c.r1, dst_rect, sizeof c.r1);
    c.a = filter;
    t->batch().commands.push_back(c);
    t->submitted();
}
void host_d9_present(uint32_t backbuffer, uint32_t width, uint32_t height) {
    recomp_ab::tick(); // TEST ONLY phase of an in-run A/B; nothing unless RECOMP_AB_PERIOD is set
    RenderThread *t = render_thread();
    if (!t)
        return;
    const uint64_t t0 = os_monotonic_ns();
    // The earlier phase of an in-run A/B (PRESENT), and every host whose render thread is
    // its own, wait for this frame's present. A sync after pipelined presents waits for
    // them too, so switching phase mid-run keeps the order.
    if (t->pumped() && t->frames_ahead() && !recomp_ab::old(recomp_ab::PRESENT))
        t->present_pipelined(backbuffer, width, height);
    else
        t->sync([&] { backend()->present(backbuffer, width, height); });
    t->count_present(os_monotonic_ns() - t0);
}
int host_d9_read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h) {
    RenderThread *t = render_thread();
    return t && t->sync([&] { return backend()->read_presented(rgb, cap, w, h); }) ? 1 : 0;
}

void host_d9_query_begin(uint32_t id) {
    push(Op::QueryBegin, id);
}
void host_d9_query_end(uint32_t id) {
    if (RenderThread *t = render_thread()) {
        Command c{Op::QueryEnd};
        c.a = id;
        c.b = t->query_issued(id);
        t->batch().commands.push_back(c);
        t->submitted();
    }
}
int host_d9_query_result(uint32_t id, uint32_t *count) {
    RenderThread *t = render_thread();
    if (!t || !count)
        return -1;
    // Only the browser pays a main-thread round trip for this; elsewhere the
    // answer stays exact and in step, as it always was.
    if (!t->pumped())
        return t->sync([&] { return backend()->query_result(id, count); });
    return t->query_peek(id, count);
}
void host_d9_query_drop(uint32_t id) {
    if (RenderThread *t = render_thread())
        t->query_forget(id);
    push(Op::QueryDrop, id);
}

bool host_d9_use_device_for_test(gpu::Device *device) {
    if (g_test_backend)
        if (RenderThread *t = render_thread())
            t->sync([] {});
    delete g_test_backend;
    g_test_backend = make_backend(device);
    return g_test_backend != nullptr;
}
bool host_d9_use_backend_for_test(D9Backend *b) {
    if (g_test_backend)
        if (RenderThread *t = render_thread())
            t->sync([] {});
    delete g_test_backend;
    g_test_backend = b;
    return b != nullptr;
}
void host_d9_probe_next_frame(const char *tag) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    Batch &b = t->batch();
    Command c{Op::Probe};
    if (tag) {
        size_t n = strlen(tag) + 1;
        c.at = b.put(tag, n);
        c.a = (uint32_t)n;
    }
    b.commands.push_back(c);
    t->submitted();
}

#ifdef __EMSCRIPTEN__
bool host_d9_web_start(gpu::Device *device) {
    D9Backend *b = make_backend(device);
    g_web_backend.store(b);
    if (b)
        fprintf(stderr, "d3d9: rendering on %s\n", b->name());
    return b != nullptr;
}
void host_d9_pump(void) {
    if (!g_web_backend.load())
        return;
    if (RenderThread *t = render_thread())
        t->pump();
}
void host_d9_frame(bool inside) {
    if (!g_web_backend.load())
        return;
    if (RenderThread *t = render_thread())
        t->frame(inside);
}
#endif
