"""The x87 cache's speedups against the per-instruction register file, with
no C compiler.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_x87_carry.py

Random branchy x87 functions - loops, joins of different stack depths,
compares feeding branches, exact FILD integers, FISTP through the helpers -
are translated with --x87-direct, with the run cache as it was (every
[translate.speedups] x87 switch off), with the default switches and with
x87_carry, and each translation runs in cmini.py from the same random states.

The cache with its switches off must leave the whole state exactly as the
helpers do. The trimmed write-back promises less, by design: a register it
leaves empty keeps whatever value it held, and st_bits matters only under
st_exact. So with trimming the comparison is of the tag word, TOP, the
status word, the integer registers and flags, memory, and of each register
in use its value, st_exact and (when exact) st_bits.

test_x87_cache.py makes the same comparison on compiled code where clang can
build a POSIX shared library; this file runs everywhere. The structural
tests below check what the generated C says about where the stack goes."""

import os
import random
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cmini  # noqa: E402
import test_translate_insns as M  # noqa: E402

T = M.T
SRC = M.SCRATCH + 0x300
DST = M.SCRATCH + 0x100
ARITH = ["FADD", "FSUB", "FMUL", "FDIV", "FSUBR", "FDIVR"]
CASES = 60
STATES = 6
X87_SWITCHES = ("x87_trim_popped", "x87_skip_inexact_bits", "x87_batch_tags", "x87_part_dirty",
                "x87_carry", "x87_st0_leaves")
CONFIGS = {
    "direct": dict.fromkeys(X87_SWITCHES, False),
    "plain": dict.fromkeys(X87_SWITCHES, False),
    "default": dict((k, T.game_config.TRANSLATE_SPEEDUPS[k]) for k in X87_SWITCHES),
    "carry": dict(dict((k, T.game_config.TRANSLATE_SPEEDUPS[k]) for k in X87_SWITCHES), x87_carry=True),
    "carry_plain": dict(dict.fromkeys(X87_SWITCHES, False), x87_carry=True),
}


def x87_op(rng):
    r = rng.random()
    o4 = 4 * rng.randint(0, 15)
    o8 = 8 * rng.randint(0, 7)
    d = 4 * rng.randint(0, 31)
    i = rng.randint(1, 3)
    if r < 0.14:
        return ["FLD dword ptr [ESI + 0x%x]" % o4]
    if r < 0.18:
        return ["FLD qword ptr [ESI + 0x%x]" % o8]
    if r < 0.22:
        return [rng.choice(["FILD dword ptr [ESI + 0x%x]" % o4, "FILD word ptr [ESI + 0x%x]" % o4])]
    if r < 0.26:
        return ["FLD ST%d" % rng.randint(0, 3)]
    if r < 0.28:
        return [rng.choice(["FLD1", "FLDZ"])]
    if r < 0.38:
        return ["%s dword ptr [ESI + 0x%x]" % (rng.choice(ARITH), o4)]
    if r < 0.44:
        a = rng.choice(ARITH)
        return [rng.choice(["%s ST0,ST%d" % (a, i), "%s ST%d,ST0" % (a, i)])]
    if r < 0.52:
        return ["%sP ST%d" % (rng.choice(ARITH), i)]
    if r < 0.62:
        return [rng.choice(["FSTP dword ptr [EDI + 0x%x]" % d, "FST dword ptr [EDI + 0x%x]" % d,
                            "FSTP qword ptr [EDI + 0x%x]" % (d & ~7)])]
    if r < 0.66:
        return [rng.choice(["FSTP ST%d" % rng.randint(0, 3), "FST ST%d" % i])]
    if r < 0.71:
        return ["FXCH ST%d" % i]
    if r < 0.74:
        return [rng.choice(["FCHS", "FABS"])]
    if r < 0.77:
        return [rng.choice(["FISTP dword ptr [EDI + 0x%x]" % d, "FIST word ptr [EDI + 0x%x]" % d])]
    if r < 0.80:
        return [rng.choice(["FUCOMIP ST0,ST%d" % i, "FCOMI ST0,ST%d" % i])]
    return [rng.choice(["MOV EAX,dword ptr [ESI + 0x%x]" % o4, "ADD EAX,ECX",
                        "MOV dword ptr [EDI + 0x%x],EAX" % d, "INC ECX", "TEST ECX,0x3",
                        "PUSH EAX", "POP EDX", "MOV EBX,EAX"])]


