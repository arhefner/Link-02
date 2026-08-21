#!/usr/bin/env python3
"""
run_relax_module_test.py - automated regression test for link02's -r
(short-branch relaxation) combined with -m (loadable-module fixup
table) output.

Answers a specific question: when relaxation shrinks branches across
several rounds (shifting addresses as it goes), does the module's own
fixup table end up describing the FINAL, fully-relaxed layout, or
could it describe a stale, intermediate round's layout instead?

Builds relax_module_test.asm (a long chain of trivially-shrinkable
branches ending in one branch that can never shrink, chosen because it
empirically forces a genuine multi-round relaxation -- see that file's
own header comment), then:

  1. Confirms relaxation actually took more than one round (otherwise
     this test isn't exercising anything interesting).
  2. Confirms the module reports exactly one remaining fixup (the one
     branch designed to never shrink).
  3. Decodes the real output binary directly and finds the actual
     3-byte LBR instruction encoding the never-shrinks branch, by its
     real byte content (opcode 0xC0 followed by tm_far_target's real
     linked address) -- not by trusting any address computed from
     source alone.
  4. Confirms the fixup table's one recorded address is exactly one
     byte past that instruction's own opcode (i.e. it points at the
     2-byte operand field, not the opcode or something else).
  5. Confirms the operand bytes, read from the file, decode to
     tm_far_target's real linked address.
  6. Simulates the actual runtime relocation a module loader performs
     (add an arbitrary nonzero load base to the fixup site) and
     confirms the result is the correct relocated branch target.

Exits 0 and prints "ALL CHECKS PASSED" on success; exits 1 with a
descriptive message on any failure.

Usage: python3 tests/run_relax_module_test.py [asm02] [link02]
  (defaults to whatever asm02/link02 are found on PATH)
"""
import subprocess
import sys
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ASM = sys.argv[1] if len(sys.argv) > 1 else "asm02"
LINK = sys.argv[2] if len(sys.argv) > 2 else "link02"


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    asm_src = os.path.join(HERE, "relax_module_test.asm")
    if not os.path.isfile(asm_src):
        fail(f"test source not found: {asm_src}")

    rc, out = run([ASM, "-L", "-C", "-r", "relax_module_test.asm"], cwd=HERE)
    print(out)
    if rc != 0 or "Errors            : 0" not in out:
        fail("asm02 did not assemble cleanly")

    rc, out = run(
        [LINK, "-b", "-be", "-r", "-m", "-o", "relax_module_test.bin",
         "-s", "relax_module_test.prg"],
        cwd=HERE,
    )
    print(out)
    if rc != 0:
        fail("link02 did not link cleanly")

    m = re.search(r"Relaxation: (\d+) of (\d+) local long branches shortened \((\d+) rounds\)", out)
    if not m:
        fail("could not find the relaxation summary line in link02's output")
    shrunk, total, rounds = int(m.group(1)), int(m.group(2)), int(m.group(3))
    print(f"Relaxation summary: {shrunk}/{total} shrunk, {rounds} round(s)")
    if rounds < 2:
        fail(
            f"only {rounds} round(s) of relaxation occurred -- this test is "
            "supposed to force a genuine multi-round convergence (a real "
            "exclude-then-retry), and with only one round it isn't actually "
            "exercising the scenario this test exists to check. The branch "
            "chain length in relax_module_test.asm may need adjusting."
        )

    m = re.search(r"Module fixups\s*:\s*(\d+)", out)
    if not m:
        fail("could not find the module fixup count in link02's output")
    num_fixups = int(m.group(1))
    if num_fixups != 1:
        fail(f"expected exactly 1 remaining module fixup, got {num_fixups}")

    sym = {}
    for line in out.splitlines():
        m = re.match(r"^(\S+)\s+([0-9a-fA-F]{4})\s*$", line)
        if m:
            sym[m.group(1)] = int(m.group(2), 16)
    if "tm_far_target" not in sym:
        fail("tm_far_target's linked address did not appear in -s output")
    far_target_addr = sym["tm_far_target"]
    print(f"tm_far_target linked address: {far_target_addr:04x}")

    bin_path = os.path.join(HERE, "relax_module_test.bin")
    with open(bin_path, "rb") as f:
        data = f.read()

    # The fixup table is the last (2 + numFixups*2) bytes: a 2-byte
    # count (which we already have from the printed summary) followed
    # by that many 2-byte big-endian addresses.
    table = data[-(2 + num_fixups * 2):]
    table_count = (table[0] << 8) | table[1]
    if table_count != num_fixups:
        fail(f"fixup table's own embedded count ({table_count}) doesn't match "
             f"the printed summary ({num_fixups})")
    fixup_addr = (table[2] << 8) | table[3]
    print(f"Fixup table entry: {fixup_addr:04x}")

    # Find the real, final LBR instruction encoding (opcode 0xC0
    # followed by tm_far_target's real address, big-endian) directly
    # in the compiled bytes -- not computed from source, so this step
    # can't be fooled by a source-level assumption that doesn't match
    # what actually got emitted.
    needle = bytes([0xC0, (far_target_addr >> 8) & 0xff, far_target_addr & 0xff])
    idx = data.find(needle)
    if idx < 0:
        fail("could not find the never-shrinks LBR instruction's real bytes "
             "(opcode 0xC0 + tm_far_target's address) anywhere in the "
             "compiled module")
    second_idx = data.find(needle, idx + 1)
    if second_idx >= 0:
        fail("found the LBR encoding at more than one file offset -- "
             "ambiguous, can't verify which one the fixup table should "
             "point at")

    # This module has no header prepended (a bare -r -m link with no
    # trampoline proc), so file offset == link-time address directly.
    real_instr_addr = idx
    real_operand_addr = real_instr_addr + 1
    print(f"Real instruction address (from compiled bytes): {real_instr_addr:04x}")
    print(f"Real operand address (opcode + 1):               {real_operand_addr:04x}")

    if fixup_addr != real_operand_addr:
        fail(
            f"THE BUG THIS TEST EXISTS TO CATCH: the module's fixup table "
            f"says the relocatable operand is at {fixup_addr:04x}, but the "
            f"instruction's REAL, FINAL location (found directly in the "
            f"compiled bytes) puts its operand at {real_operand_addr:04x}. "
            f"The fixup table does not match the final, post-relaxation "
            f"layout -- a runtime module loader would patch the WRONG "
            f"location."
        )

    stored_val = (data[fixup_addr] << 8) | data[fixup_addr + 1]
    if stored_val != far_target_addr:
        fail(
            f"the value stored at the fixup address ({stored_val:04x}) does "
            f"not match tm_far_target's real linked address "
            f"({far_target_addr:04x})"
        )

    # End-to-end simulation of what a module loader actually does at
    # runtime: add its own chosen load base to every fixup site.
    base = 0xD000
    relocated = (stored_val + base) & 0xffff
    expected = (far_target_addr + base) & 0xffff
    if relocated != expected:
        fail(f"simulated relocation with base {base:04x} produced "
             f"{relocated:04x}, expected {expected:04x}")
    print(f"Simulated relocation (base={base:04x}): {stored_val:04x} -> {relocated:04x}")

    print()
    print("ALL CHECKS PASSED")
    print(f"  - Relaxation took {rounds} rounds (genuine multi-round convergence)")
    print(f"  - Fixup table correctly reflects the FINAL, post-relaxation layout")
    print(f"  - Simulated runtime relocation produces the correct branch target")


if __name__ == "__main__":
    main()
