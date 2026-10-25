"""PE32 HIGHLOW rebasing: real PE bytes, listing operands and driver output."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import translate as T

PREFERRED = 0x10000000
LOAD = 0x00020000


def pe_bytes(kind=3, page=0x1000, block=12):
    data = bytearray(0x1200)
    def word(at, value): struct.pack_into("<H", data, at, value)
    def dword(at, value): struct.pack_into("<I", data, at, value)
    word(0, 0x5a4d); dword(0x3c, 0x80); dword(0x80, 0x4550)
    word(0x84, 0x14c); word(0x86, 1); word(0x94, 0xe0); word(0x96, 0x2102)
    opt = 0x98
    word(opt, 0x10b); dword(opt + 16, 0x1000); dword(opt + 28, PREFERRED)
    dword(opt + 32, 0x1000); dword(opt + 36, 0x200)
    dword(opt + 56, 0x2000); dword(opt + 60, 0x200); dword(opt + 92, 16)
    dword(opt + 96 + 5 * 8, 0x1800); dword(opt + 100 + 5 * 8, 12)
    sh = opt + 0xe0
    data[sh:sh + 8] = b".text\0\0\0"
    for off, value in ((8, 0x1000), (12, 0x1000), (16, 0x1000), (20, 0x200), (36, 0x60000060)):
        dword(sh + off, value)
    # MOV EAX,pointer; CALL relative; RET. Only the MOV immediate relocates.
    data[0x200:0x20b] = b"\xb8" + struct.pack("<I", PREFERRED + 0x1080) + b"\xe8\x76\x00\x00\x00\xc3"
    data[0x280] = 0xc3
    dword(0xa00, page); dword(0xa04, block)
    word(0xa08, kind << 12 | 1); word(0xa0a, 0)
    return bytes(data)


def test_image_and_listing_rebase(tmp_path):
    path = tmp_path / "module.dll"
    path.write_bytes(pe_bytes())
    original = T.Image(str(path))
    image = T.Image(str(path), base=LOAD)
    assert original.rd32(PREFERRED + 0x1001) == PREFERRED + 0x1080
    assert image.rd32(LOAD + 0x1001) == LOAD + 0x1080
    assert image.exec_ranges == [(LOAD + 0x1000, LOAD + 0x2000, ".text")]
    assert image.relocated_pointers() == {LOAD + 0x1080: LOAD + 0x1001}
    assert int.from_bytes(image.data[0x98 + 28:0x98 + 32], "little") == LOAD
    listed = T.parse_listing_text("10001000  MOV EAX,0x10001080\n10001005  CALL 0x10001080\n1000100a  RET\n")
    insns = image.rebase_listing(listed)
    assert [i.addr for i in insns] == [LOAD + 0x1000, LOAD + 0x1005, LOAD + 0x100a]
    assert insns[0].raw.endswith("MOV EAX,0x21080")
    assert insns[1].raw.endswith("CALL 0x21080")


@pytest.mark.parametrize("kind,page,block", [(1,0x1000,12), (3,0x2000,12), (3,0x1000,7), (3,0x1000,11), (3,0x1000,14)])
def test_bad_relocations_are_refused(tmp_path, kind, page, block):
    path = tmp_path / "bad.dll"
    path.write_bytes(pe_bytes(kind, page, block))
    with pytest.raises(T.TranslateError, match="relocation"):
        T.Image(str(path), base=LOAD)


def test_missing_relocations_are_refused_and_absolute_is_padding(tmp_path):
    path = tmp_path / "module.dll"
    data = bytearray(pe_bytes())
    struct.pack_into("<II", data, 0x98 + 96 + 5 * 8, 0, 0)
    path.write_bytes(data)
    with pytest.raises(T.TranslateError, match="relocation"):
        T.Image(str(path), base=LOAD)
    path.write_bytes(pe_bytes(kind=0))
    image = T.Image(str(path), base=LOAD)
    assert image.rd32(LOAD + 0x1001) == PREFERRED + 0x1080


def test_rebased_module_driver(tmp_path, monkeypatch):
    binary = tmp_path / "module.dll"
    binary.write_bytes(pe_bytes())
    listing = tmp_path / "functions"
    listing.mkdir()
    (listing / "10001000.asm").write_text("10001000  MOV EAX,0x10001080\n10001005  CALL 0x10001080\n1000100a  RET\n", encoding="utf-8")
    (listing / "10001080.asm").write_text("10001080  RET\n", encoding="utf-8")
    table = tmp_path / "functions.tsv"
    table.write_text("address\tname\tsize\n10001000\tentry\t11\n10001080\tcallee\t1\n", encoding="utf-8")
    for name, value in (("LISTINGS", str(listing)), ("FUNCS_TSV", str(table)),
                         ("BINARY", str(binary)), ("CURATED", None),
                         ("EXTRA_ENTRY_POINTS", frozenset()), ("SYMBOL_PREFIX", "recomp_aux_"),
                         ("AUX_MODULE", {"name":"aux.dll", "base":LOAD,"size":0x2000})):
        monkeypatch.setattr(T, name, value)
    monkeypatch.setattr(T, "configure", lambda cfg: None)
    monkeypatch.setattr(T.game_config, "load", lambda path: {})
    monkeypatch.setattr(sys, "argv", ["translate.py", "--game", str(tmp_path), "--out", str(tmp_path / "gen"), "--quiet"])
    assert T.main() == 0
    code = "\n".join(p.read_text(encoding="utf-8") for p in (tmp_path / "gen").glob("*.c"))
    assert "fn_00021000" in code and "fn_00021080" in code
    assert "0x21080u" in code
    assert "fn_10001000" not in code and "0x10001080u" not in code