def gen_case(rng, k):
    """Segments of straight code and counted loops (EBP counts). A forward
    branch stays inside its segment or goes to the next segment's start, so
    a loop is only ever entered at its head."""
    lines, fixups = [], []    # fixups: (line index, segment end marker)
    segments = []
    for _ in range(rng.randint(2, 4)):
        start = len(lines)
        loop = rng.random() < 0.5
        if loop:
            lines.append("MOV EBP,0x%x" % rng.randint(1, 4))
        head = len(lines)
        for _ in range(rng.randint(3, 14)):
            r = rng.random()
            if r < 0.12:
                lines.extend(["%s dword ptr [ESI + 0x%x]" % (rng.choice(["FCOMP", "FCOM", "FUCOMP"]),
                                                             4 * rng.randint(0, 15)),
                              "FNSTSW AX", "TEST AH,0x%x" % rng.choice([0x41, 0x05, 0x01, 0x40, 0x44]),
                              rng.choice(["JNZ", "JZ", "JP", "JNP"]) + " @fwd"])
            elif r < 0.2:
                lines.append(rng.choice(["JZ", "JNZ", "JS", "JNP", "JMP"]) + " @fwd")
            else:
                lines.extend(x87_op(rng))
        body_end = len(lines)
        if loop:
            lines.extend(["DEC EBP", "JNZ @%d" % head])
        segments.append((start, len(lines), body_end))
    lines += ["FNSTSW AX", "RET"]
    # Forward targets: later in the same segment, or the next segment start
    # (the final FNSTSW for the last one).
    out = []
    for j, text in enumerate(lines):
        if text.endswith("@fwd"):
            seg = next(s for s in segments if s[0] <= j < s[1])
            choices = list(range(j + 1, seg[2])) + [seg[1]]   # never onto DEC/JNZ
            text = text.replace("@fwd", "@%d" % rng.choice(choices))
        out.append(text)
    addr = M.CODE_BASE + 0x800 * k
    listing = []
    for j, text in enumerate(out):
        if "@" in text:
            head, target = text.split("@")
            text = head + "0x%x" % (addr + 4 * int(target))
        listing.append((4 * j, text))
    return M.Case("x87 carry %d" % k, addr, listing, "90" * (4 * len(out)))


class Opts(object):
    eager_flags = False

    def __init__(self, direct):
        self.x87_direct = direct


def translate(case, config, extra_lines=None):
    saved = dict(T.SPEEDUPS)
    T.SPEEDUPS.update(CONFIGS[config] if isinstance(config, str) else config)
    try:
        tr = T.Translator(M.NoImage(case), {case.addr}, Opts(config == "direct"))
        insns = T.parse_listing_text(case.listing())
        fn = T.Function(case.addr, case.name, len(case.code), insns)
        fn.measure(M.NoImage())
        tr.prepare(fn)
        return "\n".join(tr.translate(fn, case.entries)), tr
    finally:
        T.SPEEDUPS.clear()
        T.SPEEDUPS.update(saved)


def initial_state(seed):
    r = random.Random(seed)
    s = cmini.State()
    regs = [r.getrandbits(32) for _ in range(8)]
    regs[4], regs[6], regs[7] = M.ESP_INIT, SRC, DST
    s.a["r"] = regs
    s.f["eflags_misc"] = M.EFLAGS_MISC_INIT
    s.f["fpu_cw"] = r.choice([0x037F, 0x027F, 0x007F])
    top = r.randrange(8)
    s.f["fpu_top"] = top
    tag = 0xFFFF
    for k in range(r.randint(3, 6)):
        phys = (top + k) & 7
        v = r.choice([0.0, -0.0, 1.5, -2.25, 3e38, 1e-40, float("inf"), float("nan"), r.uniform(-1e6, 1e6)])
        s.a["st"][phys] = v
        tag = (tag & ~(3 << (2 * phys))) | (cmini.ftag_classify(v) << (2 * phys))
        if r.random() < 0.3:
            b = r.getrandbits(40)
            s.a["st_bits"][phys], s.a["st_exact"][phys], s.a["st"][phys] = b, 1, float(b)
            tag = (tag & ~(3 << (2 * phys))) | (cmini.ftag_classify(float(b)) << (2 * phys))
    for k in range(8):
        if r.random() < 0.2:
            s.a["st"][k] = r.uniform(-9, 9)  # stale values in empty registers
        if r.random() < 0.2:
            s.a["st_bits"][k] = r.getrandbits(64)
    s.f["fpu_tag"] = tag
    floats = [r.choice([0.0, 1.0, -1.0, 0.5, 2.0, 1e10, -3.75, r.uniform(-100, 100)]) for _ in range(16)]
    blob = struct.pack("<16f", *floats) + struct.pack("<8q", *[r.randint(-2 ** 40, 2 ** 40) for _ in range(8)])
    for k, b in enumerate(blob):
        s.mem[SRC + k] = b
    s.wr(M.ESP_INIT, 4, M.MAGIC_RET)
    return s


