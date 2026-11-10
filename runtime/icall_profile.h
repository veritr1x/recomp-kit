/* icall_profile.h - the targets each indirect CALL site reaches in a run.
 *
 * Generated code includes this when it was translated with the speedup
 * icall_profile_record (game.toml [translate.speedups]): every indirect CALL
 * then calls recomp_icall_note(site, target) before it dispatches, and the
 * generated table.c defines the table below. At exit the table is written to
 * RECOMP_ICALL_PROFILE (default icall-profile.txt in the working directory)
 * as `site target count` lines, which [translate] icall_profile hands back
 * to the translator: under the devirt speedup a site that reached at most
 * four translated functions calls them directly (translate.py
 * Translator.indirect_call). Recording is for a profiling build only; a
 * normal build carries none of it.
 *
 * Threads running free (win32.h) call it without the baton, so a slot is
 * claimed with a compare-exchange and counted with an atomic add. A full
 * table drops the pairs it has no room for. */
#ifndef RECOMP_ICALL_PROFILE_H
#define RECOMP_ICALL_PROFILE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_ICALL_SLOTS (1u << 16) /* a power of two */

typedef struct RecompIcallSlot {
    uint64_t key; /* site << 32 | target; 0: free */
    uint32_t count;
} RecompIcallSlot;

/* Defined in the generated table.c of a recording build. */
extern RecompIcallSlot recomp_icall_slots[RECOMP_ICALL_SLOTS];
extern uint32_t recomp_icall_armed;
/* For RECOMP_ICALL_PROFILE_SECS: calls noted, when the first was, and whether
 * the table has been logged. */
extern uint32_t recomp_icall_notes;
extern int64_t recomp_icall_start;
extern uint32_t recomp_icall_logged;

/* The table as `site target count` lines; each line starts with `prefix`. */
static inline void recomp_icall_print(FILE *f, const char *prefix) {
    uint32_t i;
    fprintf(f,
            "%s# indirect CALL site, target, calls (tools/recomp/translate.py "
            "[translate] icall_profile)\n",
            prefix);
    for (i = 0; i < RECOMP_ICALL_SLOTS; ++i) {
        uint64_t key = __atomic_load_n(&recomp_icall_slots[i].key, __ATOMIC_ACQUIRE);
        if (key)
            fprintf(f, "%s%08x %08x %u\n", prefix, (uint32_t)(key >> 32), (uint32_t)key,
                    __atomic_load_n(&recomp_icall_slots[i].count, __ATOMIC_RELAXED));
    }
}

static inline void recomp_icall_write(void) {
    const char *path = getenv("RECOMP_ICALL_PROFILE");
    FILE *f = fopen(path && *path ? path : "icall-profile.txt", "w");
    if (!f)
        return;
    recomp_icall_print(f, "");
    fclose(f);
}

/* A run that never exits - a browser page - has no atexit: with
 * RECOMP_ICALL_PROFILE_SECS=<n> the table goes to stderr once, n seconds after
 * the first call noted, each line prefixed `icall-profile ` to cut it from
 * the log. Checked every 2^20 calls. */
static inline void recomp_icall_tick(void) {
    const char *e = getenv("RECOMP_ICALL_PROFILE_SECS");
    int64_t now = (int64_t)time(NULL), start, no = 0;
    uint32_t zero = 0;
    if (!e || !*e)
        return;
    start = __atomic_load_n(&recomp_icall_start, __ATOMIC_ACQUIRE);
    if (!start) {
        __atomic_compare_exchange_n(&recomp_icall_start, &no, now, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE);
        return;
    }
    if (now - start < (int64_t)strtol(e, NULL, 10))
        return;
    if (__atomic_compare_exchange_n(&recomp_icall_logged, &zero, 1u, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_RELAXED)) {
        recomp_icall_print(stderr, "icall-profile ");
        fflush(stderr);
    }
}

static inline void recomp_icall_note(uint32_t site, uint32_t target) {
    uint64_t key = ((uint64_t)site << 32) | target;
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 40) & (RECOMP_ICALL_SLOTS - 1u);
    uint32_t probe;
    if (!key)
        return;
    if (__builtin_expect(
            (__atomic_add_fetch(&recomp_icall_notes, 1u, __ATOMIC_RELAXED) & 0xfffffu) == 0, 0))
        recomp_icall_tick();
    if (__builtin_expect(!__atomic_load_n(&recomp_icall_armed, __ATOMIC_RELAXED), 0)) {
        uint32_t no = 0;
        if (__atomic_compare_exchange_n(&recomp_icall_armed, &no, 1u, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED))
            atexit(recomp_icall_write);
    }
    for (probe = 0; probe < 64; ++probe) {
        RecompIcallSlot *s = &recomp_icall_slots[(h + probe) & (RECOMP_ICALL_SLOTS - 1u)];
        uint64_t seen = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
        if (seen == 0) {
            uint64_t empty = 0;
            if (__atomic_compare_exchange_n(&s->key, &empty, key, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                seen = key;
            else
                seen = empty;
        }
        if (seen == key) {
            __atomic_fetch_add(&s->count, 1u, __ATOMIC_RELAXED);
            return;
        }
    }
}

#ifdef __cplusplus
}
#endif
#endif
