#include "imports.h"
#include "msvc_eh.h"
#include "crt_internal.h"
#include "gdi32_internal.h"
#include "user32_internal.h"
#include "kernel32_internal.h"
#include "seh.h"
#include "profile.h"
#include "mods_seam.h"

// Defined in kernel32.cpp with the scheduler.
void sched_checkpoint();
#include "memory.h"
#include "win32.h"
#include "../platform/ab_phase.h"
#include "../platform/os.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <cctype>
#include <vector>
#include <string>
#include <algorithm>

namespace {
thread_local uint32_t current_import_target = 0;

struct Tramp {
    std::string dll;
    std::string name;
    std::string desc; // "DLL!name", stable storage for imports_describe
    uint8_t argc = 0;
    void (*fn)(X86 *) = nullptr;
    uint32_t calls = 0;
    // Entered without a scheduling checkpoint (see atomic_import).
    bool atomic = false;
    // The unknown-argument-count warning has been logged for this one.
    bool argc_warned = false;
    // Held around the shim as well as the baton (imports_serialize_dll).
    std::recursive_mutex *serial = nullptr;
};

// Imports whose callers have just set up state the call reads back, with no
// import in between: a thread switch at their entry checkpoint would let
// another thread replace it. Delphi's VCL sets its creation-control global and
// calls CreateWindowEx at once; the window procedure reads it on the first
// message.
bool atomic_import(const std::string &key) {
    return key == "user32.dll!CreateWindowExA" || key == "user32.dll!CreateWindowExW";
}

// Deliberately leaked: an atexit hook (RECOMP_IMPORT_STATS) reads these after
// static destructors would otherwise have run.
std::vector<Tramp> &tramps() {
    static auto *v = new std::vector<Tramp>();
    return *v;
}
std::map<std::string, uint32_t> &index() {
    static auto *m = new std::map<std::string, uint32_t>();
    return *m;
}
std::map<std::string, ImportShim> &registry() {
    static auto *m = new std::map<std::string, ImportShim>();
    return *m;
}
std::set<std::string> &registered_dlls() {
    static auto *s = new std::set<std::string>();
    return *s;
}

struct DataImport {
    uint32_t size = 0;
    uint32_t addr = 0;
    uint32_t storage = 0, prefix = 0;
    uint64_t generation = 0;
    void (*init)(uint32_t) = nullptr;
    std::string desc;
};
std::map<std::string, DataImport> &data_imports() {
    static auto *m = new std::map<std::string, DataImport>();
    return *m;
}

// Baton-free shims (imports_register_baton_free), by key: the free version and the shim it
// stands for, so a trampoline whose shim has since been replaced does not use it.
struct FreeEntry {
    bool (*fn)(X86 *) = nullptr;
    void (*shim)(X86 *) = nullptr;
};
std::map<std::string, FreeEntry> &free_registry() {
    static auto *m = new std::map<std::string, FreeEntry>();
    return *m;
}
std::map<std::string, std::recursive_mutex *> &serialized_dlls() {
    static auto *m = new std::map<std::string, std::recursive_mutex *>();
    return *m;
}
// The same, per trampoline, for the dispatcher. A thread running free reads these while the
// baton holder may be allocating trampolines, which moves tramps(); this table never moves.
// A slot's argument count is stored before its function, which is published with release.
struct FreeSlot {
    std::atomic<bool (*)(X86 *)> fn{nullptr};
    std::atomic<uint8_t> argc{0};
};
FreeSlot g_free_slots[TRAMP_MAX];

std::string lower_dll(const char *dll) {
    std::string k(dll ? dll : "");
    std::transform(k.begin(), k.end(), k.begin(),
                   [](unsigned char ch) { return (char)tolower(ch); });
    return k;
}

std::string key_of(const char *dll, const char *name) {
    std::string k = lower_dll(dll);
    k += '!';
    k += name ? name : "";
    return k;
}

// x86 stdcall exports may carry their stack byte count in _name@bytes.
// Only use this ABI evidence when no explicit signature was registered;
// fastcall (@name@bytes), C++ mangling and malformed suffixes remain unknown.
uint8_t decorated_stdcall_args(const char *name) {
    if (!name || name[0] != '_')
        return ARGC_UNKNOWN;
    const char *suffix = strrchr(name, '@');
    if (!suffix || suffix <= name + 1 || !suffix[1])
        return ARGC_UNKNOWN;
    unsigned bytes = 0;
    for (const char *p = suffix + 1; *p; ++p) {
        if (*p < '0' || *p > '9')
            return ARGC_UNKNOWN;
        bytes = bytes * 10 + unsigned(*p - '0');
        if (bytes > 4u * (ARGC_UNKNOWN - 1))
            return ARGC_UNKNOWN;
    }
    return bytes % 4 ? ARGC_UNKNOWN : uint8_t(bytes / 4);
}

// Publishes (or withdraws) trampoline `idx`'s baton-free version. Called holding the baton,
// wherever its shim or the registry may have changed.
void free_slot_update(uint32_t idx, const std::string &key) {
    if (idx >= TRAMP_MAX || idx >= tramps().size())
        return;
    const Tramp &t = tramps()[idx];
    auto fi = free_registry().find(key);
    if (fi == free_registry().end() || !t.fn || t.fn != fi->second.shim ||
        t.argc == ARGC_UNKNOWN) {
        g_free_slots[idx].fn.store(nullptr, std::memory_order_release);
        return;
    }
    g_free_slots[idx].argc.store(t.argc, std::memory_order_relaxed);
    g_free_slots[idx].fn.store(fi->second.fn, std::memory_order_release);
}

} // namespace

