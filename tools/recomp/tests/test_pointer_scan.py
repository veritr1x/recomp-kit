"""Instruction encodings cannot manufacture data-pointer entry candidates."""
import pytest
from test_translate_driver import synthetic_image


@pytest.mark.parametrize("opcode", [b"\xe8", b"\xe9", b"\x0f\x84", b"\x0f\x8f", b"\xf3\xe8"])
@pytest.mark.parametrize("known_target", [False, True])
def test_relative_displacement_is_not_a_pointer(opcode, known_target):
    holder, target = 0x401100, 0x401800
    image = synthetic_image({holder: opcode + target.to_bytes(4,"little") + b"\xc3",
                             target-1: b"\x90\x55\x89\xe5\xc3"})
    covered = {holder, target} if known_target else {holder}
    starts, interior = image.code_pointers(covered)
    assert target not in starts | interior


def test_opcode_bytes_are_not_a_pointer():
    holder, target = 0x401100, 0x4018e8
    image = synthetic_image({holder: target.to_bytes(4,"little") + b"\x00\xc3",
                             target-1: b"\x90\x55\x89\xe5\xc3"})
    starts, interior = image.code_pointers({holder, target})
    assert target not in starts | interior


@pytest.mark.parametrize("prefix", [b"\x68", b"\xb8", b"\x8d\x05"])
def test_absolute_code_immediates_remain_pointers(prefix):
    holder, target = 0x401100, 0x401800
    image = synthetic_image({holder: prefix + target.to_bytes(4,"little") + b"\xc3",
                             target-1: b"\x90\x55\x89\xe5\xc3"})
    starts, interior = image.code_pointers({holder, target})
    assert target in interior


def test_data_pointer_to_a_known_boundary_remains_valid():
    holder, target = 0x401100, 0x401800
    image = synthetic_image({holder: b"\xe8" + target.to_bytes(4,"little"),
                             target-1: b"\x90\x55\x89\xe5\xc3",
                             holder+32: target.to_bytes(4,"little")})
    starts, interior = image.code_pointers({holder, target})
    assert target in interior
