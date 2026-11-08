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

static inline void recomp_icall_write(void) {
    const char *path = getenv("RECOMP_ICALL_PROFILE");
    FILE *f = fopen(path && *path ? path : "icall-profile.txt", "w");
    uint32_t i;
    if (!f)
        return;
    fprintf(f, "# indirect CALL site, target, calls (tools/recomp/translate.py "
               "[translate] icall_profile)\n");
    for (i = 0; i < RECOMP_ICALL_SLOTS; ++i) {
        uint64_t key = __atomic_load_n(&recomp_icall_slots[i].key, __ATOMIC_ACQUIRE);
        if (key)
            fprintf(f, "%08x %08x %u\n", (uint32_t)(key >> 32), (uint32_t)key,
                    __atomic_load_n(&recomp_icall_slots[i].count, __ATOMIC_RELAXED));
    }
    fclose(f);
}

static inline void recomp_icall_note(uint32_t site, uint32_t target) {
    uint64_t key = ((uint64_t)site << 32) | target;
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 40) & (RECOMP_ICALL_SLOTS - 1u);
    uint32_t probe;
    if (!key)
        return;
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
