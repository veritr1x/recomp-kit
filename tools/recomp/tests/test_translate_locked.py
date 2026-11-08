"""Locked read-modify-writes become host atomics (runtime/x86.h recomp_lock_*).

A guest thread running free beside the baton holder (runtime/win32.h free run)
shares reference counts, queues and spinlocks with it, and the game counts on
LOCK XADD, LOCK CMPXCHG and XCHG with memory being indivisible. The unlocked
forms keep the plain read and write: nothing promises them to another core.
The results themselves are compared with Unicorn in test_translate_insns.py.
"""
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import translate as T


class Image:
    base, end = 0x400000, 0x500000
    def insn_end(self, *args): return None
    def rd8(self, *args): return None
    def rd32(self, *args): return None


class Options:
    eager_flags = False
    allow_unmodelled = None


def translated(listing):
    fn = T.Function(0x401000, "locked", 0x40, T.parse_listing_text(listing))
    fn.measure(Image())
    tr = T.Translator(Image(), {fn.addr}, Options())
    tr.prepare(fn)
    return "\n".join(tr.translate(fn))


@pytest.mark.parametrize("line,helper", [
    ("XADD.LOCK dword ptr [ECX],EDX", "recomp_lock_xadd32(ad_"),
    ("XADD.LOCK word ptr [ECX + 0x2],DX", "recomp_lock_xadd16(ad_"),
    ("XADD.LOCK byte ptr [ECX],DL", "recomp_lock_xadd8(ad_"),
    ("CMPXCHG.LOCK dword ptr [ECX],EDX", "recomp_lock_cmpxchg32(ad_"),
    ("CMPXCHG.LOCK byte ptr [ECX],DL", "recomp_lock_cmpxchg8(ad_"),
    ("XCHG dword ptr [ECX],EDX", "recomp_lock_xchg32(ad_"),
    ("XCHG EDX,dword ptr [ECX]", "recomp_lock_xchg32(ad_"),
    ("XCHG word ptr [ECX],DX", "recomp_lock_xchg16(ad_"),
])
def test_locked_forms_use_host_atomics(line, helper):
    out = translated("00401000  %s\n00401004  RET\n" % line)
    assert helper in out


@pytest.mark.parametrize("line", [
    "XADD dword ptr [ECX],EDX",
    "CMPXCHG dword ptr [ECX],EDX",
    "XADD.LOCK EAX,EDX",
    "XCHG EAX,EDX",
])
def test_unlocked_and_register_forms_stay_plain(line):
    out = translated("00401000  %s\n00401004  RET\n" % line)
    assert "recomp_lock_" not in out


def test_capstone_keeps_the_lock_prefix_and_the_operands():
    capstone = pytest.importorskip("capstone")
    image = T.Image.__new__(T.Image)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    # lock xadd [ecx],edx; lock cmpxchg [ecx],edx; rep stosd
    code = bytes.fromhex("f00fc111" "f00fb111" "f3ab")
    insns = [image.to_insn(ci) for ci in md.disasm(code, 0x401000)]
    assert [(i.mnem, i.rep) for i in insns] == [("XADD", "LOCK"), ("CMPXCHG", "LOCK"),
                                                ("STOSD", "REP")]
    assert insns[0].ops == ["dword ptr [ECX]", "EDX"]
    assert insns[1].ops == ["dword ptr [ECX]", "EDX"]
    assert insns[2].ops == []