def run(machine, seed):
    s = initial_state(seed)
    machine.run(s)
    return s


def dbits(x):
    return struct.pack("<d", x)


def same(a, b, trimmed):
    """The first difference between two end states, or None."""
    if a.mem != b.mem:
        keys = sorted(k for k in set(a.mem) | set(b.mem) if a.mem.get(k, 0) != b.mem.get(k, 0))
        if keys:
            return "memory at %08x" % keys[0]
    for k in ("r",):
        if a.a[k] != b.a[k]:
            return "registers %s" % [(x, y) for x, y in zip(a.a[k], b.a[k]) if x != y]
    for f in sorted(a.f):
        if a.f[f] != b.f[f]:
            return "%s %x != %x" % (f, a.f[f], b.f[f])
    for p in range(8):
        empty = (a.f["fpu_tag"] >> (2 * p)) & 3 == 3
        if trimmed and empty:
            continue
        if dbits(a.a["st"][p]) != dbits(b.a["st"][p]):
            return "st[%d] %r != %r" % (p, a.a["st"][p], b.a["st"][p])
        if a.a["st_exact"][p] != b.a["st_exact"][p]:
            return "st_exact[%d]" % p
        if (not trimmed or a.a["st_exact"][p]) and a.a["st_bits"][p] != b.a["st_bits"][p]:
            return "st_bits[%d]" % p
    return None


@pytest.fixture(scope="module")
def cases():
    rng = random.Random(0xCA77)
    return [gen_case(rng, k) for k in range(CASES)]


@pytest.mark.parametrize("config,trimmed", [("plain", False), ("carry_plain", False),
                                            ("default", True), ("carry", True)])
def test_cached_bodies_leave_the_state_the_helpers_leave(cases, config, trimmed):
    bad = []
    for case in cases:
        text_d, _ = translate(case, "direct")
        text_c, _ = translate(case, config)
        name = "fn_%08x" % case.addr
        md, mc = cmini.Machine(text_d, name), cmini.Machine(text_c, name)
        for k in range(STATES):
            seed = k * 7919 + case.addr
            want = run(md, seed)
            if trimmed and md.underflow:
                # A read of an empty register sees whatever it last held,
                # which is exactly what trimming stops storing; game code
                # does not underflow the stack.
                continue
            why = same(want, run(mc, seed), trimmed)
            if why:
                bad.append("%s state %d: %s\n%s" % (case.name, k, why,
                                                    "\n".join("  " + t for _o, t in case.lines)))
                break
    assert not bad, "%d of %d differ under %s; first:\n%s" % (len(bad), len(cases), config, bad[0])


def test_the_carry_is_in_use(cases):
    """The comparison means something only if labels keep the stack."""
    kept = 0
    for case in cases:
        _text, tr = translate(case, "carry")
        kept += tr.stats["_x87_carry_labels"]
    assert kept > len(cases) // 2


# ---- structural ----------------------------------------------------------

def synth(lines, name="synth"):
    addr = M.CODE_BASE
    out = []
    for j, text in enumerate(lines):
        if "@" in text:
            head, rel = text.split("@")
            text = head + "0x%x" % (addr + 4 * int(rel))
        out.append((4 * j, text))
    return M.Case(name, addr, out, "90" * (4 * len(lines)))


def body_between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


LOOP = ["FLDZ", "MOV ECX,0x4",
        "FADD dword ptr [ESI + 0x0]",       # 2: loop head
        "ADD ESI,0x4", "DEC ECX", "JNZ @2",
        "FSTP dword ptr [EDI + 0x0]", "RET"]


def test_a_loop_keeps_its_accumulator_in_a_local():
    case = synth(LOOP)
    text, tr = translate(case, "carry")
    loop = body_between(text, "L_%08x" % (case.addr + 8), "FSTP")
    assert "c->st[" not in loop and "fpu_tag" not in loop and "fpu_top" not in loop
    assert "ft0_ = c->fpu_top" not in loop
    assert tr.stats["_x87_carry_labels"] == 1
    plain, _ = translate(case, "default")
    assert "c->st[" in body_between(plain, "L_%08x" % (case.addr + 8), "FSTP")


