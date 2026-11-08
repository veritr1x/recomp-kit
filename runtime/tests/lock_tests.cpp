// lock_tests.cpp - the locked read-modify-writes in x86.h (recomp_lock_xadd/xchg/cmpxchg),
// which LOCK XADD, LOCK CMPXCHG, XCHG with memory and the Interlocked* imports use.
// What each returns and stores at every width, the plain fallback at an unaligned address,
// and that concurrent threads lose no update, which is the reason they are host atomics:
// a guest thread running free (win32.h free run) runs beside the baton holder.
// Also critsec.h: CRITICAL_SECTION ownership by compare-exchange on its OwningThread field.
#include "x86.h"
#include "critsec.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>

uint8_t *g_mem;
// Linked alone, so the test owns what the runtime would supply.
RecompDirty g_dirty[RECOMP_DIRTY_SLOTS];
uint32_t g_dirty_count;
uint32_t g_watch_base, g_watch_len; // the memory watch, never armed here
extern "C" void recomp_watch_hit(uint32_t, uint32_t, uint64_t) {}
extern "C" void recomp_null_access(uint32_t addr, int write) {
    fprintf(stderr, "null %s of %08x\n", write ? "write" : "read", addr);
    abort();
}

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

const uint32_t DATA = 0x02000000;

static void values_at_every_width() {
    wr32(DATA, 0x11223344u);
    CHECK(recomp_lock_xadd32(DATA, 0x10u) == 0x11223344u);
    CHECK(rd32(DATA) == 0x11223354u);
    CHECK(recomp_lock_xchg32(DATA, 0xcafef00du) == 0x11223354u);
    CHECK(rd32(DATA) == 0xcafef00du);
    CHECK(recomp_lock_cmpxchg32(DATA, 0xcafef00du, 7u) == 0xcafef00du); // matches: stores
    CHECK(rd32(DATA) == 7u);
    CHECK(recomp_lock_cmpxchg32(DATA, 8u, 9u) == 7u); // does not match: leaves it
    CHECK(rd32(DATA) == 7u);

    wr16(DATA + 8, 0xfffeu);
    CHECK(recomp_lock_xadd16(DATA + 8, 3u) == 0xfffeu); // wraps in 16 bits
    CHECK(rd16(DATA + 8) == 1u);
    CHECK(rd8(DATA + 10) == 0u); // and touches nothing beside it
    wr8(DATA + 12, 0xffu);
    CHECK(recomp_lock_xadd8(DATA + 12, 2u) == 0xffu);
    CHECK(rd8(DATA + 12) == 1u);
    CHECK(recomp_lock_xchg8(DATA + 12, 0x5au) == 1u);
    CHECK(rd8(DATA + 12) == 0x5au);
    CHECK(recomp_lock_cmpxchg16(DATA + 8, 1u, 0x1234u) == 1u);
    CHECK(rd16(DATA + 8) == 0x1234u);
    CHECK(recomp_lock_cmpxchg8(DATA + 12, 0x5au, 0xa5u) == 0x5au);
    CHECK(rd8(DATA + 12) == 0xa5u);
}

// No host atomic exists at an unaligned address: the same results through a plain read
// and write, and the bytes either side untouched.
static void unaligned_falls_back() {
    for (uint32_t i = 0; i < 16; ++i)
        wr8(DATA + 0x100 + i, (uint8_t)(0xa0 + i));
    const uint32_t a = DATA + 0x103;
    const uint32_t before = rd32(a);
    CHECK(recomp_lock_xadd32(a, 1u) == before);
    CHECK(rd32(a) == before + 1u);
    CHECK(recomp_lock_xchg32(a, 0x01020304u) == before + 1u);
    CHECK(recomp_lock_cmpxchg32(a, 0x01020304u, 5u) == 0x01020304u);
    CHECK(rd32(a) == 5u);
    CHECK(recomp_lock_cmpxchg32(a, 6u, 7u) == 5u); // no match: unchanged
    CHECK(rd32(a) == 5u);
    CHECK(recomp_lock_xadd16(DATA + 0x109, 0x100u) == 0xaaa9u);
    CHECK(rd8(DATA + 0x102) == 0xa2u && rd8(DATA + 0x107) == 0xa7u && rd8(DATA + 0x10b) == 0xabu);
}

