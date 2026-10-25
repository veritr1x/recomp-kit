"""Jump-table shapes the decoder has to recognise, on synthetic functions.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_jumptables.py

Each case is a listing plus a fake image holding the table's dwords.  The
shapes come from Visual C++ 6's hand-written `memcpy`, which the first
corpus (a later compiler) never showed the decoder:

    AND EAX,0x3 / JMP [EAX*4 + T]            a mask bound; slot 0 is never
                                             reached and holds code bytes
    SUB ECX,0x4 / JC S ... S: JMP [ECX*4 + T]  the guard branches TO the jump
                                             with the index in -4..-1
    CMP ECX,0x4 / JC S ... S: JMP [ECX*4 + T]  the same with the index in 0..3
    CMP ECX,0x8 / JC S ... S: NEG ECX / JMP    the index negated after the guard

A table the decoder cannot bound falls back to reading forward while the
entries look like code, which finds nothing for a hole in slot 0 and finds
the wrong entries for a negative index, so these shapes are decoded exactly."""

import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402

FN = 0x00500000        # the function
TABLE = 0x00500800     # where the table's dwords live
GARBAGE = 0x900064E7   # what a hole reads as: code bytes, not an address


class FakeImage(object):
    """Code at the listed addresses, a table of dwords, nothing else."""

    md = None

    def __init__(self, code_addrs, dwords):
        self.base = 0x00400000
        self.end = 0x00600000
        self.size = self.end - self.base
        self.code = set(code_addrs)
        self.dwords = dwords          # address -> dword

    def insn_end(self, addr, mnem):
        return None

    def rd32(self, va):
        return self.dwords.get(va)

    def rd8(self, va):
        return None

    def is_exec(self, va):
        return va in self.code

    def looks_like_code_start(self, va):
        return va in self.code

    def looks_like_function(self, va):
        return False

    def relocated_pointers(self):
        return {}


class Opts(object):
    eager_flags = False


def decode(lines, dwords, strict=False):
    """Translate `lines` and return the targets decoded for its JMP table."""
    listing = "\n".join("%08x  %s" % (FN + off, text) for off, text in lines) + "\n"
    insns = T.parse_listing_text(listing)
    fn = T.Function(FN, "case", 0x100, insns)
    image = FakeImage([i.addr for i in insns], dwords)
    fn.measure(image)
    tr = T.Translator(image, {FN}, Opts())
    tr.prepare(fn, strict=strict)
    sites = [(k, ins) for k, ins in enumerate(insns)
             if ins.mnem == "JMP" and "dword ptr" in ins.ops[0]]
    assert len(sites) == 1
    return sorted(tr.jumptables.get((FN, sites[0][1].addr), ())), tr


def table(entries):
    return {TABLE + 4 * k: v for k, v in entries.items()}


def test_pre_scaled_byte_offsets_use_unaligned_dword_table():
    base = TABLE + 2
    targets, tr = decode([(0, "MOV EAX,dword ptr [ECX + 0x40]"),
                          (3, "JMP dword ptr [EAX + 0x%x]" % base),
                          (0x10, "RET"), (0x20, "RET")],
                         {base: FN + 0x10, base + 4: FN + 0x20, base + 8: 0})
    assert targets == [FN + 0x10, FN + 0x20]
    assert (base, base + 8) in tr.table_ranges


def test_unscaled_static_field_is_not_assumed_to_be_a_table():
    targets, tr = decode([(0, "JMP dword ptr [EAX + 0x%x]" % TABLE), (0x10, "RET")],
                         {TABLE: 0x12345678, TABLE + 4: FN + 0x10})
    assert not targets
    assert not tr.table_sites


