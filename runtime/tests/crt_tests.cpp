// VC8 exports through the guest import ABI, without a proprietary game image.
#include "../imports.h"
#include "../crt_internal.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../../platform/os.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cmath>
#include <array>
#include <string>
#include <vector>
static std::string file_root;
static int file_resolver(const char *relative, int, char *out, size_t cap) {
    if (std::strcmp(relative, "crt-test.txt") && std::strcmp(relative, "crt-renamed.txt"))
        return 0;
    const std::string path = file_root + "/" + relative;
    if (path.size() >= cap)
        return 0;
    std::memcpy(out, path.c_str(), path.size() + 1);
    return 1;
}
static uint32_t guest_text(const char *s) {
    const uint32_t p = heap_alloc(uint32_t(std::strlen(s) + 1), true);
    std::memcpy(gm_ptr(p), s, std::strlen(s) + 1);
    return p;
}

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
static X86 cpu;
static uint32_t call(const char *name, std::initializer_list<uint32_t> args = {},
                     const char *dll = "msvcr80.dll") {
    const uint32_t target = imports_resolve(dll, name);
    CHECK(target);
    if (!target)
        return 0;
    const uint32_t before = cpu.r[R_ESP];
    const uint32_t esp = before - 4 * (uint32_t(args.size()) + 1), ret = IMAGE_BASE + 0x1000;
    wr32(esp, ret);
    uint32_t i = 0;
    for (auto value : args)
        wr32(esp + 4 + 4 * i++, value);
    cpu.r[R_ESP] = esp;
    CHECK(imports_dispatch(&cpu, target));
    CHECK(cpu.r[R_ESP] ==
              esp + 4 + (imports_argc(target) == ARGC_CDECL ? 0 : 4 * uint32_t(args.size())) &&
          cpu.eip == ret);
    cpu.r[R_ESP] = before;
    return cpu.r[R_EAX];
}
static void memory_tests() {
    const uint32_t source = heap_alloc(32, true), dest = heap_alloc(32, true);
    std::memcpy(gm_ptr(source), "sample", 7);
    CHECK(call("memcpy", {dest, source, 7}) == dest);
    CHECK(!std::memcmp(gm_ptr(dest), "sample", 7));
    CHECK(call("memmove", {dest + 1, dest, 6}) == dest + 1);
    CHECK(!std::memcmp(gm_ptr(dest), "ssample", 7));
    CHECK(call("memset", {dest, 0x7f, 32}) == dest && rd8(dest + 31) == 0x7f);
    CHECK(call("memchr", {source, 'p', 6}) == source + 3);
    CHECK(call("memcmp", {source, source, 32}) == 0);
    CHECK(call("memcpy", {0, 0, 0}) == 0);
    heap_free(source);
    heap_free(dest);
}
static void allocation_tests() {
    const uint32_t p = call("malloc", {32});
    CHECK(p && heap_owns(p));
    wr32(p, 0x12345678);
    CHECK(call("_msize", {p}) == 32);
    const uint32_t bigger = call("realloc", {p, 128});
    CHECK(bigger && rd32(bigger) == 0x12345678);
    call("free", {bigger});
    CHECK(!heap_owns(bigger));
    const uint32_t zero = call("calloc", {8, 4});
    CHECK(zero);
    if (zero) {
        for (uint32_t i = 0; i < 32; ++i)
            CHECK(rd8(zero + i) == 0);
        call("free", {zero});
    }
    CHECK(!call("calloc", {0x80000001, 2}));
    CHECK(rd32(call("_errno")) == 12);
    CHECK(!call("_aligned_malloc", {0xfffffff8, 4096}));
    CHECK(!call("_aligned_malloc", {16, 3}) && rd32(call("_errno")) == 22);
    const uint32_t aligned = call("_aligned_offset_malloc", {35, 64, 3});
    CHECK(aligned && (aligned + 3) % 64 == 0);
    if (aligned) {
        wr32(aligned, 0xaabbccdd);
        CHECK(call("_aligned_msize", {aligned, 64, 3}) == 35);
        const uint32_t resized = call("_aligned_offset_realloc", {aligned, 80, 64, 3});
        CHECK(resized && (resized + 3) % 64 == 0 && rd32(resized) == 0xaabbccdd);
        call("_aligned_free", {resized});
    }
    call("_dosmaperr", {32});
    CHECK(rd32(call("_errno")) == 13 && rd32(call("__doserrno")) == 32);
    call("_dosmaperr", {193});
    CHECK(rd32(call("_errno")) == 8);
    call("_dosmaperr", {0});
    CHECK(rd32(call("__doserrno")) == 0);
    call("srand", {1});
    CHECK(call("rand") == 41 && call("rand") == 18467 && call("rand") == 6334);
}
static void compare_int(X86 *c) {
    const int32_t left = int32_t(rd32(arg(c, 0))), right = int32_t(rd32(arg(c, 1)));
    set_eax(c, left < right ? UINT32_MAX : left > right ? 1 : 0);
}
static void sorting_tests() {
    const uint32_t values = heap_alloc(40, true), key = heap_alloc(4, true);
    const int32_t original[] = {8, -4, 5, 8, 0, -7, 9, 100, 2, -1};
    std::memcpy(gm_ptr(values), original, sizeof original);
    const uint32_t compare =
        imports_alloc_trampoline("tests.dll", "compare", compare_int, ARGC_CDECL);
    call("qsort", {values, 10, 4, compare});
    const int32_t expected[] = {-7, -4, -1, 0, 2, 5, 8, 8, 9, 100};
    CHECK(!std::memcmp(gm_ptr(values), expected, sizeof expected));
    wr32(key, 5);
    CHECK(call("bsearch", {key, values, 10, 4, compare}) == values + 5 * 4);
    wr32(key, 17);
    CHECK(!call("bsearch", {key, values, 10, 4, compare}));
    call("qsort", {0, 0, 4, 0});
    CHECK(!call("bsearch", {0, 0, 0, 4, 0}));
    heap_free(values);
    heap_free(key);
}
static void string_tests() {
    const uint32_t text = heap_alloc(128, true), dest = heap_alloc(128, true),
                   end = heap_alloc(4, true);
    std::memcpy(gm_ptr(text), "Abc token,rest", 15);
    CHECK(call("strlen", {text}) == 14);
    CHECK(call("strnlen", {text, 3}) == 3 && call("strnlen", {0, 0}) == 0);
    CHECK(call("strcpy", {dest, text}) == dest &&
          !std::strcmp((char *)gm_ptr(dest), "Abc token,rest"));
    CHECK(call("_stricmp", {text, dest}) == 0);
    CHECK(call("strchr", {text, ','}) == text + 9 && call("strrchr", {text, 0}) == text + 14);
    CHECK(call("strstr", {text, text + 4}) == text + 4);
    CHECK(call("strncpy", {dest, text, 3}) == dest && rd8(dest + 2) == 'c');
    wr8(dest, 0);
    CHECK(call("strcat", {dest, text + 4}) == dest);
    CHECK(!std::strcmp((char *)gm_ptr(dest), "token,rest"));
    CHECK(call("strcpy_s", {dest, 4, text}) == 34 && rd8(dest) == 0);
    CHECK(call("strncpy_s", {dest, 4, text, UINT32_MAX}) == 80);
    CHECK(!std::strcmp((char *)gm_ptr(dest), "Abc"));
    const uint32_t duplicate = call("_strdup", {text});
    CHECK(duplicate && duplicate != text);
    if (duplicate) {
        CHECK(!std::strcmp((char *)gm_ptr(duplicate), (char *)gm_ptr(text)));
        call("free", {duplicate});
    }
    const uint32_t wide = heap_alloc(64, true);
    for (uint32_t i = 0; i < 4; ++i)
        wr16(wide + i * 2, uint16_t(u"WIDE"[i]));
    CHECK(call("wcslen", {wide}) == 4);
    CHECK(call("wcscpy", {dest, wide}) == dest && rd16(dest + 6) == 'E');
    CHECK(call("_wcsicmp", {wide, dest}) == 0);
    std::memcpy(gm_ptr(text), "  -0x7fffffff tail", 19);
    CHECK(call("strtol", {text, end, 0}) == 0x80000001 && rd32(end) == text + 13);
    std::memcpy(gm_ptr(text), "-1", 3);
    call("_set_errno", {0});
    CHECK(call("strtoul", {text, end, 10}) == UINT32_MAX && rd32(call("_errno")) == 0);
    std::memcpy(gm_ptr(text), "9223372036854775808", 20);
    CHECK(call("_strtoi64", {text, end, 10}) == UINT32_MAX && cpu.r[R_EDX] == 0x7fffffff);
    CHECK(rd32(call("_errno")) == 34);
    heap_free(wide);
    heap_free(text);
    heap_free(dest);
    heap_free(end);
}
static std::array<uint32_t, 2> double_words(double value) {
    std::array<uint32_t, 2> words{};
    std::memcpy(words.data(), &value, 8);
    return words;
}
static double math1(const char *name, double value) {
    const auto bits = double_words(value);
    call(name, {bits[0], bits[1]});
    return fpop(&cpu);
}
static void math_tests() {
    cpu.fpu_cw = 0x037f;
    cpu.fpu_sw = 0;
    cpu.fpu_tag = 0xffff;
    cpu.fpu_top = 0;
    cpu.mxcsr_xor = 0;
    CHECK(math1("sqrt", 4) == 2);
    CHECK(std::fabs(math1("sin", 1.5707963267948966) - 1) < 1e-12);
    fpush(&cpu, 0);
    call("_CIsin");
    CHECK(fpop(&cpu) == 0);
    const auto input = double_words(1.5707963267948966);
    cpu.xmm[0][0] = input[0];
    cpu.xmm[0][1] = input[1];
    call("__libm_sse2_sin");
    double output;
    std::memcpy(&output, cpu.xmm[0], 8);
    CHECK(std::fabs(output - 1) < 1e-12);
    const uint32_t control = heap_alloc(4, true), sse = heap_alloc(4, true);
    CHECK(call("_controlfp_s", {0, 0x100, 0x300}) == 0);
    CHECK((cpu.fpu_cw >> 10 & 3) == 1 && (sse_mxcsr(&cpu) >> 13 & 3) == 1);
    CHECK(call("_controlfp_s", {control, 0x20000, 0x30000}) == 0 && (cpu.fpu_cw >> 8 & 3) == 0);
    CHECK((rd32(control) & 0x30000) == 0x20000);
    CHECK(call("__control87_2", {0x200, 0x300, 0, sse}) == 1);
    CHECK((cpu.fpu_cw >> 10 & 3) == 1 && (sse_mxcsr(&cpu) >> 13 & 3) == 2);
    CHECK(call("_controlfp", {0, 0}) & 0x80000000);
    CHECK(call("_controlfp_s", {control, 0x1000000, 0x3000000}) == 0);
    CHECK((sse_mxcsr(&cpu) & 0x8040) == 0x8040);
    const uint16_t before = cpu.fpu_cw;
    const uint32_t old_sse = sse_mxcsr(&cpu);
    CHECK(call("_controlfp_s", {control, 0, 0x80000}) == 0);
    CHECK(cpu.fpu_cw == before && sse_mxcsr(&cpu) == old_sse);
    CHECK(call("_controlfp_s", {control, 0x80, 0x80}) == 22 && rd32(call("_errno")) == 22);
    CHECK(cpu.fpu_cw == before && sse_mxcsr(&cpu) == old_sse);
    call("_fpreset");
    CHECK(cpu.fpu_cw == 0x027f && sse_mxcsr(&cpu) == 0x1f80);
    CHECK(std::isnan(math1("sqrt", -1)) && (call("_statusfp") & 0x10));
    CHECK(call("_clearfp") & 0x10);
    CHECK(!(cpu.fpu_sw & 0x3f));
    heap_free(control);
    heap_free(sse);
}
static void stdio_tests() {
    const uint32_t dest = heap_alloc(1024, true), fmt = guest_text("%08x:%+d:%I64u:%.*s%n"),
                   s = guest_text("sample"), n = heap_alloc(4, true);
    CHECK(call("sprintf", {dest, fmt, 0xab, uint32_t(-17), 0xffffffff, 1, 3, s, n}) == 27);
    CHECK(!std::strcmp((const char *)gm_ptr(dest), "000000ab:-17:8589934591:sam") && rd32(n) == 27);
    const uint32_t word = guest_text("%s");
    CHECK(call("_snprintf", {0, 0, word, s}) == 6);
    std::memset(gm_ptr(dest), 0xaa, 16);
    CHECK(call("_snprintf", {dest, 3, word, s}) == UINT32_MAX);
    CHECK(!std::memcmp(gm_ptr(dest), "sam", 3) && rd8(dest + 3) == 0xaa);
    CHECK(call("_snprintf", {dest, 6, word, s}) == 6 && rd8(dest + 6) == 0xaa);
    const uint32_t f = guest_text("%f");
    CHECK(call("sprintf", {dest, f, 0, 0xfff80000}) == 9);
    CHECK(!std::strcmp((const char *)gm_ptr(dest), "-1.#IND00"));
    CHECK(call("sprintf", {dest, f, 0, 0x7ff80000}) == 8);
    CHECK(!std::strcmp((const char *)gm_ptr(dest), "1.#QNAN0"));
    const uint32_t va = heap_alloc(8, true);
    wr32(va, s);
    CHECK(call("_vsnprintf", {0, 0, word, va}) == 6);
    CHECK(call("_snprintf_s", {dest, 4, UINT32_MAX, word, s}) == UINT32_MAX &&
          !std::strcmp((const char *)gm_ptr(dest), "sam"));
    CHECK(call("sprintf_s", {dest, 4, word, s}) == UINT32_MAX && rd8(dest) == 0 &&
          rd32(call("_errno")) == 34);
    const uint32_t input = guest_text(" -13 0x2a 1.25 hello!"),
                   scan = guest_text("%d %i %lf %5s%n"), number = heap_alloc(32, true);
    CHECK(call("sscanf", {input, scan, number, number + 4, number + 8, number + 16, number + 24}) ==
          4);
    double v;
    const uint64_t bits = rd64(number + 8);
    std::memcpy(&v, &bits, 8);
    CHECK(rd32(number) == uint32_t(-13) && rd32(number + 4) == 42 && v == 1.25 &&
          !std::strcmp((const char *)gm_ptr(number + 16), "hello") && rd32(number + 24) == 20);
    const uint32_t denied = guest_text("crt-no-profile.txt"), mode = guest_text("wb");
    CHECK(call("fopen", {denied, mode}) == 0 && rd32(call("_errno")) == 13);
    file_root = std::string(os_temp_dir()) + "/recomp-crt-XXXXXX";
    CHECK(os_mkdtemp(file_root.data()) == 0);
    win32_set_file_ops(file_resolver, nullptr);
    const uint32_t filename = guest_text("crt-test.txt"), renamed = heap_alloc(64, true),
                   binary = guest_text("w+b"), stream = call("fopen", {filename, binary});
    CHECK(stream);
    std::vector<uint32_t> opened;
    for (unsigned i = 0; i < 128; ++i)
        opened.push_back(call("_open", {filename, 2 | 0x8000}));
    CHECK(call("fwrite", {s, 2, 3, stream}) == 3 && call("ftell", {stream}) == 6);
    CHECK(call("fseek", {stream, 0, 0}) == 0);
    CHECK(call("fread", {dest, 2, 3, stream}) == 3 && !std::memcmp(gm_ptr(dest), "sample", 6));
    CHECK(call("fgetc", {stream}) == UINT32_MAX && call("feof", {stream}));
    CHECK(call("ungetc", {'!', stream}) == '!' && call("fgetc", {stream}) == '!');
    CHECK(call("fclose", {stream}) == 0);
    for (auto fd : opened)
        CHECK(call("_close", {fd}) == 0);
    for (uint32_t i = 0; i < 16; ++i)
        wr16(renamed + i * 2, uint16_t(u"crt-renamed.txt"[i]));
    const uint32_t oldwide = heap_alloc(64, true);
    for (uint32_t i = 0; i < 13; ++i)
        wr16(oldwide + i * 2, uint16_t(u"crt-test.txt"[i]));
    CHECK(call("_wrename", {oldwide, renamed}) == 0);
    CHECK(call("_wremove", {renamed}) == 0);
    const uint32_t textmode = guest_text("w+"), textstream = call("fopen", {filename, textmode}),
                   data = guest_text("12x 1.25hello -1.#IND00\n");
    CHECK(textstream);
    CHECK(call("fwrite", {data, 1, 24, textstream}) == 24);
    CHECK(call("fseek", {textstream, 0, 0}) == 0);
    const uint32_t numeric = guest_text("%d"), floating = guest_text("%lf");
    CHECK(call("fscanf", {textstream, numeric, number}) == 1 && rd32(number) == 12);
    CHECK(call("fgetc", {textstream}) == 'x');
    CHECK(call("fscanf", {textstream, floating, number}) == 1);
    CHECK(call("fgetc", {textstream}) == 'h');
    for (int i = 0; i < 4; ++i)
        call("fgetc", {textstream});
    CHECK(call("fscanf", {textstream, floating, number}) == 1);
    const uint64_t nanbits = rd64(number);
    double nan;
    std::memcpy(&nan, &nanbits, 8);
    CHECK(std::isnan(nan) && std::signbit(nan));
    CHECK(call("fgetc", {textstream}) == 10);
    call("fclose", {textstream});
    call("remove", {filename});
    win32_set_file_ops(nullptr, nullptr);
    CHECK(os_rmdir(file_root.c_str()) == 0);
    CHECK(call("__iob_func") == imports_alloc_data("msvcr80.dll", "_iob"));
    CHECK(call("_fileno", {call("__iob_func") + 64}) == 2);
    for (uint32_t p : {dest, fmt, s, n, word, f, va, input, scan, number, denied, mode})
        heap_free(p);
    for (uint32_t p : {filename, renamed, binary, oldwide, textmode, data, numeric, floating})
        heap_free(p);
}
static uint32_t callback_count;
static void init_callback(X86 *c) {
    ++callback_count;
    set_eax(c, 7);
}
static void startup_tests() {
    const uint32_t cmd = call("GetCommandLineA", {}, "KERNEL32.dll"),
                   wcmd = call("GetCommandLineW", {}, "KERNEL32.dll");
    CHECK(rd32(imports_alloc_data("msvcr80.dll", "_acmdln")) == cmd);
    CHECK(rd32(imports_alloc_data("msvcr80.dll", "_wcmdln")) == wcmd);
    const uint32_t args = heap_alloc(12, true);
    CHECK(call("__getmainargs", {args, args + 4, args + 8, 0, 0}) == 0);
    CHECK(rd32(args) > 0 && rd32(rd32(args + 4)) && rd32(rd32(args + 8)));
    const uint32_t assignment = guest_text("CRT_TEST_VALUE=guest-only"),
                   key = guest_text("crt_test_value");
    CHECK(call("_putenv", {assignment}) == 0);
    CHECK(!std::strcmp((const char *)gm_ptr(call("getenv", {key})), "guest-only"));
    const uint32_t out = heap_alloc(64, true);
    CHECK(call("GetEnvironmentVariableA", {key, out, 64}, "KERNEL32.dll") == 10 &&
          !std::strcmp((const char *)gm_ptr(out), "guest-only"));
    const uint32_t clear = guest_text("CRT_TEST_VALUE=");
    CHECK(call("_putenv", {clear}) == 0 && !call("getenv", {key}));
    CHECK(call("tolower", {0xc0}) == 0xc0);
    const uint32_t locale = guest_text(".1252");
    CHECK(call("setlocale", {0, locale}));
    CHECK(call("tolower", {0xc0}) == 0xe0 && call("iswalpha", {0x391}));
    const uint32_t c = guest_text("C");
    CHECK(call("setlocale", {0, c}));
    CHECK(call("setlocale", {4, locale}));
    CHECK(call("tolower", {0xc0}) == 0xc0);
    CHECK(call("setlocale", {2, locale}));
    CHECK(call("tolower", {0x8a}) == 0x9a && call("toupper", {0xff}) == 0x9f &&
          call("towupper", {0xff}) == 0x178);
    CHECK(call("setlocale", {0, c}));
    CHECK(call("isalpha", {'A'}) && !call("isalpha", {0xc0}));
    const uint32_t table = call("__pctype_func");
    CHECK(rd16(table - 2) == 0 && (rd16(table + '9' * 2) & 4));
    const uint32_t other = TEB_BASE + 0x1000;
    const uint32_t first = call("_errno");
    cpu.fs_base = other;
    const uint32_t second = call("_errno");
    CHECK(first != second);
    call("_set_errno", {7});
    cpu.fs_base = TEB_BASE;
    CHECK(rd32(first) != 7);
    crt::thread_detach(other);
    CHECK(!heap_owns(second));
    const uint32_t cb = imports_alloc_trampoline("tests.dll", "initializer", init_callback,
                                                 ARGC_CDECL),
                   table_init = heap_alloc(12, true);
    wr32(table_init, cb);
    wr32(table_init + 8, cb);
    callback_count = 0;
    call("_initterm", {table_init, table_init + 12});
    CHECK(callback_count == 2);
    CHECK(call("_initterm_e", {table_init, table_init + 12}) == 7 && callback_count == 3);
    call("atexit", {cb});
    call("atexit", {cb});
    call("_cexit");
    CHECK(callback_count == 5);
    const uint32_t epoch = heap_alloc(8, true), tm = call("_gmtime64", {epoch});
    CHECK(tm && rd32(tm + 20) == 70 && rd32(tm + 16) == 0 && rd32(tm + 24) == 4);
    const uint32_t ascii = call("asctime", {tm});
    CHECK(ascii && !std::strcmp((const char *)gm_ptr(ascii), "Thu Jan  1 00:00:00 1970\n"));
    const uint32_t datefmt = guest_text("%Y-%m-%d");
    CHECK(call("strftime", {out, 64, datefmt, tm}) == 10 &&
          !std::strcmp((const char *)gm_ptr(out), "1970-01-01"));
    heap_free(epoch);
    heap_free(datefmt);
    call("_cexit");
    CHECK(callback_count == 5);
    for (uint32_t p : {args, assignment, key, out, clear, locale, c, table_init})
        heap_free(p);
}
static void cpp_tests() {
    const std::string cls = "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@",
                      wide_cls = "?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@";
    const uint32_t object = heap_alloc(28, true), other = heap_alloc(28, true),
                   text = guest_text("0123456789abcde"), wide = heap_alloc(32, true);
    auto method = [&](const std::string &name, std::initializer_list<uint32_t> args = {}) {
        cpu.r[R_ECX] = object;
        return call(name.c_str(), args, "msvcp80.dll");
    };
    CHECK(method("??0" + cls + "QAE@PBD@Z", {text}) == object);
    CHECK(rd32(object + 20) == 15 && rd32(object + 24) == 15 &&
          !std::memcmp(gm_ptr(object + 4), "0123456789abcde", 16));
    CHECK(method("?append@" + cls + "QAEAAV12@PBDI@Z", {object + 6, 8}) == object);
    CHECK(rd32(object + 20) == 23 && rd32(object + 24) >= 23);
    const uint32_t buffer = method("?c_str@" + cls + "QBEPBDXZ");
    CHECK(!std::strcmp((const char *)gm_ptr(buffer), "0123456789abcde23456789"));
    CHECK(method("?find@" + cls + "QBEIPBDI@Z", {text, 0}) == 0);
    CHECK(method("?find@" + cls + "QBEIDI@Z", {'9', 10}) == 22);
    CHECK(method("?rfind@" + cls + "QBEIDI@Z", {'9', UINT32_MAX}) == 22);
    method("?replace@" + cls + "QAEAAV12@IIPBDI@Z", {1, 3, buffer + 2, 4});
    CHECK(!std::strcmp((const char *)gm_ptr(rd32(object + 4)), "02345456789abcde23456789"));
    method("?substr@" + cls + "QBE?AV12@II@Z", {other, 2, 5});
    CHECK(rd32(other + 20) == 5 && !std::strcmp((const char *)gm_ptr(other + 4), "34545"));
    method("?erase@" + cls + "QAEAAV12@II@Z", {0, UINT32_MAX});
    CHECK(rd32(object + 20) == 0 && method("?empty@" + cls + "QBE_NXZ") == 1);
    method("??1" + cls + "QAE@XZ");
    CHECK(!heap_owns(buffer));
    cpu.r[R_ECX] = other;
    call(("??1" + cls + "QAE@XZ").c_str(), {}, "msvcp80.dll");
    for (uint32_t i = 0; i < 8; ++i)
        wr16(wide + i * 2, uint16_t(u"wide ABI"[i]));
    cpu.r[R_ECX] = object;
    call(("??0" + wide_cls + "QAE@PB_W@Z").c_str(), {wide}, "msvcp80.dll");
    CHECK(rd32(object + 20) == 8 && rd32(object + 24) >= 8);
    cpu.r[R_ECX] = object;
    const uint32_t ptr = call(("?c_str@" + wide_cls + "QBEPB_WXZ").c_str(), {}, "msvcp80.dll");
    CHECK(ptr == rd32(object + 4) && rd16(ptr + 14) == 'I');
    cpu.r[R_ECX] = object;
    call(("??1" + wide_cls + "QAE@XZ").c_str(), {}, "msvcp80.dll");
    for (uint32_t p : {object, other, text, wide})
        heap_free(p);
}
static uint32_t thread_errno, thread_calls;
static void crt_thread(X86 *c) {
    thread_errno = guest_call(c, imports_resolve("msvcr80.dll", "_errno"), nullptr, 0);
    CHECK(c->fpu_cw == 0x027f);
    wr32(thread_errno, 123);
    ++thread_calls;
    const uint32_t code = 37;
    guest_call(c, imports_resolve("msvcr80.dll", "_endthreadex"), &code, 1);
    CHECK(false);
}
static void thread_tests() {
    X86 *parent = loader_context();
    loader_init_context(parent);
    sched_set_guest_thread(true);
    const uint32_t worker = imports_alloc_trampoline("tests.dll", "CRT worker", crt_thread, 1),
                   args[] = {0, 0, worker, 0, 0, 0};
    const uint32_t handle =
        guest_call(parent, imports_resolve("msvcr80.dll", "_beginthreadex"), args, 6);
    CHECK(handle);
    const uint32_t wait[] = {handle, 1000};
    CHECK(guest_call(parent, imports_resolve("kernel32.dll", "WaitForSingleObject"), wait, 2) == 0);
    CHECK(thread_calls == 1 && !heap_owns(thread_errno));
    const uint32_t result = heap_alloc(4, true), query[] = {handle, result};
    CHECK(guest_call(parent, imports_resolve("kernel32.dll", "GetExitCodeThread"), query, 2) == 1 &&
          rd32(result) == 37);
    CHECK(recomp_callback_depth() == 0);
    guest_call(parent, imports_resolve("kernel32.dll", "CloseHandle"), &handle, 1);
    heap_free(result);
    CHECK(sched_drive_until_stopped(1.0));
    sched_set_guest_thread(false);
}
static void unicode_startup_tests() {
    os_setenv("RECOMP_GUEST_ARGS", "\"\xc3\xa9\xf0\x9f\x99\x82\"");
    win32_reset_command_line_for_test();
    const uint32_t out = heap_alloc(12, true);
    CHECK(call("__wgetmainargs", {out, out + 4, out + 8, 0, 0}) == 0);
    CHECK(rd32(out) == 2);
    const uint32_t argument = rd32(rd32(out + 4) + 4);
    CHECK(argument && rd16(argument) == 0xe9 && rd16(argument + 2) == 0xd83d &&
          rd16(argument + 4) == 0xde42 && rd16(argument + 6) == 0);
    const uint32_t assignment = heap_alloc(128, true), key = heap_alloc(64, true);
    const std::u16string value = u"CRT_\u00e9=\u00e9\U0001f642", name = u"CRT_\u00e9";
    for (uint32_t i = 0; i < value.size(); ++i)
        wr16(assignment + i * 2, value[i]);
    for (uint32_t i = 0; i < name.size(); ++i)
        wr16(key + i * 2, name[i]);
    CHECK(call("_wputenv", {assignment}) == 0);
    const uint32_t result = call("_wgetenv", {key});
    CHECK(result && rd16(result) == 0xe9 && rd16(result + 2) == 0xd83d &&
          rd16(result + 4) == 0xde42 && rd16(result + 6) == 0);
    const uint32_t narrow = guest_text("CRT_\xc3\xa9"), ansi = call("getenv", {narrow});
    CHECK(ansi && !std::strcmp((const char *)gm_ptr(ansi), "\xc3\xa9\xf0\x9f\x99\x82"));
    const uint32_t block = call("GetEnvironmentStringsW", {}, "kernel32.dll");
    bool found = false;
    if (block) {
        const uint64_t limit = uint64_t(block) + heap_size(block);
        for (uint32_t at = block; uint64_t(at) + 2 <= limit && rd16(at);) {
            const auto entry = gm_wstr(at);
            if (entry == "CRT_\xc3\xa9=\xc3\xa9\xf0\x9f\x99\x82")
                found = true;
            at += (crt::string_length(at, 2) + 1) * 2;
        }
    }
    CHECK(found);
    const uint32_t wide_result = heap_alloc(16, true);
    CHECK(call("GetEnvironmentVariableW", {key, 0, 0}, "kernel32.dll") == 4);
    CHECK(call("GetEnvironmentVariableW", {key, wide_result, 8}, "kernel32.dll") == 3 &&
          rd16(wide_result) == 0xe9 && rd16(wide_result + 2) == 0xd83d);
    wr16(wide_result, 0xf6);
    wr16(wide_result + 2, 0xd83d);
    wr16(wide_result + 4, 0xde42);
    wr16(wide_result + 6, 0);
    CHECK(call("SetEnvironmentVariableW", {key, wide_result}, "kernel32.dll") == 1);
    const uint32_t changed = call("_wgetenv", {key});
    CHECK(changed && rd16(changed) == 0xf6 && rd16(changed + 2) == 0xd83d &&
          rd16(changed + 4) == 0xde42);
    heap_free(wide_result);
    for (auto p : {out, assignment, key, narrow})
        heap_free(p);
}
int main(int argc, char **argv) {
    mem_init();
    imports_init();
    win32_init(".");
    cpu.fs_base = TEB_BASE;
    cpu.r[R_ESP] = STACK_TOP - 4096;
    if (argc > 1 && !std::strcmp(argv[1], "unicode-startup")) {
        unicode_startup_tests();
        return failures ? 1 : 0;
    }
    if (argc > 1) {
        CHECK(std::freopen(argv[2], "w", stderr));
        const uint32_t scratch = heap_alloc(16, true);
        if (!std::strcmp(argv[1], "memcpy-oob"))
            call("memcpy", {GUEST_SIZE - 4, scratch, 8});
        if (!std::strcmp(argv[1], "memmove-oob"))
            call("memmove", {scratch, GUEST_SIZE - 4, 8});
        if (!std::strcmp(argv[1], "memset-oob"))
            call("memset", {GUEST_SIZE - 4, 0, 8});
        if (!std::strcmp(argv[1], "memcmp-oob"))
            call("memcmp", {scratch, GUEST_SIZE - 4, 8});
        if (!std::strcmp(argv[1], "memchr-oob"))
            call("memchr", {GUEST_SIZE - 4, 0, 8});
        if (!std::strcmp(argv[1], "qsort-oob"))
            call("qsort", {scratch, 0x80000000, 8, 0});
        if (!std::strcmp(argv[1], "read-oob"))
            call("_read", {0, GUEST_SIZE - 4, 8});
        if (!std::strcmp(argv[1], "write-oob"))
            call("_write", {1, GUEST_SIZE - 4, 8});
        if (!std::strcmp(argv[1], "fread-oob"))
            call("fread", {scratch, 0x80000001, 2, 0});
        if (!std::strcmp(argv[1], "fwrite-oob"))
            call("fwrite", {scratch, 0x80000001, 2, 0});
        if (!std::strcmp(argv[1], "printf-width")) {
            const uint32_t f = guest_text("%*s");
            call("_snprintf", {scratch, 16, f, 0x80000000, 0});
        }
        const uint32_t required =
            imports_alloc_trampoline("msvcr80.dll", argv[1], nullptr, ARGC_UNKNOWN);
        guest_call(&cpu, required, nullptr, 0);
        return 2;
    }
    memory_tests();
    allocation_tests();
    sorting_tests();
    string_tests();
    math_tests();
    stdio_tests();
    startup_tests();
    cpp_tests();
    CHECK(!imports_resolve("msvcr80.dll", "__optional_export_that_does_not_exist"));
    char executable[4096];
    CHECK(os_exe_path(executable, sizeof executable) == 0);
    const char *modes[] = {"__recomp_unimplemented_crt_export",
                           "memcpy-oob",
                           "memmove-oob",
                           "memset-oob",
                           "memcmp-oob",
                           "memchr-oob",
                           "qsort-oob",
                           "read-oob",
                           "write-oob",
                           "fread-oob",
                           "fwrite-oob",
                           "printf-width"};
    const char *messages[] = {"__recomp_unimplemented_crt_export",
                              "memcpy",
                              "memmove",
                              "memset",
                              "memcmp",
                              "memchr",
                              "qsort",
                              "_read",
                              "_write",
                              "fread",
                              "fwrite",
                              "_snprintf"};
    for (unsigned mode = 0; mode < sizeof modes / sizeof modes[0]; ++mode) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/crt-trap-XXXXXX", os_temp_dir());
        int fd = os_mkstemp(path);
        CHECK(fd >= 0);
        if (fd >= 0) {
            os_fd_close(fd);
            const char *args[] = {executable, modes[mode], path, nullptr};
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
            CHECK(std::strstr(log, "msvcr80.dll!") && std::strstr(log, messages[mode]));
            if (mode && mode < 11)
                CHECK(std::strstr(log, "guest range"));
            if (mode == 11)
                CHECK(std::strstr(log, "format width or precision"));
            os_unlink(path);
        }
    }
    thread_tests();
    const char *unicode_args[] = {executable, "unicode-startup", nullptr};
    int64_t unicode_pid = 0;
    int unicode_code = -1;
    CHECK(os_spawn(unicode_args, &unicode_pid) == 0 && os_wait(unicode_pid, &unicode_code) == 0 &&
          unicode_code == 0);
    mem_shutdown();
    std::printf("CRT: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