void imports_register(const ImportShim *shims, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const ImportShim &s = shims[i];
        if (s.dll && *s.dll)
            registered_dlls().insert(lower_dll(s.dll));
        std::string k = key_of(s.dll, s.name);
        auto it = registry().find(k);
        if (it != registry().end() && it->second.fn && !s.fn)
            continue;
        registry()[k] = s;
        // If a trampoline already exists, upgrade it in place.
        auto ti = index().find(k);
        if (ti != index().end()) {
            Tramp &t = tramps()[ti->second];
            if (s.fn)
                t.fn = s.fn;
            t.argc = s.argc_stdcall;
            free_slot_update(ti->second, k);
        }
    }
}

void imports_register_baton_free(const ImportFreeShim *shims, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const std::string k = key_of(shims[i].dll, shims[i].name);
        auto ri = registry().find(k);
        if (!shims[i].fn || ri == registry().end() || !ri->second.fn)
            continue; // stands for no shim
        free_registry()[k] = FreeEntry{shims[i].fn, ri->second.fn};
        auto ti = index().find(k);
        if (ti != index().end())
            free_slot_update(ti->second, k);
    }
}

void imports_register_baton_free_method(const char *dll, const char *name, bool (*fn)(X86 *),
                                        void (*shim)(X86 *)) {
    const std::string k = key_of(dll, name);
    free_registry()[k] = FreeEntry{fn, shim};
    auto ti = index().find(k);
    if (ti != index().end())
        free_slot_update(ti->second, k);
}

void imports_serialize_dll(const char *dll, std::recursive_mutex *m) {
    const std::string d = lower_dll(dll);
    serialized_dlls()[d] = m;
    for (Tramp &t : tramps())
        if (lower_dll(t.dll.c_str()) == d)
            t.serial = m;
}

bool imports_has_dll(const char *dll_lower) {
    return registered_dlls().count(lower_dll(dll_lower)) != 0;
}

