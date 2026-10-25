// VC8 basic_string has an allocator prefix, 16-byte SSO union, size and
// capacity: 28 guest bytes on every host, including native 64-bit builds.
#include "crt_internal.h"
#include "msvc_eh.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <string>

namespace crt {
namespace {
enum Error { Allocation, Length, Position };
uint64_t error_generation = 0;
uint32_t error_storage = 0;
std::array<uint32_t, 3> error_info{}, error_vtable{};
std::array<bool, 16> error_used{};
void error_copy(X86 *c) {
    const uint32_t source = arg(c, 0), dest = c->r[R_ECX];
    range(source, 40);
    range(dest, 40);
    std::memmove(gm_ptr(dest), gm_ptr(source), 40);
    if (rd32(source + 4) == source + 16)
        wr32(dest + 4, dest + 16);
    set_eax(c, dest);
}
void base_error_copy(X86 *c) {
    const uint32_t source = arg(c, 0), dest = c->r[R_ECX];
    range(source, 12);
    range(dest, 12);
    const uint32_t message = rd32(source + 4), n = string_length(message, 1) + 1,
                   p = heap_alloc(n, false);
    if (!p)
        fail("exception copy allocation failed");
    std::memcpy(gm_ptr(p), gm_ptr(message), n);
    wr32(dest, rd32(source));
    wr32(dest + 4, p);
    wr32(dest + 8, 1);
    set_eax(c, dest);
}
void error_destroy(X86 *c) {
    const uint32_t object = c->r[R_ECX];
    if (object >= error_storage && object < error_storage + 640 &&
        (object - error_storage) % 40 == 0)
        error_used[(object - error_storage) / 40] = false;
}
void errors() {
    if (error_generation == mem_generation())
        return;
    error_generation = mem_generation();
    error_used.fill(false);
    error_storage = heap_alloc(640, true);
    if (!error_storage)
        fail("cannot reserve CRT exceptions");
    const char *names[] = {".?AVbad_alloc@std@@", ".?AVlength_error@std@@",
                           ".?AVout_of_range@std@@", ".?AVlogic_error@std@@",
                           ".?AVexception@std@@"};
    std::array<uint32_t, 5> types{};
    for (uint32_t i = 0; i < 5; ++i) {
        types[i] = heap_alloc(64, true);
        if (!types[i])
            fail("cannot allocate CRT RTTI");
        wr32(types[i], imports_alloc_data("msvcr80.dll", "??_7type_info@@6B@"));
        std::memcpy(gm_ptr(types[i] + 8), names[i], std::strlen(names[i]) + 1);
    }
    const uint32_t destructor = imports_alloc_trampoline(
                       "msvcp80.dll", "__recomp_crt_error_release", error_destroy, 0),
                   copy = imports_alloc_trampoline("msvcp80.dll", "__recomp_crt_error_copy",
                                                   error_copy, 1),
                   base_copy = imports_alloc_trampoline(
                       "msvcp80.dll", "__recomp_crt_error_base_copy", base_error_copy, 1);
    for (uint32_t kind = 0; kind < 3; ++kind) {
        const uint32_t p = heap_alloc(512, true);
        if (!p)
            fail("cannot allocate CRT throw info");
        error_info[kind] = p;
        wr32(p + 4, destructor);
        wr32(p + 12, p + 16);
        const uint32_t count = kind ? 3 : 2;
        wr32(p + 16, count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t type_index = i == 0           ? kind
                                        : i + 1 == count ? 4
                                                         : 3,
                           ct = p + 40 + i * 28;
            wr32(p + 20 + i * 4, ct);
            wr32(ct + 4, types[type_index]);
            wr32(ct + 12, UINT32_MAX);
            wr32(ct + 20, type_index == 4 || kind == 0 ? 12 : 40);
            wr32(ct + 24, type_index == 4 || kind == 0 ? base_copy : copy);
        }
        // A complete-object locator preserves the dynamic type for diagnostics.
        const uint32_t col = p + 300, vt = p + 464;
        wr32(col + 12, types[kind]);
        wr32(col + 16, p + 320);
        wr32(p + 328, count);
        wr32(p + 332, p + 340);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t base = p + 360 + i * 28, type_index = i == 0           ? kind
                                                                 : i + 1 == count ? 4
                                                                                  : 3;
            wr32(p + 340 + i * 4, base);
            wr32(base, types[type_index]);
            wr32(base + 4, i ? 0 : count - 1);
            wr32(base + 12, UINT32_MAX);
        }
        wr32(vt - 4, col);
        wr32(vt, imports_resolve("msvcr80.dll", "??_Gexception@std@@UAEPAXI@Z"));
        wr32(vt + 4, imports_resolve("msvcr80.dll", "?what@exception@std@@UBEPBDXZ"));
        error_vtable[kind] = vt;
    }
}
[[noreturn]] void error(X86 *c, Error kind) {
    errors();
    uint32_t slot = 0;
    while (slot < error_used.size() && error_used[slot])
        ++slot;
    if (slot == error_used.size())
        fail("nested CRT exception limit exceeded");
    error_used[slot] = true;
    const uint32_t p = error_storage + slot * 40;
    std::memset(gm_ptr(p), 0, 40);
    const char *message = kind == Allocation ? "bad allocation"
                          : kind == Length   ? "string too long"
                                             : "string position";
    wr32(p, error_vtable[kind]);
    wr32(p + 4, p + 16);
    std::memcpy(gm_ptr(p + 16), message, std::strlen(message) + 1);
    wr32(p + 32, uint32_t(std::strlen(message)));
    wr32(p + 36, 15);
    recomp_msvc_throw(c, p, error_info[kind]);
    fail("C++ throw unexpectedly returned");
}
struct View {
    uint32_t p = 0, n = 0, character = 0;
    bool repeated = false;
};
template <unsigned U> struct String {
    uint32_t object, p, n, cap;
    explicit String(uint32_t address) : object(address) {
        range(object, 28);
        n = rd32(object + 20);
        cap = rd32(object + 24);
        p = cap < 16 / U ? object + 4 : rd32(object + 4);
        if (n > cap)
            fail("invalid basic_string size", object);
        range(p, (uint64_t(cap) + 1) * U);
    }
    View view() const {
        return {p, n, 0, false};
    }
};
template <unsigned U> void initialize(uint32_t object) {
    range(object, 28);
    std::memset(gm_ptr(object), 0, 28);
    wr32(object + 24, 16 / U - 1);
}
template <unsigned U> uint32_t read(View v, uint32_t i) {
    if (v.repeated)
        return v.character;
    return U == 2 ? rd16(v.p + i * U) : rd8(v.p + i);
}
template <unsigned U> View pointer(uint32_t p, uint32_t n = UINT32_MAX) {
    if (n == UINT32_MAX)
        n = string_length(p, U);
    range(p, uint64_t(n) * U);
    return {p, n, 0, false};
}
template <unsigned U> View substring(X86 *c, View v, uint32_t pos, uint32_t count) {
    if (pos > v.n)
        error(c, Position);
    v.n = std::min(count, v.n - pos);
    if (!v.repeated)
        v.p += pos * U;
    return v;
}
template <unsigned U>
void mutate(X86 *c, uint32_t object, uint32_t pos, uint32_t remove, View source,
            uint32_t minimum = 0) {
    errors();
    String<U> old(object);
    if (pos > old.n)
        error(c, Position);
    remove = std::min(remove, old.n - pos);
    const uint64_t total = uint64_t(old.n) - remove + source.n;
    if (total > UINT32_MAX / U - 1 || minimum > UINT32_MAX / U - 1)
        error(c, Length);
    const uint32_t n = uint32_t(total), needed = std::max(n, minimum);
    // Read all three views before releasing the old allocation. Even append
    // from a pointer inside this string is safe when it crosses SSO capacity.
    uint32_t capacity = std::max(needed, old.cap);
    const bool small = capacity < 16 / U;
    std::array<uint8_t, 16> local{};
    uint32_t allocation = 0;
    uint8_t *dest = local.data();
    if (!small) {
        capacity |= 16 / U - 1;
        if (capacity > UINT32_MAX / U - 1)
            capacity = needed;
        allocation = heap_alloc((capacity + 1) * U, false);
        if (!allocation)
            error(c, Allocation);
        dest = gm_ptr(allocation);
    }
    auto put = [&](uint32_t i, uint32_t value) {
        dest[i * U] = uint8_t(value);
        if (U == 2)
            dest[i * U + 1] = uint8_t(value >> 8);
    };
    for (uint32_t i = 0; i < pos; ++i)
        put(i, read<U>(old.view(), i));
    for (uint32_t i = 0; i < source.n; ++i)
        put(pos + i, read<U>(source, i));
    for (uint32_t i = pos + remove; i < old.n; ++i)
        put(pos + source.n + i - pos - remove, read<U>(old.view(), i));
    put(n, 0);
    if (old.cap >= 16 / U)
        heap_free(old.p);
    if (small)
        std::memcpy(gm_ptr(object + 4), local.data(), 16);
    else
        wr32(object + 4, allocation);
    wr32(object + 20, n);
    wr32(object + 24, small ? 16 / U - 1 : capacity);
}
enum Source { CString, Count, Fill, Object, Subobject, PointerRange, Character };
template <unsigned U, Source S> View source(X86 *c, uint32_t first) {
    const uint32_t a = arg(c, first);
    if (S == CString)
        return pointer<U>(a);
    if (S == Count)
        return pointer<U>(a, arg(c, first + 1));
    if (S == PointerRange) {
        const uint32_t end = arg(c, first + 1);
        if (end < a || (end - a) % U)
            error(c, Length);
        return pointer<U>(a, (end - a) / U);
    }
    if (S == Fill)
        return {0, a, arg(c, first + 1), true};
    if (S == Character)
        return {0, 1, a, true};
    const auto v = String<U>(a).view();
    if (S == Subobject)
        return substring<U>(c, v, arg(c, first + 1), arg(c, first + 2));
    return v;
}
template <unsigned U, Source S, bool Constructor = false, int Operation = 0> void change(X86 *c) {
    const uint32_t object = c->r[R_ECX];
    const uint32_t first = Operation == 2 ? 1 : Operation == 3 ? 2 : 0;
    const auto v = source<U, S>(c, first);
    if (Constructor)
        initialize<U>(object);
    String<U> current(object);
    const uint32_t pos = Operation == 1   ? current.n
                         : Operation >= 2 ? arg(c, 0)
                                          : 0,
                   remove = Operation == 0   ? current.n
                            : Operation == 3 ? arg(c, 1)
                                             : 0;
    mutate<U>(c, object, pos, remove, v);
    set_eax(c, object);
}
template <unsigned U> void ctor(X86 *c) {
    initialize<U>(c->r[R_ECX]);
    set_eax(c, c->r[R_ECX]);
}
template <unsigned U> void dtor(X86 *c) {
    String<U> value(c->r[R_ECX]);
    if (value.cap >= 16 / U)
        heap_free(value.p);
    initialize<U>(value.object);
}
template <unsigned U, unsigned Kind> void query(X86 *c) {
    String<U> value(c->r[R_ECX]);
    set_eax(c, Kind == 0   ? value.p
               : Kind == 1 ? value.n
               : Kind == 2 ? value.cap
               : Kind == 3 ? uint32_t(!value.n)
                           : UINT32_MAX / U - 1);
}
template <unsigned U, bool Checked = false> void index(X86 *c) {
    String<U> value(c->r[R_ECX]);
    const uint32_t pos = arg(c, 0);
    if (pos > value.n || (Checked && pos == value.n))
        error(c, Position);
    set_eax(c, value.p + pos * U);
}
template <unsigned U> void erase(X86 *c) {
    mutate<U>(c, c->r[R_ECX], arg(c, 0), arg(c, 1), {});
    set_eax(c, c->r[R_ECX]);
}
template <unsigned U> void clear(X86 *c) {
    mutate<U>(c, c->r[R_ECX], 0, UINT32_MAX, {});
}
template <unsigned U, bool CharacterArg = false> void resize(X86 *c) {
    String<U> value(c->r[R_ECX]);
    const uint32_t n = arg(c, 0);
    mutate<U>(c, value.object, std::min(n, value.n), n < value.n ? value.n - n : 0,
              {0, n > value.n ? n - value.n : 0, CharacterArg ? arg(c, 1) : 0, true});
}
template <unsigned U> void reserve(X86 *c) {
    String<U> value(c->r[R_ECX]);
    if (arg(c, 0) > value.cap)
        mutate<U>(c, value.object, value.n, 0, {}, arg(c, 0));
}
template <unsigned U> void swap(X86 *c) {
    const uint32_t a = c->r[R_ECX], b = arg(c, 0);
    String<U> left(a), right(b);
    std::array<uint8_t, 28> tmp{};
    std::memcpy(tmp.data(), gm_ptr(a), 28);
    std::memmove(gm_ptr(a), gm_ptr(b), 28);
    std::memcpy(gm_ptr(b), tmp.data(), 28);
}
template <unsigned U> void substr(X86 *c) {
    const auto v = substring<U>(c, String<U>(c->r[R_ECX]).view(), arg(c, 1), arg(c, 2));
    const uint32_t result = arg(c, 0);
    initialize<U>(result);
    mutate<U>(c, result, 0, 0, v);
    set_eax(c, result);
}
template <unsigned U, Source S, bool LeftSlice = false> void compare(X86 *c) {
    View a = String<U>(c->r[R_ECX]).view();
    const auto b = source<U, S>(c, LeftSlice ? 2 : 0);
    if (LeftSlice)
        a = substring<U>(c, a, arg(c, 0), arg(c, 1));
    uint32_t i = 0;
    while (i < std::min(a.n, b.n)) {
        const auto x = read<U>(a, i), y = read<U>(b, i);
        if (x != y) {
            set_eax(c, x < y ? UINT32_MAX : 1);
            return;
        }
        ++i;
    }
    set_eax(c, a.n < b.n ? UINT32_MAX : a.n > b.n ? 1 : 0);
}
template <unsigned U, Source S, unsigned Kind = 0> void find(X86 *c) {
    const auto a = String<U>(c->r[R_ECX]).view();
    const uint32_t start = arg(c, 1);
    View b;
    if (S == Count)
        b = pointer<U>(arg(c, 0), arg(c, 2));
    else
        b = source<U, S>(c, 0);
    const bool backwards = Kind == 1 || Kind >= 4;
    uint32_t pos = backwards ? std::min(start, a.n) : start;
    if (Kind == 1)
        pos = std::min(pos, a.n >= b.n ? a.n - b.n : 0);
    if (pos > a.n) {
        set_eax(c, UINT32_MAX);
        return;
    }
    for (;;) {
        bool match = false;
        if (Kind <= 1) {
            match = b.n <= a.n - pos;
            for (uint32_t j = 0; match && j < b.n; ++j)
                match = read<U>(a, pos + j) == read<U>(b, j);
        } else if (pos < a.n) {
            for (uint32_t j = 0; j < b.n; ++j)
                if (read<U>(a, pos) == read<U>(b, j)) {
                    match = true;
                    break;
                }
            if (Kind == 3 || Kind == 5)
                match = !match;
        }
        if (match) {
            set_eax(c, pos);
            return;
        }
        if (backwards) {
            if (!pos)
                break;
            --pos;
        } else {
            if (pos == a.n)
                break;
            ++pos;
        }
    }
    set_eax(c, UINT32_MAX);
}
template <unsigned U> void copy(X86 *c) {
    const auto a = String<U>(c->r[R_ECX]).view();
    const auto v = substring<U>(c, a, arg(c, 2), arg(c, 1));
    range(arg(c, 0), uint64_t(v.n) * U);
    std::memmove(gm_ptr(arg(c, 0)), gm_ptr(v.p), size_t(v.n) * U);
    set_eax(c, v.n);
}
template <bool Nothrow = false> void new_(X86 *c) {
    if (!Nothrow)
        errors();
    const uint32_t p = heap_alloc(std::max(arg(c, 0), 1u), false);
    if (!p && !Nothrow)
        error(c, Allocation);
    set_eax(c, p);
}
void delete_(X86 *c) {
    if (arg(c, 0))
        heap_free(arg(c, 0));
}
template <Error Kind> void string_error(X86 *c) {
    error(c, Kind);
}
std::deque<std::string> names;
void add(const std::string &name, uint8_t argc, void (*fn)(X86 *)) {
    names.push_back(name);
    const ImportShim shim = {"msvcp80.dll", names.back().c_str(), argc, fn};
    imports_register(&shim, 1);
}
template <unsigned U> void strings() {
    const std::string t = U == 1 ? "D" : "_W", pc = U == 1 ? "PBD" : "PB_W",
                      pm = U == 1 ? "PAD" : "PA_W",
                      klass = "?$basic_string@" + t + "U?$char_traits@" + t +
                              "@std@@V?$allocator@" + t + "@2@@std@@";
    auto method = [&](const char *name, const std::string &sig, uint8_t argc, void (*fn)(X86 *)) {
        add("?" + std::string(name) + "@" + klass + sig, argc, fn);
    };
    auto op = [&](const char *name, const std::string &sig, uint8_t argc, void (*fn)(X86 *)) {
        add("??" + std::string(name) + klass + sig, argc, fn);
    };
    op("0", "QAE@XZ", 0, ctor<U>);
    op("0", "QAE@ABV?$allocator@" + t + "@1@@Z", 1, ctor<U>);
    op("1", "QAE@XZ", 0, dtor<U>);
    op("0", "QAE@" + pc + "@Z", 1, change<U, CString, true>);
    op("0", "QAE@" + pc + "I@Z", 2, change<U, Count, true>);
    op("0", "QAE@" + pc + "0@Z", 2, change<U, PointerRange, true>);
    op("0", "QAE@I" + t + "@Z", 2, change<U, Fill, true>);
    op("0", "QAE@ABV01@@Z", 1, change<U, Object, true>);
    op("0", "QAE@ABV01@II@Z", 3, change<U, Subobject, true>);
    const std::string allocator = "ABV?$allocator@" + t + "@1@@Z";
    op("0", "QAE@" + pc + allocator, 2, change<U, CString, true>);
    op("0", "QAE@" + pc + "I" + allocator, 3, change<U, Count, true>);
    op("0", "QAE@I" + t + allocator, 3, change<U, Fill, true>);
    op("0", "QAE@ABV01@II" + allocator, 4, change<U, Subobject, true>);
    for (const auto &name : {"assign", "append"}) {
        const bool app = std::strcmp(name, "append") == 0;
        if (app) {
            method(name, "QAEAAV12@" + pc + "@Z", 1, change<U, CString, false, 1>);
            method(name, "QAEAAV12@" + pc + "I@Z", 2, change<U, Count, false, 1>);
            method(name, "QAEAAV12@" + pc + "0@Z", 2, change<U, PointerRange, false, 1>);
            method(name, "QAEAAV12@I" + t + "@Z", 2, change<U, Fill, false, 1>);
            method(name, "QAEAAV12@ABV12@@Z", 1, change<U, Object, false, 1>);
            method(name, "QAEAAV12@ABV12@II@Z", 3, change<U, Subobject, false, 1>);
        } else {
            method(name, "QAEAAV12@" + pc + "@Z", 1, change<U, CString>);
            method(name, "QAEAAV12@" + pc + "I@Z", 2, change<U, Count>);
            method(name, "QAEAAV12@" + pc + "0@Z", 2, change<U, PointerRange>);
            method(name, "QAEAAV12@I" + t + "@Z", 2, change<U, Fill>);
            method(name, "QAEAAV12@ABV12@@Z", 1, change<U, Object>);
            method(name, "QAEAAV12@ABV12@II@Z", 3, change<U, Subobject>);
        }
    }
    op("4", "QAEAAV01@" + pc + "@Z", 1, change<U, CString>);
    op("4", "QAEAAV01@ABV01@@Z", 1, change<U, Object>);
    op("4", "QAEAAV01@" + t + "@Z", 1, change<U, Character>);
    op("Y", "QAEAAV01@" + pc + "@Z", 1, change<U, CString, false, 1>);
    op("Y", "QAEAAV01@ABV01@@Z", 1, change<U, Object, false, 1>);
    op("Y", "QAEAAV01@" + t + "@Z", 1, change<U, Character, false, 1>);
    method("insert", "QAEAAV12@I" + pc + "@Z", 2, change<U, CString, false, 2>);
    method("insert", "QAEAAV12@I" + pc + "I@Z", 3, change<U, Count, false, 2>);
    method("insert", "QAEAAV12@II" + t + "@Z", 3, change<U, Fill, false, 2>);
    method("insert", "QAEAAV12@IABV12@@Z", 2, change<U, Object, false, 2>);
    method("insert", "QAEAAV12@IABV12@II@Z", 4, change<U, Subobject, false, 2>);
    method("replace", "QAEAAV12@II" + pc + "@Z", 3, change<U, CString, false, 3>);
    method("replace", "QAEAAV12@II" + pc + "I@Z", 4, change<U, Count, false, 3>);
    method("replace", "QAEAAV12@III" + t + "@Z", 4, change<U, Fill, false, 3>);
    method("replace", "QAEAAV12@IIABV12@@Z", 3, change<U, Object, false, 3>);
    method("replace", "QAEAAV12@IIABV12@II@Z", 5, change<U, Subobject, false, 3>);
    method("c_str", "QBE" + pc + "XZ", 0, query<U, 0>);
    method("data", "QBE" + pc + "XZ", 0, query<U, 0>);
    method("size", "QBEIXZ", 0, query<U, 1>);
    method("length", "QBEIXZ", 0, query<U, 1>);
    method("capacity", "QBEIXZ", 0, query<U, 2>);
    method("empty", "QBE_NXZ", 0, query<U, 3>);
    method("max_size", "QBEIXZ", 0, query<U, 4>);
    const std::string ref = U == 1 ? "AAD" : "AA_W", cref = U == 1 ? "ABD" : "AB_W";
    op("A", "QAE" + ref + "I@Z", 1, index<U>);
    op("A", "QBE" + cref + "I@Z", 1, index<U>);
    method("at", "QAE" + ref + "I@Z", 1, index<U, true>);
    method("at", "QBE" + cref + "I@Z", 1, index<U, true>);
    method("erase", "QAEAAV12@II@Z", 2, erase<U>);
    method("clear", "QAEXXZ", 0, clear<U>);
    method("reserve", "QAEXI@Z", 1, reserve<U>);
    method("resize", "QAEXI@Z", 1, resize<U>);
    method("resize", "QAEXI" + t + "@Z", 2, resize<U, true>);
    method("swap", "QAEXAAV12@@Z", 1, swap<U>);
    method("substr", "QBE?AV12@II@Z", 3, substr<U>);
    method("copy", "QBEI" + pm + "II@Z", 3, copy<U>);
    method("compare", "QBEH" + pc + "@Z", 1, compare<U, CString>);
    method("compare", "QBEHABV12@@Z", 1, compare<U, Object>);
    method("compare", "QBEHII" + pc + "@Z", 3, compare<U, CString, true>);
    method("compare", "QBEHII" + pc + "I@Z", 4, compare<U, Count, true>);
    method("compare", "QBEHIIABV12@@Z", 3, compare<U, Object, true>);
    method("compare", "QBEHIIABV12@II@Z", 5, compare<U, Subobject, true>);
#define FIND(name, k)                                                                              \
    method(name, "QBEI" + pc + "I@Z", 2, find<U, CString, k>);                                     \
    method(name, "QBEI" + pc + "II@Z", 3, find<U, Count, k>);                                      \
    method(name, "QBEI" + t + "I@Z", 2, find<U, Character, k>);                                    \
    method(name, "QBEIABV12@I@Z", 2, find<U, Object, k>)
    FIND("find", 0);
    FIND("rfind", 1);
    FIND("find_first_of", 2);
    FIND("find_first_not_of", 3);
    FIND("find_last_of", 4);
    FIND("find_last_not_of", 5);
#undef FIND
}
} // namespace
void register_cpp() {
    strings<1>();
    strings<2>();
    add("?_Xlen@_String_base@std@@SAXXZ", ARGC_CDECL, string_error<Length>);
    add("?_Xran@_String_base@std@@SAXXZ", ARGC_CDECL, string_error<Position>);
    const ImportShim entries[] = {
        {"msvcr80.dll", "??2@YAPAXI@Z", ARGC_CDECL, new_<>},
        {"msvcr80.dll", "??_U@YAPAXI@Z", ARGC_CDECL, new_<>},
        {"msvcr80.dll", "??3@YAXPAX@Z", ARGC_CDECL, delete_},
        {"msvcr80.dll", "??_V@YAXPAX@Z", ARGC_CDECL, delete_},
        {"msvcr80.dll", "??2@YAPAXIABUnothrow_t@std@@@Z", ARGC_CDECL, new_<true>},
        {"msvcr80.dll", "??_U@YAPAXIABUnothrow_t@std@@@Z", ARGC_CDECL, new_<true>}};
    imports_register(entries, sizeof entries / sizeof entries[0]);
}
} // namespace crt
