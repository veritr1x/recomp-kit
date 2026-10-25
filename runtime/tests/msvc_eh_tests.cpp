// Synthetic VC8 x86 metadata, exercised through the actual import/callback ABI.
#include "../msvc_eh.h"
#include "../imports.h"
#include "../memory.h"
#include "../intrinsics.h"
#include "../seh.h"
#include "../../platform/os.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
static X86 cpu;
static uint32_t catch_fn, cleanup_fn, copy_fn, destructor_fn, throw_fn;
static uint32_t outer_reg, inner_reg, expected_reg, expected_value, expected_reference;
static uint32_t object, second_object, throw_info, type, catchable_type, continuation;
static uint32_t catches, cleanups, copies, destructions;
static bool rethrowing, new_throw, throw_in_cleanup;
static const char *fatal_mode;
static jmp_buf callback_escape;
extern "C" void msvc_fixture_load(void);
extern "C" void msvc_fixture_run(X86 *);

extern "C" void recomp_call(X86 *c, uint32_t address) {
    if (imports_dispatch(c, address))
        return;
    // This is the synthetic equivalent of the translated B8/FF25 handler.
    if (gm_valid(address, 11) && rd8(address) == 0xb8 && rd8(address + 5) == 0xff &&
        rd8(address + 6) == 0x25) {
        c->r[R_EAX] = rd32(address + 1);
        imports_dispatch(c, rd32(rd32(address + 7)));
        return;
    }
    std::fprintf(stderr, "missing test dispatch %08x\n", address);
    std::abort();
}
extern "C" void recomp_jump(X86 *c, uint32_t address) {
    recomp_call(c, address);
}
extern "C" int32_t recomp_index_of(uint32_t) {
    return -1;
}
extern "C" int recomp_is_call_return(uint32_t) {
    return 0;
}

static uint32_t alloc(uint32_t size = 256) {
    auto p = heap_alloc(size, true);
    CHECK(p);
    return p;
}
static uint32_t invoke(const char *name, std::initializer_list<uint32_t> args = {},
                       uint32_t ecx = 0) {
    if (ecx)
        cpu.r[R_ECX] = ecx;
    return guest_call(&cpu, imports_resolve("msvcr80.dll", name), args.begin(), int(args.size()));
}
[[noreturn]] static void raise(uint32_t value, uint32_t info) {
    cpu.r[R_ESP] -= 12;
    wr32(cpu.r[R_ESP], continuation);
    wr32(cpu.r[R_ESP] + 4, value);
    wr32(cpu.r[R_ESP] + 8, info);
    imports_dispatch(&cpu, throw_fn);
    CHECK(false);
    std::abort();
}
static void cleanup(X86 *c) {
    ++cleanups;
    CHECK(c->r[R_EBP] == inner_reg + 12 || c->r[R_EBP] == outer_reg + 12);
    CHECK(invoke("?uncaught_exception@std@@YA_NXZ") == 1);
    if (throw_in_cleanup)
        raise(object, throw_info);
    set_eax(c, 0);
}
static void destructor(X86 *c) {
    ++destructions;
    CHECK(c->r[R_ECX] == object || c->r[R_ECX] == second_object);
    CHECK(invoke("?uncaught_exception@std@@YA_NXZ") == 1);
    set_eax(c, 0xdeadbeef);
}
static void copy(X86 *c) {
    ++copies;
    CHECK(invoke("?uncaught_exception@std@@YA_NXZ") == 1);
    const uint32_t source = arg(c, 0), dest = c->r[R_ECX];
    wr32(dest, rd32(source));
    set_eax(c, dest);
}
static void caught(X86 *c) {
    ++catches;
    CHECK(invoke("?uncaught_exception@std@@YA_NXZ") == 0);
    CHECK(c->r[R_EBP] == expected_reg + 12);
    if (expected_reference)
        CHECK(rd32(expected_reg + 12 - 64) == expected_reference);
    else if (expected_value)
        CHECK(rd32(expected_reg + 12 - 64) == expected_value);
    if ((rethrowing || new_throw) && expected_reg == inner_reg) {
        expected_reg = outer_reg;
        if (new_throw) {
            expected_value = rd32(second_object);
            raise(second_object, throw_info);
        }
        raise(0, 0);
    }
    if (fatal_mode && !std::strcmp(fatal_mode, "try-in-catch")) {
        wr32(expected_reg + 8, 2);
        raise(object, throw_info);
    }
    // A funclet may change its scratch ESP, but the parent's saved ESP survives.
    c->r[R_ESP] -= 32;
    set_eax(c, continuation);
}
static void callback_throw(X86 *) {
    raise(object, throw_info);
}
static void crt_caught(X86 *c) {
    ++catches;
    const uint32_t copied = outer_reg + 12 - 64;
    CHECK(!std::strcmp((const char *)gm_ptr(rd32(copied + 4)), "string position"));
    // The message in a by-value exception is its own inline string, rather
    // than a pointer into the reserved throwing object that will be reused.
    CHECK(rd32(copied + 4) == copied + 16 && rd32(copied + 32) == 15);
    set_eax(c, continuation);
}
static void abandoned_callback(X86 *) {
    recomp_callback_truncate(0);
    longjmp(callback_escape, 1);
}
static void foreign_cleanup(X86 *c) {
    const uint32_t record = arg(c, 0), context = arg(c, 2);
    CHECK(arg(c, 1) == inner_reg && rd32(record) == 0xe06d7363 && (rd32(record + 4) & 2));
    CHECK(context && gm_valid(context, 0x2cc) && rd32(context) == 0x10003);
    CHECK(rd32(context + 0xc4) == c->r[R_ESP] + 20 && rd32(context + 0xb4) == inner_reg + 12);
    set_eax(c, 1);
}