uint32_t imports_alloc_trampoline(const char *dll, const char *name, void (*fn)(X86 *),
                                  uint8_t argc_stdcall) {
    std::string k = key_of(dll, name);
    auto it = index().find(k);
    if (it != index().end()) {
        Tramp &t = tramps()[it->second];
        if (fn) {
            t.fn = fn;
            t.argc = argc_stdcall;
            free_slot_update(it->second, k);
        }
        return TRAMP_BASE + TRAMP_STRIDE * it->second;
    }

    Tramp t;
    t.atomic = atomic_import(k);
    {
        auto si = serialized_dlls().find(lower_dll(dll));
        if (si != serialized_dlls().end())
            t.serial = si->second;
    }
    t.dll = dll ? dll : "?";
    t.name = name ? name : "?";
    t.desc = t.dll + "!" + t.name;
    t.fn = fn;
    t.argc = argc_stdcall;

    auto ri = registry().find(k);
    if (ri != registry().end()) {
        if (!t.fn)
            t.fn = ri->second.fn;
        if (!fn)
            t.argc = ri->second.argc_stdcall;
    }
    if (t.argc == ARGC_UNKNOWN)
        t.argc = decorated_stdcall_args(name);
    if (!t.fn) {
        ImportShim fallback{};
        if (crt_import_fallback(dll, name, &fallback)) {
            t.fn = fallback.fn;
            t.argc = fallback.argc_stdcall;
        }
    }

    uint32_t idx = (uint32_t)tramps().size();
    if (idx >= TRAMP_MAX) {
        LOGW("imports: trampoline space exhausted (%u entries) for %s", idx, t.desc.c_str());
        return 0;
    }
    tramps().push_back(t);
    index()[k] = idx;
    free_slot_update(idx, k);
    return TRAMP_BASE + TRAMP_STRIDE * idx;
}

// The data imports D3DPopTB.exe has. weanetr.dll exports three GUIDs (16 bytes
// each) and options_to_parity_table (an int array; 4 KB of zeroed storage is
// what the plan specifies). Registering more is how a later task adds its own.
void imports_register_data(const char *dll, const char *name, uint32_t size) {
    std::string k = key_of(dll, name);
    DataImport &d = data_imports()[k];
    if (d.size < size)
        d.size = size;
    if (d.desc.empty())
        d.desc = std::string(dll) + "!" + name;
}
void imports_register_data_init(const char *dll, const char *name, uint32_t size,
                                void (*init)(uint32_t), uint32_t prefix) {
    imports_register_data(dll, name, size);
    DataImport &data = data_imports()[key_of(dll, name)];
    data.init = init;
    data.prefix = prefix;
}

static void register_default_data_imports() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register_data("weanetr.dll", "?BFAID_INet@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?BFAID_MODEM@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?BFSPGUID_MODEM@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?options_to_parity_table@@3PAHA", 4096);
}

uint32_t imports_alloc_data(const char *dll, const char *name) {
    register_default_data_imports();
    auto it = data_imports().find(key_of(dll, name));
    if (it == data_imports().end())
        return 0;
    DataImport &d = it->second;
    // mem_init() invalidates the heap, so a stale address is re-allocated.
    if (!d.addr || d.generation != mem_generation() || !heap_owns(d.storage)) {
        d.storage = d.size <= UINT32_MAX - d.prefix ? heap_alloc(d.size + d.prefix, true) : 0;
        d.addr = d.storage ? d.storage + d.prefix : 0;
        d.generation = mem_generation();
        if (!d.addr)
            LOGW("imports: cannot allocate %u bytes for data import %s", d.size, d.desc.c_str());
        else {
            LOGV("data import %s -> %u zeroed bytes at %08x", d.desc.c_str(), d.size, d.addr);
            if (d.init)
                d.init(d.addr);
        }
    }
    return d.addr;
}

uint32_t imports_data_address(const char *dll, const char *name) {
    auto it = data_imports().find(key_of(dll, name));
    return it == data_imports().end() || it->second.generation != mem_generation()
               ? 0
               : it->second.addr;
}

uint32_t imports_data_count() {
    return (uint32_t)data_imports().size();
}

uint32_t imports_trampoline_for(const char *dll, const char *name) {
    auto it = index().find(key_of(dll, name));
    if (it == index().end())
        return 0;
    return TRAMP_BASE + TRAMP_STRIDE * it->second;
}

uint32_t imports_resolve(const char *dll, const char *name) {
    uint32_t a = imports_trampoline_for(dll, name);
    if (a)
        return a;
    auto ri = registry().find(key_of(dll, name));
    if (ri == registry().end()) {
        // Optional GetProcAddress probes still report absent exports. The
        // loader's required IAT imports receive named CRT traps at allocation.
        return 0;
    }
    return imports_alloc_trampoline(ri->second.dll, ri->second.name, ri->second.fn,
                                    ri->second.argc_stdcall);
}

