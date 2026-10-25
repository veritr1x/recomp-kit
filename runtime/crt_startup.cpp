// Guest-owned CRT startup data, deterministic locale, and exit/thread helpers.
#include "crt_internal.h"
#include "kernel32_internal.h"
#include "../platform/os.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace crt {
namespace {
uint64_t generation = 0;
bool extended = false;
std::vector<uint32_t> exits;
uint32_t ctype_table = 0, wide_ctype_table = 0;
std::array<uint32_t, 6> locale_names{};
std::array<bool, 6> locale_values{};
uint32_t multibyte_codepage = 0;
std::map<std::pair<std::string, bool>, uint32_t> env_values;
std::map<uint32_t, uint32_t> locks;
void reset() {
    if (generation == mem_generation())
        return;
    generation = mem_generation();
    exits.clear();
    env_values.clear();
    locks.clear();
    extended = false;
    ctype_table = wide_ctype_table = 0;
    locale_names.fill(0);
    locale_values.fill(false);
    multibyte_codepage = 0;
}
uint32_t duplicate(const std::string &s, bool wide = false) {
    if (s.size() > 1u << 20)
        fail("oversized startup string");
    const uint32_t p = heap_alloc(uint32_t((s.size() + 1) * (wide ? 2 : 1)), true);
    if (!p)
        fail("cannot allocate startup data");
    // The Win32 environment/command-line seam is UTF-8. Decode rather than
    // widening bytes; byte count is a safe upper bound on UTF-16 units.
    if (wide)
        gm_put_wstr(p, s, uint32_t(s.size() + 1));
    else
        std::memcpy(gm_ptr(p), s.c_str(), s.size() + 1);
    return p;
}
uint16_t classify(uint32_t ch) {
    uint16_t flags = 0;
    if (ch >= 'A' && ch <= 'Z')
        flags |= 1;
    if (ch >= 'a' && ch <= 'z')
        flags |= 2;
    if (ch >= '0' && ch <= '9')
        flags |= 4 | 128;
    if (ch == ' ' || (ch >= 9 && ch <= 13))
        flags |= 8;
    if (ch < 32 || ch == 127)
        flags |= 32;
    if (ch == ' ')
        flags |= 64;
    if ((ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))
        flags |= 128;
    if (ch >= 33 && ch <= 126 && !(flags & (1 | 2 | 4)))
        flags |= 16;
    if (extended) {
        if ((ch >= 0xc0 && ch <= 0xd6) || (ch >= 0xd8 && ch <= 0xde))
            flags |= 1;
        if ((ch >= 0xdf && ch <= 0xf6) || (ch >= 0xf8 && ch <= 0xff))
            flags |= 2;
        if ((ch >= 0x391 && ch <= 0x3ab && ch != 0x3a2) || (ch >= 0x410 && ch <= 0x42f))
            flags |= 1;
        if ((ch >= 0x3b1 && ch <= 0x3cb) || (ch >= 0x430 && ch <= 0x44f))
            flags |= 2;
        if (ch == 0xa0)
            flags |= 8 | 64;
    }
    return flags;
}
void update_tables() {
    if (!ctype_table)
        return;
    wr16(ctype_table, 0);
    wr16(wide_ctype_table, 0);
    for (uint32_t i = 0; i < 256; ++i) {
        wr16(ctype_table + 2 + i * 2, classify(i));
        wr16(wide_ctype_table + 2 + i * 2, classify(i));
    }
}
void table_init(uint32_t p) {
    reset();
    if (!ctype_table) {
        ctype_table = heap_alloc(514, true);
        wide_ctype_table = heap_alloc(514, true);
        if (!ctype_table || !wide_ctype_table)
            fail("cannot allocate ctype tables");
        update_tables();
    }
    wr32(p, ctype_table + 2);
}
void wtable_init(uint32_t p) {
    table_init(p);
    wr32(p, wide_ctype_table + 2);
}
void command_init(uint32_t p) {
    X86 c{};
    get_command_line(&c);
    wr32(p, c.r[R_EAX]);
}
void wcommand_init(uint32_t p) {
    X86 c{};
    get_command_line_w(&c);
    wr32(p, c.r[R_EAX]);
}
void one_init(uint32_t p) {
    wr32(p, 1);
}
void huge_init(uint32_t p) {
    wr64(p, 0x7ff0000000000000ull);
}
uint32_t environment_array(bool wide) {
    const auto env = win32_environment();
    const uint32_t p = heap_alloc(uint32_t((env.size() + 1) * 4), true);
    if (!p)
        fail("cannot allocate environment");
    for (uint32_t i = 0; i < env.size(); ++i)
        wr32(p + i * 4, duplicate(env[i], wide));
    return p;
}
void env_init(uint32_t p) {
    wr32(p, environment_array(false));
}
void wenv_init(uint32_t p) {
    wr32(p, environment_array(true));
}
void refresh_env() {
    for (auto name : {"_environ", "__initenv", "_wenviron", "__winitenv"}) {
        const uint32_t p = imports_data_address("msvcr80.dll", name);
        if (p)
            wr32(p, environment_array(name[1] == 'w' || name[2] == 'w'));
    }
}
std::vector<std::string> arguments() {
    X86 c{};
    get_command_line(&c);
    const std::string line = text(c.r[R_EAX]);
    std::vector<std::string> result;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i == line.size())
            break;
        std::string word;
        bool quote = false;
        while (i < line.size() && (quote || (line[i] != ' ' && line[i] != '\t'))) {
            size_t slashes = 0;
            while (i < line.size() && line[i] == '\\') {
                ++slashes;
                ++i;
            }
            if (i < line.size() && line[i] == '"') {
                word.append(slashes / 2, '\\');
                if (slashes % 2)
                    word += '"';
                else if (quote && i + 1 < line.size() && line[i + 1] == '"') {
                    word += '"';
                    ++i;
                } else
                    quote = !quote;
                ++i;
            } else {
                word.append(slashes, '\\');
                if (i < line.size() && (quote || (line[i] != ' ' && line[i] != '\t')))
                    word += line[i++];
            }
        }
        result.push_back(word);
    }
    return result;
}
void argv_init(uint32_t p, bool wide) {
    const auto args = arguments();
    const uint32_t a = heap_alloc(uint32_t((args.size() + 1) * 4), true);
    if (!a)
        fail("cannot allocate argv");
    for (uint32_t i = 0; i < args.size(); ++i)
        wr32(a + i * 4, duplicate(args[i], wide));
    wr32(p, a);
}
void argv_a(uint32_t p) {
    argv_init(p, false);
}
void argv_w(uint32_t p) {
    argv_init(p, true);
}
void argc_init(uint32_t p) {
    wr32(p, uint32_t(arguments().size()));
}
template <bool Wide = false> void mainargs(X86 *c) {
    const uint32_t count = imports_alloc_data("msvcr80.dll", "__argc"),
                   argv = imports_alloc_data("msvcr80.dll", Wide ? "__wargv" : "__argv"),
                   env = imports_alloc_data("msvcr80.dll", Wide ? "_wenviron" : "_environ");
    for (uint32_t i = 0; i < 3; ++i)
        range(arg(c, i), 4);
    wr32(arg(c, 0), rd32(count));
    wr32(arg(c, 1), rd32(argv));
    wr32(arg(c, 2), rd32(env));
    set_eax(c, 0);
}
std::string wide_text(uint32_t p) {
    return gm_wstr(p, string_length(p, 2, 1u << 20));
}
template <bool Wide = false> void getenv_(X86 *c) {
    reset();
    std::string name = Wide ? wide_text(arg(c, 0)) : text(arg(c, 0));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return char(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); });
    for (const auto &entry : win32_environment()) {
        const auto eq = entry.find('=');
        std::string key = entry.substr(0, eq);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
            return char(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        });
        if (key != name)
            continue;
        const std::string value = entry.substr(eq + 1);
        const auto cache = std::make_pair(entry, Wide);
        auto it = env_values.find(cache);
        if (it == env_values.end())
            it = env_values.emplace(cache, duplicate(value, Wide)).first;
        set_eax(c, it->second);
        return;
    }
    set_eax(c, 0);
}
template <bool Wide = false> void putenv_(X86 *c) {
    const std::string entry = Wide ? wide_text(arg(c, 0)) : text(arg(c, 0));
    const auto eq = entry.find('=');
    if (eq == std::string::npos || !eq) {
        set_errno(c, EINVAL_);
        set_eax(c, UINT32_MAX);
        return;
    }
    const auto value = entry.substr(eq + 1);
    if (!win32_set_environment(entry.substr(0, eq), &value)) {
        set_eax(c, UINT32_MAX);
        return;
    }
    refresh_env();
    set_eax(c, 0);
}
template <const char *Name> void data_ptr(X86 *c) {
    set_eax(c, imports_alloc_data("msvcr80.dll", Name));
}
constexpr char argc_name[] = "__argc", argv_name[] = "__argv", wargv_name[] = "__wargv",
               cmd_name[] = "_acmdln", wcmd_name[] = "_wcmdln", env_name[] = "_environ",
               wenv_name[] = "_wenviron", fmode_name[] = "_fmode", commode_name[] = "_commode",
               ctype_name[] = "_pctype", wctype_name[] = "_pwctype", mb_name[] = "__mb_cur_max";
