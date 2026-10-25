// VC8's x86 FuncInfo, unwind/try maps and catchable-type records contain
// guest addresses throughout. Native checkpoints only implement the transfer
// back into a live translated body; no native exception unwinds guest code.
#include "msvc_eh.h"
#include "imports.h"
#include "memory.h"
#include "mods_seam.h"
#include "profile.h"
#include "seh.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void sched_atomic_unwind_to_esp(uint32_t esp);
namespace {
constexpr uint32_t END = 0xffffffffu, MAX_STATES = 4096, MAX_TYPES = 256;
struct Frame {
    X86 *cpu;
    X86 saved;
    uint32_t registration, handler, info, callback_depth, profile_depth, continuation = 0;
    jmp_buf env;
};
struct Exception {
    uint32_t object, info;
    bool destroyed = false;
    uint32_t record = 0;
};
struct Catch {
    Frame *frame;
    Exception *exception;
    uint32_t try_entry, callback_depth;
};
struct State {
    std::vector<Frame *> frames;
    std::vector<Catch *> catches;
    struct Boundary {
        X86 *cpu;
        uint32_t depth;
    };
    std::vector<Boundary> seh_callbacks;
    unsigned unwinding = 0;
    Frame *landing = nullptr;
    ~State() {
        for (auto *f : frames)
            delete f;
        for (auto *h : catches) {
            delete h->exception;
            delete h;
        }
    }
};
thread_local State state;

[[noreturn]] void fail(const char *what, uint32_t at = 0) {
    std::fprintf(stderr, "MSVC C++ EH: %s (%08x)\n", what, at);
    std::fflush(stderr);
    std::abort();
}
void memory(uint32_t address, uint32_t bytes, const char *what) {
    if (!address || !gm_valid(address, bytes))
        fail(what, address);
}
uint32_t add(uint32_t base, int32_t offset, uint32_t bytes, const char *what) {
    const int64_t value = int64_t(base) + offset;
    if (value <= 0 || value > UINT32_MAX)
        fail(what, base);
    memory(uint32_t(value), bytes, what);
    return uint32_t(value);
}
uint32_t head(X86 *c) {
    memory(c->fs_base, 12, "invalid TEB");
    return rd32(c->fs_base);
}
void registration(X86 *c, uint32_t reg) {
    const uint32_t lo = rd32(c->fs_base + 8), hi = rd32(c->fs_base + 4);
    if (lo >= hi || hi - lo < 12 || !gm_valid(lo, hi - lo) || reg < lo || reg > hi - 12 ||
        (reg & 3))
        fail("registration outside guest stack", reg);
}
bool frame_handler(uint32_t target, unsigned depth = 0) {
    if (depth > 2)
        return false;
    if (const char *name = imports_describe(target)) {
        const char *export_name = std::strchr(name, '!');
        return export_name && !std::strcmp(export_name + 1, "__CxxFrameHandler3");
    }
    if (!target || !gm_valid(target, 6))
        return false;
    if (rd8(target) == 0xe9)
        return frame_handler(target + 5 + rd32(target + 1), depth + 1);
    if (rd8(target) == 0xff && rd8(target + 1) == 0x25) {
        const uint32_t slot = rd32(target + 2);
        return gm_valid(slot, 4) && frame_handler(rd32(slot), depth + 1);
    }
    return false;
}
uint32_t func_info(uint32_t handler) {
    if (!handler || !gm_valid(handler, 11) || rd8(handler) != 0xb8 || !frame_handler(handler + 5))
        return 0;
    const uint32_t info = rd32(handler + 1);
    memory(info, 36, "invalid FuncInfo");
    const uint32_t magic = rd32(info) & 0x1fffffffu;
    if (magic < 0x19930520u || magic > 0x19930522u)
        fail("unsupported FuncInfo version", info);
    if (rd32(info + 4) > MAX_STATES || rd32(info + 12) > MAX_STATES)
        fail("oversized FuncInfo", info);
    if (rd32(info + 4))
        memory(rd32(info + 8), rd32(info + 4) * 8, "invalid unwind map");
    if (rd32(info + 12))
        memory(rd32(info + 16), rd32(info + 12) * 20, "invalid try map");
    return info;
}
Frame *checkpoint(X86 *c, uint32_t reg, uint32_t handler, uint32_t info) {
    for (size_t i = state.frames.size(); i-- > 0;) {
        Frame *f = state.frames[i];
        if (f->cpu != c || f->registration != reg)
            continue;
        if (f->handler != handler || f->info != info || f->saved.r[R_ESP] < c->r[R_ESP])
            fail("stale checkpoint handler or ESP", reg);
        const uint32_t depth = recomp_callback_depth();
        bool own_callback = false;
        for (Catch *h : state.catches)
            if (h->frame->cpu == c && h->callback_depth == depth &&
                h->frame->callback_depth == f->callback_depth)
                own_callback = true;
        for (const auto &boundary : state.seh_callbacks) {
            if (boundary.cpu != c || boundary.depth != depth)
                continue;
            if (f->callback_depth + 1 == depth)
                own_callback = true;
            for (Catch *h : state.catches)
                if (h->frame->cpu == c && h->callback_depth + 1 == depth &&
                    h->frame->callback_depth == f->callback_depth)
                    own_callback = true;
        }
        if (depth != f->callback_depth && !own_callback)
            fail("unwind crosses a foreign guest_call", reg);
        return f;
    }
    fail("catch has no live checkpoint", reg);
}
const char *type_name(uint32_t type) {
    memory(type, 9, "invalid type descriptor");
    const uint32_t start = type + 8;
    for (uint32_t n = 0; n < 512; ++n) {
        memory(start + n, 1, "unterminated type name");
        if (!rd8(start + n))
            return (const char *)gm_ptr(start);
    }
    fail("oversized type name", type);
}
uint32_t catchable(Exception *ex, uint32_t handler) {
    const uint32_t type = rd32(handler + 4), adjectives = rd32(handler);
    if (!type || !*type_name(type))
        return END; // ellipsis, no catch object
    memory(ex->info, 16, "invalid ThrowInfo");
    const uint32_t array = rd32(ex->info + 12);
    memory(array, 4, "invalid catchable-type array");
    const uint32_t count = rd32(array);
    if (!count || count > MAX_TYPES)
        fail("invalid catchable-type count", array);
    memory(array + 4, count * 4, "truncated catchable-type array");
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t ct = rd32(array + 4 + i * 4);
        memory(ct, 28, "invalid catchable type");
        if ((rd32(ct) & 2) && !(adjectives & 8))
            continue; // by-reference only
        if ((adjectives & 8) && (rd32(ex->info) & 3 & ~adjectives))
            continue;
        if (!std::strcmp(type_name(type), type_name(rd32(ct + 4))))
            return ct;
    }
    return 0;
}
uint32_t adjust(uint32_t object, uint32_t pmd) {
    const int32_t mdisp = int32_t(rd32(pmd)), pdisp = int32_t(rd32(pmd + 4)),
                  vdisp = int32_t(rd32(pmd + 8));
    if (pdisp >= 0) {
        const uint32_t vbptr = add(object, pdisp, 4, "invalid virtual-base pointer");
        const uint32_t displacement = add(rd32(vbptr), vdisp, 4, "invalid virtual-base table");
        object = add(vbptr, int32_t(rd32(displacement)), 1, "invalid virtual-base adjustment");
    }
    return add(object, mdisp, 1, "invalid catch-object adjustment");
}
uint32_t call_funclet(X86 *c, uint32_t address, uint32_t ebp) {
    memory(address, 1, "invalid funclet");
    const uint32_t saved_ebp = c->r[R_EBP];
    c->r[R_EBP] = ebp;
    const uint32_t result = guest_call(c, address, nullptr, 0);
    c->r[R_EBP] = saved_ebp;
    return result;
}
void destroy(X86 *c, Exception *ex) {
    if (ex->destroyed)
        fail("exception object destroyed twice", ex->object);
    ex->destroyed = true;
    const uint32_t destructor = rd32(ex->info + 4);
    if (destructor) {
        ++state.unwinding;
        const uint32_t saved_ecx = c->r[R_ECX];
        c->r[R_ECX] = ex->object;
        guest_call(c, destructor, nullptr, 0);
        c->r[R_ECX] = saved_ecx;
        --state.unwinding;
    }
    if (ex->record)
        heap_free(ex->record);
    delete ex;
}
void unwind_frame(X86 *c, uint32_t reg, uint32_t info, int32_t target) {
    const uint32_t count = rd32(info + 4), map = rd32(info + 8);
    int32_t current = int32_t(rd32(reg + 8));
    if (current < -1 || current >= int32_t(count) || target < -1 || target > current)
        fail("invalid unwind state", reg);
    while (current > target) {
        const uint32_t entry = map + uint32_t(current) * 8;
        const int32_t next = int32_t(rd32(entry));
        if (next < -1 || next >= current || next < target)
            fail("non-descending unwind map", entry);
        wr32(reg + 8, uint32_t(next)); // publish before destructor, including recursive throws
        ++state.unwinding;
        if (const uint32_t action = rd32(entry + 4))
            call_funclet(c, action, reg + 12);
        --state.unwinding;
        current = next;
    }
}
void build_catch(X86 *c, Exception *ex, uint32_t reg, uint32_t handler, uint32_t ct) {
    if (ct == END || !rd32(handler + 8))
        return;
    const uint32_t properties = rd32(ct), size = rd32(ct + 20);
    if (!size || size > (1u << 24))
        fail("invalid catch-object size", ct);
    const bool reference = (rd32(handler) & 8) != 0;
    const uint32_t dest =
        add(reg + 12, int32_t(rd32(handler + 8)), reference ? 4 : size, "invalid catch object");
    const uint32_t source = (properties & 1) ? ex->object : adjust(ex->object, ct + 8);
    memory(source, size, "invalid thrown object");
    if (reference)
        wr32(dest, source);
    else if (properties & 1) {
        std::memmove(gm_ptr(dest), gm_ptr(source), size);
        if (size == 4 && (rd32(ct + 8) || int32_t(rd32(ct + 12)) >= 0))
            wr32(dest, adjust(rd32(dest), ct + 8));
    } else if (const uint32_t copy = rd32(ct + 24)) {
        const uint32_t saved_ecx = c->r[R_ECX], saved_ebp = c->r[R_EBP];
        uint32_t args[] = {source, 1};
        c->r[R_ECX] = dest;
        c->r[R_EBP] = reg + 12;
        ++state.unwinding; // a failed exception copy terminates, as native EH does
        guest_call(c, copy, args, (properties & 4) ? 2 : 1);
        --state.unwinding;
        c->r[R_ECX] = saved_ecx;
        c->r[R_EBP] = saved_ebp;
    } else
        std::memmove(gm_ptr(dest), gm_ptr(source), size);
}