bool imports_serves_module(const char *dll) {
    if (!dll || !*dll)
        return false;
    // key_of applies the same DLL normalization as registration/resolution.
    // Including '!' makes the prefix match the complete module name only.
    std::string prefix = key_of(dll, nullptr);
    auto it = registry().lower_bound(prefix);
    return (it != registry().end() && it->first.compare(0, prefix.size(), prefix) == 0) ||
           crt_import_fallback(dll, nullptr, nullptr);
}

const char *imports_describe(uint32_t target) {
    if (!imports_is_trampoline(target))
        return nullptr;
    uint32_t idx = (target - TRAMP_BASE) / TRAMP_STRIDE;
    if (idx >= tramps().size())
        return nullptr;
    return tramps()[idx].desc.c_str();
}
const char *imports_current_description() {
    return imports_describe(current_import_target);
}

uint8_t imports_argc(uint32_t trampoline) {
    if (!imports_is_trampoline(trampoline))
        return 0;
    uint32_t idx = (trampoline - TRAMP_BASE) / TRAMP_STRIDE;
    return idx < tramps().size() ? tramps()[idx].argc : 0;
}

uint32_t imports_count() {
    return (uint32_t)tramps().size();
}

uint32_t imports_call_count(const char *dll, const char *name) {
    auto it = index().find(key_of(dll, name));
    if (it == index().end())
        return 0;
    return tramps()[it->second].calls;
}

void imports_dump_stats(FILE *out) {
    std::vector<const Tramp *> v;
    for (const Tramp &t : tramps())
        if (t.calls)
            v.push_back(&t);
    std::sort(v.begin(), v.end(),
              [](const Tramp *a, const Tramp *b) { return a->calls > b->calls; });
    fprintf(out, "import calls (%zu of %zu entries used):\n", v.size(), tramps().size());
    for (const Tramp *t : v)
        fprintf(out, "  %8u  %-40s %s\n", t->calls, t->desc.c_str(), t->fn ? "" : "(logging only)");
}

void imports_coverage(uint32_t *implemented, uint32_t *logging_only, uint32_t *unknown_argc) {
    uint32_t impl = 0, log_only = 0, unknown = 0;
    for (const Tramp &t : tramps()) {
        if (t.fn)
            ++impl;
        else
            ++log_only;
        if (t.argc == ARGC_UNKNOWN)
            ++unknown;
    }
    if (implemented)
        *implemented = impl;
    if (logging_only)
        *logging_only = log_only;
    if (unknown_argc)
        *unknown_argc = unknown;
}

void imports_dump_coverage(FILE *out) {
    std::map<std::string, std::pair<uint32_t, uint32_t>> per_dll; // implemented, logging-only
    std::vector<const Tramp *> log_only, unknown;
    for (const Tramp &t : tramps()) {
        auto &e = per_dll[t.dll];
        if (t.fn)
            ++e.first;
        else {
            ++e.second;
            log_only.push_back(&t);
        }
        if (t.argc == ARGC_UNKNOWN)
            unknown.push_back(&t);
    }
    fprintf(out, "import coverage by DLL (implemented / logging-only):\n");
    for (const auto &kv : per_dll)
        fprintf(out, "  %-16s %3u / %3u\n", kv.first.c_str(), kv.second.first, kv.second.second);
    fprintf(out, "logging-only imports (%zu):\n", log_only.size());
    for (const Tramp *t : log_only)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "imports with an unknown stdcall argument count (%zu):\n", unknown.size());
    for (const Tramp *t : unknown)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "data imports backed by guest storage (%zu):\n", data_imports().size());
    for (const auto &kv : data_imports())
        fprintf(out, "  %-44s %5u bytes at %08x\n", kv.second.desc.c_str(), kv.second.size,
                kv.second.addr);
}

void imports_dump_report(FILE *out) {
    std::vector<const Tramp *> called, not_reached, log_only;
    for (const Tramp &t : tramps()) {
        if (!t.fn)
            log_only.push_back(&t);
        else if (t.calls)
            called.push_back(&t);
        else
            not_reached.push_back(&t);
    }
    auto by_name = [](const Tramp *a, const Tramp *b) { return a->desc < b->desc; };
    std::sort(called.begin(), called.end(), by_name);
    std::sort(not_reached.begin(), not_reached.end(), by_name);
    std::sort(log_only.begin(), log_only.end(), by_name);

    fprintf(out, "implemented and called (%zu):\n", called.size());
    for (const Tramp *t : called)
        fprintf(out, "  %-44s %u calls\n", t->desc.c_str(), t->calls);
    fprintf(out, "implemented but not reached (%zu):\n", not_reached.size());
    for (const Tramp *t : not_reached)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "logging-only (%zu), of which called: ", log_only.size());
    uint32_t hit = 0;
    for (const Tramp *t : log_only)
        if (t->calls)
            ++hit;
    fprintf(out, "%u\n", hit);
    for (const Tramp *t : log_only)
        fprintf(out, "  %-44s %s\n", t->desc.c_str(), t->calls ? "CALLED" : "");
}

