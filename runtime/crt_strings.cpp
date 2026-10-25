#include "crt_internal.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace crt {
namespace {
template <unsigned U> uint32_t at(uint32_t base, uint32_t index) {
    const uint64_t address = uint64_t(base) + uint64_t(index) * U;
    if (address > UINT32_MAX)
        fail("string offset wraps", base, address);
    range(uint32_t(address), U);
    return uint32_t(address);
}
template <unsigned U> uint32_t get(uint32_t base, uint32_t index = 0) {
    const uint32_t address = at<U>(base, index);
    return U == 2 ? rd16(address) : rd8(address);
}
template <unsigned U> void put(uint32_t base, uint32_t index, uint32_t value) {
    const uint32_t address = at<U>(base, index);
    if (U == 2)
        wr16(address, uint16_t(value));
    else
        wr8(address, uint8_t(value));
}
uint32_t fold(uint32_t ch, unsigned unit) {
    return locale_lower(ch, unit == 2);
}
template <unsigned U> uint32_t bounded_length(uint32_t address, uint32_t limit) {
    for (uint32_t i = 0; i < limit; ++i)
        if (!get<U>(address, i))
            return i;
    return limit;
}
template <unsigned U, bool Limited = false> void length(X86 *c) {
    set_eax(c, Limited ? bounded_length<U>(arg(c, 0), arg(c, 1)) : string_length(arg(c, 0), U));
}
template <unsigned U, bool Fold = false, bool Limited = false> void compare(X86 *c) {
    const uint32_t left = arg(c, 0), right = arg(c, 1), limit = Limited ? arg(c, 2) : 1u << 24;
    for (uint32_t i = 0; i < limit; ++i) {
        uint32_t a = get<U>(left, i), b = get<U>(right, i);
        if (Fold) {
            a = fold(a, U);
            b = fold(b, U);
        }
        if (a != b) {
            set_eax(c, a < b ? UINT32_MAX : 1);
            return;
        }
        if (!a) {
            set_eax(c, 0);
            return;
        }
    }
    if (!Limited)
        fail("oversized string", left, limit);
    set_eax(c, 0);
}
template <unsigned U, bool Limited = false> void copy(X86 *c) {
    const uint32_t dest = arg(c, 0), source = arg(c, 1);
    if (Limited) {
        const uint32_t count = arg(c, 2);
        range(dest, uint64_t(count) * U);
        bool ended = false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t value = ended ? 0 : get<U>(source, i);
            put<U>(dest, i, value);
            ended |= !value;
        }
    } else {
        const uint32_t count = string_length(source, U) + 1;
        range(dest, uint64_t(count) * U);
        std::memmove(gm_ptr(dest), gm_ptr(source), size_t(count) * U);
    }
    set_eax(c, dest);
}
template <unsigned U, bool Limited = false> void concatenate(X86 *c) {
    const uint32_t dest = arg(c, 0), source = arg(c, 1), old = string_length(dest, U);
    const uint32_t added =
        Limited ? bounded_length<U>(source, arg(c, 2)) : string_length(source, U);
    range(dest, (uint64_t(old) + added + 1) * U);
    range(source, uint64_t(added) * U);
    if (added)
        std::memmove(gm_ptr(dest + old * U), gm_ptr(source), size_t(added) * U);
    put<U>(dest, old + added, 0);
    set_eax(c, dest);
}
template <unsigned U, bool Last = false> void character(X86 *c) {
    const uint32_t source = arg(c, 0), needle = arg(c, 1) & (U == 2 ? 0xffff : 255),
                   len = string_length(source, U);
    uint32_t found = 0;
    for (uint32_t i = 0; i <= len; ++i)
        if (get<U>(source, i) == needle) {
            found = source + i * U;
            if (!Last)
                break;
        }
    set_eax(c, found);
}
template <unsigned U> void substring(X86 *c) {
    const uint32_t hay = arg(c, 0), needle = arg(c, 1), n = string_length(hay, U),
                   m = string_length(needle, U);
    if (m <= n)
        for (uint32_t i = 0; i <= n - m; ++i)
            if (!std::memcmp(gm_ptr(hay + i * U), gm_ptr(needle), size_t(m) * U)) {
                set_eax(c, hay + i * U);
                return;
            }
    set_eax(c, 0);
}
template <unsigned U> void duplicate(X86 *c) {
    const uint32_t source = arg(c, 0), len = string_length(source, U) + 1;
    const uint64_t bytes = uint64_t(len) * U;
    const uint32_t dest = bytes < GUEST_SIZE ? heap_alloc(uint32_t(bytes), false) : 0;
    if (!dest)
        set_errno(c, ENOMEM_);
    else {
        range(dest, bytes);
        range(source, bytes);
        std::memcpy(gm_ptr(dest), gm_ptr(source), size_t(bytes));
    }
    set_eax(c, dest);
}
template <unsigned U, bool Upper = false> void case_change(X86 *c) {
    const uint32_t source = arg(c, 0), len = string_length(source, U);
    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t original = get<U>(source, i);
        const uint32_t value = Upper ? locale_upper(original, U == 2) : fold(original, U);
        put<U>(source, i, value);
    }
    set_eax(c, source);
}
template <unsigned U> void reverse(X86 *c) {
    const uint32_t source = arg(c, 0), len = string_length(source, U);
    for (uint32_t i = 0; i < len / 2; ++i) {
        const auto a = get<U>(source, i), b = get<U>(source, len - 1 - i);
        put<U>(source, i, b);
        put<U>(source, len - 1 - i, a);
    }
    set_eax(c, source);
}
template <unsigned U, unsigned Kind> void span(X86 *c) {
    const uint32_t source = arg(c, 0), set = arg(c, 1), len = string_length(source, U),
                   n = string_length(set, U);
    std::array<uint64_t, U == 1 ? 4 : 1024> bits{};
    for (uint32_t i = 0; i < n; ++i) {
        const auto ch = get<U>(set, i);
        bits[ch / 64] |= uint64_t(1) << (ch % 64);
    }
    uint32_t i = 0;
    for (; i < len; ++i) {
        const auto ch = get<U>(source, i);
        const bool member = (bits[ch / 64] >> (ch % 64) & 1) != 0;
        if (Kind == 0 ? !member : member)
            break;
    }
    set_eax(c, Kind == 2 ? (i < len ? source + i * U : 0) : i);
}
template <unsigned U, bool Secure = false> void tokenize(X86 *c) {
    const uint32_t context = Secure ? arg(c, 2) : token_context(c, U), separators = arg(c, 1);
    range(context, 4);
    uint32_t cursor = arg(c, 0) ? arg(c, 0) : rd32(context);
    if (!cursor) {
        set_eax(c, 0);
        return;
    }
    const uint32_t n = string_length(separators, U);
    auto separator = [&](uint32_t ch) {
        for (uint32_t j = 0; j < n; ++j)
            if (get<U>(separators, j) == ch)
                return true;
        return false;
    };
    while (get<U>(cursor) && separator(get<U>(cursor)))
        cursor = at<U>(cursor, 1);
    if (!get<U>(cursor)) {
        wr32(context, 0);
        set_eax(c, 0);
        return;
    }
    const uint32_t start = cursor;
    while (get<U>(cursor) && !separator(get<U>(cursor)))
        cursor = at<U>(cursor, 1);
    if (get<U>(cursor)) {
        put<U>(cursor, 0, 0);
        cursor = at<U>(cursor, 1);
    } else
        cursor = 0;
    wr32(context, cursor);
    set_eax(c, start);
}
template <unsigned U, bool Append = false, bool Limited = false> void secure_copy(X86 *c) {
    const uint32_t dest = arg(c, 0), capacity = arg(c, 1), source = arg(c, 2),
                   count = Limited ? arg(c, 3) : UINT32_MAX;
    if (!dest || !capacity) {
        set_errno(c, EINVAL_);
        set_eax(c, EINVAL_);
        return;
    }
    range(dest, uint64_t(capacity) * U);
    auto error = [&](uint32_t value) {
        put<U>(dest, 0, 0);
        set_errno(c, value);
        set_eax(c, value);
    };
    if (!source) {
        error(EINVAL_);
        return;
    }
    const uint32_t old = Append ? bounded_length<U>(dest, capacity) : 0;
    if (old == capacity) {
        error(EINVAL_);
        return;
    }
    const uint32_t n = Limited && count != UINT32_MAX ? bounded_length<U>(source, count)
                                                      : string_length(source, U);
    uint32_t written = n, status = 0;
    if (uint64_t(old) + n >= capacity) {
        if (!Limited || count != UINT32_MAX) {
            error(ERANGE_);
            return;
        }
        written = capacity - old - 1;
        status = 80; // STRUNCATE
    }
    range(source, uint64_t(written) * U);
    if (written)
        std::memmove(gm_ptr(dest + old * U), gm_ptr(source), size_t(written) * U);
    put<U>(dest, old + written, 0);
    set_eax(c, status);
}
uint32_t digit(uint32_t ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A' + 10;
    return 36;
}
bool whitespace(uint32_t ch) {
    return ch == ' ' || (ch >= 9 && ch <= 13);
}
template <unsigned U, unsigned Bits, bool Signed, bool Simple = false> void integer(X86 *c) {
    const uint32_t source = arg(c, 0), end = Simple ? 0 : arg(c, 1),
                   base_arg = Simple ? 10 : arg(c, 2);
    const uint32_t length = string_length(source, U);
    if (end)
        range(end, 4);
    auto result = [&](uint64_t value) {
        if (Bits == 64)
            set_eax64(c, value);
        else
            set_eax(c, uint32_t(value));
    };
    if (base_arg && (base_arg < 2 || base_arg > 36)) {
        if (end)
            wr32(end, source);
        set_errno(c, EINVAL_);
        result(0);
        return;
    }
    uint32_t p = 0, base = base_arg;
    while (p < length && whitespace(get<U>(source, p)))
        ++p;
    bool negative = false;
    if (p < length && (get<U>(source, p) == '-' || get<U>(source, p) == '+'))
        negative = get<U>(source, p++) == '-';
    if ((!base || base == 16) && p + 2 < length && get<U>(source, p) == '0' &&
        (get<U>(source, p + 1) == 'x' || get<U>(source, p + 1) == 'X') &&
        digit(get<U>(source, p + 2)) < 16) {
        base = 16;
        p += 2;
    }
    if (!base)
        base = p < length && get<U>(source, p) == '0' ? 8 : 10;
    const uint64_t maximum = Signed ? ((uint64_t(1) << (Bits - 1)) - (!negative))
                                    : (Bits == 64 ? UINT64_MAX : UINT32_MAX);
    uint64_t value = 0;
    bool any = false, overflow = false;
    for (; p < length; ++p) {
        const uint32_t d = digit(get<U>(source, p));
        if (d >= base)
            break;
        any = true;
        if (value > (maximum - d) / base)
            overflow = true;
        else if (!overflow)
            value = value * base + d;
    }
    if (end)
        wr32(end, any ? source + p * U : source);
    if (overflow) {
        set_errno(c, ERANGE_);
        value = maximum;
    }
    if (negative && !(overflow && !Signed))
        value = uint64_t(0) - value;
    result(value);
}
template <unsigned U, unsigned Bits, bool Signed, bool Secure = false> void integer_string(X86 *c) {
    Args args{c->r[R_ESP] + 4};
    const uint64_t raw = Bits == 64 ? args.u64() : args.u32();
    const uint32_t dest = args.u32(), capacity = Secure ? args.u32() : UINT32_MAX,
                   base = args.u32();
    if (base < 2 || base > 36 || !dest || !capacity) {
        if (dest && capacity)
            put<U>(dest, 0, 0);
        set_errno(c, EINVAL_);
        set_eax(c, Secure ? EINVAL_ : 0);
        return;
    }
    const bool negative = Signed && base == 10 && ((raw >> (Bits - 1)) & 1);
    uint64_t value =
        negative ? (Bits == 64 ? uint64_t(0) - raw : uint32_t(0) - uint32_t(raw)) : raw;
    char digits[66];
    uint32_t n = 0;
    do {
        const auto d = uint32_t(value % base);
        digits[n++] = char(d < 10 ? '0' + d : 'a' + d - 10);
        value /= base;
    } while (value);
    if (negative)
        digits[n++] = '-';
    if (n >= capacity) {
        put<U>(dest, 0, 0);
        set_errno(c, ERANGE_);
        set_eax(c, Secure ? ERANGE_ : 0);
        return;
    }
    range(dest, uint64_t(n + 1) * U);
    for (uint32_t i = 0; i < n; ++i)
        put<U>(dest, i, uint8_t(digits[n - 1 - i]));
    put<U>(dest, n, 0);
    set_eax(c, Secure ? 0 : dest);
}
} // namespace