void setlocale_(X86 *c) {
    reset();
    if (arg(c, 0) > 5) {
        set_eax(c, 0);
        return;
    }
    if (arg(c, 1)) {
        std::string name = text(arg(c, 1));
        const bool c_locale = name == "C" || name == "POSIX";
        if (!c_locale && name != "" && name != ".1252" && name != "English_United States.1252" &&
            name != "en-US") {
            set_eax(c, 0);
            return;
        }
        auto invalidate = [](uint32_t category) {
            if (locale_names[category])
                heap_free(locale_names[category]);
            locale_names[category] = 0;
        };
        if (arg(c, 0) == 0) {
            for (uint32_t i = 0; i < 6; ++i)
                invalidate(i);
            locale_values.fill(!c_locale);
        } else {
            invalidate(arg(c, 0));
            locale_values[arg(c, 0)] = !c_locale;
            invalidate(0);
        }
        extended = locale_values[2];
        if (arg(c, 0) == 0 || arg(c, 0) == 2)
            multibyte_codepage = extended ? 1252 : 0;
        update_tables();
    }
    auto &name = locale_names[arg(c, 0)];
    if (!name) {
        if (arg(c, 0) == 0 && !std::all_of(locale_values.begin() + 1, locale_values.end(),
                                           [](bool value) { return value == locale_values[1]; })) {
            const char *categories[] = {"LC_COLLATE", "LC_CTYPE", "LC_MONETARY", "LC_NUMERIC",
                                        "LC_TIME"};
            std::string joined;
            for (uint32_t i = 1; i < 6; ++i) {
                if (i > 1)
                    joined += ';';
                joined += categories[i - 1];
                joined += '=';
                joined += locale_values[i] ? "English_United States.1252" : "C";
            }
            name = duplicate(joined);
        } else
            name = duplicate(locale_values[arg(c, 0) ? arg(c, 0) : 1] ? "English_United States.1252"
                                                                      : "C");
    }
    set_eax(c, name);
}
template <int Mask> void ctype_(X86 *c) {
    reset();
    const uint32_t ch = arg(c, 0);
    set_eax(c, classify(ch) & Mask);
}
void isctype_(X86 *c) {
    reset();
    set_eax(c, classify(arg(c, 0)) & arg(c, 1));
}
template <bool Wide = false> void lower_(X86 *c) {
    set_eax(c, locale_lower(arg(c, 0), Wide));
}
template <bool Wide = false> void upper_(X86 *c) {
    set_eax(c, locale_upper(arg(c, 0), Wide));
}
template <bool Wide = false> void pctype_(X86 *c) {
    set_eax(c, rd32(imports_alloc_data("msvcr80.dll", Wide ? "_pwctype" : "_pctype")));
}
void mbmax_(X86 *c) {
    set_eax(c, 1);
}
void codepage_(X86 *c) {
    reset();
    set_eax(c, multibyte_codepage);
}
void setcp_(X86 *c) {
    const int32_t cp = int32_t(arg(c, 0));
    if (cp != 0 && cp != 1252 && cp != -3 && cp != -4) {
        set_errno(c, EINVAL_);
        set_eax(c, UINT32_MAX);
        return;
    }
    reset();
    multibyte_codepage = cp == -4 ? (extended ? 1252 : 0) : cp == -3 ? 1252 : uint32_t(cp);
    set_eax(c, 0);
}
template <bool Error = false> void initterm(X86 *c) {
    const uint32_t lo = arg(c, 0), hi = arg(c, 1);
    if (hi < lo || (hi - lo) % 4)
        fail("invalid initializer range", lo, hi);
    range(lo, hi - lo);
    for (uint32_t p = lo; p < hi; p += 4) {
        const uint32_t fn = rd32(p);
        if (fn) {
            const uint32_t result = guest_call(c, fn, nullptr, 0);
            if (Error && result) {
                set_eax(c, result);
                return;
            }
        }
    }
    set_eax(c, 0);
}
void onexit_(X86 *c) {
    reset();
    const uint32_t fn = arg(c, 0);
    range(fn, 1);
    exits.push_back(fn);
    set_eax(c, fn);
}
void atexit_(X86 *c) {
    onexit_(c);
    set_eax(c, 0);
}
void cexit_(X86 *c) {
    reset();
    while (!exits.empty()) {
        const uint32_t fn = exits.back();
        exits.pop_back();
        guest_call(c, fn, nullptr, 0);
    }
    set_eax(c, 0);
}
template <bool Clean = true, bool Thread = false, bool Zero = false> void exit_(X86 *c) {
    const uint32_t code = Zero ? 0 : arg(c, 0);
    if (Thread) {
        const uint32_t cell = errno_address(c), handle = rd32(cell + 28);
        if (handle) {
            wr32(cell + 28, 0);
            win32_call(c, "CloseHandle", &handle, 1);
        }
    }
    if (Clean)
        cexit_(c);
    const uint32_t args[] = {code};
    win32_call(c, Thread ? "ExitThread" : "ExitProcess", args, 1);
}
void abort_(X86 *c) {
    fail("abort requested");
}
void thread_entry(X86 *c) {
    const uint32_t desc = arg(c, 0);
    range(desc, 16);
    const uint32_t fn = rd32(desc), param = rd32(desc + 4), cdecl = rd32(desc + 8),
                   handle = rd32(desc + 12);
    heap_free(desc);
    if (cdecl)
        wr32(errno_address(c) + 28, handle);
    c->fpu_cw = 0x027f;
    c->mxcsr_xor = 0;
    const uint32_t result = guest_call(c, fn, &param, 1);
    if (cdecl && handle)
        win32_call(c, "CloseHandle", &handle, 1);
    const uint32_t code = cdecl ? 0 : result;
    win32_call(c, "ExitThread", &code, 1);
}
template <bool Ex = true> void beginthread_(X86 *c) {
    const uint32_t fn = arg(c, Ex ? 2 : 0), param = arg(c, Ex ? 3 : 2), size = arg(c, 1);
    range(fn, 1);
    const uint32_t desc = heap_alloc(16, true);
    if (!desc) {
        set_errno(c, ENOMEM_);
        set_eax(c, Ex ? 0 : UINT32_MAX);
        return;
    }
    wr32(desc, fn);
    wr32(desc + 4, param);
    wr32(desc + 8, Ex ? 0 : 1);
    const uint32_t trampoline =
        imports_alloc_trampoline("msvcr80.dll", "__recomp_crt_thread", thread_entry, 1);
    const uint32_t args[] = {Ex ? arg(c, 0) : 0, size, trampoline, desc, Ex ? arg(c, 4) : 0,
                             Ex ? arg(c, 5) : 0};
    const uint32_t result = win32_call(c, "CreateThread", args, 6);
    const char *mode = recomp_env("CREATETHREAD");
    const bool sync = mode && !std::strcmp(mode, "sync"), skip = mode && !std::strcmp(mode, "skip");
    if (!result) {
        heap_free(desc);
        set_errno(c, ENOMEM_);
    } else if (skip)
        heap_free(desc);
    else if (!sync)
        wr32(desc + 12, result);
    if (result && !Ex && (sync || skip))
        win32_call(c, "CloseHandle", &result, 1);
    set_eax(c, result ? result : Ex ? 0 : UINT32_MAX);
}
template <bool Large = true> void time_(X86 *c) {
    const uint64_t now = os_wall_time_us() / 1000000;
    const uint32_t p = arg(c, 0);
    if (p) {
        range(p, Large ? 8 : 4);
        if (Large)
            wr64(p, now);
        else
            wr32(p, uint32_t(now));
    }
    set_eax(c, uint32_t(now));
    if (Large)
        c->r[R_EDX] = uint32_t(now >> 32);
}
template <bool Local = true, bool Large = true, bool Secure = false> void broken_time(X86 *c) {
    const uint32_t input = arg(c, Secure ? 1 : 0);
    range(input, Large ? 8 : 4);
    const int64_t seconds = Large ? int64_t(rd64(input)) : int32_t(rd32(input));
    struct tm value{};
    const int result = Local ? os_localtime(seconds, &value) : os_gmtime(seconds, &value);
    if (result) {
        set_errno(c, EINVAL_);
        set_eax(c, Secure ? EINVAL_ : 0);
        return;
    }
    const uint32_t p = Secure ? arg(c, 0) : errno_address(c) + 32;
    range(p, 36);
    const int fields[] = {value.tm_sec,  value.tm_min,  value.tm_hour, value.tm_mday, value.tm_mon,
                          value.tm_year, value.tm_wday, value.tm_yday, value.tm_isdst};
    for (uint32_t i = 0; i < 9; ++i)
        wr32(p + i * 4, uint32_t(fields[i]));
    set_eax(c, Secure ? 0 : p);
}
void clock_(X86 *c) {
    static uint64_t start = 0;
    if (!start)
        start = os_monotonic_ns();
    set_eax(c, uint32_t((os_monotonic_ns() - start) / 1000000));
}
struct tm read_tm(uint32_t p) {
    range(p, 36);
    struct tm v{};
    int *fields[] = {&v.tm_sec,  &v.tm_min,  &v.tm_hour, &v.tm_mday, &v.tm_mon,
                     &v.tm_year, &v.tm_wday, &v.tm_yday, &v.tm_isdst};
    for (uint32_t i = 0; i < 9; ++i)
        *fields[i] = int32_t(rd32(p + i * 4));
    return v;
}
void write_tm(uint32_t p, const struct tm &v) {
    range(p, 36);
    const int fields[] = {v.tm_sec,  v.tm_min,  v.tm_hour, v.tm_mday, v.tm_mon,
                          v.tm_year, v.tm_wday, v.tm_yday, v.tm_isdst};
    for (uint32_t i = 0; i < 9; ++i)
        wr32(p + i * 4, uint32_t(fields[i]));
}
template <bool Large = true> void mktime_(X86 *c) {
    auto v = read_tm(arg(c, 0));
    if (v.tm_year < 69 || v.tm_year > 1100 || v.tm_mon < -24 || v.tm_mon > 24 || v.tm_mday < -366 ||
        v.tm_mday > 366 || v.tm_hour < -48 || v.tm_hour > 48 || v.tm_min < -120 || v.tm_min > 120 ||
        v.tm_sec < -120 || v.tm_sec > 120) {
        set_errno(c, EINVAL_);
        set_eax64(c, UINT64_MAX);
        return;
    }
    const int64_t seconds = os_mktime(&v);
    if (seconds < 0 || (!Large && seconds > INT32_MAX)) {
        set_errno(c, EINVAL_);
        set_eax64(c, UINT64_MAX);
        return;
    }
    write_tm(arg(c, 0), v);
    if (Large)
        set_eax64(c, uint64_t(seconds));
    else
        set_eax(c, uint32_t(seconds));
}
template <bool Secure = false> void asctime_(X86 *c) {
    const auto v = read_tm(arg(c, Secure ? 2 : 0));
    if (v.tm_wday < 0 || v.tm_wday > 6 || v.tm_mon < 0 || v.tm_mon > 11 || v.tm_mday < 1 ||
        v.tm_mday > 31 || v.tm_year < -1900 || v.tm_year > 8099 || v.tm_hour < 0 ||
        v.tm_hour > 23 || v.tm_min < 0 || v.tm_min > 59 || v.tm_sec < 0 || v.tm_sec > 60 ||
        (Secure && arg(c, 1) < 26)) {
        set_errno(c, EINVAL_);
        set_eax(c, Secure ? EINVAL_ : 0);
        return;
    }
    const uint32_t p = Secure ? arg(c, 0) : errno_address(c) + 80;
    range(p, 26);
    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
               *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%s %s %2d %02d:%02d:%02d %04d\n", days[v.tm_wday],
                  months[v.tm_mon], v.tm_mday, v.tm_hour, v.tm_min, v.tm_sec, v.tm_year + 1900);
    std::memcpy(gm_ptr(p), buffer, 26);
    set_eax(c, Secure ? 0 : p);
}
template <bool Wide = false> void strftime_(X86 *c) {
    const uint32_t dest = arg(c, 0), cap = arg(c, 1);
    range(dest, uint64_t(cap) * (Wide ? 2 : 1));
    if (!cap) {
        set_eax(c, 0);
        return;
    }
    if (cap > 1u << 20)
        fail("time format capacity exceeds limit");
    std::string fmt;
    if (Wide) {
        for (auto ch : wtext(arg(c, 2))) {
            if (ch > 127) {
                set_eax(c, 0);
                return;
            }
            fmt += char(ch);
        }
    } else
        fmt = text(arg(c, 2));
    const auto v = read_tm(arg(c, 3));
    if (v.tm_wday < 0 || v.tm_wday > 6 || v.tm_mon < 0 || v.tm_mon > 11) {
        set_errno(c, EINVAL_);
        set_eax(c, 0);
        return;
    }
    std::vector<char> out(cap);
    const size_t n = std::strftime(out.data(), cap, fmt.c_str(), &v);
    if (!n) {
        set_eax(c, 0);
        return;
    }
    for (uint32_t i = 0; i <= n; ++i)
        if (Wide)
            wr16(dest + i * 2, uint8_t(out[i]));
        else
            wr8(dest + i, uint8_t(out[i]));
    set_eax(c, uint32_t(n));
}
template <bool Large = true, bool Secure = false> void ftime_(X86 *c) {
    const uint32_t p = arg(c, 0), offset = Large ? 8 : 4;
    range(p, Large ? 16 : 12);
    const uint64_t us = os_wall_time_us(), seconds = us / 1000000;
    if (Large)
        wr64(p, seconds);
    else
        wr32(p, uint32_t(seconds));
    wr16(p + offset, uint16_t(us / 1000 % 1000));
    struct tm local{}, utc{};
    os_localtime(int64_t(seconds), &local);
    os_gmtime(int64_t(seconds), &utc);
    utc.tm_isdst = local.tm_isdst;
    const int64_t utc_as_local = os_mktime(&utc);
    wr16(p + offset + 2, uint16_t(int16_t((utc_as_local - int64_t(seconds)) / 60)));
    wr16(p + offset + 4, uint16_t(local.tm_isdst > 0));
    if (Secure)
        set_eax(c, 0);
}
template <bool Large = true> void difftime_(X86 *c) {
    const int64_t a = Large ? int64_t(uint64_t(arg(c, 0)) | uint64_t(arg(c, 1)) << 32)
                            : int32_t(arg(c, 0)),
                  b = Large ? int64_t(uint64_t(arg(c, 2)) | uint64_t(arg(c, 3)) << 32)
                            : int32_t(arg(c, 1));
    fpush(c, double(a) - double(b));
}
void fpecode_(X86 *c) {
    set_eax(c, errno_address(c) + 20);
}
void empty_(X86 *c) {
    set_eax(c, 0);
}
template <bool Unlock = false> void lock_(X86 *c) {
    reset();
    const uint32_t id = arg(c, 0);
    auto it = locks.find(id);
    uint32_t handle = it == locks.end() ? 0 : it->second;
    if (!handle && !Unlock) {
        const uint32_t args[] = {0, 0, 0};
        handle = win32_call(c, "CreateMutexA", args, 3);
        if (!handle)
            fail("cannot create CRT lock");
        locks[id] = handle;
    }
    if (!handle) {
        set_errno(c, EINVAL_);
        return;
    }
    const uint32_t args[] = {handle, UINT32_MAX};
    win32_call(c, Unlock ? "ReleaseMutex" : "WaitForSingleObject", args, Unlock ? 1 : 2);
}
} // namespace
uint32_t locale_lower(uint32_t ch, bool wide) {
    reset();
    if (ch >= 'A' && ch <= 'Z')
        return ch + 32;
    if (extended) {
        if (!wide) {
            if (ch == 0x8a || ch == 0x8c || ch == 0x8e)
                return ch + 16;
            if (ch == 0x9f)
                return 0xff;
        }
        if ((ch >= 0xc0 && ch <= 0xd6) || (ch >= 0xd8 && ch <= 0xde) ||
            (ch >= 0x391 && ch <= 0x3ab && ch != 0x3a2) || (ch >= 0x410 && ch <= 0x42f))
            return ch + 32;
        if (ch == 0x178)
            return 0xff;
    }
    return ch;
}
uint32_t locale_upper(uint32_t ch, bool wide) {
    reset();
    if (ch >= 'a' && ch <= 'z')
        return ch - 32;
    if (extended) {
        if (!wide) {
            if (ch == 0x9a || ch == 0x9c || ch == 0x9e)
                return ch - 16;
            if (ch == 0xff)
                return 0x9f;
        }
        if ((ch >= 0xe0 && ch <= 0xf6) || (ch >= 0xf8 && ch <= 0xfe) ||
            (ch >= 0x3b1 && ch <= 0x3cb && ch != 0x3c2) || (ch >= 0x430 && ch <= 0x44f))
            return ch - 32;
        if (ch == 0xff)
            return 0x178;
    }
    return ch;
}
void register_startup() {
#define E(name, ...)                                                                               \
    {                                                                                              \
        "msvcr80.dll", name, ARGC_CDECL, __VA_ARGS__                                               \
    }
    const ImportShim entries[] = {E("_mktime64", mktime_<>),
                                  E("_mktime32", mktime_<false>),
                                  E("mktime", mktime_<>),
                                  E("asctime", asctime_<>),
                                  E("asctime_s", asctime_<true>),
                                  E("strftime", strftime_<>),
                                  E("wcsftime", strftime_<true>),
                                  E("_ftime64", ftime_<>),
                                  E("_ftime32", ftime_<false>),
                                  E("_ftime64_s", ftime_<true, true>),
                                  E("__getmainargs", mainargs<>),
                                  E("__wgetmainargs", mainargs<true>),
                                  E("getenv", getenv_<>),
                                  E("_wgetenv", getenv_<true>),
                                  E("_putenv", putenv_<>),
                                  E("_wputenv", putenv_<true>),
                                  E("__p___argc", data_ptr<argc_name>),
                                  E("__p___argv", data_ptr<argv_name>),
                                  E("__p___wargv", data_ptr<wargv_name>),
                                  E("__p__acmdln", data_ptr<cmd_name>),
                                  E("__p__wcmdln", data_ptr<wcmd_name>),
                                  E("__p__environ", data_ptr<env_name>),
                                  E("__p__wenviron", data_ptr<wenv_name>),
                                  E("__p__fmode", data_ptr<fmode_name>),
                                  E("__p__commode", data_ptr<commode_name>),
                                  E("__p__pctype", data_ptr<ctype_name>),
                                  E("__p__pwctype", data_ptr<wctype_name>),
                                  E("__p___mb_cur_max", data_ptr<mb_name>),
                                  E("setlocale", setlocale_),
                                  E("__pctype_func", pctype_<>),
                                  E("__pwctype_func", pctype_<true>),
                                  E("___mb_cur_max_func", mbmax_),
                                  E("_getmbcp", codepage_),
                                  E("_setmbcp", setcp_),
                                  E("isalnum", ctype_<1 | 2 | 4>),
                                  E("isalpha", ctype_<1 | 2>),
                                  E("iscntrl", ctype_<32>),
                                  E("isdigit", ctype_<4>),
                                  E("isgraph", ctype_<1 | 2 | 4 | 16>),
                                  E("islower", ctype_<2>),
                                  E("isprint", ctype_<1 | 2 | 4 | 16 | 64>),
                                  E("ispunct", ctype_<16>),
                                  E("isspace", ctype_<8>),
                                  E("isupper", ctype_<1>),
                                  E("isxdigit", ctype_<128>),
                                  E("_isctype", isctype_),
                                  E("iswalnum", ctype_<1 | 2 | 4>),
                                  E("iswalpha", ctype_<1 | 2>),
                                  E("iswcntrl", ctype_<32>),
                                  E("iswdigit", ctype_<4>),
                                  E("iswgraph", ctype_<1 | 2 | 4 | 16>),
                                  E("iswlower", ctype_<2>),
                                  E("iswprint", ctype_<1 | 2 | 4 | 16 | 64>),
                                  E("iswpunct", ctype_<16>),
                                  E("iswspace", ctype_<8>),
                                  E("iswupper", ctype_<1>),
                                  E("iswxdigit", ctype_<128>),
                                  E("iswctype", isctype_),
                                  E("tolower", lower_<>),
                                  E("toupper", upper_<>),
                                  E("towlower", lower_<true>),
                                  E("towupper", upper_<true>),
                                  E("_initterm", initterm<>),
                                  E("_initterm_e", initterm<true>),
                                  E("_onexit", onexit_),
                                  E("atexit", atexit_),
                                  E("_cexit", cexit_),
                                  E("_c_exit", empty_),
                                  E("exit", exit_<>),
                                  E("_exit", exit_<false>),
                                  E("_endthreadex", exit_<false, true>),
                                  E("_endthread", exit_<false, true, true>),
                                  E("abort", abort_),
                                  E("_amsg_exit", abort_),
                                  E("_beginthreadex", beginthread_<>),
                                  E("_beginthread", beginthread_<false>),
                                  E("_time64", time_<>),
                                  E("_time32", time_<false>),
                                  E("time", time_<>),
                                  E("clock", clock_),
                                  E("_difftime64", difftime_<>),
                                  E("_difftime32", difftime_<false>),
                                  E("difftime", difftime_<>),
                                  E("_localtime64", broken_time<>),
                                  E("_gmtime64", broken_time<false>),
                                  E("_localtime32", broken_time<true, false>),
                                  E("_gmtime32", broken_time<false, false>),
                                  E("_localtime64_s", broken_time<true, true, true>),
                                  E("_gmtime64_s", broken_time<false, true, true>),
                                  E("__fpecode", fpecode_),
                                  E("_unlock", lock_<true>),
                                  E("_lock", lock_<>),
                                  E("__set_app_type", empty_),
                                  E("_set_app_type", empty_),
                                  E("_setusermatherr", empty_)};
#undef E
    imports_register(entries, sizeof entries / sizeof entries[0]);
    struct Data {
        const char *name;
        uint32_t size;
        void (*init)(uint32_t);
    };
    const Data data[] = {
        {"_acmdln", 4, command_init}, {"_wcmdln", 4, wcommand_init}, {"__argc", 4, argc_init},
        {"__argv", 4, argv_a},        {"__wargv", 4, argv_w},        {"_environ", 4, env_init},
        {"_wenviron", 4, wenv_init},  {"__initenv", 4, env_init},    {"__winitenv", 4, wenv_init},
        {"_pctype", 4, table_init},   {"_pwctype", 4, wtable_init},  {"__mb_cur_max", 4, one_init},
        {"_fmode", 4, nullptr},       {"_commode", 4, nullptr},      {"_HUGE", 8, huge_init}};
    for (const auto &d : data)
        imports_register_data_init("msvcr80.dll", d.name, d.size, d.init);
}
} // namespace crt