// Keep only trivial automatic values across guest_call: a rethrow can abandon
// this entire native dispatcher through a live generated checkpoint.
[[noreturn]] void dispatch(X86 *c, Exception *ex) {
    uint32_t target = END, try_entry = 0, handler = 0, ct = 0;
    uint32_t reg = head(c), links = 0, previous = 0;
    while (reg != END) {
        registration(c, reg);
        if (++links > MAX_STATES || (previous && reg <= previous))
            fail("cyclic registration chain", reg);
        const uint32_t info = func_info(rd32(reg + 4));
        if (info) {
            const int32_t current = int32_t(rd32(reg + 8));
            if (current < -1 || current >= int32_t(rd32(info + 4)))
                fail("invalid search state", reg);
            uint32_t selected_span = UINT32_MAX;
            for (uint32_t i = 0; i < rd32(info + 12); ++i) {
                const uint32_t entry = rd32(info + 16) + 20 * i;
                const int32_t low = int32_t(rd32(entry)), high = int32_t(rd32(entry + 4));
                if (low < 0 || high < low || high >= int32_t(rd32(info + 4)))
                    fail("invalid try range", entry);
                if (current < low || current > high)
                    continue;
                bool skip = false, nested_catch = false;
                for (Catch *active : state.catches) {
                    if (active->frame->cpu != c || active->frame->registration != reg)
                        continue;
                    if (active->try_entry == entry)
                        skip = true;
                    else if (low > int32_t(rd32(active->try_entry + 4)))
                        nested_catch = true;
                }
                if (skip || uint32_t(high - low) >= selected_span)
                    continue;
                const uint32_t n = rd32(entry + 12), handlers = rd32(entry + 16);
                if (!n || n > MAX_TYPES)
                    fail("invalid catch count", entry);
                memory(handlers, n * 16, "invalid catch table");
                for (uint32_t j = 0; j < n; ++j) {
                    const uint32_t candidate = handlers + 16 * j;
                    if (const uint32_t match = catchable(ex, candidate)) {
                        if (nested_catch)
                            fail("try inside a catch is unsupported", entry);
                        target = reg;
                        try_entry = entry;
                        handler = candidate;
                        ct = match;
                        selected_span = uint32_t(high - low);
                        break;
                    }
                }
            }
            if (target != END)
                break;
        }
        previous = reg;
        reg = rd32(reg);
    }
    if (target == END) {
        const uint32_t array = rd32(ex->info + 12);
        memory(array, 8, "invalid uncaught type array");
        const uint32_t type = rd32(array + 4);
        memory(type, 28, "invalid uncaught type");
        std::fprintf(stderr, "MSVC C++ EH: uncaught throw of %s\n", type_name(rd32(type + 4)));
        fail("uncaught C++ exception", ex->object);
    }
    Frame *frame = checkpoint(c, target, rd32(target + 4), func_info(rd32(target + 4)));
    build_catch(c, ex, target, handler, ct);
    reg = head(c);
    while (reg != target) {
        const uint32_t next = rd32(reg), info = func_info(rd32(reg + 4));
        if (info)
            unwind_frame(c, reg, info, -1);
        else {
            // A non-C++ registration may provide ordinary SEH cleanup. Handler4
            // is deliberately a pass-through, as its cookie/filter VM is absent.
            if (!ex->record) {
                ex->record = heap_alloc(80 + 0x2cc, true);
                if (!ex->record)
                    fail("cannot allocate unwind record");
                wr32(ex->record, 0xe06d7363);
                wr32(ex->record + 4, 2);
                wr32(ex->record + 12, c->eip);
                wr32(ex->record + 16, 3);
                wr32(ex->record + 20, 0x19930520);
                wr32(ex->record + 24, ex->object);
                wr32(ex->record + 28, ex->info);
                recomp_seh_capture_context(c, ex->record + 80);
            }
            const uint32_t args[] = {ex->record, reg, ex->record + 80, 0};
            ++state.unwinding;
            const uint32_t disposition = guest_call(c, rd32(reg + 4), args, 4);
            --state.unwinding;
            if (disposition != 1)
                fail("unsupported foreign SEH unwind disposition", reg);
        }
        wr32(c->fs_base, next);
        reg = next;
    }
    unwind_frame(c, target, frame->info, int32_t(rd32(try_entry)));
    for (size_t i = state.catches.size(); i-- > 0;) {
        Catch *old = state.catches[i];
        if (old->frame->cpu == c && old->frame->registration <= target) {
            state.catches.erase(state.catches.begin() + i);
            if (old->exception != ex)
                destroy(c, old->exception);
            delete old;
        }
    }
    Catch *active = new Catch{frame, ex, try_entry, recomp_callback_depth() + 1};
    state.catches.push_back(active);
    const uint32_t continuation = call_funclet(c, rd32(handler + 12), target + 12);
    if (!continuation || !gm_valid(continuation, 1))
        fail("invalid catch continuation", continuation);
    if (state.catches.empty() || state.catches.back() != active)
        fail("catch ownership mismatch", target);
    state.catches.pop_back();
    delete active;
    destroy(c, ex);
    // Restore the establishment's ESP, not the throw/funclet's temporary stack.
    // Only nonvolatile registers survive the call; EAX carries the funclet result.
    c->r[R_ESP] = frame->saved.r[R_ESP];
    c->r[R_EBP] = frame->saved.r[R_EBP];
    c->r[R_EBX] = frame->saved.r[R_EBX];
    c->r[R_ESI] = frame->saved.r[R_ESI];
    c->r[R_EDI] = frame->saved.r[R_EDI];
    c->r[R_EAX] = continuation;
    c->eip = continuation;
    frame->continuation = continuation;
    mods_hooks_unwind_to_esp(c->r[R_ESP]);
    sched_atomic_unwind_to_esp(c->r[R_ESP]);
    recomp_profile_truncate(frame->profile_depth);
    recomp_seh_callback_leave(c, frame->callback_depth + 1);
    recomp_callback_truncate(frame->callback_depth);
    recomp_msvc_unwind_to_esp(c, c->r[R_ESP]);
    state.landing = frame;
    longjmp(frame->env, 1);
}
void throw_exception(X86 *c) {
    if (state.unwinding)
        fail("throw during exception copy or unwinding destructor terminates");
    const uint32_t object = arg(c, 0), info = arg(c, 1);
    if (!object && !info) {
        if (state.catches.empty())
            fail("rethrow outside a catch");
        dispatch(c, state.catches.back()->exception);
    }
    memory(object, 1, "null exception object");
    memory(info, 16, "invalid ThrowInfo");
    dispatch(c, new Exception{object, info});
}
void pass(X86 *c) {
    set_eax(c, 1);
}
void frame_handler3(X86 *c) {
    const uint32_t record = arg(c, 0), reg = arg(c, 1);
    memory(record, 80, "invalid C++ exception record");
    (void)head(c);
    registration(c, reg);
    const uint32_t info = func_info(rd32(reg + 4));
    if (!info || info != c->r[R_EAX])
        fail("handler stub FuncInfo mismatch", reg);
    if (rd32(record + 4) & 6) {
        unwind_frame(c, reg, info, -1);
        set_eax(c, 1);
        return;
    }
    if (rd32(record) != 0xe06d7363 || rd32(record + 16) < 3) {
        set_eax(c, 1);
        return;
    }
    if (state.unwinding)
        fail("throw while unwinding terminates");
    const uint32_t magic = rd32(record + 20), object = rd32(record + 24),
                   throw_info = rd32(record + 28);
    if (magic < 0x19930520 || magic > 0x19930522)
        fail("unsupported C++ exception version", magic);
    memory(object, 1, "invalid exception object");
    memory(throw_info, 16, "invalid ThrowInfo");
    dispatch(c, new Exception{object, throw_info});
}
void zero(X86 *c) {
    set_eax(c, 0);
}
void uncaught(X86 *c) {
    set_eax(c, state.unwinding != 0);
}