// Four threads add to one word and take turns on a cmpxchg spinlock guarding another: with
// a plain read and write both lose updates; as host atomics neither does.
static void threads_lose_nothing() {
    const uint32_t counter = DATA + 0x200, lock = DATA + 0x204, guarded = DATA + 0x208;
    wr32(counter, 0);
    wr32(lock, 0);
    wr32(guarded, 0);
    const int threads = 4, rounds = 100000;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([=] {
            for (int i = 0; i < rounds; ++i) {
                recomp_lock_xadd32(counter, 1u);
                while (recomp_lock_cmpxchg32(lock, 0u, 1u) != 0u) {
                }
                wr32(guarded, rd32(guarded) + 1u);
                recomp_lock_xchg32(lock, 0u);
            }
        });
    for (std::thread &t : pool)
        t.join();
    CHECK(rd32(counter) == (uint32_t)(threads * rounds));
    CHECK(rd32(guarded) == (uint32_t)(threads * rounds));
}

// One section: taking a free one, entering again as its owner, the fields where Windows keeps
// them, another thread kept out, a non-owner's leave refused, and the last leave freeing it.
static void critsec_fields() {
    const uint32_t cs = DATA + 0x400;
    memset(g_mem + cs, 0, 24);
    wr32(cs + critsec::LOCK_COUNT, 0xffffffffu);
    CHECK(critsec::atomic_ok(cs));
    CHECK(!critsec::atomic_ok(cs + 2)); // owner word not aligned: the scheduler's path
    CHECK(!critsec::atomic_ok(0));
    CHECK(critsec::try_take(cs, 7));
    CHECK(rd32(cs + critsec::OWNER) == 7 && rd32(cs + critsec::RECURSION) == 1 &&
          rd32(cs + critsec::LOCK_COUNT) == 0);
    CHECK(critsec::try_take(cs, 7));
    CHECK(rd32(cs + critsec::RECURSION) == 2 && rd32(cs + critsec::LOCK_COUNT) == 1);
    CHECK(!critsec::try_take(cs, 9));
    CHECK(rd32(cs + critsec::OWNER) == 7 && rd32(cs + critsec::RECURSION) == 2);
    CHECK(critsec::leave(cs, 9) == critsec::Left::NotOwner);
    CHECK(critsec::leave(cs, 7) == critsec::Left::StillHeld);
    CHECK(rd32(cs + critsec::OWNER) == 7 && rd32(cs + critsec::RECURSION) == 1 &&
          rd32(cs + critsec::LOCK_COUNT) == 0);
    CHECK(critsec::leave(cs, 7) == critsec::Left::Freed);
    CHECK(rd32(cs + critsec::OWNER) == 0 && rd32(cs + critsec::RECURSION) == 0 &&
          rd32(cs + critsec::LOCK_COUNT) == 0xffffffffu);
    CHECK(critsec::try_take(cs, 9));
    CHECK(critsec::leave(cs, 9) == critsec::Left::Freed);
}

// Mutual exclusion under real contention: threads take the section (spinning on try_take, as
// the scheduler's waiters retry), re-enter it once, and do a plain read-modify-write of a guest
// counter inside. Any overlap of two owners loses increments; the recursion fields must also
// stay the owner's own.
static void critsec_excludes_threads() {
    const uint32_t cs = DATA + 0x500, counter = DATA + 0x540;
    memset(g_mem + cs, 0, 24);
    wr32(cs + critsec::LOCK_COUNT, 0xffffffffu);
    wr32(counter, 0);
    const int threads = 6, rounds = 20000;
    std::atomic<int> bad_recursion{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, tid = uint32_t(100 + t)] {
            for (int i = 0; i < rounds; ++i) {
                while (!critsec::try_take(cs, tid))
                    std::this_thread::yield();
                critsec::try_take(cs, tid); // entered again by its owner
                if (rd32(cs + critsec::RECURSION) != 2)
                    ++bad_recursion;
                wr32(counter, rd32(counter) + 1);
                critsec::leave(cs, tid);
                critsec::leave(cs, tid);
            }
        });
    for (auto &t : pool)
        t.join();
    CHECK(rd32(counter) == uint32_t(threads * rounds));
    CHECK(bad_recursion.load() == 0);
    CHECK(rd32(cs + critsec::OWNER) == 0 && rd32(cs + critsec::LOCK_COUNT) == 0xffffffffu);
}

int main() {
    g_mem = (uint8_t *)calloc(GUEST_SIZE, 1);
    if (!g_mem) {
        fprintf(stderr, "lock_tests: cannot allocate guest memory\n");
        return 2;
    }
    values_at_every_width();
    unaligned_falls_back();
    threads_lose_nothing();
    critsec_fields();
    critsec_excludes_threads();
    printf("lock_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
