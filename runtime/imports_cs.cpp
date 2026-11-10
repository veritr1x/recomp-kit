// imports_cs.cpp - the import command stream (see imports_cs.h).
//
// One producer (whichever guest thread holds the baton: only it dispatches imports) and one
// consumer (the worker). The queue is a byte ring: each entry is a header, the call's stack
// arguments and the copied bytes of its pointer argument. The worker runs an entry's shim on its
// own X86 whose stack is a scratch block of guest memory, laid out as the call's frame with the
// pointer argument rewritten to the copy, so the shim reads its arguments as usual.
#include "imports_cs.h"

#include "imports.h"
#include "guest.h"
#include "memory.h"
#include "../platform/ab_phase.h"
#include "../platform/os.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t RING_BYTES = 8u << 20;
constexpr uint32_t MAX_ARGS = 12;
constexpr uint32_t SCRATCH_BYTES = 64u << 10;
constexpr uint32_t COPY_AT = 64; // offset of the copied bytes in the scratch frame
constexpr uint32_t MAX_COPY = SCRATCH_BYTES - COPY_AT;

struct Header {
    void (*fn)(X86 *); // null: a wrap marker, skip to the ring's start
    uint32_t size;     // whole entry, 8-aligned
    uint8_t argc;      // stack arguments
    int8_t ptr_arg;    // argument rewritten to the copy, or -1
    uint16_t pad;
    uint32_t copy_len;
    uint32_t args[MAX_ARGS];
};

std::vector<ImportDeferSpec> &specs() {
    static auto *v = new std::vector<ImportDeferSpec>();
    return *v;
}
std::unordered_map<std::string, int> &spec_index() {
    static auto *m = new std::unordered_map<std::string, int>();
    return *m;
}

uint8_t *g_ring = nullptr;
std::atomic<uint64_t> g_head{0}; // consumed (run) bytes
std::atomic<uint64_t> g_tail{0}; // produced bytes
uint32_t g_scratch = 0;          // guest block the worker builds frames in
std::thread *g_worker = nullptr;
std::mutex g_m;
std::condition_variable g_cv;      // the worker sleeps here when the ring is empty
std::condition_variable g_idle_cv; // a drain sleeps here
std::atomic<bool> g_sleeping{false};
std::atomic<bool> g_draining{false};
void (*g_drain_hook)() = nullptr;
thread_local bool t_worker = false;

bool enabled_by_env() {
    static const bool on = [] {
        const char *e = recomp_env("CS"); // on unless RECOMP_CS=0
        return !(e && *e == '0');
    }();
    return on;
}

void run_entry(const Header &h, const uint8_t *copy) {
    // The frame: a return address the shim never uses, the arguments, the copy.
    wr32(g_scratch, 0);
    for (uint32_t i = 0; i < h.argc && i < MAX_ARGS; ++i)
        wr32(g_scratch + 4 + 4 * i, h.args[i]);
    if (h.ptr_arg >= 0 && h.copy_len) {
        if (uint8_t *dst = gm_ptr(g_scratch + COPY_AT))
            memcpy(dst, copy, h.copy_len);
        wr32(g_scratch + 4 + 4 * (uint32_t)h.ptr_arg, g_scratch + COPY_AT);
    }
    static thread_local X86 cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[R_ESP] = g_scratch;
    cpu.eflags_misc = 0x202;
    cpu.fpu_tag = 0xffff;
    h.fn(&cpu);
}

void worker_main() {
    t_worker = true;
    os_thread_prefer_performance();
    uint64_t head = g_head.load(std::memory_order_relaxed);
    for (;;) {
        uint64_t tail = g_tail.load(std::memory_order_acquire);
        if (head == tail) {
            if (g_draining.load(std::memory_order_seq_cst)) {
                std::lock_guard<std::mutex> lock(g_m);
                g_idle_cv.notify_all();
            }
            // A short spin first: a frame's calls come in bursts.
            for (int spin = 0; spin < 4000 && head == tail; ++spin)
                tail = g_tail.load(std::memory_order_acquire);
            if (head == tail) {
                std::unique_lock<std::mutex> lock(g_m);
                g_sleeping.store(true, std::memory_order_seq_cst);
                g_cv.wait(lock, [&] { return g_tail.load(std::memory_order_acquire) != head; });
                g_sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        while (head != tail) {
            const Header *h = (const Header *)(g_ring + (head % RING_BYTES));
            if (h->fn)
                run_entry(*h, (const uint8_t *)(h + 1));
            head += h->size;
            g_head.store(head, std::memory_order_seq_cst);
        }
    }
}

void start() {
    g_ring = new uint8_t[RING_BYTES];
    g_scratch = heap_alloc(SCRATCH_BYTES, true, 16);
    g_worker = new std::thread(worker_main);
    LOGW("imports: command stream on: deferrable shims run on their own thread (RECOMP_CS=0 turns "
         "it off)");
}

} // namespace

void imports_cs_register(const ImportDeferSpec *list, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        spec_index()[list[i].name] = (int)specs().size();
        specs().push_back(list[i]);
    }
}
namespace {
std::unordered_map<std::string, bool> &independent_names() {
    static auto *m = new std::unordered_map<std::string, bool>();
    return *m;
}
} // namespace
void imports_cs_register_independent(const char *const *names, size_t count) {
    for (size_t i = 0; i < count; ++i)
        independent_names()[names[i]] = true;
}
bool imports_cs_independent(const char *name) {
    return independent_names().count(name ? name : "") != 0;
}
void imports_cs_set_drain_hook(void (*fn)()) {
    g_drain_hook = fn;
}
bool imports_cs_on_worker() {
    return t_worker;
}
int imports_cs_spec_of(const char *name) {
    auto it = spec_index().find(name ? name : "");
    return it == spec_index().end() ? -1 : it->second;
}
bool imports_cs_active() {
    return enabled_by_env() && !recomp_ab::old(recomp_ab::CS);
}