namespace {
ImportReturnObserver g_return_observer = nullptr;
}

void imports_set_return_observer(ImportReturnObserver fn) {
    g_return_observer = fn;
}

namespace {
ImportCallObserver g_call_observer = nullptr;
}

void imports_set_call_observer(ImportCallObserver fn) {
    g_call_observer = fn;
}

ImportReturnObserver imports_set_return_observer_get(void) {
    return g_return_observer;
}

ImportCallObserver imports_set_call_observer_get(void) {
    return g_call_observer;
}

// Dispatch a guest import trampoline at a scheduler checkpoint.
// Copy dispatch metadata before calling a shim, then restore EIP/ESP according to its calling convention.
namespace {
uint64_t g_import_calls = 0;
} // namespace
uint64_t imports_call_count() {
    return g_import_calls;
}
void imports_call_leaves_surfaces() {
    --g_import_calls;
}

static bool imports_dispatch_holding(X86 *c, uint32_t target);

// RECOMP_FREE_RUN_STATS=1: every 5 s, the imports a thread running free made holding the
// baton, most frequent first - what is left to make baton-free (imports_register_baton_free).
// Called holding the baton (just before the call it counts).
static void free_fallback_note(uint32_t target) {
    static const bool on = recomp_env("FREE_RUN_STATS") != nullptr;
    if (!on)
        return;
    static std::map<uint32_t, uint32_t> counts;
    static uint64_t from = 0;
    ++counts[target];
    const uint64_t now = os_monotonic_ns();
    if (!from)
        from = now;
    if (now - from < 5000000000ull)
        return;
    std::vector<std::pair<uint32_t, uint32_t>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(),
              [](const std::pair<uint32_t, uint32_t> &a, const std::pair<uint32_t, uint32_t> &b) {
                  return a.second > b.second;
              });
    char line[768];
    int n = snprintf(line, sizeof line, "free run: imports taking the baton, last %.1f s:",
                     (now - from) * 1e-9);
    for (size_t i = 0; i < v.size() && i < 8 && n > 0 && n < (int)sizeof line - 96; ++i) {
        const char *name = imports_describe(v[i].first);
        n += snprintf(line + n, sizeof line - n, " %s %u;", name ? name : "?", v[i].second);
    }
    LOGW("%s", line);
    counts.clear();
    from = now;
}

// A thread running free calls a baton-free shim directly (imports_register_baton_free): no
// baton, no scheduling checkpoint (a free thread has nothing to yield) and no per-import
// counts (they are the baton holder's). Not while a call observer watches: it expects to see
// every call. The return observer is not told: it reports the DirectX layer's failures
// (dx.cpp), and no baton-free shim returns one. False when the import has none, or its shim
// needs the baton for this call.
static bool imports_dispatch_free(X86 *c, uint32_t target) {
    const uint32_t idx = (target - TRAMP_BASE) / TRAMP_STRIDE;
    if (idx >= TRAMP_MAX || g_call_observer)
        return false;
    bool (*fn)(X86 *) = g_free_slots[idx].fn.load(std::memory_order_acquire);
    if (!fn)
        return false;
    const uint32_t ret_addr = rd32(c->r[R_ESP]);
    if (!fn(c))
        return false;
    const uint8_t argc = g_free_slots[idx].argc.load(std::memory_order_relaxed);
    c->r[R_ESP] += argc == ARGC_CDECL ? 4u : 4u + 4u * argc;
    c->eip = ret_addr;
    return true;
}