void register_strings() {
#define CRT(name, ...)                                                                             \
    {                                                                                              \
        "msvcr80.dll", name, ARGC_CDECL, __VA_ARGS__                                               \
    }
    const ImportShim entries[] = {
        CRT("strlen", length<1>),
        CRT("wcslen", length<2>),
        CRT("strnlen", length<1, true>),
        CRT("_strnlen", length<1, true>),
        CRT("wcsnlen", length<2, true>),
        CRT("_wcsnlen", length<2, true>),
        CRT("strcmp", compare<1>),
        CRT("wcscmp", compare<2>),
        CRT("strncmp", compare<1, false, true>),
        CRT("wcsncmp", compare<2, false, true>),
        CRT("_stricmp", compare<1, true>),
        CRT("_strcmpi", compare<1, true>),
        CRT("_wcsicmp", compare<2, true>),
        CRT("_strnicmp", compare<1, true, true>),
        CRT("_wcsnicmp", compare<2, true, true>),
        CRT("strcpy", copy<1>),
        CRT("wcscpy", copy<2>),
        CRT("strncpy", copy<1, true>),
        CRT("wcsncpy", copy<2, true>),
        CRT("strcat", concatenate<1>),
        CRT("wcscat", concatenate<2>),
        CRT("strncat", concatenate<1, true>),
        CRT("wcsncat", concatenate<2, true>),
        CRT("strchr", character<1>),
        CRT("wcschr", character<2>),
        CRT("strrchr", character<1, true>),
        CRT("wcsrchr", character<2, true>),
        CRT("strstr", substring<1>),
        CRT("wcsstr", substring<2>),
        CRT("_strdup", duplicate<1>),
        CRT("_wcsdup", duplicate<2>),
        CRT("_strlwr", case_change<1>),
        CRT("_wcslwr", case_change<2>),
        CRT("_strupr", case_change<1, true>),
        CRT("_wcsupr", case_change<2, true>),
        CRT("_strrev", reverse<1>),
        CRT("_wcsrev", reverse<2>),
        CRT("strspn", span<1, 0>),
        CRT("wcsspn", span<2, 0>),
        CRT("strcspn", span<1, 1>),
        CRT("wcscspn", span<2, 1>),
        CRT("strpbrk", span<1, 2>),
        CRT("wcspbrk", span<2, 2>),
        CRT("strtok", tokenize<1>),
        CRT("wcstok", tokenize<2>),
        CRT("strtok_s", tokenize<1, true>),
        CRT("wcstok_s", tokenize<2, true>),
        CRT("strcpy_s", secure_copy<1>),
        CRT("wcscpy_s", secure_copy<2>),
        CRT("strncpy_s", secure_copy<1, false, true>),
        CRT("wcsncpy_s", secure_copy<2, false, true>),
        CRT("strcat_s", secure_copy<1, true>),
        CRT("wcscat_s", secure_copy<2, true>),
        CRT("strncat_s", secure_copy<1, true, true>),
        CRT("wcsncat_s", secure_copy<2, true, true>),
        CRT("strtol", integer<1, 32, true>),
        CRT("wcstol", integer<2, 32, true>),
        CRT("strtoul", integer<1, 32, false>),
        CRT("wcstoul", integer<2, 32, false>),
        CRT("_strtoi64", integer<1, 64, true>),
        CRT("_wcstoi64", integer<2, 64, true>),
        CRT("_strtoui64", integer<1, 64, false>),
        CRT("_wcstoui64", integer<2, 64, false>),
        CRT("_strtol_l", integer<1, 32, true>),
        CRT("_wcstol_l", integer<2, 32, true>),
        CRT("_strtoul_l", integer<1, 32, false>),
        CRT("_wcstoul_l", integer<2, 32, false>),
        CRT("_strtoi64_l", integer<1, 64, true>),
        CRT("_wcstoi64_l", integer<2, 64, true>),
        CRT("_strtoui64_l", integer<1, 64, false>),
        CRT("_wcstoui64_l", integer<2, 64, false>),
        CRT("atoi", integer<1, 32, true, true>),
        CRT("atol", integer<1, 32, true, true>),
        CRT("_wtoi", integer<2, 32, true, true>),
        CRT("_wtol", integer<2, 32, true, true>),
        CRT("_atoi64", integer<1, 64, true, true>),
        CRT("_wtoi64", integer<2, 64, true, true>),
        CRT("_itoa", integer_string<1, 32, true>),
        CRT("_itow", integer_string<2, 32, true>),
        CRT("_ltoa", integer_string<1, 32, true>),
        CRT("_ltow", integer_string<2, 32, true>),
        CRT("_ultoa", integer_string<1, 32, false>),
        CRT("_ultow", integer_string<2, 32, false>),
        CRT("_i64toa", integer_string<1, 64, true>),
        CRT("_i64tow", integer_string<2, 64, true>),
        CRT("_ui64toa", integer_string<1, 64, false>),
        CRT("_ui64tow", integer_string<2, 64, false>),
        CRT("_itoa_s", integer_string<1, 32, true, true>),
        CRT("_itow_s", integer_string<2, 32, true, true>),
        CRT("_i64toa_s", integer_string<1, 64, true, true>),
        CRT("_i64tow_s", integer_string<2, 64, true, true>),
    };
    imports_register(entries, sizeof entries / sizeof entries[0]);
#undef CRT
}
} // namespace crt
