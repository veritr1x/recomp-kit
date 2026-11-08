"""The x87 run cache against the per-instruction register file.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_x87_cache.py

translate.py holds a straight-line run's x87 stack in C locals (X87Cache) and
writes it back at each boundary. This translates random runs of x87 and plain
integer code - pushes, pops, exchanges, register copies, FILD's exact
integers, compares with FNSTSW and FCOMI, branches into the middle of a run -
twice, once with the cache and once with --x87-direct, and runs both from the
same random states: random TOP, tags, stale values in empty registers, exact
integers, precision and rounding control. The whole X86 struct (registers,
flags, st[], st_bits[], st_exact[], TOP, status and tag words) and the scratch
memory must match; FNSAVE at the end of each run also stores the state in
guest memory. Both sides use the same runtime helpers, so any difference is
the cache's.

clang must be on PATH and the harness builds as a POSIX shared library (a
skip otherwise), as for test_translate_insns.py."""

import ctypes as C
import os
import platform
import random
import shutil
import struct
import subprocess
import tempfile

import pytest

import test_translate_insns as M

T = M.T
SRC = M.SCRATCH + 0x300
DST = M.SCRATCH + 0x100
ARITH = ["FADD", "FSUB", "FMUL", "FDIV", "FSUBR", "FDIVR"]
CASES = 150
STATES = 12


def gen_case(rng, k):
    lines = []
    for _ in range(rng.randint(4, 40)):
        r = rng.random()
        o4 = 4 * rng.randint(0, 15)
        o8 = 8 * rng.randint(0, 7)
        d = 4 * rng.randint(0, 31)
        i = rng.randint(1, 3)
        if r < 0.12:
            lines.append("FLD dword ptr [ESI + 0x%x]" % o4)
        elif r < 0.17:
            lines.append("FLD qword ptr [ESI + 0x%x]" % o8)
        elif r < 0.21:
            lines.append(rng.choice(["FILD dword ptr [ESI + 0x%x]" % o4, "FILD qword ptr [ESI + 0x%x]" % o8,
                                     "FILD word ptr [ESI + 0x%x]" % o4]))
        elif r < 0.25:
            lines.append("FLD ST%d" % rng.randint(0, 3))
        elif r < 0.27:
            lines.append(rng.choice(["FLD1", "FLDZ", "FLDPI"]))
        elif r < 0.36:
            lines.append("%s dword ptr [ESI + 0x%x]" % (rng.choice(ARITH), o4))
        elif r < 0.42:
            a = rng.choice(ARITH)
            lines.append(rng.choice(["%s ST0,ST%d" % (a, i), "%s ST%d,ST0" % (a, i)]))
        elif r < 0.50:
            lines.append("%sP ST%d" % (rng.choice(ARITH), i))
        elif r < 0.58:
            lines.append(rng.choice(["FSTP dword ptr [EDI + 0x%x]" % d, "FST dword ptr [EDI + 0x%x]" % d,
                                     "FSTP qword ptr [EDI + 0x%x]" % (d & ~7)]))
        elif r < 0.62:
            lines.append(rng.choice(["FSTP ST%d" % rng.randint(0, 3), "FST ST%d" % i]))
        elif r < 0.66:
            lines.append("FXCH ST%d" % i)
        elif r < 0.69:
            lines.append(rng.choice(["FCHS", "FABS", "FSQRT"]))
        elif r < 0.75:
            lines.append(rng.choice(["FCOMP dword ptr [ESI + 0x%x]" % o4, "FCOM dword ptr [ESI + 0x%x]" % o4,
                                     "FCOMPP", "FUCOMPP", "FCOMP ST%d" % i, "FUCOM ST%d" % i]))
            lines.append("FNSTSW AX")
            lines.append("TEST AH,0x41")
            lines.append("JNZ @%d" % rng.randint(1, 6))
        elif r < 0.78:
            lines.append(rng.choice(["FUCOMIP ST0,ST%d" % i, "FCOMI ST0,ST%d" % i]))
            lines.append("JBE @%d" % rng.randint(1, 6))
        elif r < 0.80:
            lines.append(rng.choice(["FISTP dword ptr [EDI + 0x%x]" % d, "FISTP qword ptr [EDI + 0x%x]" % (d & ~7)]))
        elif r < 0.81:
            lines.append(rng.choice(["FNSTCW word ptr [EDI + 0x%x]" % d, "FXAM", "FNSTSW word ptr [EDI + 0x%x]" % d]))
        elif r < 0.90:
            lines.append(rng.choice(["MOV EAX,dword ptr [ESI + 0x%x]" % o4, "ADD EAX,ECX",
                                     "MOV dword ptr [EDI + 0x%x],EAX" % d, "INC ECX", "TEST ECX,0x3",
                                     "PUSH EAX", "POP EDX", "MOV EBX,EAX"]))
        else:
            lines.append(rng.choice(["JZ @%d", "JNZ @%d", "JS @%d", "JNP @%d"]) % rng.randint(1, 6))
    lines += ["FNSAVE [EDI + 0x200]", "RET"]
    addr = M.CODE_BASE + 0x400 * k
    out = []
    for j, text in enumerate(lines):
        if "@" in text:
            head, rel = text.split("@")
            text = head + "0x%x" % (addr + 4 * min(j + int(rel), len(lines) - 2))
        out.append((4 * j, text))
    # Four bytes per line; no form here needs its bytes to decide direction.
    return M.Case("x87 run %d" % k, addr, out, "90" * (4 * len(lines)))


