"""[translate.speedups] switches other than the x87 ones (test_x87_carry.py).

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_speedups.py

Each switch changes generated text in one place; these pin that text with
the switch on and off, so an A/B regeneration changes only what it names."""

import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_translate_insns as M  # noqa: E402

T = M.T


class Opts(object):
    eager_flags = False
    x87_direct = False


@pytest.fixture
def speed():
    saved = dict(T.SPEEDUPS), dict(T.ICALL_PROFILE)

    def set_(**kw):
        T.SPEEDUPS.update(kw)
    yield set_
    T.SPEEDUPS.clear()
    T.SPEEDUPS.update(saved[0])
    T.ICALL_PROFILE.clear()
    T.ICALL_PROFILE.update(saved[1])


def translate(lines, known=()):
    addr = M.CODE_BASE
    case = M.Case("synth", addr, [(4 * j, t) for j, t in enumerate(lines)], "90" * (4 * len(lines)))
    tr = T.Translator(M.NoImage(case), {addr} | set(known), Opts())
    fn = T.Function(addr, case.name, len(case.code), T.parse_listing_text(case.listing()))
    fn.measure(M.NoImage())
    tr.prepare(fn)
    return "\n".join(tr.translate(fn, ())), tr


SITE = M.CODE_BASE  # the CALL below is the first instruction


def test_indirect_calls_test_the_shim_range_themselves(speed):
    speed(icall_shim_inline=True)
    text, _ = translate(["CALL EAX", "RET"])
    assert ("if (t_ - GUEST_SHIM_BASE < GUEST_SHIM_END - GUEST_SHIM_BASE && !recomp_frame_watch) "
            "recomp_shim_call(c, t_); else recomp_call(c, t_);") in text
    speed(icall_shim_inline=False)
    text, _ = translate(["CALL EAX", "RET"])
    assert "recomp_shim_call" not in text and "recomp_call(c, t_);" in text


def test_devirtualized_site_calls_its_recorded_targets(speed):
    a, b = 0x0D020000, 0x0D030000
    speed(devirt=True, icall_shim_inline=False)
    T.ICALL_PROFILE[SITE] = [(b, 90), (a, 10), (0x0FF00010, 5)]   # and a shim
    text, tr = translate(["CALL dword ptr [EAX + 0x8]", "RET"], known=(a, b))
    assert "if (t_ == 0xd030000u) { CALL_FN(0d030000); }" in text
    assert "else if (t_ == 0xd020000u) { CALL_FN(0d020000); }" in text
    assert "else { recomp_call(c, t_); }" in text
    assert tr.stats["_call_devirt"] == 1
    speed(devirt=False)
    text, _ = translate(["CALL dword ptr [EAX + 0x8]", "RET"], known=(a, b))
    assert "CALL_FN" not in text


def test_a_megamorphic_site_stays_generic(speed):
    targets = [0x0D020000 + 0x100 * k for k in range(T.Translator.DEVIRT_MAX_TARGETS + 1)]
    speed(devirt=True)
    T.ICALL_PROFILE[SITE] = [(t, 1) for t in targets]
    text, tr = translate(["CALL EAX", "RET"], known=targets)
    assert "CALL_FN" not in text and not tr.stats.get("_call_devirt")


def test_unknown_profile_targets_are_not_called_directly(speed):
    speed(devirt=True)
    T.ICALL_PROFILE[SITE] = [(0x0D050000, 7)]   # not a translated function
    text, _ = translate(["CALL EAX", "RET"])
    assert "CALL_FN" not in text


def test_recording_sites_note_their_targets(speed):
    speed(icall_profile_record=True)
    text, _ = translate(["CALL EAX", "RET"])
    assert "recomp_icall_note(0xd010000u, t_);" in text
    speed(icall_profile_record=False)
    assert "recomp_icall_note" not in translate(["CALL EAX", "RET"])[0]


def test_icall_profile_reads_and_merges(tmp_path):
    p = tmp_path / "icall.txt"
    p.write_text("# comment\n00401000 00402000 5\n00401000 00403000 9\n00401000 00402000 1\n")
    assert T.read_icall_profile(str(p)) == {0x00401000: [(0x00403000, 9), (0x00402000, 6)]}
    assert T.read_icall_profile(str(tmp_path / "missing.txt")) == {}
    p.write_text("00401000 nonsense\n")
    with pytest.raises(T.TranslateError):
        T.read_icall_profile(str(p))


def test_shim_range_first_in_dispatch(speed):
    template = "inner{@SHIM_FIRST_CALL@\n    lookup;\njump@SHIM_FIRST_JUMP@\n"
    speed(shim_first=True)
    on = T.shim_first_dispatch(template)
    assert on.count("target >= GUEST_SHIM_BASE && target < GUEST_SHIM_END") == 2
    assert on.index("GUEST_SHIM_BASE") < on.index("    lookup;")
    speed(shim_first=False)
    assert T.shim_first_dispatch(template) == "inner{\n    lookup;\njump\n"


RESTORE_ONLY = ["MOV ECX,dword ptr [EBP + -0xc]", "MOV dword ptr FS:[0x0],ECX", "RET"]


def test_a_restore_only_body_retires_its_frame(speed):
    speed(seh_separate_restores=True)
    text, _ = translate(RESTORE_ONLY)
    assert "recomp_seh_frame_leave(c);" in text
    speed(seh_separate_restores=False)
    assert "recomp_seh_frame_leave" not in translate(RESTORE_ONLY)[0]


def test_an_unproven_establishment_keeps_its_restore_plain(speed):
    speed(seh_separate_restores=True)
    # Publishes ESP as the chain head without the handler push the frame
    # sites recognise: nothing proven to retire.
    text, _ = translate(["MOV dword ptr FS:[0x0],ESP", "MOV ECX,dword ptr [EBP + -0xc]",
                         "MOV dword ptr FS:[0x0],ECX", "RET"])
    assert "recomp_seh_frame_leave" not in text