bool imports_cs_defer(X86 *c, int spec, void (*fn)(X86 *), uint8_t argc) {
    if (spec < 0 || (size_t)spec >= specs().size() || !fn || argc > MAX_ARGS)
        return false;
    const ImportDeferSpec &s = specs()[(size_t)spec];
    uint32_t args[MAX_ARGS];
    for (uint32_t i = 0; i < argc; ++i)
        args[i] = arg(c, (int)i);
    if (s.handle_arg >= 0 && (s.handle_arg >= argc || args[s.handle_arg] < 0xF0000000u))
        return false;
    uint32_t copy_len = 0;
    if (s.ptr_arg >= 0) {
        if (s.ptr_arg >= argc)
            return false;
        uint64_t n = s.count_arg < 0 ? 1 : (s.count_arg < argc ? args[s.count_arg] : ~0u);
        uint64_t bytes = s.size_fn ? (uint64_t)s.size_fn(args) : n * s.unit;
        if (args[s.ptr_arg] && bytes) {
            if (bytes > MAX_COPY || !gm_valid(args[s.ptr_arg], (uint32_t)bytes))
                return false;
            copy_len = (uint32_t)bytes;
        }
    }
    if (!g_worker)
        start();
    if (!g_scratch)
        return false;
    if (s.pre && !s.pre(c))
        return false;
    if (s.deferred)
        fn = s.deferred;
    const uint32_t size = (uint32_t)((sizeof(Header) + copy_len + 7) & ~size_t(7));
    uint64_t tail = g_tail.load(std::memory_order_relaxed);
    size_t at = tail % RING_BYTES;
    const size_t wrap = RING_BYTES - at < size ? RING_BYTES - at : 0;
    // Room for the entry (and a wrap marker before it).
    while (tail + wrap + size - g_head.load(std::memory_order_acquire) > RING_BYTES)
        std::this_thread::yield();
    if (wrap) {
        Header *m = (Header *)(g_ring + at);
        m->fn = nullptr;
        m->size = (uint32_t)wrap;
        tail += wrap;
        at = 0;
    }
    Header *h = (Header *)(g_ring + at);
    h->fn = fn;
    h->size = size;
    h->argc = argc;
    h->ptr_arg = s.ptr_arg;
    h->copy_len = copy_len;
    memcpy(h->args, args, 4 * argc);
    if (copy_len)
        memcpy(h + 1, gm_ptr(args[s.ptr_arg]), copy_len);
    g_tail.store(tail + size, std::memory_order_seq_cst);
    if (g_sleeping.load(std::memory_order_seq_cst)) {
        std::lock_guard<std::mutex> lock(g_m);
        g_cv.notify_one();
    }
    return true;
}

bool imports_cs_drain() {
    if (!g_worker || t_worker)
        return false;
    const uint64_t tail = g_tail.load(std::memory_order_acquire);
    const bool waited = g_head.load(std::memory_order_acquire) != tail;
    if (waited) {
        for (int spin = 0; spin < 2000; ++spin)
            if (g_head.load(std::memory_order_acquire) == tail)
                break;
        if (g_head.load(std::memory_order_acquire) != tail) {
            g_draining.store(true, std::memory_order_seq_cst);
            std::unique_lock<std::mutex> lock(g_m);
            g_cv.notify_one();
            g_idle_cv.wait(lock, [&] { return g_head.load(std::memory_order_acquire) == tail; });
            g_draining.store(false, std::memory_order_relaxed);
        }
    }
    if (g_drain_hook)
        g_drain_hook();
    return waited;
}