class Opts(object):
    eager_flags = False

    def __init__(self, direct):
        self.x87_direct = direct


#: The x87 [translate.speedups] switches. This file compares whole states, so
#: the write-back trimming (which by design leaves empty registers and inexact
#: st_bits alone) is off on both sides; test_x87_carry.py compares the trimmed
#: forms everywhere. carry=True also carries runs across labels.
X87_SWITCHES = ("x87_trim_popped", "x87_skip_inexact_bits", "x87_batch_tags", "x87_part_dirty",
                "x87_carry", "x87_st0_leaves")


def translate(case, cases, direct, carry=False):
    saved = dict(T.SPEEDUPS)
    T.SPEEDUPS.update(dict.fromkeys(X87_SWITCHES, False), x87_carry=carry)
    try:
        tr = T.Translator(M.NoImage(case), {c.addr for c in cases}, Opts(direct))
        insns = T.parse_listing_text(case.listing())
        fn = T.Function(case.addr, case.name, len(case.code), insns)
        fn.measure(M.NoImage())
        tr.prepare(fn)
        return "\n".join(tr.translate(fn, case.entries))
    finally:
        T.SPEEDUPS.clear()
        T.SPEEDUPS.update(saved)


def build(clang, cases, direct, carry=False):
    work = tempfile.mkdtemp(prefix="x87-cache-")
    with open(os.path.join(work, "synth.c"), "w") as fh:
        fh.write('#include "x86.h"\n\n' + "\n".join(translate(c, cases, direct, carry) for c in cases))
    with open(os.path.join(work, "table.c"), "w") as fh:
        fh.write('#include "x86.h"\n')
        for c in cases:
            fh.write("void fn_%08x(X86 *RECOMP_CTX c);\n" % c.addr)
        fh.write("int recomp_is_call_return(uint32_t t) { return t == 0x%08xu; }\n" % M.MAGIC_RET)
        fh.write("int32_t recomp_index_of(uint32_t t) { switch (t) {\n")
        for i, c in enumerate(cases):
            fh.write("case 0x%08xu: return %d;\n" % (c.addr, i))
        fh.write("default: return -1; } }\n")
        fh.write("void recomp_call(X86 *c, uint32_t t) { switch (t) {\n")
        for c in cases:
            fh.write("case 0x%08xu: fn_%08x(c); return;\n" % (c.addr, c.addr))
        fh.write("default: recomp_unknown_call(c, t); } }\n")
        fh.write("void recomp_jump(X86 *c, uint32_t t) { if (t == 0x%08xu) { c->eip = t; return; } "
                 "recomp_call(c, t); }\n" % M.MAGIC_RET)
    lib = os.path.join(work, "libx87" + (".dylib" if platform.system() == "Darwin" else ".so"))
    subprocess.check_call([clang, "-O1", "-std=c11", "-w", "-fPIC", "-shared", "-I",
                           os.path.join(M.ROOT, "runtime"), os.path.join(M.HERE, "harness.c"),
                           os.path.join(work, "synth.c"), os.path.join(work, "table.c"), "-o", lib])
    return M.Native(lib)


