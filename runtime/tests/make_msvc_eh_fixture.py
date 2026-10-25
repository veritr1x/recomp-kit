"""Emit a tiny synthetic VC8 function with the production translator."""
from pathlib import Path
import struct
import sys
from types import SimpleNamespace

import capstone

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "recomp"))
import translate as T

BASE = 0x0D02A000
STUB, CATCH, CLEANUP, LANDING = (BASE + n for n in (0x180, 0x200, 0x240, 0x60))
INFO, UNWIND, TRY, HANDLERS, SLOT, THROW, OBJECT, THROW_INFO = (
    BASE + n for n in (0x1000, 0x1040, 0x1060, 0x1080, 0x1100, 0x1104, 0x1120, 0x1124))


def generate(path):
    image = T.Image.__new__(T.Image)
    image.base, image.size, image.end = BASE, 0x1200, BASE + 0x1200
    data = bytearray(image.size)
    def put(address, raw):
        data[address - BASE:address - BASE + len(raw)] = raw
    head = bytes.fromhex("558bec6aff68") + struct.pack("<I", STUB)
    head += bytes.fromhex("64a1000000005083ec408d45f464a300000000c745fc01000000")
    head += b"\xff\x35" + struct.pack("<I", THROW_INFO) + b"\xff\x35" + struct.pack("<I", OBJECT)
    head += b"\xff\x15" + struct.pack("<I", THROW) + b"\xcc"
    tail = bytes.fromhex("8b4df464890d000000008be55db84d000000c3")
    put(BASE, head); put(LANDING, tail)
    put(STUB, b"\xb8" + struct.pack("<I", INFO) + b"\xff\x25" + struct.pack("<I", SLOT))
    put(CATCH, b"\xb8" + struct.pack("<I", LANDING) + b"\xc3")
    put(CLEANUP, b"\xc3")
    put(INFO, struct.pack("<9I", 0x19930522, 2, UNWIND, 1, TRY, 0, 0, 0, 0))
    put(UNWIND, struct.pack("<4I", 0xffffffff, CLEANUP, 0, CLEANUP))
    put(TRY, struct.pack("<5I", 0, 1, 1, 1, HANDLERS))
    put(HANDLERS, struct.pack("<4I", 0, 0, 0, CATCH))
    image.data = bytes(data)
    image.exec_ranges, image.data_ranges = [(BASE, BASE + 0x800, ".text")], [(INFO, image.end, ".rdata")]
    image.iat_names = {SLOT: "__CxxFrameHandler3", THROW: "_CxxThrowException"}
    image.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    image.recover_errors = []
    image.noreturn_callees = set()
    image.md.detail = True
    instructions = [image.to_insn(ci) for address, raw in ((BASE, head), (LANDING, tail))
                    for ci in image.md.disasm(raw, address)]
    image.md.detail = False
    fn = T.Function(BASE, "synthetic_msvc_catch", LANDING + len(tail) - BASE, instructions)
    fn.measure(image)
    translator = T.Translator(image, {BASE}, SimpleNamespace(eager_flags=False))
    translator.prepare(fn)
    source = ['#include "x86.h"', '#include "msvc_eh.h"', '#include <string.h>']
    source += translator.translate(fn)
    source += ["void msvc_fixture_run(X86 *c) { fn_%08x(c); }" % BASE,
               "void msvc_fixture_load(void) {",
               "static const unsigned char image[] = {%s};" % ",".join(str(b) for b in data),
               "memcpy(g_mem + 0x%xu, image, sizeof image);" % BASE, "}"]
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text("\n".join(source) + "\n", encoding="utf-8")


if __name__ == "__main__":
    generate(sys.argv[1])
