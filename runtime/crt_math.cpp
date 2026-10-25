#include "crt_internal.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <cerrno>

namespace crt {
namespace {
constexpr uint32_t EM = 0x8001f, RC = 0x300, PC = 0x30000, IC = 0x40000, DN = 0x3000000;
constexpr uint32_t masks[] = {0x10, 0x80000, 8, 4, 2, 1};
uint32_t exception_flags(uint32_t flags) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 6; ++i)
        if (flags >> i & 1)
            result |= masks[i];
    return result;
}
uint32_t x87_control(const X86 *c) {
    constexpr uint32_t precision[] = {0x20000, 0x30000, 0x10000, 0};
    return exception_flags(c->fpu_cw & 63) | ((uint32_t(c->fpu_cw) >> 2) & RC) |
           precision[c->fpu_cw >> 8 & 3] | (c->fpu_cw & 0x1000 ? IC : 0);
}
uint32_t sse_control(const X86 *c) {
    const uint32_t csr = sse_mxcsr(c);
    uint32_t denormal = 0;
    if (csr & 0x8000)
        denormal = csr & 0x40 ? 0x1000000 : 0x3000000;
    else if (csr & 0x40)
        denormal = 0x2000000;
    return exception_flags(csr >> 7 & 63) | ((csr >> 5) & RC) | denormal;
}
uint32_t combined_control(const X86 *c) {
    const uint32_t a = x87_control(c), b = sse_control(c);
    return a | b | (((a ^ b) & (EM | RC)) ? 0x80000000 : 0);
}
void change_control(X86 *c, uint32_t value, uint32_t mask, bool x87, bool sse) {
    if (x87) {
        const uint32_t flags = (x87_control(c) & ~mask) | (value & mask);
        for (unsigned i = 0; i < 6; ++i)
            if (mask & masks[i])
                c->fpu_cw = uint16_t((c->fpu_cw & ~(1u << i)) | ((flags & masks[i]) ? 1u << i : 0));
        if (mask & RC)
            c->fpu_cw = uint16_t((c->fpu_cw & ~0xc00u) | ((flags & RC) << 2));
        if (mask & PC) {
            constexpr uint32_t precision[] = {3, 2, 0, 1};
            c->fpu_cw = uint16_t((c->fpu_cw & ~0x300u) | (precision[flags >> 16 & 3] << 8));
        }
        if (mask & IC)
            c->fpu_cw = uint16_t((c->fpu_cw & ~0x1000u) | ((flags & IC) ? 0x1000 : 0));
    }
    if (sse) {
        const uint32_t flags = (sse_control(c) & ~mask) | (value & mask);
        uint32_t csr = sse_mxcsr(c);
        for (unsigned i = 0; i < 6; ++i)
            if (mask & masks[i])
                csr = (csr & ~(1u << (i + 7))) | ((flags & masks[i]) ? 1u << (i + 7) : 0);
        if (mask & RC)
            csr = (csr & ~0x6000u) | ((flags & RC) << 5);
        if (mask & DN) {
            csr &= ~0x8040u;
            const auto mode = (flags & DN) >> 24;
            if (mode == 1)
                csr |= 0x8040;
            else if (mode == 2)
                csr |= 0x40;
            else if (mode == 3)
                csr |= 0x8000;
        }
        c->mxcsr_xor = csr ^ 0x1f80u;
    }
}
void controlfp_s(X86 *c) {
    const uint32_t output = arg(c, 0), value = arg(c, 1), mask = arg(c, 2) & ~0x80000u;
    if (output)
        range(output, 4);
    if (value & mask & ~(EM | RC | PC | IC | DN)) {
        if (output)
            wr32(output, combined_control(c));
        set_errno(c, EINVAL_);
        set_eax(c, EINVAL_);
        return;
    }
    change_control(c, value, mask, true, true);
    if (output)
        wr32(output, combined_control(c));
    set_eax(c, 0);
}
template <bool Denormal> void controlfp(X86 *c) {
    const uint32_t mask = arg(c, 1) & (Denormal ? UINT32_MAX : ~0x80000u);
    change_control(c, arg(c, 0), mask, true, true);
    set_eax(c, combined_control(c));
}
void control87_2(X86 *c) {
    const uint32_t x87 = arg(c, 2), sse = arg(c, 3);
    if (x87)
        range(x87, 4);
    if (sse)
        range(sse, 4);
    change_control(c, arg(c, 0), arg(c, 1), x87 != 0, sse != 0);
    if (x87)
        wr32(x87, x87_control(c));
    if (sse)
        wr32(sse, sse_control(c));
    set_eax(c, 1);
}
void statusfp(X86 *c) {
    set_eax(c, exception_flags((c->fpu_sw | sse_mxcsr(c)) & 63));
}
void statusfp2(X86 *c) {
    if (arg(c, 0)) {
        range(arg(c, 0), 4);
        wr32(arg(c, 0), exception_flags(c->fpu_sw & 63));
    }
    if (arg(c, 1)) {
        range(arg(c, 1), 4);
        wr32(arg(c, 1), exception_flags(sse_mxcsr(c) & 63));
    }
}
void clearfp(X86 *c) {
    statusfp(c);
    c->fpu_sw &= uint16_t(~0x80ffu);
    c->mxcsr_xor = (sse_mxcsr(c) & ~63u) ^ 0x1f80u;
}
void fpreset(X86 *c) {
    c->fpu_cw = 0x027f;
    c->fpu_sw = 0;
    c->fpu_tag = 0xffff;
    c->fpu_top = 0;
    c->mxcsr_xor = 0;
    for (auto &v : c->st_exact)
        v = 0;
}
double indefinite() {
    const uint64_t bits = UINT64_C(0xfff8000000000000);
    double result;
    std::memcpy(&result, &bits, 8);
    return result;
}
enum class Unary {
    Abs,
    Acos,
    Asin,
    Atan,
    Ceil,
    Cos,
    Cosh,
    Exp,
    Floor,
    Log,
    Log10,
    Sin,
    Sinh,
    Sqrt,
    Tan,
    Tanh,
    Logb
};
enum class Binary { Atan2, Pow, Fmod, Hypot, CopySign, NextAfter };
double compute(Unary op, double x) {
    switch (op) {
    case Unary::Abs:
        return std::fabs(x);
    case Unary::Acos:
        return std::acos(x);
    case Unary::Asin:
        return std::asin(x);
    case Unary::Atan:
        return std::atan(x);
    case Unary::Ceil:
        return std::ceil(x);
    case Unary::Cos:
        return std::cos(x);
    case Unary::Cosh:
        return std::cosh(x);
    case Unary::Exp:
        return std::exp(x);
    case Unary::Floor:
        return std::floor(x);
    case Unary::Log:
        return std::log(x);
    case Unary::Log10:
        return std::log10(x);
    case Unary::Sin:
        return std::sin(x);
    case Unary::Sinh:
        return std::sinh(x);
    case Unary::Sqrt:
        return std::sqrt(x);
    case Unary::Tan:
        return std::tan(x);
    case Unary::Tanh:
        return std::tanh(x);
    case Unary::Logb:
        return std::logb(x);
    }
    return indefinite();
}
double compute(Binary op, double a, double b) {
    switch (op) {
    case Binary::Atan2:
        return std::atan2(a, b);
    case Binary::Pow:
        return std::pow(a, b);
    case Binary::Fmod:
        return std::fmod(a, b);
    case Binary::Hypot:
        return std::hypot(a, b);
    case Binary::CopySign:
        return std::copysign(a, b);
    case Binary::NextAfter:
        return std::nextafter(a, b);
    }
    return indefinite();
}
double mark(X86 *c, double result, double a, double b, bool sse, bool zero_divide = false) {
    uint32_t status = 0;
    if (std::isnan(result) && !std::isnan(a) && !std::isnan(b)) {
        status = 1;
        set_errno(c, 33);
        result = indefinite();
    } else if (std::isinf(result) && std::isfinite(a) && std::isfinite(b)) {
        status = zero_divide ? 4 : 8;
        set_errno(c, ERANGE_);
    }
    if (sse)
        c->mxcsr_xor = (sse_mxcsr(c) | status) ^ 0x1f80u;
    else
        c->fpu_sw |= uint16_t(status);
    return result;
}
template <bool Single> double argument(Args &args) {
    if (Single) {
        const uint32_t bits = args.u32();
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }
    return args.f64();
}
template <bool Single> double xmm(const X86 *c, unsigned reg) {
    if (Single) {
        float value;
        std::memcpy(&value, c->xmm[reg], 4);
        return value;
    }
    double value;
    std::memcpy(&value, c->xmm[reg], 8);
    return value;
}
template <bool Single, bool SSE> void result(X86 *c, double value) {
    if (Single) {
        const float converted = float(value);
        if (SSE)
            std::memcpy(c->xmm[0], &converted, 4);
        else
            fpush(c, converted);
    } else if (SSE)
        std::memcpy(c->xmm[0], &value, 8);
    else
        fpush(c, value);
}
template <Unary Op, bool Intrinsic = false, bool Single = false, bool SSE = false>
void unary(X86 *c) {
    Args args{c->r[R_ESP] + 4};
    const double input = SSE ? xmm<Single>(c, 0) : Intrinsic ? fpop(c) : argument<Single>(args);
    const bool zero_divide =
        input == 0 && (Op == Unary::Log || Op == Unary::Log10 || Op == Unary::Logb);
    double output = mark(c, compute(Op, input), input, 0, SSE, zero_divide);
    if (Op == Unary::Exp && output == 0 && std::isfinite(input)) {
        set_errno(c, ERANGE_);
        if (SSE)
            c->mxcsr_xor |= 16;
        else
            c->fpu_sw |= 16;
    }
    result<Single, SSE>(c, output);
}
template <Binary Op, bool Intrinsic = false, bool Single = false, bool SSE = false>
void binary(X86 *c) {
    Args args{c->r[R_ESP] + 4};
    double a, b;
    if (SSE) {
        a = xmm<Single>(c, 0);
        b = xmm<Single>(c, 1);
    } else if (Intrinsic) {
        b = fpop(c);
        a = fpop(c);
    } else {
        a = argument<Single>(args);
        b = argument<Single>(args);
    }
    result<Single, SSE>(c, mark(c, compute(Op, a, b), a, b, SSE));
}
template <unsigned Kind> void classification(X86 *c) {
    Args args{c->r[R_ESP] + 4};
    const uint64_t bits = args.u64();
    double value;
    std::memcpy(&value, &bits, 8);
    if (Kind == 0) {
        set_eax(c, std::isfinite(value));
        return;
    }
    if (Kind == 1) {
        set_eax(c, std::isnan(value));
        return;
    }
    const bool sign = (bits >> 63) != 0;
    const uint64_t mantissa = bits & UINT64_C(0xfffffffffffff);
    const auto exponent = uint32_t(bits >> 52 & 0x7ff);
    uint32_t answer;
    if (exponent == 0x7ff)
        answer = mantissa ? (mantissa & (UINT64_C(1) << 51) ? 2 : 1) : (sign ? 4 : 0x200);
    else if (!exponent)
        answer = mantissa ? (sign ? 0x10 : 0x80) : (sign ? 0x20 : 0x40);
    else
        answer = sign ? 8 : 0x100;
    set_eax(c, answer);
}
void frexp_(X86 *c) {
    Args args{c->r[R_ESP] + 4};
    const double value = args.f64();
    const uint32_t output = args.u32();
    range(output, 4);
    int exponent = 0;
    const double fraction = std::frexp(value, &exponent);
    wr32(output, uint32_t(exponent));
    fpush(c, fraction);
}
void modf_(X86 *c) {
    Args args{c->r[R_ESP] + 4};
    const double value = args.f64();
    const uint32_t output = args.u32();
    range(output, 8);
    double integral;
    const double fraction = std::modf(value, &integral);
    uint64_t bits;
    std::memcpy(&bits, &integral, 8);
    wr64(output, bits);
    fpush(c, fraction);
}
void ldexp_(X86 *c) {
    Args args{c->r[R_ESP] + 4};
    const double value = args.f64();
    const int exponent = int32_t(args.u32());
    fpush(c, mark(c, std::ldexp(value, exponent), value, exponent, false));
}
void ftol(X86 *c) {
    const uint32_t slot = c->fpu_top & 7;
    const bool exact = c->st_exact[slot] != 0;
    uint64_t bits = c->st_bits[slot];
    const double value = fpop(c);
    if (!exact) {
        if (!std::isfinite(value) || value >= 9223372036854775808.0 ||
            value < -9223372036854775808.0) {
            bits = UINT64_C(0x8000000000000000);
            c->fpu_sw |= 1;
        } else {
            const double truncated = std::trunc(value);
            bits = uint64_t(int64_t(truncated));
            if (truncated != value)
                c->fpu_sw |= 32;
        }
    }
    set_eax64(c, bits);
}
template <bool Wide = false> void abs_(X86 *c) {
    Args args{c->r[R_ESP] + 4};
    const uint64_t raw = Wide ? args.u64() : args.u32();
    if (Wide)
        set_eax64(c, raw >> 63 ? uint64_t(0) - raw : raw);
    else
        set_eax(c, raw >> 31 ? uint32_t(0) - uint32_t(raw) : uint32_t(raw));
}
template <bool Wide = false, bool End = true> void strtod_(X86 *c) {
    const uint32_t start = arg(c, 0);
    std::string s;
    if (Wide) {
        for (auto ch : wtext(start)) {
            if (ch > 127)
                break;
            s += char(ch);
        }
    } else
        s = text(start);
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(s.c_str(), &end);
    if (errno == ERANGE)
        set_errno(c, ERANGE_);
    if (End && arg(c, 1)) {
        range(arg(c, 1), 4);
        wr32(arg(c, 1), start + uint32_t(end - s.c_str()) * (Wide ? 2 : 1));
    }
    fpush(c, value);
}
} // namespace
void register_math() {
#define C(name, ...) {"msvcr80.dll", name, ARGC_CDECL, __VA_ARGS__}
#define U(name, op)                                                                                \
    C(name, unary<Unary::op>), C(name "f", unary<Unary::op, false, true>),                         \
        C("_CI" name, unary<Unary::op, true>),                                                     \
        C("__libm_sse2_" name, unary<Unary::op, false, false, true>),                              \
        C("__libm_sse2_" name "f", unary<Unary::op, false, true, true>)
#define B(name, op)                                                                                \
    C(name, binary<Binary::op>), C(name "f", binary<Binary::op, false, true>),                     \
        C("_CI" name, binary<Binary::op, true>),                                                   \
        C("__libm_sse2_" name, binary<Binary::op, false, false, true>),                            \
        C("__libm_sse2_" name "f", binary<Binary::op, false, true, true>)
    const ImportShim entries[] = {
        U("acos", Acos),
        U("asin", Asin),
        U("atan", Atan),
        U("ceil", Ceil),
        U("cos", Cos),
        U("cosh", Cosh),
        U("exp", Exp),
        U("fabs", Abs),
        U("floor", Floor),
        U("log", Log),
        U("log10", Log10),
        U("sin", Sin),
        U("sinh", Sinh),
        U("sqrt", Sqrt),
        U("tan", Tan),
        U("tanh", Tanh),
        B("atan2", Atan2),
        B("pow", Pow),
        B("fmod", Fmod),
        C("_hypot", binary<Binary::Hypot>),
        C("_hypotf", binary<Binary::Hypot, false, true>),
        C("_copysign", binary<Binary::CopySign>),
        C("_nextafter", binary<Binary::NextAfter>),
        C("_logb", unary<Unary::Logb>),
        C("_finite", classification<0>),
        C("_isnan", classification<1>),
        C("_fpclass", classification<2>),
        C("frexp", frexp_),
        C("modf", modf_),
        C("ldexp", ldexp_),
        C("_scalb", ldexp_),
        C("_controlfp_s", controlfp_s),
        C("_controlfp", controlfp<false>),
        C("_control87", controlfp<true>),
        C("__control87_2", control87_2),
        C("_statusfp", statusfp),
        C("_status87", statusfp),
        C("_statusfp2", statusfp2),
        C("_clearfp", clearfp),
        C("_clear87", clearfp),
        C("_fpreset", fpreset),
        C("_preset", fpreset),
        C("_ftol", ftol),
        C("_ftol2", ftol),
        C("_ftol2_sse", ftol),
        C("_ftol2_sse_excpt", ftol),
        C("abs", abs_<false>),
        C("labs", abs_<false>),
        C("_abs64", abs_<true>),
        C("strtod", strtod_<>),
        C("wcstod", strtod_<true>),
        C("atof", strtod_<false, false>),
        C("_wtof", strtod_<true, false>),
        C("_strtod_l", strtod_<>),
        C("_wcstod_l", strtod_<true>),
        C("_atof_l", strtod_<false, false>),
    };
    imports_register(entries, sizeof entries / sizeof entries[0]);
#undef U
#undef B
#undef C
}
} // namespace crt
