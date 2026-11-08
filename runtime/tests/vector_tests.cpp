// Floating conversion edges checked against architectural integer-indefinite
// values, including exception status Unicorn does not faithfully expose; and
// the bulk REP MOVS/STOS path checked against the element loop it replaces.
#include "../x86.h"
#include "../memory.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <limits>
#include <initializer_list>

static int checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
// The element loop the bulk path stands in for, run with the same helpers.
static void rep_loop(X86 *c, uint32_t width, bool stos) {
    while (c->r[R_ECX]) {
        if (stos)
            width == 1 ? stosb(c) : width == 2 ? stosw(c) : stosd(c);
        else
            width == 1 ? movsb(c) : width == 2 ? movsw(c) : movsd(c);
        c->r[R_ECX]--;
    }
}
static void rep_translated(X86 *c, uint32_t width, bool stos) {
    if (stos)
        width == 1 ? rep_stosb(c) : width == 2 ? rep_stosw(c) : rep_stosd(c);
    else
        width == 1 ? rep_movsb(c) : width == 2 ? rep_movsw(c) : rep_movsd(c);
}

// Runs one case both ways from the same memory and registers, with a dirty range
// open over part of the window, and compares everything the guest can see.
// Returns whether the bulk path took it.
static bool rep_case(uint32_t width, bool stos, bool down, uint32_t count, int32_t dst_off,
                     int32_t src_off, uint32_t value) {
    const uint32_t window = 0x00200000u, size = 0x4000u, mid = window + size / 2;
    std::vector<uint8_t> start(size), loop_mem(size);
    uint32_t seed = (count * 2654435761u) ^ ((uint32_t)dst_off * 40503u) ^ (uint32_t)src_off;
    for (auto &b : start) {
        seed = seed * 1103515245u + 12345u;
        b = (uint8_t)(seed >> 16);
    }
    X86 init{};
    init.r[R_ECX] = count;
    init.r[R_EDI] = mid + (uint32_t)dst_off;
    init.r[R_ESI] = mid + (uint32_t)src_off;
    init.r[R_EAX] = value;
    init.eflags_df = down;
    const RecompDirty range = {mid - 40, 100, 0xffffffffu, 0};

    std::memcpy(g_mem + window, start.data(), size);
    g_dirty[0] = range;
    g_dirty_count = 1;
    X86 loop = init;
    rep_loop(&loop, width, stos);
    std::memcpy(loop_mem.data(), g_mem + window, size);
    const RecompDirty loop_dirty = g_dirty[0];

    std::memcpy(g_mem + window, start.data(), size);
    g_dirty[0] = range;
    X86 probe = init;
    const bool bulk = recomp_rep_bulk(&probe, width, stos) != 0;
    std::memcpy(g_mem + window, start.data(), size);
    g_dirty[0] = range;
    X86 fast = init;
    rep_translated(&fast, width, stos);
    g_dirty_count = 0;

    CHECK(std::memcmp(loop_mem.data(), g_mem + window, size) == 0);
    CHECK(fast.r[R_ECX] == 0 && fast.r[R_EDI] == loop.r[R_EDI] && fast.r[R_ESI] == loop.r[R_ESI]);
    CHECK(g_dirty[0].lo == loop_dirty.lo && g_dirty[0].hi == loop_dirty.hi);
    if (!bulk)
        CHECK(probe.r[R_ECX] == count); // a refusal changes nothing
    return bulk;
}

static void rep_bulk_tests() {
    mem_init();
    int taken = 0, refused = 0;
    const uint32_t counts[] = {0, 1, 7, 8, 16, 31, 32, 33, 100, 1000};
    const int32_t offsets[] = {0, 1, 2, 3, 4, 5, 8, 31, 64, 4000};
    for (uint32_t width : {1u, 2u, 4u})
        for (int stos = 0; stos < 2; ++stos)
            for (int down = 0; down < 2; ++down)
                for (uint32_t count : counts)
                    for (int32_t off : offsets)
                        for (int sign : {1, -1}) {
                            const uint32_t value = (stos && (off & 1)) ? 0x5a5a5a5au : 0x12345678u;
                            if (rep_case(width, stos, down, count, sign * off, 0, value))
                                ++taken;
                            else
                                ++refused;
                        }
    // Both kinds of case ran: the comparison above covered the bulk path and
    // the fallback, and an overlap the loop replicates was refused.
    CHECK(taken > 0 && refused > 0);
    X86 c{};
    c.r[R_ECX] = 64;
    c.r[R_ESI] = 0x00200100u;
    c.r[R_EDI] = 0x00200104u; // up, destination 4 bytes above an overlapping source
    CHECK(recomp_rep_bulk(&c, 4, 0) == 0 && c.r[R_ECX] == 64);
    c.r[R_EDI] = 0x00000100u; // the null page belongs to the loop
    CHECK(recomp_rep_bulk(&c, 4, 1) == 0);
    c.r[R_EDI] = 0x00300000u;
    CHECK(recomp_rep_bulk(&c, 4, 1) == 1 && c.r[R_ECX] == 0 && c.r[R_EDI] == 0x00300100u);
    // An armed watch reports every element's store, so the loop keeps it.
    g_watch_base = 0x00300000u;
    g_watch_len = 4;
    c.r[R_ECX] = 64;
    c.r[R_EDI] = 0x00400000u;
    CHECK(recomp_rep_bulk(&c, 4, 1) == 0 && c.r[R_ECX] == 64);
    g_watch_len = 0;
}