void discard_frame(Frame *frame) {
    for (size_t i = state.catches.size(); i-- > 0;) {
        Catch *h = state.catches[i];
        if (h->frame == frame) {
            state.catches.erase(state.catches.begin() + i);
            if (h->exception->record)
                heap_free(h->exception->record);
            delete h->exception;
            delete h;
        }
    }
    delete frame;
}
// VC8 std::exception: vftable, _m_what, _m_doFree. The prefixed exported
// vtable carries a complete-object locator, not a native host RTTI pointer.
void exception_vtable(uint32_t table);
void typeinfo_vtable(uint32_t table);
void init_unknown(uint32_t address) {
    std::memcpy(gm_ptr(address), "Unknown exception", 18);
}
uint32_t unknown_message() {
    return imports_alloc_data("msvcr80.dll", "__recomp_exception_unknown");
}
void rtti(uint32_t table, const char *name) {
    const uint32_t data = heap_alloc(256, true);
    if (!data)
        fail("cannot allocate exception RTTI");
    const uint32_t type = data + 32, hierarchy = data + 112, bases = data + 128, base = data + 144;
    wr32(data + 12, type);
    wr32(data + 16, hierarchy);
    wr32(type, imports_alloc_data("msvcr80.dll", "??_7type_info@@6B@"));
    std::memcpy(gm_ptr(type + 8), name, std::strlen(name) + 1);
    wr32(hierarchy + 8, 1);
    wr32(hierarchy + 12, bases);
    wr32(bases, base);
    wr32(base, type);
    wr32(base + 12, END);
    wr32(table - 4, data);
}
void exception_release(uint32_t object) {
    memory(object, 12, "invalid std::exception object");
    if (rd32(object + 8) && rd32(object + 4))
        heap_free(rd32(object + 4));
    wr32(object + 4, 0);
    wr32(object + 8, 0);
}
void exception_initialize(uint32_t object, uint32_t message, bool copy) {
    memory(object, 12, "invalid std::exception object");
    wr32(object, imports_alloc_data("msvcr80.dll", "??_7exception@std@@6B@"));
    wr32(object + 4, message);
    wr32(object + 8, 0);
    if (message && copy) {
        uint32_t size = 0;
        do {
            memory(message + size, 1, "invalid exception message");
            if (size == (1u << 20))
                fail("oversized exception message", message);
        } while (rd8(message + size++));
        const uint32_t owned = heap_alloc(size, false);
        if (!owned)
            fail("cannot allocate exception message");
        std::memcpy(gm_ptr(owned), gm_ptr(message), size);
        wr32(object + 4, owned);
        wr32(object + 8, 1);
    }
}
void exception_default(X86 *c) {
    exception_initialize(c->r[R_ECX], 0, false);
    set_eax(c, c->r[R_ECX]);
}
void exception_message(X86 *c) {
    const uint32_t ref = arg(c, 0);
    memory(ref, 4, "invalid exception message reference");
    exception_initialize(c->r[R_ECX], rd32(ref), true);
    set_eax(c, c->r[R_ECX]);
}
void exception_borrowed(X86 *c) {
    const uint32_t ref = arg(c, 0);
    memory(ref, 4, "invalid exception message reference");
    exception_initialize(c->r[R_ECX], rd32(ref), false);
    set_eax(c, c->r[R_ECX]);
}
void exception_copy(X86 *c) {
    const uint32_t source = arg(c, 0);
    memory(source, 12, "invalid exception copy");
    exception_initialize(c->r[R_ECX], rd32(source + 4), rd32(source + 8) != 0);
    set_eax(c, c->r[R_ECX]);
}
void exception_assign(X86 *c) {
    const uint32_t object = c->r[R_ECX], source = arg(c, 0);
    memory(source, 12, "invalid exception assignment");
    if (source != object) {
        exception_release(object);
        exception_copy(c);
    }
    set_eax(c, object);
}
void exception_destruct(X86 *c) {
    exception_release(c->r[R_ECX]);
}
void exception_delete(X86 *c) {
    const uint32_t object = c->r[R_ECX], flags = arg(c, 0);
    if (flags & 2)
        fail("vector deleting exception destructor is unsupported", object);
    exception_release(object);
    if (flags & 1)
        heap_free(object);
    set_eax(c, object);
}
void exception_what(X86 *c) {
    const uint32_t object = c->r[R_ECX];
    memory(object, 12, "invalid exception what");
    set_eax(c, rd32(object + 4) ? rd32(object + 4) : unknown_message());
}
void typeinfo_destruct(X86 *c) {
    const uint32_t object = c->r[R_ECX];
    memory(object, 8, "invalid type_info object");
    if (rd32(object + 4))
        heap_free(rd32(object + 4));
    wr32(object + 4, 0);
}
void typeinfo_delete(X86 *c) {
    const uint32_t object = c->r[R_ECX], flags = arg(c, 0);
    if (flags & 2)
        fail("vector deleting type_info destructor is unsupported", object);
    typeinfo_destruct(c);
    if (flags & 1)
        heap_free(object);
    set_eax(c, object);
}
void exception_vtable(uint32_t table) {
    rtti(table, ".?AVexception@std@@");
    wr32(table, imports_resolve("msvcr80.dll", "??_Gexception@std@@UAEPAXI@Z"));
    wr32(table + 4, imports_resolve("msvcr80.dll", "?what@exception@std@@UBEPBDXZ"));
}
void typeinfo_vtable(uint32_t table) {
    rtti(table, ".?AVtype_info@@");
    wr32(table, imports_resolve("msvcr80.dll", "??_Gtype_info@@UAEPAXI@Z"));
}
} // namespace

