// VC8 CRT implementation helpers. Every pointer crossing this boundary is a
// 32-bit guest address; bulk host operations validate the complete range.
#pragma once
#include "imports.h"
#include "memory.h"
#include <string>

namespace crt {
constexpr uint32_t EINVAL_ = 22, ERANGE_ = 34, ENOMEM_ = 12, EBADF_ = 9, ENOENT_ = 2, EACCES_ = 13;
[[noreturn]] void fail(const char *why, uint32_t address = 0, uint64_t size = 0);
void range(uint32_t address, uint64_t size);
std::string text(uint32_t address, uint32_t limit = 1u << 20);
std::u16string wtext(uint32_t address, uint32_t limit = 1u << 20);
uint32_t string_length(uint32_t address, uint32_t unit, uint32_t limit = 1u << 24);
void set_errno(X86 *c, uint32_t value, uint32_t dos = 0);
uint32_t errno_address(X86 *c);
uint32_t doserrno_address(X86 *c);
uint32_t token_context(X86 *c, uint32_t unit);
void thread_detach(uint32_t teb);
uint32_t locale_lower(uint32_t ch, bool wide = true);
uint32_t locale_upper(uint32_t ch, bool wide = true);
uint32_t win32_call(X86 *c, const char *name, const uint32_t *args, uint32_t count);
void register_core();
void register_strings();
void register_math();
void register_stdio();
void register_startup();
void register_cpp();
struct Args {
    uint32_t at;
    uint32_t u32() {
        range(at, 4);
        const auto value = rd32(at);
        at += 4;
        return value;
    }
    uint64_t u64() {
        const uint64_t low = u32();
        return low | uint64_t(u32()) << 32;
    }
    double f64();
};
} // namespace crt
void crt_register();
bool crt_import_fallback(const char *dll, const char *name, ImportShim *out);
