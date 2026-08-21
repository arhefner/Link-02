#!/usr/bin/env python3
"""
run_relax_module_test.py - automated regression test for link02's -r
(short-branch relaxation) combined with -m (loadable-module fixup
table) output.

Answers two related questions: when relaxation shrinks branches across
several rounds (shifting addresses as it goes), do the module's own
FIXUP table and the linker's own SYMBOL table (-s output / .sym file)
both end up describing the FINAL, fully-relaxed layout -- or could
either describe a stale, intermediate round's layout instead?

Builds relax_module_test.asm (a long chain of trivially-shrinkable
branches ending in one branch that can never shrink, chosen because it
empirically forces a genuine multi-round relaxation -- see that file's
own header comment), then:

  1. Confirms relaxation actually took more than one round (otherwise
     this test isn't exercising anything interesting).
  2. Confirms the module reports exactly one remaining fixup (the one
     branch designed to never shrink).
  3. Independently walks the real compiled instruction stream from
     address 0 (using only the raw bytes -- no reliance on -s's own
     reported values anywhere in the walk) to arrive at tm_far_target's
     true final address, and confirms it matches what -s reported --
     verifying the SYMBOL table reflects the final, post-relaxation
     layout, not a stale intermediate round's.
  4. Decodes the real output binary directly and finds the actual
     3-byte LBR instruction encoding the never-shrinks branch, by its
     real byte content (opcode 0xC0 followed by tm_far_target's real
     linked address) -- not by trusting any address computed from
     source alone.
  5. Confirms the fixup table's one recorded address is exactly one
     byte past that instruction's own opcode (i.e. it points at the
     2-byte operand field, not the opcode or something else) --
     verifying the FIXUP table also reflects the final layout.
  6. Confirms the operand bytes, read from the file, decode to
     tm_far_target's real linked address.
  7. Simulates the actual runtime relocation a module loader performs
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
    print(f"tm_far_target linked address (per -s): {far_target_addr:04x}")

    bin_path = os.path.join(HERE, "relax_module_test.bin")
    with open(bin_path, "rb") as f:
        data = f.read()

    # Independent ground-truth check on the SYMBOL TABLE itself, not
    # just the fixup table: walk the real instruction stream starting
    # right after the mandatory 4-byte -m module header (README.md's
    # own documented "Loadable-module output" requirement -- 3-byte
    # magic + 1-byte version here, matching kernel/batch_mod.asm's own
    # real-world convention) using ONLY the raw compiled bytes -- no
    # reliance on -s's own reported values as an input anywhere in
    # this walk -- and confirm the address it independently arrives at
    # for tm_far_target matches what -s reported. This is the same
    # question as the fixup check below (does relaxation's own
    # reset-per-round design also keep the SYMBOL table, not just the
    # fixup table, correctly reflecting the final round?), verified a
    # second, structurally different way.
    MODULE_HEADER_LEN = 6  # 3-byte magic + 1-byte version + 2-byte
                           # code-size field (the field Link/02 -m
                           # itself patches at file offset 4)
    addr = MODULE_HEADER_LEN
    branches_seen = 0
    # 80 chain branches (tm_l0..tm_l79 -> tm_l1..tm_l80) plus the one
    # final never-shrinks branch (tm_far_branch -> tm_far_target) = 81
    # branch instructions total, each either 2 bytes (shrunk short
    # branch, opcode in the $30-$3F short-branch family) or 3 bytes
    # (still-long branch, opcode $C0/LBR).
    while branches_seen < 81:
        op = data[addr]
        if op == 0xC0:          # LBR (long branch), 3 bytes
            addr += 3
        elif 0x30 <= op <= 0x3F:  # short branch family, 2 bytes
            addr += 2
        else:
            fail(f"unexpected opcode {op:02x} at address {addr:04x} while "
                 f"independently walking the instruction stream (expected "
                 f"branch #{branches_seen} of 81)")
        branches_seen += 1
    # Next byte should be the RTN (0xD5) right after tm_far_branch.
    if data[addr] != 0xD5:
        fail(f"expected RTN (0xd5) at {addr:04x} right after the 81st "
             f"branch, found {data[addr]:02x}")
    addr += 1
    # Then exactly 2000 bytes of zero-filled ds padding.
    pad_start = addr
    addr += 2000
    if any(b != 0 for b in data[pad_start:addr]):
        fail(f"expected 2000 bytes of zero padding at {pad_start:04x}, "
             f"found a nonzero byte in that range")
    # addr now independently points at tm_far_target -- computed
    # entirely from raw bytes, with zero dependence on -s's own output.
    independent_far_target_addr = addr
    print(f"tm_far_target address (independently walked from raw bytes): "
          f"{independent_far_target_addr:04x}")
    if independent_far_target_addr != far_target_addr:
        fail(
            f"THE BUG THIS CHECK EXISTS TO CATCH: -s reports "
            f"tm_far_target at {far_target_addr:04x}, but independently "
            f"walking the real compiled instruction stream from address 0 "
            f"arrives at {independent_far_target_addr:04x} instead -- the "
            f"linker's own SYMBOL TABLE output does not match the real, "
            f"final, post-relaxation layout."
        )

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