DIAMOND = ["FLD dword ptr [ESI + 0x0]", "TEST EAX,EAX", "JZ @5",
           "FMUL dword ptr [ESI + 0x4]", "JMP @6",
           "FADD dword ptr [ESI + 0x8]",     # 5
           "FSTP dword ptr [EDI + 0x0]",     # 6: join
           "RET"]


def test_a_join_of_equal_depths_copies_into_one_local():
    case = synth(DIAMOND)
    text, tr = translate(case, "carry")
    assert tr.stats["_x87_carry_labels"] == 2
    head = text[:text.index("FSTP")]
    assert "c->st[" not in head and "c->fpu_top =" not in head


UNEVEN = ["FLD dword ptr [ESI + 0x0]", "TEST EAX,EAX", "JZ @4",
          "FLD dword ptr [ESI + 0x4]",       # one deeper on this path
          "FSTP dword ptr [EDI + 0x0]",      # 4: join of depths 1 and 2
          "RET"]


def test_a_join_of_different_depths_writes_back_on_its_edges():
    case = synth(UNEVEN)
    text, tr = translate(case, "carry")
    assert tr.stats["_x87_carry_labels"] == 0
    jz = body_between(text, "JZ", "FLD dword ptr [ESI + 0x4]")
    assert "c->st[" in jz and "goto L_" in jz
    # The fall-through path writes back before the label, too.
    tail = body_between(text, "FLD dword ptr [ESI + 0x4]", "L_%08x" % (case.addr + 16))
    assert "c->fpu_top" in tail


def test_a_jump_table_target_keeps_nothing():
    case = synth(["FLD dword ptr [ESI + 0x0]", "TEST EAX,EAX", "JZ @3",
                  "FSTP dword ptr [EDI + 0x0]", "RET"])
    tr = T.Translator(M.NoImage(case), {case.addr}, Opts(False))
    fn = T.Function(case.addr, case.name, len(case.code), T.parse_listing_text(case.listing()))
    fn.measure(M.NoImage())
    tr.prepare(fn)
    target = case.addr + 12
    tr.jumptables[(fn.addr, case.addr + 4)] = [target]  # as if a table named it
    saved = dict(T.SPEEDUPS)
    T.SPEEDUPS["x87_carry"] = True
    try:
        text = "\n".join(tr.translate(fn, ()))
    finally:
        T.SPEEDUPS.clear()
        T.SPEEDUPS.update(saved)
    assert tr.stats["_x87_carry_labels"] == 0
    assert "c->st[" in body_between(text, "JZ", "L_%08x" % target)


def test_a_goto_the_carry_did_not_issue_demotes_its_label():
    carry = T.X87Carry([0x10], set())
    cache = T.X87Cache()
    cache.begin()
    cache.push("1.0")
    cache.take_pre()
    carry.edge(cache, 0x10)
    assert carry.finish_pass(["goto L_00000010;", "goto L_00000010;"]) is False
    assert carry.shape[0x10] is None


def test_an_alternate_entry_keeps_nothing():
    case = synth(LOOP)
    case.entries = [case.addr + 8]
    text, tr = translate(case, "carry")
    assert tr.stats.get("_x87_carry_labels", 0) == 0


def test_carry_off_is_the_old_body():
    case = synth(LOOP)
    a, _ = translate(case, dict(CONFIGS["default"], x87_carry=False))
    b, _ = translate(case, "default")
    assert a == b and "fv1_ = fv0_" not in a


def test_trimmed_write_back_stores_only_tags_for_pops():
    case = synth(["FLD dword ptr [ESI + 0x0]", "FLD dword ptr [ESI + 0x4]",
                  "FADDP ST1", "FSTP dword ptr [EDI + 0x0]", "RET"])
    plain, _ = translate(case, "plain")
    trim, _ = translate(case, "default")
    assert "c->st_exact[(ft0_ + 7u) & 7u] = 0u;" in plain
    assert "c->st[" not in trim and "st_exact" not in trim
    assert "c->fpu_tag |=" in trim


def test_batched_tags_read_the_tag_word_once():
    case = synth(["FLD dword ptr [ESI + 0x0]", "FLD dword ptr [ESI + 0x4]",
                  "MOV EAX,0x1", "CALL EAX", "RET"])
    text, _ = translate(case, "default")
    assert text.count("c->fpu_tag") == 2   # one read, one write
    assert "ftag_put" not in text
    plain, _ = translate(case, "plain")
    assert plain.count("ftag_put") == 2