@pytest.mark.parametrize("operand", [
    "dword ptr [EAX + EBX*0x4 + -0x75]",
    "dword ptr [EAX*0x4 + 0x10097]",
    "dword ptr [EBX + ECX*0x4 + -0x68]",
    "dword ptr [ECX + EDX*0x4 + 0x100ae]",
    "dword ptr [EBX + -0x75]",
    "dword ptr [EAX*0x4 + 0x3fffff]",  # just below the image
    "dword ptr [EAX*0x4 + 0x600000]",  # the image's exclusive end
])
def test_indirect_jump_through_a_structure_field_is_not_a_table(operand):
    """Out-of-image offsets produce computed jumps, not undecoded tables.

    The four sites recovered from the pinned Siege.exe (their entries are
    absent from the Ghidra export) have these exact instruction lines:

    008b97e9  JMP dword ptr [EAX + EBX*0x4 + -0x75]
    00972b58  JMP dword ptr [EAX*0x4 + 0x10097]
    0098542a  JMP dword ptr [EBX + ECX*0x4 + -0x68]
    00ad6996  JMP dword ptr [ECX + EDX*0x4 + 0x100ae]

    None of those displacements names table storage in the image. The same
    applies to a simple structure field with no scaled index.
    """
    listing = ("%08x  MOV EBX,dword ptr [ESP + 0x4]\n"
               "%08x  JMP %s\n") % (FN, FN + 4, operand)
    insns = T.parse_listing_text(listing)
    fn = T.Function(FN, "field_jump", 0x100, insns)
    image = FakeImage([i.addr for i in insns], {})
    fn.measure(image)
    tr = T.Translator(image, {FN}, Opts())
    tr.prepare(fn, strict=True)
    source = "\n".join(tr.translate(fn))
    assert "uint32_t t_ = rd32(" in source
    assert "recomp_jump(c, t_); return;" in source
    # A computed jump first checks local instruction boundaries, then uses
    # runtime dispatch. This does not turn the operand into a decoded table.
    assert "switch (t_)" in source
    for addr in sorted(fn.addrs):
        assert "case %s: RECOMP_BACKEDGE(c); goto L_%08x;" % (T.hexlit(addr), addr) in source
    assert "default: break;" in source
    assert tr.table_sites == {}
    assert tr.jumptables == {}


@pytest.mark.parametrize("index", ["EAX*0x4", "EBX + EAX*0x4"])
def test_in_image_table_without_entries_still_counts_as_a_table(index):
    """An unreadable in-image table must still trip the coverage gate."""
    targets, tr = decode([(0, "JMP dword ptr [%s + 0x%x]" % (index, TABLE))], {})
    assert targets == []
    assert tr.table_sites == {(FN, FN): TABLE}
    assert tr.jumptables == {}


def test_table_storage_in_an_executable_section_is_decoded():
    """Switch tables can live in .text, outside Image.data_ranges."""
    from test_translate_driver import synthetic_image
    image = synthetic_image({TABLE: (FN + 0x10).to_bytes(4, "little"),
                             FN + 0x10: b"\xc3"}, base=FN)
    assert image.is_exec(TABLE)
    assert image.data_ranges == []
    insns = T.parse_listing_text("%08x  JMP dword ptr [EAX*0x4 + 0x%x]\n"
                                "%08x  RET\n" % (FN, TABLE, FN + 0x10))
    fn = T.Function(FN, "text_table", 0x100, insns)
    fn.measure(image)
    tr = T.Translator(image, {FN}, Opts())
    tr.prepare(fn, strict=True)
    assert tr.jumptables == {(FN, FN): [FN + 0x10]}
    assert tr.table_sites == {(FN, FN): TABLE}
    assert "switch (t_)" in "\n".join(tr.translate(fn))


def test_and_mask_bound_with_a_hole_in_slot_zero():
    # LeadUpVec: `AND EAX,3` leaves 0..3, but 0 was diverted earlier and the
    # slot holds the tail of the previous instruction.
    lines = [(0x00, "AND EAX,0x3"), (0x03, "ADD ECX,EAX"),
             (0x05, "JMP dword ptr [EAX*0x4 + 0x%x]" % TABLE),
             (0x10, "RET"), (0x20, "RET"), (0x30, "RET")]
    targets, tr = decode(lines, table({0: GARBAGE, 1: FN + 0x10, 2: FN + 0x20, 3: FN + 0x30}))
    assert targets == [FN + 0x10, FN + 0x20, FN + 0x30]
    decode(lines, table({0: GARBAGE, 1: FN + 0x10, 2: FN + 0x20, 3: FN + 0x30}), strict=True)