def initial_state(seed):
    r = random.Random(seed)
    c = M.X86()
    for i in range(8):
        c.r[i] = r.getrandbits(32)
    c.r[4], c.r[6], c.r[7] = M.ESP_INIT, SRC, DST
    c.eflags_misc = M.EFLAGS_MISC_INIT
    c.fpu_cw = r.choice([0x037F, 0x027F, 0x007F, 0x0C7F, 0x047F])
    c.fpu_top = r.randrange(8)
    tag = 0xFFFF
    for k in range(r.randint(0, 5)):
        phys = (c.fpu_top + k) & 7
        v = r.choice([0.0, -0.0, 1.5, -2.25, 3e38, 1e-40, float("inf"), float("nan"), r.uniform(-1e6, 1e6)])
        c.st[phys] = v
        special = v != v or v == float("inf")
        tag = (tag & ~(3 << (2 * phys))) | ((1 if v == 0 else 2 if special else 0) << (2 * phys))
        if r.random() < 0.3:
            b = r.getrandbits(63)
            c.st_bits[phys], c.st_exact[phys], c.st[phys] = b, 1, float(b)
    for k in range(8):
        if r.random() < 0.2:
            c.st[k] = r.uniform(-9, 9)  # stale values in empty registers
    c.fpu_tag = tag
    floats = [r.choice([0.0, 1.0, -1.0, 0.5, 2.0, 1e10, -3.75, r.uniform(-100, 100)]) for _ in range(16)]
    mem = struct.pack("<16f", *floats) + struct.pack("<8q", *[r.randint(-2**40, 2**40) for _ in range(8)])
    return c, mem


def run(native, case, seed):
    c, mem = initial_state(seed)
    native.zero(M.STACK_BASE, M.STACK_SIZE)
    native.zero(M.SCRATCH, M.SCRATCH_SIZE)
    native.write(SRC, mem)
    native.write(M.ESP_INIT, struct.pack("<I", M.MAGIC_RET))
    native.lib.harness_run(case.addr, C.byref(c))
    return c, native.read(M.SCRATCH, M.SCRATCH_SIZE)


@pytest.fixture(scope="module")
def engines():
    clang = shutil.which("clang")
    if clang is None:
        pytest.skip("clang is not on PATH")
    if platform.system() == "Windows":
        pytest.skip("the instruction harness builds as a POSIX shared library")
    rng = random.Random(0x87CAC4E)
    cases = [gen_case(rng, k) for k in range(CASES)]
    return (cases, build(clang, cases, True), build(clang, cases, False),
            build(clang, cases, False, True))


def field_bytes(state, name):
    f = getattr(M.X86, name)
    return C.string_at(C.addressof(state) + f.offset, f.size)


@pytest.mark.parametrize("which", [2, 3], ids=["cached", "carried"])
def test_cached_runs_leave_the_state_the_helpers_leave(engines, which):
    cases, direct, cached = engines[0], engines[1], engines[which]
    bad = []
    for case in cases:
        for s in range(STATES):
            seed = s * 7919 + case.addr
            a, am = run(direct, case, seed)
            b, bm = run(cached, case, seed)
            if bytes(a) != bytes(b) or am != bm:
                fields = [f for f, _t in M.X86._fields_ if field_bytes(a, f) != field_bytes(b, f)]
                bad.append("%s state %d: %s%s\n%s" % (case.name, s, ", ".join(fields),
                           " memory" if am != bm else "",
                           "\n".join("  " + t for _o, t in case.lines)))
                break
    assert not bad, "%d of %d runs differ; first:\n%s" % (len(bad), len(cases), bad[0])


def test_the_cache_is_in_use(engines):
    """The comparison means something only if the cached side caches."""
    cases = engines[0]
    cached = "\n".join(translate(c, cases, False) for c in cases[:20])
    direct = "\n".join(translate(c, cases, True) for c in cases[:20])
    assert cached.count("ft0_ = c->fpu_top;") > 20
    assert "ft0_" not in direct