extern "C" {
void recomp_msvc_throw(X86 *c, uint32_t object, uint32_t info) {
    if (state.unwinding)
        fail("throw during exception copy or unwinding destructor terminates");
    memory(object, 1, "null exception object");
    memory(info, 16, "invalid ThrowInfo");
    dispatch(c, new Exception{object, info});
}
jmp_buf *recomp_msvc_frame_enter(X86 *c, uint32_t handler, uint32_t info) {
    const uint32_t reg = head(c);
    registration(c, reg);
    if (state.frames.size() >= MAX_STATES)
        fail("checkpoint limit exceeded", reg);
    if (c->r[R_EBP] != reg + 12 || c->r[R_ESP] > reg || c->r[R_ESP] < rd32(c->fs_base + 8))
        fail("unsupported checkpoint EBP or ESP", reg);
    if (rd32(reg + 4) != handler || func_info(handler) != info || !rd32(info + 12))
        fail("checkpoint does not match published frame", reg);
    for (Frame *f : state.frames)
        if (f->cpu == c && f->registration == reg)
            fail("duplicate live checkpoint", reg);
    Frame *f = new Frame{c, *c, reg, handler, info, recomp_callback_depth(), recomp_profile_depth(),
                         0, {}};
    state.frames.push_back(f);
    return &f->env;
}
uint32_t recomp_msvc_frame_land(X86 *c) {
    Frame *f = state.landing;
    if (!f || f->cpu != c || !f->continuation)
        fail("landing without a live checkpoint");
    state.landing = nullptr;
    const uint32_t target = f->continuation;
    f->continuation = 0;
    return target;
}
void recomp_msvc_frame_leave(X86 *c) {
    // Called after FS:[0] is restored, often before ESP passes the record.
    const uint32_t current = head(c);
    for (size_t i = state.frames.size(); i-- > 0;) {
        Frame *f = state.frames[i];
        if (f->cpu == c && (current == END || f->registration < current) &&
            f->callback_depth == recomp_callback_depth()) {
            state.frames.erase(state.frames.begin() + i);
            discard_frame(f);
        }
    }
}
void recomp_msvc_unwind_to_esp(X86 *c, uint32_t esp) {
    for (size_t i = state.frames.size(); i-- > 0;) {
        Frame *f = state.frames[i];
        if (f->cpu == c && f->registration < esp) {
            state.frames.erase(state.frames.begin() + i);
            discard_frame(f);
        }
    }
}
void recomp_msvc_callback_leave(X86 *c, uint32_t depth) {
    for (size_t i = state.seh_callbacks.size(); i-- > 0;)
        if (state.seh_callbacks[i].cpu == c && state.seh_callbacks[i].depth >= depth)
            state.seh_callbacks.erase(state.seh_callbacks.begin() + i);
    for (size_t i = state.frames.size(); i-- > 0;) {
        Frame *f = state.frames[i];
        if (f->cpu == c && f->callback_depth >= depth) {
            state.frames.erase(state.frames.begin() + i);
            discard_frame(f);
        }
    }
}
void recomp_msvc_seh_enter(X86 *c) {
    state.seh_callbacks.push_back({c, recomp_callback_depth() + 1});
}
void recomp_msvc_seh_leave(X86 *c) {
    const uint32_t depth = recomp_callback_depth() + 1;
    for (size_t i = state.seh_callbacks.size(); i-- > 0;)
        if (state.seh_callbacks[i].cpu == c && state.seh_callbacks[i].depth == depth) {
            state.seh_callbacks.erase(state.seh_callbacks.begin() + i);
            break;
        }
}
void recomp_msvc_reset(X86 *c) {
    for (size_t i = state.seh_callbacks.size(); i-- > 0;)
        if (!c || state.seh_callbacks[i].cpu == c)
            state.seh_callbacks.erase(state.seh_callbacks.begin() + i);
    for (size_t i = state.catches.size(); i-- > 0;) {
        Catch *h = state.catches[i];
        if (!c || h->frame->cpu == c) {
            state.catches.erase(state.catches.begin() + i);
            if (h->exception->record)
                heap_free(h->exception->record);
            delete h->exception;
            delete h;
        }
    }
    for (size_t i = state.frames.size(); i-- > 0;)
        if (!c || state.frames[i]->cpu == c) {
            delete state.frames[i];
            state.frames.erase(state.frames.begin() + i);
        }
    state.landing = nullptr;
    state.unwinding = 0;
}
#ifdef POPM_TESTING
uint32_t recomp_msvc_test_frame_count(X86 *c) {
    uint32_t count = 0;
    for (const Frame *f : state.frames)
        if (!c || f->cpu == c)
            ++count;
    return count;
}
#endif
} // extern C