def test_guard_branching_to_the_jump_after_sub_gives_a_negative_range():
    # TrailUpVec through `SUB ECX,4 / JC`: at the jump ECX is -4..-1, so the
    # entries sit below the displacement.
    lines = [(0x00, "SUB ECX,0x4"), (0x03, "JC 0x%08x" % (FN + 0x0c)),
             (0x05, "JMP dword ptr [ECX*0x4 + 0x%x]" % (TABLE + 0x100)),
             (0x0c, "JMP dword ptr [ECX*0x4 + 0x%x]" % TABLE),
             (0x20, "RET"), (0x30, "RET"), (0x40, "RET"), (0x50, "RET")]
    dwords = table({-4: FN + 0x20, -3: FN + 0x30, -2: FN + 0x40, -1: FN + 0x50,
                    0: GARBAGE})
    # The other JMP is a decoy the decoder must not confuse with this one.
    dwords.update({TABLE + 0x100: FN + 0x20})
    listing = "\n".join("%08x  %s" % (FN + off, text) for off, text in lines) + "\n"
    insns = T.parse_listing_text(listing)
    fn = T.Function(FN, "case", 0x100, insns)
    image = FakeImage([i.addr for i in insns], dwords)
    fn.measure(image)
    tr = T.Translator(image, {FN}, Opts())
    tr.prepare(fn)
    assert sorted(tr.jumptables.get((FN, FN + 0x0c), ())) == [FN + 0x20, FN + 0x30, FN + 0x40, FN + 0x50]
    tr.prepare(fn, strict=True)


def test_guard_branching_to_the_jump_after_cmp_bounds_from_zero():
    lines = [(0x00, "CMP ECX,0x4"), (0x03, "JC 0x%08x" % (FN + 0x0c)),
             (0x05, "MOV EAX,0x0"), (0x0a, "RET"),
             (0x0c, "JMP dword ptr [ECX*0x4 + 0x%x]" % TABLE),
             (0x20, "RET"), (0x30, "RET"), (0x40, "RET"), (0x50, "RET")]
    targets, _ = decode(lines, table({0: FN + 0x20, 1: FN + 0x30, 2: FN + 0x40, 3: FN + 0x50,
                                      4: GARBAGE}))
    assert targets == [FN + 0x20, FN + 0x30, FN + 0x40, FN + 0x50]


def test_negated_index_after_a_guard_reads_downwards():
    # `CMP ECX,8 / JC S ... S: NEG ECX / JMP [ECX*4 + T]`: 0..7 becomes -7..0.
    lines = [(0x00, "CMP ECX,0x8"), (0x03, "JC 0x%08x" % (FN + 0x0c)),
             (0x05, "MOV EAX,0x0"), (0x0a, "RET"),
             (0x0c, "NEG ECX"),
             (0x0e, "JMP dword ptr [ECX*0x4 + 0x%x]" % TABLE)] + \
            [(0x20 + 0x10 * k, "RET") for k in range(8)]
    entries = {-k: FN + 0x20 + 0x10 * k for k in range(8)}
    entries[-8] = GARBAGE
    entries[1] = GARBAGE
    targets, _ = decode(lines, table(entries))
    assert targets == [FN + 0x20 + 0x10 * k for k in range(8)]


def test_bounded_table_recovers_targets_before_the_entry():
    insns = T.parse_listing_text(
        "%08x  RET\n%08x  CMP EAX,0x0\n%08x  JA 0x%08x\n"
        "%08x  JMP dword ptr [EAX*0x4 + 0x%x]\n%08x  RET\n" %
        (FN-0x100,FN,FN+3,FN+0x40,FN+5,TABLE,FN+0x40))
    target = FN-0x80
    fn = T.Function(FN,"backward",0x100,insns)
    image = FakeImage([i.addr for i in insns]+[target],{TABLE:target})
    fn.measure(image)
    tr = T.Translator(image,{FN},Opts())
    tr.prepare(fn)
    assert target in tr.unlisted_targets
    assert tr.jumptables[(FN,FN+5)] == [target]


