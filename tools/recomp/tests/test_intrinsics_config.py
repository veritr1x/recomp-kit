"""Audited CRT addresses select host setjmp/longjmp substitutions."""
from pathlib import Path
import pytest
from test_translate_driver import synthetic_image, translate_entry_fixture, T
import game_config

ROOT = Path(__file__).resolve().parents[3]
PAIR = {"setjmp": 0x401100, "longjmp": 0x401200}


@pytest.fixture(autouse=True)
def restore_intrinsics(monkeypatch):
    for name in ("INTRINSIC_SETJMP", "INTRINSIC_LONGJMP", "INTRINSIC_BODY",
                 "LISTINGS", "FUNCS_TSV", "BINARY", "CURATED", "ANIMATION_COUNTER",
                 "VISUAL_ANIMATION_READS", "EXTRA_ENTRY_POINTS", "FUNCTION_ALIGNMENT",
                 "SYMBOL_PREFIX", "AUX_MODULE", "RESUMABLE_STACKS", "OPERAND_REDIRECTS",
                 "INSTRUCTION_PATCHES", "DATA_SEEDS"):
        monkeypatch.setattr(T, name, getattr(T, name))


def test_configure_selects_pair_and_clears_it_for_modules():
    cfg = game_config.load(ROOT / "games/stub")
    cfg["translate"]["intrinsics"] = PAIR
    T.configure(cfg)
    assert T.INTRINSIC_BODY == {PAIR["setjmp"]: "recomp_setjmp(c);", PAIR["longjmp"]: "recomp_longjmp(c);"}
    cfg["aux_modules"] = [{"key":"aux","listings_path":Path("analysis/aux"),
        "path":Path("aux.dll"),"function_alignment":4}]
    T.configure_module(cfg,"aux")
    assert T.INTRINSIC_BODY == {} and T.INTRINSIC_SETJMP is None
    cfg["translate"]["intrinsics"] = {}
    T.configure(cfg)
    assert T.INTRINSIC_BODY == {}
    del cfg["translate"]["intrinsics"]
    T.configure(cfg)
    assert T.INTRINSIC_SETJMP == T.LEGACY_INTRINSICS["setjmp"]


def test_pair_loads_from_toml(tmp_path):
    (tmp_path / "globals.toml").write_text("")
    text = (ROOT / "games/stub/game.toml").read_text()
    (tmp_path / "game.toml").write_text(text+'\n[translate.intrinsics]\nsetjmp = 0x401100\nlongjmp = 0x401200\n')
    cfg = game_config.load(tmp_path)
    T.configure(cfg)
    assert T.INTRINSIC_SETJMP == PAIR["setjmp"] and T.INTRINSIC_LONGJMP == PAIR["longjmp"]


@pytest.mark.parametrize("values", [
    'setjmp = 0x401100', 'other = 1',
    'setjmp = true\nlongjmp = 0x401200',
    'setjmp = -1\nlongjmp = 0x401200',
    'setjmp = 0x100000000\nlongjmp = 0x401200',
    'setjmp = 0x401100\nlongjmp = 0x401100',
    'setjmp = "0x401100"\nlongjmp = 0x401200'])
def test_invalid_pairs_are_rejected(tmp_path,values):
    (tmp_path / "globals.toml").write_text("")
    text = (ROOT / "games/stub/game.toml").read_text()
    (tmp_path / "game.toml").write_text(text+'\n[translate.intrinsics]\n'+values+'\n')
    with pytest.raises(ValueError,match="intrinsic"):
        game_config.load(tmp_path)


def test_callsite_saves_setjmp_in_the_callers_host_frame():
    T.configure_intrinsics(PAIR)
    image = synthetic_image({0x401000:b"\xe8\xfb\x00\x00\x00\xc3"})
    fn = T.Function(0x401000,"caller",6,T.parse_listing_text(
        "00401000  CALL 0x00401100\n00401005  RET\n"))
    fn.measure(image)
    tr = T.Translator(image,set(PAIR.values()),type("Opts",(),{"eager_flags":False})())
    tr.prepare(fn)
    body = "\n".join(tr.translate(fn))
    assert "if (setjmp(*b_) == 0)" in body
    assert "else recomp_setjmp_return(c, (int32_t)c->r[R_EAX]);" in body
    assert "recomp_setjmp_prepare(c)" in body
    assert "CALL_FN(00401100)" not in body


def test_intrinsic_body_skips_original_instructions_and_alternates():
    T.configure_intrinsics(PAIR)
    fn = T.Function(PAIR["longjmp"],"jumper",2,T.parse_listing_text("00401200  UD2\n00401201  RET\n"))
    tr = T.Translator(synthetic_image({}),set(PAIR.values()),type("Opts",(),{"eager_flags":False})())
    tr.prepare(fn,strict=True)
    body = "\n".join(tr.translate(fn,{0x401201}))
    assert "recomp_longjmp(c);" in body and "UD2" not in body
    assert "body_" not in body and "fn_00401201" not in body
    assert T.intrinsic_alternates_removed({0x401201:fn}) == {}


def test_alternate_is_absent_from_generated_dispatch(tmp_path,monkeypatch):
    T.configure_intrinsics(PAIR)
    image = synthetic_image({PAIR["setjmp"]:b"\x90\xc3",PAIR["longjmp"]:b"\xc3",
                             0x401500:(PAIR["setjmp"]+1).to_bytes(4,"little")})
    source = translate_entry_fixture(tmp_path,monkeypatch,image,
        {PAIR["setjmp"]:b"\x90\xc3",PAIR["longjmp"]:b"\xc3"})
    assert "recomp_setjmp(c);" in source and "recomp_longjmp(c);" in source
    table = (tmp_path / "gen/table.c").read_text()
    assert "FN(00401101)" not in table and "fn_00401101" not in source