def test_an_inexact_value_leaves_st_bits_alone():
    case = synth(["FLD dword ptr [ESI + 0x0]", "FILD dword ptr [ESI + 0x4]",
                  "MOV EAX,0x1", "CALL EAX", "RET"])
    text, _ = translate(case, "default")
    assert text.count("c->st_bits[") == 1       # the FILD only
    assert "st_exact[(ft0_ + 6u) & 7u] = 1u" in text


def test_st0_leaf_hands_st0_to_the_recipe():
    leaf = 0x0D0F0000
    case = synth(["FLD dword ptr [ESI + 0x0]", "FMUL dword ptr [ESI + 0x4]",
                  "CALL 0x%x" % leaf, "MOV dword ptr [EDI + 0x0],EAX", "RET"])
    saved = dict(T.ST0_LEAVES)
    T.ST0_LEAVES[leaf] = "ftol2"
    try:
        tr = T.Translator(M.NoImage(case), {case.addr, leaf}, Opts(False))
        fn = T.Function(case.addr, case.name, len(case.code), T.parse_listing_text(case.listing()))
        fn.measure(M.NoImage())
        tr.prepare(fn)
        old = dict(T.SPEEDUPS)
        T.SPEEDUPS["x87_st0_leaves"] = True
        try:
            text = "\n".join(tr.translate(fn, ()))
        finally:
            T.SPEEDUPS.clear()
            T.SPEEDUPS.update(old)
    finally:
        T.ST0_LEAVES.clear()
        T.ST0_LEAVES.update(saved)
    assert "recomp_x87_ftol2(c, fv1_, 0, 0);" in text
    assert "CALL_FN" not in text and "c->st[" not in text
    # ST(0) and the register above it end empty, TOP back where it was.
    assert "0xf000u << (2u * ft0_)" in text


def test_st0_leaf_matches_the_call_it_replaces():
    """The recipe against the helpers' view of _ftol2: a body that calls the
    leaf through the cache, run in cmini, against FISTP-free arithmetic on
    the same value (the model of x87_leaves.h is checked line for line
    against the C there by review; this checks the cache side)."""
    leaf = 0x0D0F0000
    case = synth(["FLD qword ptr [ESI + 0x0]", "CALL 0x%x" % leaf,
                  "MOV dword ptr [EDI + 0x0],EAX", "MOV dword ptr [EDI + 0x4],EDX", "RET"])
    saved = dict(T.ST0_LEAVES)
    T.ST0_LEAVES[leaf] = "ftol2"
    try:
        tr = T.Translator(M.NoImage(case), {case.addr, leaf}, Opts(False))
        fn = T.Function(case.addr, case.name, len(case.code), T.parse_listing_text(case.listing()))
        fn.measure(M.NoImage())
        tr.prepare(fn)
        old = dict(T.SPEEDUPS)
        T.SPEEDUPS.update(x87_st0_leaves=True)
        try:
            text = "\n".join(tr.translate(fn, ()))
        finally:
            T.SPEEDUPS.clear()
            T.SPEEDUPS.update(old)
    finally:
        T.ST0_LEAVES.clear()
        T.ST0_LEAVES.update(saved)
    m = cmini.Machine(text, "fn_%08x" % case.addr)
    for v in (2.5, -2.5, 3.7, -3.7, 1e12 + 0.5, -0.0, 7.0):
        s = initial_state(1)
        s.wr(SRC, 8, struct.unpack("<Q", struct.pack("<d", v))[0])
        top = s.f["fpu_top"]
        m.run(s)
        got = s.rd(DST, 8)
        got = got - (1 << 64) if got >> 63 else got
        assert got == int(v), (v, got)       # toward zero
        assert s.f["fpu_top"] == top
        assert (s.f["fpu_tag"] >> (2 * ((top - 1) & 7))) & 3 == 3


def test_speedup_overrides_parse_and_reject():
    assert T.game_config.parse_speedup_overrides("x87_carry=1, devirt=off", "t") == {
        "x87_carry": True, "devirt": False}
    with pytest.raises(ValueError):
        T.game_config.parse_speedup_overrides("x87_carryy=1", "t")
    with pytest.raises(ValueError):
        T.game_config.translate_speedups({"x87_carry": 1}, "t", environ={})
    merged = T.game_config.translate_speedups({"x87_carry": True}, "t",
                                              environ={"RECOMP_TRANSLATE_SPEEDUPS": "x87_carry=0"})
    assert merged["x87_carry"] is False and merged["shim_first"] is True