void msvc_eh_register() {
    const ImportShim shims[] = {
        {"msvcr80.dll", "_CxxThrowException", 2, throw_exception},
        {"msvcr80.dll", "__CxxFrameHandler3", ARGC_CDECL, frame_handler3},
        {"msvcr80.dll", "_except_handler4_common", ARGC_CDECL, pass},
        {"msvcr80.dll", "_XcptFilter", ARGC_CDECL, zero},
        {"msvcr80.dll", "?uncaught_exception@std@@YA_NXZ", ARGC_CDECL, uncaught},
        {"msvcr80.dll", "??0exception@std@@QAE@XZ", 0, exception_default},
        {"msvcr80.dll", "??0exception@std@@QAE@ABQBD@Z", 1, exception_message},
        {"msvcr80.dll", "??0exception@std@@QAE@ABQBDH@Z", 2, exception_borrowed},
        {"msvcr80.dll", "??0exception@std@@QAE@ABV01@@Z", 1, exception_copy},
        {"msvcr80.dll", "??4exception@std@@QAEAAV01@ABV01@@Z", 1, exception_assign},
        {"msvcr80.dll", "??1exception@std@@UAE@XZ", 0, exception_destruct},
        {"msvcr80.dll", "??_Gexception@std@@UAEPAXI@Z", 1, exception_delete},
        {"msvcr80.dll", "??_Eexception@std@@UAEPAXI@Z", 1, exception_delete},
        {"msvcr80.dll", "?what@exception@std@@UBEPBDXZ", 0, exception_what},
        {"msvcr80.dll", "??1type_info@@UAE@XZ", 0, typeinfo_destruct},
        {"msvcr80.dll", "??_Gtype_info@@UAEPAXI@Z", 1, typeinfo_delete},
    };
    imports_register(shims, sizeof shims / sizeof shims[0]);
    imports_register_data_init("msvcr80.dll", "??_7exception@std@@6B@", 8, exception_vtable, 4);
    imports_register_data_init("msvcr80.dll", "??_7type_info@@6B@", 4, typeinfo_vtable, 4);
    imports_register_data_init("msvcr80.dll", "__recomp_exception_unknown", 18, init_unknown);
}
