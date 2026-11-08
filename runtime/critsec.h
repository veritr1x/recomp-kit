// critsec.h - ownership of a guest CRITICAL_SECTION, on the guest's own fields, with host atomics.
//
// The section is 24 bytes and its fields stay where Windows keeps them, so a guest that reads
// them sees what it expects: LockCount at +4 (-1 unowned, else recursion - 1), RecursionCount
// at +8 and OwningThread at +12 (0 unowned). OwningThread is the lock word: a thread takes a free
// section with one compare-exchange on it and gives it up with one store, so taking and leaving
// an uncontended section needs neither the scheduler's mutex nor the guest baton. That is what
// lets a guest thread running free (win32.h sched_free_run_enter) use the game's critical
// sections beside the baton holder. RecursionCount and LockCount are written only by the owner,
// between its taking the word (acquire) and giving it up (release).
//
// Only the uncontended part lives here. A thread that finds the section owned by another thread
// waits in the scheduler (kernel32.cpp sched_enter_critsec), which takes the section for it with
// try_take when it comes free.
#pragma once
#include "guest.h"
#include "x86.h"
#include <cstdint>

namespace critsec {

constexpr uint32_t LOCK_COUNT = 4, RECURSION = 8, OWNER = 12;

// A section the atomics can serve: inside guest memory, with its owner word naturally aligned
// (a host atomic needs that; anything else takes the scheduler's locked path, as before).
inline bool atomic_ok(uint32_t cs) {
    return cs && gm_valid(cs, 24) && ((cs + OWNER) & 3u) == 0;
}

inline uint32_t *owner_word(uint32_t cs) {
    return reinterpret_cast<uint32_t *>(RECOMP_GUEST_BASE + cs + OWNER);
}

inline uint32_t owner(uint32_t cs) {
    return __atomic_load_n(owner_word(cs), __ATOMIC_ACQUIRE);
}

// Takes the section for thread `tid` when it is free, or enters it again when `tid` owns it.
// False, with nothing changed, when another thread owns it.
inline bool try_take(uint32_t cs, uint32_t tid) {
    uint32_t *word = owner_word(cs);
    uint32_t held = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if (held == tid) {
        const uint32_t rec = rd32(cs + RECURSION) + 1;
        wr32(cs + RECURSION, rec);
        wr32(cs + LOCK_COUNT, rec - 1);
        return true;
    }
    if (held != 0)
        return false;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(word, &expected, tid, false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_ACQUIRE))
        return false;
    wr32(cs + RECURSION, 1);
    wr32(cs + LOCK_COUNT, 0);
    return true;
}

enum class Left { NotOwner, StillHeld, Freed };

// Leaves the section once for thread `tid`. Freed when that was its last entry: the owner word is
// cleared last, with a full fence, so a waiter that then finds the word free also sees the fields.
inline Left leave(uint32_t cs, uint32_t tid) {
    uint32_t *word = owner_word(cs);
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != tid)
        return Left::NotOwner;
    uint32_t rec = rd32(cs + RECURSION);
    if (rec)
        --rec;
    wr32(cs + RECURSION, rec);
    wr32(cs + LOCK_COUNT, rec ? rec - 1 : 0xffffffffu);
    if (rec)
        return Left::StillHeld;
    __atomic_store_n(word, 0u, __ATOMIC_SEQ_CST);
    return Left::Freed;
}

} // namespace critsec