// Every shim assumes the baton, and a guest thread running free (win32.h free run) holds
// none: unless the import has a baton-free version, it takes the baton for the call and,
// while the stack is still inside the free-run function (below its entry ESP), runs free
// again afterwards.
bool imports_dispatch(X86 *c, uint32_t target) {
    if (RECOMP_UNLIKELY(sched_free_running()) && imports_is_trampoline(target)) {
        if (imports_dispatch_free(c, target))
            return true;
        const uint32_t entry_esp = sched_free_run_leave();
        free_fallback_note(target);
        const bool done = imports_dispatch_holding(c, target);
        if (entry_esp && c->r[R_ESP] < entry_esp)
            sched_free_run_enter(entry_esp);
        sched_parallel_rejoin();
        return done;
    }
    const bool done = imports_dispatch_holding(c, target);
    // RECOMP_SCHED=parallel: a worker that held the baton runs free again. The mode is fixed
    // at start, so the call is made only when it is on.
    static const bool parallel = sched_parallel();
    if (RECOMP_UNLIKELY(parallel))
        sched_parallel_rejoin();
    return done;
}

static bool imports_dispatch_holding(X86 *c, uint32_t target) {
    if (!imports_is_trampoline(target))
        return false;
    // A bounded scheduling checkpoint. Cooperative threads must not depend on
    // the guest happening to call a blocking API: the frame limiter spins on
    // GetTickCount and would otherwise starve the service threads for as long
    // as it liked, expired waits and all. Every call into the runtime is a
    // checkpoint, rate limited to one yield per millisecond per thread, and it
    // runs before the shim so the guest state is a consistent call boundary -
    // except at an atomic import, and inside a sched_atomic_enter stretch.
    uint32_t idx = (target - TRAMP_BASE) / TRAMP_STRIDE;
    // The table object stays put; only its elements move when a shim adds a
    // trampoline, so it is indexed afresh after the call rather than held.
    std::vector<Tramp> &table = tramps();
    // Tracing and chain validation only run at log level 2. The level is read once
    // (memory.cpp caches it at first use and it never changes), so no call is made for it
    // on each of the tens of thousands of imports a frame.
    static const bool verbose = log_level() >= 2;
    if (idx >= table.size() || !table[idx].atomic)
        sched_checkpoint();
    if (idx >= table.size()) {
        // No shim to say how many arguments to drop, but the return address
        // must still be consumed or the guest stack is displaced from here on.
        LOGW("imports: call to unallocated trampoline %08x (ESP=%08x, return=%08x)", target,
             c->r[R_ESP], rd32(c->r[R_ESP]));
        c->eip = rd32(c->r[R_ESP]);
        c->r[R_ESP] += 4;
        set_eax(c, 0);
        return true;
    }
    // The shim may allocate trampolines (GetProcAddress, a COM shim, a guest
    // callback), which reallocates this vector, so nothing may hold a
    // reference to the element across the call.
    void (*fn)(X86 *) = table[idx].fn;
    uint8_t argc = table[idx].argc;
    // A guest exception can longjmp across fn(c). Keep no C++ owner live
    // across that call, and do not retain a pointer into the movable vector:
    // the shim may allocate trampolines and reallocate it. The name is read
    // from the table where it is used, never held across fn(c); this runs on
    // every guest import, tens of thousands of times a frame, so it is not
    // copied unless an observer (which may itself reallocate) is installed.
    auto desc_now = [&table, idx]() -> const char * { return table[idx].desc.c_str(); };
    ++table[idx].calls;
    ++g_import_calls;

    uint32_t ret_addr = rd32(c->r[R_ESP]);
    if (verbose) {
        log_msg(2, "-> %s (esp=%08x ret=%08x)", desc_now(), c->r[R_ESP], ret_addr);
        recomp_seh_validate_chain(c, "import enter", desc_now());
    }
    // TEST ONLY: the earlier phase of an in-run A/B (RECOMP_AB_PERIOD, platform/ab_phase.h)
    // repeats the per-call work this replaced - a copy of the name, the level reads and the
    // chain-check calls - so the two can be compared on the same scene.
    char ab_desc[512];
    const bool ab_old = recomp_ab::old(recomp_ab::IMPORTS);
    if (RECOMP_UNLIKELY(ab_old)) {
        const size_t n = std::min(table[idx].desc.size(), sizeof ab_desc - 1);
        memcpy(ab_desc, table[idx].desc.data(), n);
        ab_desc[n] = '\0';
        LOGV("-> %s (esp=%08x ret=%08x)", ab_desc, c->r[R_ESP], ret_addr);
        recomp_seh_validate_chain(c, "import enter", ab_desc);
    }

    // The arguments as they are NOW, before the shim runs: a stdcall shim pops
    // them, so after the call they are gone and this is the only point they
    // can be read. Only meaningful for a known stdcall count; a cdecl or
    // unknown shim reports none rather than inventing them.
    bool skip = false;
    if (g_call_observer) {
        uint32_t args[8];
        uint32_t n = (argc == ARGC_CDECL || argc == ARGC_UNKNOWN) ? 0u : (uint32_t)argc;
        if (n > 8)
            n = 8;
        for (uint32_t i = 0; i < n; ++i)
            args[i] = rd32(c->r[R_ESP] + 4 + 4 * i);
        uint32_t result = 0;
        const std::string desc = desc_now(); // the observer may reallocate the table
        if (!g_call_observer(desc.c_str(), args, n, &result)) {
            set_eax(c, result);
            skip = true;
        }
    }

    const uint32_t previous_import = current_import_target;
    current_import_target = target;
    if (skip) {
        // The observer refused the call and has set the result itself.
    } else if (fn) {
        // No lock_guard: a guest exception may longjmp across fn(c) (above). A serialized
        // DLL's shims check their pointers and raise none. Under RECOMP_SCHED=parallel the
        // nest says that host code holding the baton is on this stack (win32.h
        // sched_nest_enter); an exception that skips its leave lands above this ESP and
        // closes it there.
        static const bool parallel = sched_parallel();
        std::recursive_mutex *serial = table[idx].serial;
        if (serial)
            serial->lock();
        if (RECOMP_UNLIKELY(parallel))
            sched_nest_enter(c->r[R_ESP]);
        fn(c);
        if (RECOMP_UNLIKELY(parallel))
            sched_nest_leave();
        if (serial)
            serial->unlock();
    } else {
        log_once(desc_now(), "unimplemented import %s: returning 0", desc_now());
        set_eax(c, 0);
    }
    // Read after the shim ran, so the name is from the table as it is now.
    // Return observers only report (the D3D failure log runs on every call),
    // so the name is passed without a copy.
    if (g_return_observer)
        g_return_observer(desc_now(), c->r[R_EAX]);
    if (verbose)
        log_msg(2, "<- %s (eax=%08x)", desc_now(), c->r[R_EAX]);
    if (RECOMP_UNLIKELY(ab_old))
        LOGV("<- %s (eax=%08x)", ab_desc, c->r[R_EAX]);
    current_import_target = previous_import;

    uint32_t pop = 4;
    if (argc == ARGC_CDECL) {
        pop = 4;
    } else if (argc == ARGC_UNKNOWN) {
        pop = 4;
        // Once per trampoline, by a flag on it: formatting the key for
        // log_once on every call cost a string format per call of a shim the
        // game may call every frame (Galavic).
        if (!table[idx].argc_warned) {
            table[idx].argc_warned = true;
            char key[512 + 6];
            snprintf(key, sizeof key, "%.511s#argc", desc_now());
            log_once(key,
                     "%s has an unknown stdcall argument count: not adjusting ESP, "
                     "the guest stack will drift if it is really stdcall",
                     desc_now());
        }
    } else {
        pop = 4 + 4u * argc;
    }
    c->r[R_ESP] += pop;
    c->eip = ret_addr;
    if (verbose)
        recomp_seh_validate_chain(c, "import leave", desc_now());
    if (RECOMP_UNLIKELY(ab_old))
        recomp_seh_validate_chain(c, "import leave", ab_desc);
    return true;
}

