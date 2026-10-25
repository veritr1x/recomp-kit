#include "crt_internal.h"
#include "../platform/os.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace crt {
[[noreturn]] void fail(const char *why, uint32_t address, uint64_t size) {
    const char *description = imports_current_description();
    std::fprintf(stderr, "CRT %s: %s (%08x, %llu bytes)\n",
                 description ? description : "initialization", why, address,
                 (unsigned long long)size);
    std::fflush(stderr);
    std::abort();
}
void range(uint32_t address, uint64_t size) {
    if (!size)
        return;
    if (!address || size > UINT32_MAX || !gm_valid(address, uint32_t(size)))
        fail("guest range is outside the arena", address, size);
}
uint32_t string_length(uint32_t address, uint32_t unit, uint32_t limit) {
    for (uint32_t n = 0; n < limit; ++n) {
        const uint64_t next = uint64_t(address) + uint64_t(n) * unit;
        if (next > UINT32_MAX)
            fail("string address wraps", address, next);
        range(uint32_t(next), unit);
        if (unit == 2 ? !rd16(uint32_t(next)) : !rd8(uint32_t(next)))
            return n;
    }
    fail("unterminated or oversized string", address, limit);
}
std::string text(uint32_t address, uint32_t limit) {
    const uint32_t length = string_length(address, 1, limit);
    return std::string((const char *)gm_ptr(address), length);
}
std::u16string wtext(uint32_t address, uint32_t limit) {
    const uint32_t length = string_length(address, 2, limit);
    std::u16string result;
    result.reserve(length);
    for (uint32_t i = 0; i < length; ++i)
        result.push_back(char16_t(rd16(address + i * 2)));
    return result;
}
double Args::f64() {
    const uint64_t bits = u64();
    double value;
    std::memcpy(&value, &bits, 8);
    return value;
}
namespace {
struct ThreadData {
    uint64_t generation = 0;
    uint32_t address = 0;
};
thread_local std::map<uint32_t, ThreadData> thread_data;
uint32_t cell(X86 *c) {
    auto &data = thread_data[c->fs_base];
    if (data.generation != mem_generation() || !heap_owns(data.address)) {
        data.address = heap_alloc(128, true);
        data.generation = mem_generation();
        if (!data.address)
            fail("cannot allocate CRT thread data");
        wr32(data.address + 8, 1); // MSVC rand's per-thread default seed
    }
    return data.address;
}
void errno_ptr(X86 *c) {
    set_eax(c, errno_address(c));
}
void doserrno_ptr(X86 *c) {
    set_eax(c, doserrno_address(c));
}
void get_errno(X86 *c) {
    if (!arg(c, 0)) {
        set_eax(c, EINVAL_);
        return;
    }
    range(arg(c, 0), 4);
    wr32(arg(c, 0), rd32(errno_address(c)));
    set_eax(c, 0);
}
void put_errno(X86 *c) {
    wr32(errno_address(c), arg(c, 0));
    set_eax(c, 0);
}
void get_doserrno(X86 *c) {
    if (!arg(c, 0)) {
        set_eax(c, EINVAL_);
        return;
    }
    range(arg(c, 0), 4);
    wr32(arg(c, 0), rd32(doserrno_address(c)));
    set_eax(c, 0);
}
void put_doserrno(X86 *c) {
    wr32(doserrno_address(c), arg(c, 0));
    set_eax(c, 0);
}
void dosmaperr(X86 *c) {
    const uint32_t dos = arg(c, 0);
    uint32_t error = EINVAL_;
    if (dos >= 19 && dos <= 36)
        error = EACCES_;
    if (dos >= 188 && dos <= 202)
        error = 8; // ENOEXEC
    switch (dos) {
    case 2:
    case 3:
    case 15:
    case 18:
    case 53:
    case 67:
    case 161:
        error = ENOENT_;
        break;
    case 4:
        error = 24;
        break; // EMFILE
    case 5:
    case 16:
    case 65:
    case 82:
    case 83:
    case 108:
    case 132:
        error = EACCES_;
        break;
    case 6:
    case 114:
    case 130:
        error = EBADF_;
        break;
    case 7:
    case 8:
    case 9:
    case 14:
    case 1816:
        error = ENOMEM_;
        break;
    case 10:
        error = 7;
        break; // E2BIG
    case 11:
        error = 8;
        break;
    case 17:
        error = 18;
        break; // EXDEV
    case 80:
    case 183:
        error = 17;
        break; // EEXIST
    case 89:
    case 164:
    case 215:
        error = 11;
        break; // EAGAIN
    case 109:
        error = 32;
        break; // EPIPE
    case 112:
        error = 28;
        break; // ENOSPC
    case 128:
    case 129:
        error = 10;
        break; // ECHILD
    case 145:
        error = 41;
        break; // ENOTEMPTY (MSVC's errno values)
    case 158:
        error = 13;
        break;
    case 206:
        error = 38;
        break; // ENAMETOOLONG
    default:
        break;
    }
    set_errno(c, error, dos);
    wr32(doserrno_address(c), dos);
}
void memory_copy(X86 *c) {
    const uint32_t dst = arg(c, 0), src = arg(c, 1), size = arg(c, 2);
    range(dst, size);
    range(src, size);
    if (size)
        std::memmove(gm_ptr(dst), gm_ptr(src), size);
    set_eax(c, dst);
}
void memory_set(X86 *c) {
    const uint32_t dst = arg(c, 0), size = arg(c, 2);
    range(dst, size);
    if (size)
        std::memset(gm_ptr(dst), int(arg(c, 1)), size);
    set_eax(c, dst);
}
void memory_compare(X86 *c) {
    const uint32_t left = arg(c, 0), right = arg(c, 1), size = arg(c, 2);
    range(left, size);
    range(right, size);
    set_eax(c, size ? uint32_t(std::memcmp(gm_ptr(left), gm_ptr(right), size)) : 0);
}
void memory_chr(X86 *c) {
    const uint32_t address = arg(c, 0), size = arg(c, 2);
    range(address, size);
    const void *found = size ? std::memchr(gm_ptr(address), int(arg(c, 1)), size) : nullptr;
    set_eax(c, found ? uint32_t((const uint8_t *)found - g_mem) : 0);
}
void memory_icmp(X86 *c) {
    const uint32_t left = arg(c, 0), right = arg(c, 1), size = arg(c, 2);
    range(left, size);
    range(right, size);
    for (uint32_t i = 0; i < size; ++i) {
        auto fold = [](uint32_t a) { return a >= 'A' && a <= 'Z' ? a + 32 : a; };
        const auto a = fold(rd8(left + i)), b = fold(rd8(right + i));
        if (a != b) {
            set_eax(c, a < b ? UINT32_MAX : 1);
            return;
        }
    }
    set_eax(c, 0);
}
uint32_t allocate(X86 *c, uint64_t size, bool zero = false) {
    if (size > GUEST_SIZE - 16) {
        set_errno(c, ENOMEM_);
        return 0;
    }
    const uint32_t result = heap_alloc(uint32_t(size), zero);
    if (!result)
        set_errno(c, ENOMEM_);
    return result;
}
void malloc_(X86 *c) {
    set_eax(c, allocate(c, arg(c, 0)));
}
void calloc_(X86 *c) {
    set_eax(c, allocate(c, uint64_t(arg(c, 0)) * arg(c, 1), true));
}
void free_(X86 *c) {
    if (arg(c, 0) && !heap_free(arg(c, 0)))
        set_errno(c, EINVAL_);
}
void realloc_(X86 *c) {
    const uint32_t address = arg(c, 0), size = arg(c, 1);
    if (!address) {
        set_eax(c, allocate(c, size));
        return;
    }
    if (!heap_owns(address)) {
        set_errno(c, EINVAL_);
        set_eax(c, 0);
        return;
    }
    if (!size) {
        heap_free(address);
        set_eax(c, 0);
        return;
    }
    if (size > GUEST_SIZE - 16) {
        set_errno(c, ENOMEM_);
        set_eax(c, 0);
        return;
    }
    const uint32_t next = heap_realloc(address, size);
    if (!next)
        set_errno(c, ENOMEM_);
    set_eax(c, next);
}
void msize(X86 *c) {
    const uint32_t size = heap_size(arg(c, 0));
    if (size == UINT32_MAX)
        set_errno(c, EINVAL_);
    set_eax(c, size);
}
struct Aligned {
    uint32_t base, size, alignment, offset;
    uint64_t generation;
};
std::map<uint32_t, Aligned> aligned;
uint32_t aligned_allocate(X86 *c, uint32_t size, uint32_t alignment, uint32_t offset) {
    if (!alignment || (alignment & (alignment - 1)) || (offset >= size && (size || offset))) {
        set_errno(c, EINVAL_);
        return 0;
    }
    alignment = std::max(alignment, 4u);
    // Refuse wrapping overhead before either the heap or align-up arithmetic.
    const uint64_t bytes = uint64_t(size) + alignment + 8;
    if (bytes > UINT32_MAX || bytes > GUEST_SIZE - 16) {
        set_errno(c, ENOMEM_);
        return 0;
    }
    const uint32_t base = allocate(c, bytes);
    if (!base)
        return 0;
    const uint32_t address =
        uint32_t((uint64_t(base) + 8 + offset + alignment - 1) & ~uint64_t(alignment - 1)) - offset;
    aligned[address] = {base, size, alignment, offset, mem_generation()};
    return address;
}
void aligned_malloc(X86 *c) {
    set_eax(c, aligned_allocate(c, arg(c, 0), arg(c, 1), 0));
}
void aligned_offset_malloc(X86 *c) {
    set_eax(c, aligned_allocate(c, arg(c, 0), arg(c, 1), arg(c, 2)));
}
void aligned_free(X86 *c) {
    const uint32_t address = arg(c, 0);
    if (!address)
        return;
    const auto found = aligned.find(address);
    if (found == aligned.end() || found->second.generation != mem_generation()) {
        set_errno(c, EINVAL_);
        return;
    }
    heap_free(found->second.base);
    aligned.erase(found);
}
void aligned_msize(X86 *c) {
    const auto found = aligned.find(arg(c, 0));
    if (found == aligned.end() || found->second.generation != mem_generation() ||
        found->second.alignment != std::max(arg(c, 1), 4u) || found->second.offset != arg(c, 2)) {
        set_errno(c, EINVAL_);
        set_eax(c, UINT32_MAX);
        return;
    }
    set_eax(c, found->second.size);
}
void aligned_reallocate(X86 *c, bool offset) {
    const uint32_t address = arg(c, 0), size = arg(c, 1), alignment = arg(c, 2),
                   position = offset ? arg(c, 3) : 0;
    if (!address) {
        set_eax(c, aligned_allocate(c, size, alignment, position));
        return;
    }
    auto found = aligned.find(address);
    if (found == aligned.end() || found->second.generation != mem_generation()) {
        set_errno(c, EINVAL_);
        set_eax(c, 0);
        return;
    }
    if (!size) {
        heap_free(found->second.base);
        aligned.erase(found);
        set_eax(c, 0);
        return;
    }
    const uint32_t next = aligned_allocate(c, size, alignment, position);
    if (next) {
        const uint32_t bytes = std::min(size, found->second.size);
        range(next, bytes);
        range(address, bytes);
        std::memcpy(gm_ptr(next), gm_ptr(address), bytes);
        heap_free(found->second.base);
        aligned.erase(found);
    }
    set_eax(c, next);
}
void aligned_realloc(X86 *c) {
    aligned_reallocate(c, false);
}
void aligned_offset_realloc(X86 *c) {
    aligned_reallocate(c, true);
}
void srand_(X86 *c) {
    wr32(cell(c) + 8, arg(c, 0));
}
void rand_(X86 *c) {
    const uint32_t address = cell(c) + 8, next = rd32(address) * 214013u + 2531011u;
    wr32(address, next);
    set_eax(c, (next >> 16) & 32767);
}
int32_t compare(X86 *c, uint32_t comparator, uint32_t left, uint32_t right) {
    const uint32_t args[] = {left, right};
    return int32_t(guest_call(c, comparator, args, 2));
}
void swap_elements(uint32_t base, uint32_t left, uint32_t right, uint32_t width) {
    if (left == right)
        return;
    uint8_t *a = gm_ptr(base + left * width), *b = gm_ptr(base + right * width);
    // The whole table was range-checked. Raw iterators own no native resource
    // if a comparator performs a guest longjmp during a later comparison.
    std::swap_ranges(a, a + width, b);
}
void sift(X86 *c, uint32_t base, uint32_t root, uint32_t count, uint32_t width,
          uint32_t comparator) {
    for (;;) {
        const uint64_t next = uint64_t(root) * 2 + 1;
        if (next >= count)
            return;
        uint32_t child = uint32_t(next);
        if (child + 1 < count &&
            compare(c, comparator, base + child * width, base + (child + 1) * width) < 0)
            ++child;
        if (compare(c, comparator, base + root * width, base + child * width) >= 0)
            return;
        swap_elements(base, root, child, width);
        root = child;
    }
}
void qsort_(X86 *c) {
    const uint32_t base = arg(c, 0), count = arg(c, 1), width = arg(c, 2), comparator = arg(c, 3);
    range(base, uint64_t(count) * width);
    if (count < 2 || !width)
        return;
    range(comparator, 1);
    for (uint32_t start = count / 2; start-- > 0;)
        sift(c, base, start, count, width, comparator);
    for (uint32_t end = count; --end > 0;) {
        swap_elements(base, 0, end, width);
        sift(c, base, 0, end, width, comparator);
    }
}
void bsearch_(X86 *c) {
    const uint32_t key = arg(c, 0), base = arg(c, 1), count = arg(c, 2), width = arg(c, 3),
                   comparator = arg(c, 4);
    range(base, uint64_t(count) * width);
    if (!count || !width) {
        set_eax(c, 0);
        return;
    }
    range(key, 1);
    range(comparator, 1);
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2, address = base + mid * width;
        const int32_t order = compare(c, comparator, key, address);
        if (!order) {
            set_eax(c, address);
            return;
        }
        if (order < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    set_eax(c, 0);
}
void unknown(X86 *) {
    fail("unimplemented import");
}
} // namespace
uint32_t errno_address(X86 *c) {
    return cell(c);
}
uint32_t doserrno_address(X86 *c) {
    return cell(c) + 4;
}
uint32_t token_context(X86 *c, uint32_t unit) {
    return cell(c) + (unit == 2 ? 16 : 12);
}
void thread_detach(uint32_t teb) {
    auto it = thread_data.find(teb);
    if (it == thread_data.end())
        return;
    if (it->second.generation == mem_generation() && heap_owns(it->second.address))
        heap_free(it->second.address);
    thread_data.erase(it);
}
void set_errno(X86 *c, uint32_t value, uint32_t dos) {
    wr32(errno_address(c), value);
    if (dos)
        wr32(doserrno_address(c), dos);
}
uint32_t win32_call(X86 *c, const char *name, const uint32_t *args, uint32_t count) {
    const uint32_t target = imports_resolve("KERNEL32.dll", name);
    if (!target || count > 16)
        fail("invalid internal Win32 dispatch");
    const uint32_t esp = c->r[R_ESP], eip = c->eip;
    if (esp < 4 * (count + 1))
        fail("Win32 argument stack wraps", esp);
    const uint32_t temporary = esp - 4 * (count + 1);
    range(temporary, 4 * (count + 1));
    range(esp, 4);
    wr32(temporary, rd32(esp));
    for (uint32_t i = 0; i < count; ++i)
        wr32(temporary + 4 + i * 4, args[i]);
    c->r[R_ESP] = temporary;
    // No guest_call ownership record: ExitThread/ExitProcess abandon this call.
    imports_dispatch(c, target);
    c->r[R_ESP] = esp;
    c->eip = eip;
    return c->r[R_EAX];
}
void register_core() {
    const ImportShim entries[] = {
        {"msvcr80.dll", "memcpy", ARGC_CDECL, memory_copy},
        {"msvcr80.dll", "memmove", ARGC_CDECL, memory_copy},
        {"msvcr80.dll", "memset", ARGC_CDECL, memory_set},
        {"msvcr80.dll", "memcmp", ARGC_CDECL, memory_compare},
        {"msvcr80.dll", "memchr", ARGC_CDECL, memory_chr},
        {"msvcr80.dll", "_memicmp", ARGC_CDECL, memory_icmp},
        {"msvcr80.dll", "malloc", ARGC_CDECL, malloc_},
        {"msvcr80.dll", "calloc", ARGC_CDECL, calloc_},
        {"msvcr80.dll", "free", ARGC_CDECL, free_},
        {"msvcr80.dll", "realloc", ARGC_CDECL, realloc_},
        {"msvcr80.dll", "_msize", ARGC_CDECL, msize},
        {"msvcr80.dll", "_aligned_malloc", ARGC_CDECL, aligned_malloc},
        {"msvcr80.dll", "_aligned_offset_malloc", ARGC_CDECL, aligned_offset_malloc},
        {"msvcr80.dll", "_aligned_free", ARGC_CDECL, aligned_free},
        {"msvcr80.dll", "_aligned_realloc", ARGC_CDECL, aligned_realloc},
        {"msvcr80.dll", "_aligned_msize", ARGC_CDECL, aligned_msize},
        {"msvcr80.dll", "_aligned_offset_realloc", ARGC_CDECL, aligned_offset_realloc},
        {"msvcr80.dll", "_errno", ARGC_CDECL, errno_ptr},
        {"msvcr80.dll", "__doserrno", ARGC_CDECL, doserrno_ptr},
        {"msvcr80.dll", "_get_errno", ARGC_CDECL, get_errno},
        {"msvcr80.dll", "_set_errno", ARGC_CDECL, put_errno},
        {"msvcr80.dll", "_get_doserrno", ARGC_CDECL, get_doserrno},
        {"msvcr80.dll", "_set_doserrno", ARGC_CDECL, put_doserrno},
        {"msvcr80.dll", "_dosmaperr", ARGC_CDECL, dosmaperr},
        {"msvcr80.dll", "srand", ARGC_CDECL, srand_},
        {"msvcr80.dll", "rand", ARGC_CDECL, rand_},
        {"msvcr80.dll", "qsort", ARGC_CDECL, qsort_},
        {"msvcr80.dll", "bsearch", ARGC_CDECL, bsearch_},
    };
    imports_register(entries, sizeof entries / sizeof entries[0]);
}
bool fallback(const char *dll, const char *name, ImportShim *out) {
    if (!dll || (os_strcasecmp(dll, "msvcr80.dll") && os_strcasecmp(dll, "msvcp80.dll")))
        return false;
    if (out)
        *out = {dll, name, ARGC_CDECL, unknown};
    return true;
}
} // namespace crt
void crt_register() {
    crt::register_core();
    crt::register_strings();
    crt::register_math();
    crt::register_stdio();
    crt::register_startup();
    crt::register_cpp();
}
bool crt_import_fallback(const char *dll, const char *name, ImportShim *out) {
    return crt::fallback(dll, name, out);
}
