// In-run A/B (TEST ONLY): compare CPU-side changes inside one run, so both sides see the same
// route, traffic and scene mix. Separate runs of a busy area vary by more than the changes
// measured (NFSMW: 95-117 FPS on an outskirts road, 40-56 at the lighthouse, one build).
//
// RECOMP_AB_PERIOD=<seconds>: during odd periods of the monotonic clock the switches that ask
// recomp_ab::old(which) take their earlier behaviour; during even periods, the current one. The
// phase is set once per presented frame (host_d9_present), and every 30 s the frame times of
// each side so far are printed to stderr ("recomp ab: ...").
// RECOMP_AB_SWITCHES=webgpu,consts,... picks the switches that take part (default: all).
// RECOMP_AB_FORCE_OLD=1 keeps the earlier behaviour for the whole run (image comparisons).
// Unset (every normal run): always the current behaviour; a switch costs one relaxed load.
#pragma once

#include "os.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recomp_ab {
enum : uint32_t {
    STRIDE = 1u,         // the scheduler's clock read on every import call (runtime/kernel32.cpp)
    SAMPLER = 2u,        // D3DX sampler texture lookup per CommitChanges (dx/d3dx9.cpp)
    IMPORTS = 4u,        // import dispatch bookkeeping (runtime/imports.cpp)
    PRESHADER = 16u,     // D3DX preshader input lookup (dx/d3dx9.cpp)
    CONSTS = 64u,        // incremental effect constant binding (dx/d3dx9.cpp bind_constants)
    MIRRORS = 128u,      // per-draw lookups (dx/d3d9.cpp resource mirrors, decoded programs)
    WEBGPU = 512u,       // the WebGPU renderer's skipped repeat bindings and reused uniform blocks
    FREE_RUN = 2048u,    // guest threads running beside the baton holder (game.toml free_run)
    REPS = 4096u,        // bulk REP MOVS/STOS (runtime/x86.h)
    LOCKS = 8192u,       // persistent vertex/index buffer lock staging (dx/d3d9.cpp)
    CONSTBANK = 16384u,  // shader constants copied into a draw only when they changed
    PRESENT = 32768u,    // pipelined browser Present (host/gpu/d3d9_host.cpp)
    SUBMIT = 65536u,     // buffer rewrites without a mid-frame WebGPU submit
    GROUPS = 131072u,    // dropping only the bind groups of a retired texture
    DIRECT = 262144u,    // direct COM/devirtualized calls from translated code
    PACING = 524288u,    // NFSMW frame pacing at Present (game mod)
    FFP = 1048576u,      // draws with no shaders drawn through the fixed-function pipeline
    POOL = 2097152u,     // D3DX shared parameters taken by each effect when it next draws
    FXSTATES = 8388608u, // D3DX pass and sampler states re-applied only when the device's changed
    CS = 16777216u,  // deferrable shims run on the command-stream thread (on unless RECOMP_CS=0)
    LEAF = 4194304u, // leaf imports called without the dispatcher's observers and bookkeeping
    CONSTCMP = 33554432u, // draw constants with a fresh serial copied without a byte compare
};

inline std::atomic<int> old_phase{0};

inline uint64_t period_ns() {
    static const uint64_t p = [] {
        const char *e = recomp_env("AB_PERIOD");
        long s = e ? strtol(e, nullptr, 10) : 0;
        return s > 0 ? (uint64_t)s * 1000000000ull : 0ull;
    }();
    return p;
}

inline uint32_t switches() {
    static const uint32_t m = [] {
        const char *e = recomp_env("AB_SWITCHES");
        if (!e || !*e)
            return ~0u;
        static const struct {
            const char *name;
            uint32_t bit;
        } names[] = {{"stride", STRIDE},
                     {"sampler", SAMPLER},
                     {"imports", IMPORTS},
                     {"preshader", PRESHADER},
                     {"consts", CONSTS},
                     {"mirrors", MIRRORS},
                     {"webgpu", WEBGPU},
                     {"free", FREE_RUN},
                     {"reps", REPS},
                     {"locks", LOCKS},
                     {"constbank", CONSTBANK},
                     {"present", PRESENT},
                     {"submit", SUBMIT},
                     {"groups", GROUPS},
                     {"direct", DIRECT},
                     {"pacing", PACING},
                     {"ffp", FFP},
                     {"pool", POOL},
                     {"fxstates", FXSTATES},
                     {"cs", CS},
                     {"leaf", LEAF},
                     {"constcmp", CONSTCMP}};
        uint32_t bits = 0;
        for (const auto &n : names)
            if (strstr(e, n.name))
                bits |= n.bit;
        return bits;
    }();
    return m;
}

// Once per presented frame, on the thread that presents.
inline void tick() {
    static const bool force_old = [] {
        const char *e = recomp_env("AB_FORCE_OLD");
        return e && !strcmp(e, "1");
    }();
    if (force_old) {
        old_phase.store(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t p = period_ns();
    if (!p)
        return;
    const uint64_t now = os_monotonic_ns();
    const int phase = (int)((now / p) & 1);
    // The time since the last frame belongs to the phase that frame ran in.
    static uint64_t last = 0, report = 0, ns[2] = {0, 0}, frames[2] = {0, 0};
    static int last_phase = 0;
    if (last && now - last < 1000000000ull) { // a pause or a load is not a frame
        ns[last_phase] += now - last;
        ++frames[last_phase];
    }
    last = now;
    last_phase = phase;
    if (!report)
        report = now;
    if (now - report >= 30000000000ull && frames[0] && frames[1]) {
        report = now;
        const double cur = ns[0] / 1e6 / (double)frames[0], old = ns[1] / 1e6 / (double)frames[1];
        fprintf(stderr,
                "recomp ab: current %.2f ms/frame (%.1f FPS, %llu frames), earlier %.2f ms/frame "
                "(%.1f FPS, %llu frames): %+.1f%%\n",
                cur, 1000.0 / cur, (unsigned long long)frames[0], old, 1000.0 / old,
                (unsigned long long)frames[1], (cur / old - 1.0) * 100.0);
    }
    old_phase.store(phase, std::memory_order_relaxed);
}

inline bool old(uint32_t which) {
    return old_phase.load(std::memory_order_relaxed) != 0 && (switches() & which) != 0;
}
} // namespace recomp_ab