// ---------------------------------------------------------------------------
// Calling guest code from a shim.
// ---------------------------------------------------------------------------
namespace {
struct Callback {
    X86 *cpu;
    jmp_buf env;
    uint32_t saved_esp, saved_eip, return_sp, profile_depth;
};
// A thread's callback records, and the ones it has finished with. A window
// procedure, a timer or a mod hook calls guest code from a shim many times a
// frame; each record holds a jmp_buf, and allocating and freeing one per call
// was a heap round trip for every one of them (Galavic). Up to kPoolMax are
// kept for reuse; deeper nesting allocates as before.
struct Callbacks {
    static constexpr size_t kPoolMax = 32;
    std::vector<Callback *> stack;
    std::vector<Callback *> pool;
    Callback *acquire() {
        if (pool.empty())
            return new Callback{};
        Callback *call = pool.back();
        pool.pop_back();
        *call = Callback{};
        return call;
    }
    void release(Callback *call) {
        if (pool.size() < kPoolMax)
            pool.push_back(call);
        else
            delete call;
    }
    ~Callbacks() {
        for (Callback *call : stack)
            delete call;
        for (Callback *call : pool)
            delete call;
    }
};
thread_local Callbacks callbacks;
} // namespace

uint32_t recomp_callback_depth(void) {
    return (uint32_t)callbacks.stack.size();
}

