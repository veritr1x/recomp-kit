// Cooperative preemption and timer deadlines without an original image.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../../platform/os.h"
#include <cstdio>
#include <initializer_list>
#include <atomic>

static int checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
static uint32_t word, event;
static uint32_t foreign_checkpoints;
static std::atomic<int> active{0}, overlap{0};
static uint32_t passes;
static void handoffs(X86 *c) {
    for (int i = 0; i < 32; ++i) {
        if (active.fetch_add(1) != 0 || !sched_holds_baton())
            overlap.store(1);
        ++passes;
        active.fetch_sub(1);
        host_guest_yield();
    }
    set_eax(c, 0);
}
static void writer(X86 *c) {
    wr32(word, 1);
    set_eax(c, 0);
}
static void checkpoint() {
    if (!sched_is_guest_thread())
        ++foreign_checkpoints;
}
static void *host_poll(void *) {
    os_sleep_us(5000);
    guest_event_signal_from_host(event);
    recomp_backedge_poll();
    sched_checkpoint();
    return nullptr;
}
static uint32_t invoke(X86 *c, const char *name, std::initializer_list<uint32_t> args = {}) {
    return guest_call(c, imports_resolve("kernel32.dll", name), args.begin(), int(args.size()));
}
int main() {
    mem_init();
    imports_init();
    win32_init(".");
    X86 *c = loader_context();
    loader_init_context(c);
    sched_set_guest_thread(true);
    word = heap_alloc(16, true);
    const uint32_t entry = imports_alloc_trampoline("test", "writer", writer, 1);
    auto spawn = [&] {
        wr32(word, 0);
        return invoke(c, "CreateThread", {0, 0, entry, 0, 0, 0});
    };
    uint32_t thread = spawn();
    CHECK(thread);
    c->backedge_count = 0;
    for (int i = 0; i < 4095; ++i)
        RECOMP_BACKEDGE(c);
    CHECK(rd32(word) == 0 && c->backedge_count == 4095);
    RECOMP_BACKEDGE(c);
    CHECK(c->backedge_count == 0);
    // A new worker may checkpoint once before entering its first shim.
    // It must still run within two budget polls, with no import in this loop.
    for (int i = 0; i < 4096; ++i)
        RECOMP_BACKEDGE(c);
    CHECK(rd32(word) == 1);
    CHECK(invoke(c, "WaitForSingleObject", {thread, 100}) == 0);

    thread = spawn();
    CHECK(thread);
    c->backedge_count = 0;
    sched_atomic_enter(c->r[R_ESP]);
    for (int i = 0; i < 8192; ++i)
        RECOMP_BACKEDGE(c);
    CHECK(rd32(word) == 0);
    sched_atomic_leave();
    for (int i = 0; i < 8192; ++i)
        RECOMP_BACKEDGE(c);
    CHECK(rd32(word) == 1);
    CHECK(invoke(c, "WaitForSingleObject", {thread, 100}) == 0);

    event = invoke(c, "CreateEventA", {0, 1, 0, 0});
    thread = spawn();
    CHECK(thread);
    sched_set_checkpoint(checkpoint);
    OsThread *host = os_thread_create(host_poll, nullptr, 0);
    CHECK(host != nullptr);
    if (host)
        os_thread_join(host);
    CHECK(rd32(word) == 0 && foreign_checkpoints == 0);
    c->backedge_count = 0;
    for (int i = 0; i < 8192; ++i)
        RECOMP_BACKEDGE(c);
    CHECK(rd32(word) == 1);
    CHECK(invoke(c, "WaitForSingleObject", {event, 0}) == 0);
    CHECK(invoke(c, "WaitForSingleObject", {thread, 100}) == 0);
    sched_set_checkpoint(nullptr);

    uint32_t timer = invoke(c, "CreateWaitableTimerA", {0, 0, 0});
    CHECK(timer != 0);
    const uint32_t due = word + 8;
    wr64(due, uint64_t(int64_t(-200000))); // 20 ms, in 100 ns units
    CHECK(invoke(c, "SetWaitableTimer", {timer, due, 0, 0, 0, 0}) == 1);
    const uint64_t start = os_monotonic_ns();
    CHECK(invoke(c, "WaitForSingleObject", {timer, 0xffffffffu}) == 0);
    const uint64_t elapsed = os_monotonic_ns() - start;
    CHECK(elapsed >= 10000000 && elapsed < 250000000);
    const uint32_t many = heap_alloc(16 * 4, true);
    const uint32_t handoff_entry = imports_alloc_trampoline("test", "handoffs", handoffs, 1);
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t handle = invoke(c, "CreateThread", {0, 0, handoff_entry, 0, 0, 0});
        CHECK(handle != 0);
        wr32(many + 4 * i, handle);
    }
    CHECK(invoke(c, "WaitForMultipleObjects", {16, many, 1, 4000}) == 0);
    CHECK(passes == 16 * 32 && overlap.load() == 0 && active.load() == 0);
    sched_set_guest_thread(false);
    std::printf("scheduler: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