def test_detached_case_within_listed_extent_exceeds_64k():
    target = FN + 0x18000
    insns = T.parse_listing_text(
        "%08x  CMP EAX,0\n%08x  JA 0x%08x\n"
        "%08x  JMP dword ptr [EAX*4 + 0x%x]\n%08x  RET\n" %
        (FN, FN+3, FN+0x19000, FN+5, TABLE, FN+0x19000))
    image = FakeImage([i.addr for i in insns] + [target], {TABLE: target})
    fn = T.Function(FN, "detached", 0x19001, insns)
    fn.measure(image)
    tr = T.Translator(image, {FN}, Opts())
    tr.prepare(fn)
    assert tr.jumptables[(FN, FN+5)] == [target]
    assert target in tr.unlisted_targets


@pytest.mark.parametrize("mutation", [None, "slot", "frame", "esp", "call", "incoming"])
def test_byte_index_bound_through_stable_frame_slot(mutation):
    byte_base = TABLE + 0x100
    frame = "ESP" if mutation == "esp" else "EBP"
    lines = [(0, "CMP dword ptr [%s + -0x4],0x2" % frame),
             (4, "JA 0x%08x" % (FN+0x40))]
    extra = {"slot": "MOV dword ptr [EBP + -0x4],EAX", "frame": "INC EBP",
             "call": "CALL EDI"}.get(mutation)
    if extra:
        lines.append((6, extra))
    lines += [(10, "MOV ECX,dword ptr [%s + -0x4]" % frame),
              (14, "MOVZX EAX,byte ptr [ECX + 0x%x]" % byte_base),
              (21, "JMP dword ptr [EAX*4 + 0x%x]" % TABLE),
              (0x30, "RET"), (0x40, "RET")]
    if mutation == "incoming":
        lines.append((0x50, "JMP 0x%08x" % (FN+10)))
    insns = T.parse_listing_text("\n".join("%08x  %s" % (FN+off, text) for off, text in lines))
    image = FakeImage([i.addr for i in insns], {TABLE:FN+0x30, TABLE+4:GARBAGE,
                                              TABLE+8:FN+0x40})
    image.rd8 = lambda addr: {byte_base:0, byte_base+1:2, byte_base+2:0}.get(addr)
    fn = T.Function(FN, "frame_slot", 0x60, insns)
    fn.measure(image)
    tr = T.Translator(image, {FN}, Opts())
    tr.prepare(fn)
    got = tr.jumptables.get((FN, FN+21))
    assert got == ([FN+0x30,FN+0x40,FN+0x30] if mutation is None else [FN+0x30])


def test_branch_into_register_guard_invalidates_bound():
    lines = [(0,"JMP 0x%08x" % (FN+8)), (3,"CMP EAX,0x1"),
             (6,"JA 0x%08x" % (FN+0x40)),
             (8,"JMP dword ptr [EAX*4 + 0x%x]" % TABLE), (0x40,"RET")]
    _, tr = decode(lines, table({0: FN+0x40, 1:GARBAGE}))
    assert tr.stats["_jmp_table_bounded"] == 0


@pytest.mark.parametrize("clobber", [False, True])
def test_byte_index_bound_through_lea(clobber):
    byte_base = TABLE+0x100
    lines = [(0,"LEA EAX,dword ptr [ECX + -0x5]")]
    if clobber:
        lines.append((3,"INC ECX"))
    lines += [(4,"CMP EAX,0x2"),(7,"JA 0x%08x" % (FN+0x40)),
              (9,"MOVZX EAX,byte ptr [ECX + 0x%x]" % byte_base),
              (16,"JMP dword ptr [EAX*0x4 + 0x%x]" % TABLE),
              (0x20,"RET"),(0x30,"RET"),(0x40,"RET")]
    insns = T.parse_listing_text("\n".join("%08x  %s" % (FN+off,text) for off,text in lines))
    fn = T.Function(FN,"lea_guard",0x100,insns)
    image = FakeImage([i.addr for i in insns],table({0:GARBAGE,1:FN+0x20,3:FN+0x30}))
    image.rd8 = lambda address: {byte_base+5:1,byte_base+6:3,byte_base+7:1}.get(address)
    fn.measure(image)
    tr = T.Translator(image,{FN},Opts())
    tr.prepare(fn)
    targets = tr.jumptables.get((FN,FN+16))
    if clobber:
        assert targets is None
    else:
        assert targets == [FN+0x20,FN+0x30,FN+0x20]
        assert (byte_base+5,byte_base+8) in tr.table_ranges
        tr.prepare(fn,strict=True)