void recomp_callback_truncate(uint32_t depth) {
    while (callbacks.stack.size() > depth) {
        // Delphi and C++ landings both abandon native callback records.
        // Dispatcher authorization must die with the record it described.
        recomp_msvc_callback_leave(callbacks.stack.back()->cpu, uint32_t(callbacks.stack.size()));
        callbacks.release(callbacks.stack.back());
        callbacks.stack.pop_back();
    }
}

void recomp_callback_reset(X86 *c) {
    for (size_t i = callbacks.stack.size(); i-- > 0;)
        if (!c || callbacks.stack[i]->cpu == c) {
            callbacks.release(callbacks.stack[i]);
            callbacks.stack.erase(callbacks.stack.begin() + i);
        }
}

void recomp_callback_return(X86 *c) {
    if (callbacks.stack.empty())
        return;
    Callback *call = callbacks.stack.back();
    // Every translated return here is deeper on the host stack than this
    // live guest_call. Match the guest return slot as well: a completing SEH
    // landing may RET to an older callback's identical sentinel, in which
    // case intercept must finish that function through its SEH checkpoint.
    if (call->cpu != c || c->r[R_ESP] < call->return_sp + 4 || c->r[R_ESP] > call->saved_esp)
        return;
    mods_hooks_unwind_to_esp(call->return_sp);
    recomp_profile_truncate(call->profile_depth);
    longjmp(call->env, 1);
}

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) {
    Callback *call = callbacks.acquire();
    call->cpu = c;
    call->saved_esp = c->r[R_ESP];
    call->saved_eip = c->eip;
    call->profile_depth = recomp_profile_depth();
    uint32_t esp = call->saved_esp;
    for (int i = nargs - 1; i >= 0; --i) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, GUEST_RETURN_SENTINEL);
    c->r[R_ESP] = esp;
    call->return_sp = esp;
    callbacks.stack.push_back(call);
    if (!setjmp(call->env))
        recomp_run(c, fn);
    recomp_seh_callback_leave(c, recomp_callback_depth());
    uint32_t eax = c->r[R_EAX];
    c->r[R_ESP] = call->saved_esp;
    c->eip = call->saved_eip;
    callbacks.stack.pop_back();
    callbacks.release(call);
    return eax;
}

// ---------------------------------------------------------------------------
// Shim tables owned by this runtime.
// ---------------------------------------------------------------------------
void imports_init() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_kernel32_shims, g_kernel32_shim_count);
    kernel32_wide_register();
    oleaut32_register();
    misc_dlls_register();
    extern void msvc_eh_register();
    msvc_eh_register();
    crt_register();
    comctl32_register();
    media_foundation_register();
    imports_register(g_user32_shims, g_user32_shim_count);
    user32_wide_register();
    user32_vcl_register();
    imports_register(g_misc_shims, g_misc_shim_count);
    imports_register(g_gdi32_shims, g_gdi32_shim_count);
    extern void gdi_model_register();
    gdi_model_register();
    extern void gdi_bitmaps_register();
    gdi_bitmaps_register();
    gdi::register_draw();
    gdi::register_text();
    extern void msimg32_register();
    msimg32_register();
    // Last: each stands for the shim registered by then.
    imports_register_baton_free(g_kernel32_free_shims, g_kernel32_free_shim_count);
    imports_register_baton_free(g_misc_free_shims, g_misc_free_shim_count);
}
