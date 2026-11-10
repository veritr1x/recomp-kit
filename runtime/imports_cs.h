// imports_cs.h - the import command stream (on by default; RECOMP_CS=0 turns it off).
//
// A shim marked deferrable (imports_cs_register) is not run by the guest thread that calls it:
// its arguments, and the memory one pointer argument names, are copied into a queue, the call
// returns 0 (S_OK) at once, and a host worker thread runs the shim later, in order. The guest
// thread goes on with its own code meanwhile, so the work of the shims overlaps the game's.
//
// Ordering: every other import, except those of the DLLs named independent (they touch no
// state a deferred shim reads or writes), first drains the queue: the worker has run everything
// queued before it returns. So a deferred shim never runs beside a shim of the same layer, and
// whatever the guest reads back (a Get*, a Lock, Present) sees every earlier call done.
//
// What a deferrable shim may do, beside its own layer's state: read the guest memory its
// arguments name at the time it runs (only the copied pointer argument is safe), take and drop
// object references (a last release is handed back to the guest thread, imports_cs_on_worker),
// and log. It must not allocate or free guest memory, yield the baton, call guest code, write an
// out-parameter, or return anything the caller reads but S_OK.
#pragma once

#include "x86.h"

#include <cstddef>
#include <cstdint>

struct ImportDeferSpec {
    const char *name;       // function name in any DLL ("ID3DXEffect::SetMatrix")
    int8_t ptr_arg = -1;    // stack argument (0 = first) holding a pointer to copy, or -1
    uint32_t unit = 0;      // bytes per element of that pointer's data
    int8_t count_arg = -1;  // argument holding the element count; -1: one element
    int8_t handle_arg = -1; // argument deferred only when >= 0xF0000000 (an encoded handle,
                            // not a pointer to a name that would have to be copied)
    // Bytes to copy from the pointer argument, from all the arguments, in place of unit and
    // count_arg (a draw's inline vertices); 0 copies nothing.
    uint32_t (*size_fn)(const uint32_t *args) = nullptr;
    // Run on the guest thread when the call is queued, for what the caller reads back at once
    // (an out-parameter it can fill from state only synchronous calls change); false: do not
    // defer this call.
    bool (*pre)(X86 *c) = nullptr;
    // Run on the worker in place of the shim, when the shim's own work is split this way.
    void (*deferred)(X86 *c) = nullptr;
};

// Marks the named imports deferrable (by function name, any DLL).
void imports_cs_register(const ImportDeferSpec *specs, size_t count);
// Imports, by function name in any DLL, that read and write only their own arguments (pure math
// with an out-parameter): they neither wait for the queue nor are deferred.
void imports_cs_register_independent(const char *const *names, size_t count);
bool imports_cs_independent(const char *name);
// Run on the guest thread after every drain: work the worker handed back (last releases).
void imports_cs_set_drain_hook(void (*fn)());
// True on the command-stream worker.
bool imports_cs_on_worker();

// For the dispatcher (imports.cpp). The spec index of a trampoline name, or -1.
int imports_cs_spec_of(const char *name);
// True when deferral is on for this call (not RECOMP_CS=0, not the earlier A/B phase).
bool imports_cs_active();
// Queues the call; false when it cannot be deferred (then the caller drains and runs it).
bool imports_cs_defer(X86 *c, int spec, void (*fn)(X86 *), uint8_t argc);
// Waits until the worker has run everything queued. Holding the baton; cheap when idle. True
// when it had to wait.
bool imports_cs_drain();
