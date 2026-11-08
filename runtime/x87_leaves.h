/* x87_leaves.h - leaf functions a translated caller hands ST(0) to directly.
 *
 * Under the speedup x87_st0_leaves, a CALL to an address [translate]
 * st0_leaves names (with a recipe here) does not write the caller's cached
 * x87 stack back, push a return address and dispatch: the caller passes the
 * value in ST(0), with its exact-integer metadata, to the recipe, and pops
 * ST(0) in its own cache (translate.py Translator.emit_st0_leaf). A recipe
 * leaves the guest registers, EIP and the status word as the function would;
 * the stack slot below ESP the CALL would have written is not written, and
 * the scratch values the function leaves in registers it pops are not
 * stored (nothing reads an empty register's value). Hooks and profiling of
 * the leaf address are bypassed: it is no longer called.
 *
 * Included by the generated funcs.h when the speedup is on. */
#ifndef RECOMP_X87_LEAVES_H
#define RECOMP_X87_LEAVES_H
#include "x86.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MSVC _ftol2: ST(0) to EDX:EAX, toward zero. FISTP rounds under the current
 * mode; the result then steps one toward zero when that rounding went away
 * from zero, decided by the signs of the float value and of the float
 * remainder, exactly as the CRT's code does (ECX holds the remainder's
 * adjusted bits after it). `exact`/`bits` are ST(0)'s st_exact/st_bits. */
static inline void recomp_x87_ftol2(X86 *c, double v, int exact, uint64_t bits) {
    float vf_f = fto_float(c, v);
    uint32_t vf, eax, edx, ecx = c->r[R_ECX];
    uint64_t r;
    memcpy(&vf, &vf_f, 4);
    r = exact ? bits : (uint64_t)fto_i64(c, v);
    eax = (uint32_t)r;
    edx = (uint32_t)(r >> 32);
    if (!(eax == 0 && (edx & 0x7fffffffu) == 0)) {
        double diff = fx87(c, v - (double)(int64_t)r);
        float d_f = fto_float(c, diff);
        uint32_t d, sign = eax == 0 ? edx : vf;
        memcpy(&d, &d_f, 4);
        if ((int32_t)sign < 0) {
            uint64_t s = (uint64_t)(d ^ 0x80000000u) + 0x7fffffffu;
            uint64_t lo;
            ecx = (uint32_t)s;
            lo = (uint64_t)eax + (uint32_t)(s >> 32);
            eax = (uint32_t)lo;
            edx += (uint32_t)(lo >> 32);
        } else {
            uint64_t s = (uint64_t)d + 0x7fffffffu;
            uint32_t borrow = (uint32_t)(s >> 32), b2 = eax < borrow;
            ecx = (uint32_t)s;
            eax -= borrow;
            edx -= b2;
        }
    }
    c->r[R_EAX] = eax;
    c->r[R_EDX] = edx;
    c->r[R_ECX] = ecx;
}

#ifdef __cplusplus
}
#endif
#endif
