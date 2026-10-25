"""Every same-function control transfer uses the budgeted back-edge helper."""
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


@pytest.mark.parametrize("mnem", ["JMP","JNZ","LOOP","JECXZ"])
@pytest.mark.parametrize("backward", [False,True])
def test_direct_branch(mnem, backward):
    text = "00401000  NOP\n00401001  %s %s\n00401003  RET\n" % (mnem,"0x401000" if backward else "0x401003")
    fn = T.Function(0x401000,"loop",4,T.parse_listing_text(text))
    fn.measure(Image())
    tr = T.Translator(Image(),{fn.addr},Options()); tr.prepare(fn)
    out = "\n".join(tr.translate(fn))
    assert ("RECOMP_BACKEDGE(c); goto L_00401000;" in out) == backward
    assert out.count("RECOMP_BACKEDGE(c)") == int(backward)


def test_indirect_cases_and_pushed_return_use_source_address():
    fn = T.Function(0x401000,"computed",9,T.parse_listing_text(
        "00401000  NOP\n00401001  JMP EAX\n00401003  PUSH 0x401000\n00401008  RET\n"))
    fn.measure(Image())
    tr = T.Translator(Image(),{fn.addr},Options()); tr.prepare(fn)
    fn.return_jumps = set()
    fn.seh_escapes = set()
    indirect = "\n".join(tr.emit_indirect_jump(fn,1,fn.insns[1],T.parse_operand("EAX")))
    assert "case 0x401000u: RECOMP_BACKEDGE(c); goto L_00401000;" in indirect
    assert "case 0x401008u: goto L_00401008;" in indirect
    fn.pushed_continuations = {0x401000,0x401008}
    result = "\n".join(tr._emit(fn,3,fn.insns[3],"RET",None,T.ALL_FLAGS))
    assert "case 0x401000u: RECOMP_BACKEDGE(c); goto L_00401000;" in result
    assert "case 0x401008u: RECOMP_BACKEDGE(c); goto L_00401008;" in result


def test_only_the_helper_spells_a_generated_goto():
    import inspect
    # Entry dispatches have no source PC and consume no guest loop budget.
    assert T.Translator.goto_label(1) == "goto L_00000001;"
    assert T.Translator.goto_label(1,1).startswith("RECOMP_BACKEDGE(c);")
    source = inspect.getsource(T.Translator)
    assert source.count('"goto L_%08x;"') == 1