int main() {
    rep_bulk_tests();
    // Model an ARM-generated positive NaN independently of the host CPU.
    const float positive_nan = sse_float(0x7fc00000u);
    CHECK(sse_arithmetic_bits(0.0f, -0.0f, positive_nan) == 0xffc00000u);
    CHECK(sse_arithmetic_bits(sse_float(0x7f800000u), sse_float(0x7f800000u), positive_nan) ==
          0xffc00000u);
    CHECK(sse_arithmetic_bits(sse_float(0x7fc01234u), 1.0f, sse_float(0x7fc01234u)) == 0x7fc01234u);
    CHECK(sse_arithmetic_bits(1.0f, sse_float(0xff801234u), sse_float(0xffc01234u)) == 0xffc01234u);
    CHECK(sse_arithmetic_bits(1.0f, 2.0f, 0.5f) == 0x3f000000u);
    CHECK(sse_arithmetic_bits(-0.0f, 1.0f, -0.0f) == 0x80000000u);
    X86 c{};
    CHECK(sse_mxcsr(&c) == 0x1f80);
    const int32_t positive[] = {2, 1, 2, 1}, negative[] = {-2, -2, -1, -1};
    for (uint32_t rc = 0; rc < 4; ++rc) {
        c.mxcsr_xor = rc << 13;
        CHECK(sse_i32(&c, 1.5, 0) == uint32_t(positive[rc]));
        CHECK(sse_i32(&c, -1.5, 0) == uint32_t(negative[rc]));
        CHECK(sse_i32(&c, 1.5, 1) == 1);
        CHECK(sse_i32(&c, -1.5, 1) == uint32_t(-1));
        CHECK(sse_mxcsr(&c) == (0x1fa0u | rc << 13)); // precision/inexact
    }
    c.mxcsr_xor = 0;
    CHECK(sse_i32(&c, 2.5, 0) == 2 && sse_i32(&c, 3.5, 0) == 4);
    for (double invalid :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(), 2147483648.0, -2147483649.0}) {
        c.mxcsr_xor = 0;
        CHECK(sse_i32(&c, invalid, 1) == 0x80000000u);
        CHECK(sse_mxcsr(&c) == 0x1f81); // masked invalid
    }
    c.mxcsr_xor = 0;
    CHECK(sse_i32(&c, -2147483648.0, 1) == 0x80000000u && sse_mxcsr(&c) == 0x1f80);
    c.fpu_cw = 0x0b7f;
    c.fpu_top = 3;
    c.fpu_tag = 0xffff;
    fset(&c, 0, -2.75);
    CHECK(fisttp_i16(&c) == -2 && fisttp_i32(&c) == -2 && fisttp_i64(&c) == -2);
    CHECK(c.fpu_cw == 0x0b7f && c.fpu_top == 3);
    fset(&c, 0, std::numeric_limits<double>::quiet_NaN());
    CHECK(fisttp_i16(&c) == INT16_MIN && fisttp_i32(&c) == INT32_MIN &&
          fisttp_i64(&c) == INT64_MIN);
    CHECK(c.fpu_sw & 1);
    fset(&c, 0, 9223372036854775808.0);
    CHECK(fisttp_i64(&c) == INT64_MIN);
    c.st_exact[c.fpu_top] = 1;
    c.st_bits[c.fpu_top] = 0x7fffffffffffffffull;
    CHECK(fisttp_i64(&c) == INT64_MAX);
    std::printf("vector: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
