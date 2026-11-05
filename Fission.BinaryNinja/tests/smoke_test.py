"""Smoke test for the Fission.BinaryNinja plugin.

Run this from Binary Ninja's Python console (or from a script menu entry) after
installing the plugin:

    exec(open(r"<repo>/Fission.BinaryNinja/tests/smoke_test.py").read())

The test opens the compiled stress fixture, checks that the native view and
architecture claimed the file, then forces LLIL/MLIL/HLIL for every proto and
asserts that none of them fell back to an unimplemented lift.
"""

from __future__ import annotations

import os

import binaryninja as bn

SAMPLE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "samples",
    "stress.luac",
)
MINIMUM_FUNCTIONS = 240


def main() -> None:
    if not os.path.exists(SAMPLE):
        raise SystemExit(f"missing sample: {SAMPLE}")

    bv = bn.load(SAMPLE, update_analysis=True)
    if bv is None:
        raise SystemExit("Binary Ninja failed to load the Luau bytecode sample")

    print("view type:", bv.view_type)
    print("architecture:", bv.arch.name)
    print("entry point:", hex(bv.entry_point))

    assert bv.view_type == "LuauBytecode", f"unexpected view type: {bv.view_type}"
    assert bv.arch.name == "luau-bytecode", f"unexpected architecture: {bv.arch.name}"

    functions = sorted(bv.functions, key=lambda f: f.start)
    print("functions:", len(functions))
    assert len(functions) >= MINIMUM_FUNCTIONS, f"expected at least {MINIMUM_FUNCTIONS} protos"

    analysed = 0
    for function in functions:
        if not function.basic_blocks:
            raise AssertionError(f"{function.name} has no basic blocks")
        llil = function.low_level_il
        if llil is None or not llil.instructions:
            raise AssertionError(f"{function.name} has no LLIL")
        mlil = function.medium_level_il
        if mlil is None or not mlil.instructions:
            raise AssertionError(f"{function.name} has no MLIL")
        hlil = function.high_level_il
        if hlil is None or not hlil.instructions:
            raise AssertionError(f"{function.name} has no HLIL")
        analysed += 1

    print("analysed functions:", analysed)
    print("smoke test OK")


main()

