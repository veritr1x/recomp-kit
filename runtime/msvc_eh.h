// VC8 x86 C++ checkpoints. setjmp executes in the live translated body.
#pragma once
#include "x86.h"
#include <setjmp.h>
#ifdef __cplusplus
extern "C" {
#endif
jmp_buf *recomp_msvc_frame_enter(X86 *c, uint32_t handler, uint32_t info);
uint32_t recomp_msvc_frame_land(X86 *c);
void recomp_msvc_frame_leave(X86 *c);
void recomp_msvc_unwind_to_esp(X86 *c, uint32_t esp);
void recomp_msvc_callback_leave(X86 *c, uint32_t depth);
// Identify only SEH's own handler callback as an allowed dispatcher boundary.
void recomp_msvc_seh_enter(X86 *c);
void recomp_msvc_seh_leave(X86 *c);
void recomp_msvc_reset(X86 *c);
// Runtime CRT operations throw directly, without inventing a guest_call
// boundary between the operation and its translated caller's checkpoint.
void recomp_msvc_throw(X86 *c, uint32_t object, uint32_t throw_info);
#ifdef POPM_TESTING
uint32_t recomp_msvc_test_frame_count(X86 *c);
#endif
#ifdef __cplusplus
}
void msvc_eh_register();
#endif