static uint32_t info(uint32_t handler_type, bool tries = true) {
    const uint32_t data = alloc(), unwind = data + 40, trymap = data + 80, handlers = data + 144;
    wr32(data, 0x19930522);
    wr32(data + 4, 3);
    wr32(data + 8, unwind);
    wr32(data + 12, tries ? 1 : 0);
    wr32(data + 16, trymap);
    wr32(unwind, 0xffffffff);
    wr32(unwind + 4, cleanup_fn);
    wr32(unwind + 8, 0);
    wr32(unwind + 12, cleanup_fn);
    wr32(unwind + 16, 1);
    wr32(unwind + 20, cleanup_fn);
    wr32(trymap, 0);
    wr32(trymap + 4, 1);
    wr32(trymap + 8, 2);
    wr32(trymap + 12, 1);
    wr32(trymap + 16, handlers);
    wr32(handlers + 4, handler_type);
    wr32(handlers + 8, uint32_t(-64));
    wr32(handlers + 12, catch_fn);
    return data;
}
static uint32_t stub(uint32_t data) {
    const uint32_t code = alloc(16), slot = alloc(4);
    wr8(code, 0xb8);
    wr32(code + 1, data);
    wr8(code + 5, 0xff);
    wr8(code + 6, 0x25);
    wr32(code + 7, slot);
    wr32(slot, imports_resolve("msvcr80.dll", "__CxxFrameHandler3"));
    return code;
}
static void publish(uint32_t reg, uint32_t next, uint32_t handler, int32_t state) {
    wr32(reg, next);
    wr32(reg + 4, handler);
    wr32(reg + 8, uint32_t(state));
    wr32(cpu.fs_base, reg);
    cpu.r[R_EBP] = reg + 12;
    cpu.r[R_ESP] = reg - 256;
}
static void reset() {
    recomp_seh_reset(&cpu);
    cpu = {};
    cpu.fs_base = TEB_BASE;
    wr32(TEB_BASE, 0xffffffff);
    wr32(TEB_BASE + 4, STACK_TOP);
    wr32(TEB_BASE + 8, STACK_TOP - STACK_SIZE);
    outer_reg = STACK_TOP - 4096;
    inner_reg = outer_reg - 1024;
    expected_reg = outer_reg;
    expected_value = 42;
    expected_reference = 0;
    object = alloc(32);
    wr32(object, 42);
    second_object = alloc(16);
    wr32(second_object, 79);
    type = alloc(64);
    std::memcpy(gm_ptr(type + 8), ".?AVSynthetic@@", 15);
    throw_info = alloc();
    catchable_type = throw_info + 32;
    wr32(throw_info + 4, destructor_fn);
    wr32(throw_info + 12, throw_info + 80);
    wr32(throw_info + 80, 1);
    wr32(throw_info + 84, catchable_type);
    wr32(catchable_type, 1);
    wr32(catchable_type + 4, type);
    wr32(catchable_type + 12, 0xffffffff);
    wr32(catchable_type + 20, 4);
    continuation = alloc(16);
    wr8(continuation, 0xc3);
    catches = cleanups = copies = destructions = 0;
    rethrowing = new_throw = throw_in_cleanup = false;
}
static void run_catch(unsigned mode) {
    reset();
    uint32_t catch_type = type;
    if (mode == 1) {
        catch_type = 0;
        expected_value = 0;
    }
    uint32_t data = info(catch_type), handler = stub(data);
    if (mode == 2) {
        wr32(data + 144, 8);
        wr32(catchable_type, 2); // by-reference-only catchable type
        expected_reference = object;
    }
    if (mode == 3) {
        wr32(catchable_type, 0);
        wr32(catchable_type + 8, 4);
        wr32(object + 4, 58);
        wr32(catchable_type + 24, copy_fn);
        expected_value = 58;
    }
    if (mode == 4) {
        const uint32_t base = alloc(32);
        wr32(object, base);
        wr32(catchable_type + 8, 4);
        expected_value = base + 4;
    }
    if (mode == 5) {
        wr32(catchable_type, 0);
        wr32(catchable_type + 12, 4);
        const uint32_t vb = alloc(4);
        wr32(vb, 8);
        wr32(object + 4, vb);
        wr32(object + 12, 67);
        expected_value = 67;
    }
    if (mode == 6) {
        const uint32_t different = alloc(64), equivalent = alloc(64);
        std::memcpy(gm_ptr(different + 8), ".?AVOther@@", 11);
        std::memcpy(gm_ptr(equivalent + 8), gm_ptr(type + 8), 15);
        wr32(data + 80 + 12, 2);
        wr32(data + 144 + 4, different);
        wr32(data + 160 + 4, equivalent);
        wr32(data + 160 + 8, uint32_t(-64));
        wr32(data + 160 + 12, catch_fn);
        wr32(data + 144 + 12,
             imports_alloc_trampoline("tests.dll", "wrong-catch", callback_throw, 0));
    }
    publish(outer_reg, 0xffffffff, handler, 1);
    jmp_buf *env = recomp_msvc_frame_enter(&cpu, handler, data);
    if (!setjmp(*env))
        raise(object, throw_info);
    CHECK(recomp_msvc_frame_land(&cpu) == continuation);
    CHECK(cpu.r[R_ESP] == outer_reg - 256 && cpu.r[R_EBP] == outer_reg + 12);
    CHECK(cpu.r[R_EAX] == continuation && rd32(outer_reg + 8) == 0);
    CHECK(catches == 1 && destructions == 1 && cleanups == 1 && copies == (mode == 3 ? 1u : 0u));
    CHECK(recomp_callback_depth() == 0 && recomp_msvc_test_frame_count(&cpu) == 1);
    wr32(cpu.fs_base, 0xffffffff);
    recomp_msvc_frame_leave(&cpu);
    CHECK(recomp_msvc_test_frame_count(&cpu) == 0);
}
static void crt_string_exception() {
    reset();
    std::memcpy(gm_ptr(type + 8), ".?AVout_of_range@std@@", 23);
    const uint32_t data = info(type), handler = stub(data);
    wr32(data + 144 + 12, imports_alloc_trampoline("tests.dll", "crt catch", crt_caught, 0));
    const uint32_t str = alloc(28);
    wr32(str + 24, 15);
    publish(outer_reg, 0xffffffff, handler, 0);
    jmp_buf *env = recomp_msvc_frame_enter(&cpu, handler, data);
    if (!setjmp(*env)) {
        cpu.r[R_ECX] = str;
        cpu.r[R_ESP] -= 8;
        wr32(cpu.r[R_ESP], continuation);
        wr32(cpu.r[R_ESP] + 4, 1);
        const uint32_t at = imports_resolve(
            "msvcp80.dll",
            "?at@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAADI@Z");
        CHECK(at);
        imports_dispatch(&cpu, at);
        CHECK(false);
    }
    CHECK(recomp_msvc_frame_land(&cpu) == continuation && catches == 1);
    wr32(cpu.fs_base, 0xffffffff);
    recomp_msvc_frame_leave(&cpu);
}
static void nested(bool rethrow, bool fresh = false) {
    reset();
    rethrowing = rethrow;
    new_throw = fresh;
    const uint32_t outer_info = info(type), outer_handler = stub(outer_info);
    const uint32_t inner_info = info(type, rethrow || fresh), inner_handler = stub(inner_info);
    publish(outer_reg, 0xffffffff, outer_handler, 0);
    jmp_buf *outer_env = recomp_msvc_frame_enter(&cpu, outer_handler, outer_info);
    if (!setjmp(*outer_env)) {
        publish(inner_reg, outer_reg, inner_handler, 1);
        if (rethrow || fresh) {
            expected_reg = inner_reg;
            jmp_buf *inner_env = recomp_msvc_frame_enter(&cpu, inner_handler, inner_info);
            if (setjmp(*inner_env)) {
                CHECK(false);
                std::abort();
            }
        }
        raise(object, throw_info);
    }
    CHECK(recomp_msvc_frame_land(&cpu) == continuation);
    CHECK(rd32(cpu.fs_base) == outer_reg && cleanups == 2 && destructions == (fresh ? 2u : 1u));
    CHECK(catches == ((rethrow || fresh) ? 2u : 1u) && recomp_callback_depth() == 0);
    CHECK(recomp_msvc_test_frame_count(&cpu) == 1);
    wr32(cpu.fs_base, 0xffffffff);
    recomp_msvc_frame_leave(&cpu);
}
static void exception_object() {
    reset();
    cpu.r[R_ESP] = outer_reg - 1024;
    const uint32_t message = alloc(32), ref = alloc(4), first = alloc(12), second = alloc(12);
    std::memcpy(gm_ptr(message), "synthetic exception", 20);
    wr32(ref, message);
    CHECK(invoke("??0exception@std@@QAE@ABQBD@Z", {ref}, first) == first);
    CHECK(rd32(first + 8) == 1 && rd32(first + 4) != message);
    const uint32_t vf = rd32(first), locator = rd32(vf - 4);
    CHECK(vf == imports_alloc_data("msvcr80.dll", "??_7exception@std@@6B@"));
    CHECK(!std::strcmp((char *)gm_ptr(rd32(locator + 12) + 8), ".?AVexception@std@@"));
    CHECK(invoke("??0exception@std@@QAE@ABV01@@Z", {first}, second) == second);
    CHECK(rd32(first + 4) != rd32(second + 4));
    CHECK(!std::strcmp((char *)gm_ptr(invoke("?what@exception@std@@UBEPBDXZ", {}, second)),
                       "synthetic exception"));
    const uint32_t owned = rd32(first + 4);
    invoke("??1exception@std@@UAE@XZ", {}, first);
    CHECK(!heap_owns(owned));
    invoke("??1exception@std@@UAE@XZ", {}, second);
    invoke("??0exception@std@@QAE@XZ", {}, first);
    CHECK(!std::strcmp((char *)gm_ptr(invoke("?what@exception@std@@UBEPBDXZ", {}, first)),
                       "Unknown exception"));
    CHECK(invoke("_XcptFilter", {0, 0}) == 0);
    CHECK(invoke("_except_handler4_common", {0, 0, 0, 0, 0, 0}) == 1);
}
static void longjmp_retirement() {
    reset();
    cpu.r[R_ESP] = outer_reg + 64;
    const uint32_t buffer = alloc(128);
    wr32(cpu.r[R_ESP], continuation);
    wr32(cpu.r[R_ESP] + 4, buffer);
    jmp_buf *env = recomp_setjmp_prepare(&cpu);
    if (!setjmp(*env)) {
        recomp_setjmp_return(&cpu, 0);
        const uint32_t data = info(type), handler = stub(data);
        publish(outer_reg, 0xffffffff, handler, 0);
        (void)recomp_msvc_frame_enter(&cpu, handler, data);
        CHECK(recomp_msvc_test_frame_count(&cpu) == 1);
        cpu.r[R_ESP] -= 12;
        wr32(cpu.r[R_ESP] + 4, buffer);
        wr32(cpu.r[R_ESP] + 8, 7);
        recomp_longjmp(&cpu);
        CHECK(false);
    } else
        recomp_setjmp_return(&cpu, (int32_t)cpu.r[R_EAX]);
    CHECK(recomp_msvc_test_frame_count(&cpu) == 0 && cpu.r[R_EAX] == 7);
    CHECK(rd32(cpu.fs_base) == 0xffffffff);
    wr32(cpu.fs_base, 0xffffffff);
}
static void seh_record_dispatch() {
    reset();
    const uint32_t data = info(type), handler = stub(data), params = alloc(12);
    wr32(params, 0x19930520);
    wr32(params + 4, object);
    wr32(params + 8, throw_info);
    publish(outer_reg, 0xffffffff, handler, 1);
    jmp_buf *env = recomp_msvc_frame_enter(&cpu, handler, data);
    if (!setjmp(*env)) {
        // RaiseException itself is a guest CALL, not a native callback boundary.
        cpu.r[R_ESP] -= 20;
        wr32(cpu.r[R_ESP], continuation);
        wr32(cpu.r[R_ESP] + 4, 0xe06d7363);
        wr32(cpu.r[R_ESP] + 8, 1);
        wr32(cpu.r[R_ESP] + 12, 3);
        wr32(cpu.r[R_ESP] + 16, params);
        imports_dispatch(&cpu, imports_resolve("KERNEL32.dll", "RaiseException"));
        CHECK(false);
    }
    CHECK(recomp_msvc_frame_land(&cpu) == continuation);
    CHECK(catches == 1 && destructions == 1 && recomp_callback_depth() == 0);
    wr32(cpu.fs_base, 0xffffffff);
    recomp_msvc_frame_leave(&cpu);
}
static void mixed_registration(bool observe = false) {
    reset();
    const uint32_t data = info(type), handler = stub(data);
    publish(outer_reg, 0xffffffff, handler, 0);
    jmp_buf *env = recomp_msvc_frame_enter(&cpu, handler, data);
    if (!setjmp(*env)) {
        publish(inner_reg, outer_reg,
                observe ? imports_alloc_trampoline("tests.dll", "foreign-cleanup", foreign_cleanup,
                                                   ARGC_CDECL)
                        : imports_resolve("msvcr80.dll", "_except_handler4_common"),
                0);
        raise(object, throw_info);
    }
    CHECK(recomp_msvc_frame_land(&cpu) == continuation);
    CHECK(catches == 1 && cleanups == 0 && destructions == 1 && rd32(cpu.fs_base) == outer_reg);
    wr32(cpu.fs_base, 0xffffffff);
    recomp_msvc_frame_leave(&cpu);
}
static void data_arena_reset() {
    const uint32_t old = imports_alloc_data("msvcr80.dll", "??_7exception@std@@6B@");
    CHECK(old);
    mem_init();
    CHECK(!imports_data_address("msvcr80.dll", "??_7exception@std@@6B@"));
    // Occupy the old arena location: heap membership alone cannot validate it.
    CHECK(alloc(4096));
    const uint32_t current = imports_alloc_data("msvcr80.dll", "??_7exception@std@@6B@");
    CHECK(current && current != old && rd32(current) && rd32(current - 4));
}
static void emitted_function() {
    reset();
    msvc_fixture_load();
    constexpr uint32_t base = 0x0d02a000;
    wr32(base + 0x1100, imports_resolve("msvcr80.dll", "__CxxFrameHandler3"));
    wr32(base + 0x1104, throw_fn);
    wr32(base + 0x1120, object);
    wr32(base + 0x1124, throw_info);
    wr32(base + 0x1080 + 12, catch_fn);
    wr32(base + 0x1040 + 4, cleanup_fn);
    wr32(base + 0x1040 + 12, cleanup_fn);
    expected_value = 0;
    continuation = base + 0x60;
    cpu.r[R_EBP] = 0x12345678;
    cpu.r[R_ESP] = outer_reg + 16;
    wr32(cpu.r[R_ESP], base + 0x400);
    msvc_fixture_run(&cpu);
    CHECK(catches == 1 && cleanups == 1 && destructions == 1);
    CHECK(cpu.r[R_EAX] == 77 && cpu.r[R_EBP] == 0x12345678);
    CHECK(cpu.r[R_ESP] == outer_reg + 20 && cpu.eip == base + 0x400);
    CHECK(recomp_msvc_test_frame_count(&cpu) == 0 && rd32(cpu.fs_base) == 0xffffffff);
}
static void fatal(const char *mode) {
    reset();
    fatal_mode = mode;
    uint32_t data = info(type), handler = stub(data);
    if (!std::strcmp(mode, "uncaught")) {
        publish(outer_reg, 0xffffffff, handler, -1);
        raise(object, throw_info);
    }
    publish(outer_reg, 0xffffffff, handler, 1);
    if (!std::strcmp(mode, "missing"))
        raise(object, throw_info);
    if (!std::strcmp(mode, "try-in-catch")) {
        wr32(data + 12, 2);
        // Second try occurs in the first catch's state range.
        wr32(data + 100, 2);
        wr32(data + 104, 2);
        wr32(data + 108, 2);
        wr32(data + 112, 1);
        wr32(data + 116, data + 144);
    }
    jmp_buf *env = recomp_msvc_frame_enter(&cpu, handler, data);
    if (setjmp(*env)) {
        (void)recomp_msvc_frame_land(&cpu);
        CHECK(false);
        return;
    }
    if (!std::strcmp(mode, "stale-esp"))
        cpu.r[R_ESP] = outer_reg - 64;
    if (!std::strcmp(mode, "stale-handler"))
        wr32(outer_reg + 4, stub(data));
    if (!std::strcmp(mode, "double-throw"))
        throw_in_cleanup = true;
    if (!std::strcmp(mode, "foreign-after-truncate")) {
        if (!setjmp(callback_escape)) {
            recomp_msvc_seh_enter(&cpu);
            const uint32_t callback =
                imports_alloc_trampoline("tests.dll", "abandoned", abandoned_callback, 0);
            guest_call(&cpu, callback, nullptr, 0);
            CHECK(false);
        }
    }
    if (!std::strcmp(mode, "foreign") || !std::strcmp(mode, "foreign-after-truncate")) {
        const uint32_t callback =
            imports_alloc_trampoline("tests.dll", "foreign", callback_throw, 0);
        guest_call(&cpu, callback, nullptr, 0);
        return;
    }
    raise(object, throw_info);
}
int main(int argc, char **argv) {
    mem_init();
    imports_init();
    throw_fn = imports_resolve("msvcr80.dll", "_CxxThrowException");
    catch_fn = imports_alloc_trampoline("tests.dll", "catch", caught, 0);
    cleanup_fn = imports_alloc_trampoline("tests.dll", "cleanup", cleanup, 0);
    copy_fn = imports_alloc_trampoline("tests.dll", "copy", copy, 1);
    destructor_fn = imports_alloc_trampoline("tests.dll", "destructor", destructor, 0);
    if (argc > 1) {
        CHECK(std::freopen(argv[2], "w", stderr));
        fatal(argv[1]);
        return 1;
    }
    for (unsigned mode = 0; mode < 7; ++mode)
        run_catch(mode);
    nested(false);
    nested(true);
    nested(false, true);
    exception_object();
    longjmp_retirement();
    seh_record_dispatch();
    mixed_registration();
    mixed_registration(true);
    emitted_function();
    crt_string_exception();
    data_arena_reset();
    char executable[4096];
    CHECK(os_exe_path(executable, sizeof executable) == 0);
    const char *modes[] = {"uncaught",     "missing", "stale-esp",    "stale-handler",
                           "double-throw", "foreign", "try-in-catch", "foreign-after-truncate"};
    const char *messages[] = {"Synthetic",          "no live checkpoint", "stale checkpoint",
                              "stale checkpoint",   "terminates",         "foreign guest_call",
                              "try inside a catch", "foreign guest_call"};
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/msvc-fatal-XXXXXX", os_temp_dir());
        int fd = os_mkstemp(path);
        CHECK(fd >= 0);
        if (fd < 0)
            continue;
        os_fd_close(fd);
        const char *args[] = {executable, modes[i], path, nullptr};
        int64_t pid = 0;
        int code = -1;
        CHECK(os_spawn(args, &pid) == 0 && os_wait(pid, &code) == 0 && code == 134);
        FILE *file = std::fopen(path, "rb");
        char log[4096] = {};
        CHECK(file);
        if (file) {
            std::fread(log, 1, sizeof log - 1, file);
            std::fclose(file);
        }
        CHECK(std::strstr(log, messages[i]));
        os_unlink(path);
    }
    mem_shutdown();
    std::printf("MSVC EH: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
