#!/usr/bin/env python3
"""
run_low_byte_test.py - regression test for the low byte of a local
address ('v' records) with -r.

asm02 writes "ldi label.1" as a '^' record, which carries the whole
target, and "ldi label.0" as a 'v' record, which carries only its low
byte.  When -r regenerates a proc, a 'v' needs the whole target to find
where it moved to.  relax.c once took it from the '^' next to the 'v' in
the list of fixups, whether or not that '^' was for the same label, so
in a proc such as

    ldi   vars.1
    phi   r8
    ldi   v_b.0

the low byte of v_b became the low byte of vars.  cts.lib of Elf-speech
has many of these, and ElfC -O, which links with -r, broke it.

Checks, each without and with -r:

  1. A lone "ldi x.0" next to a "ldi y.1" of another label, in a proc of
     more than 256 bytes, gets the low byte of x.
  2. A lone "ldi x.0" in a proc of one page gets the low byte of x where
     x moved to, after a long branch before it was shortened.
  3. A "ldi x.1 / ldi x.0" pair in a proc of more than 256 bytes, where
     a shortened branch moves x, gets both bytes of x.
  4. A lone "ldi x.0" in a proc of more than 256 bytes, where a shortened
     branch would move x by a different amount than an address 256 bytes
     away, so that its low byte can not be worked out: the branches of
     the proc are left long, and the low byte is right.

Exits 0 and prints "ALL CHECKS PASSED" on success; exits 1 with a
descriptive message on any failure.

Usage: python3 tests/run_low_byte_test.py [asm02] [link02]
  (defaults to whatever asm02/link02 are found on PATH)
"""
import os
import subprocess
import sys
import tempfile

ASM = sys.argv[1] if len(sys.argv) > 1 else "asm02"
LINK = sys.argv[2] if len(sys.argv) > 2 else "link02"

failures = 0


def check(cond, msg):
    global failures
    print(("ok    " if cond else "FAIL  ") + msg)
    if not cond:
        failures += 1


def assemble(tmp, name, source, relax=False):
    with open(os.path.join(tmp, name + ".asm"), "w") as f:
        f.write(source)
    flags = ["-q", "-L"] + (["-r"] if relax else [])
    r = subprocess.run([ASM] + flags + [name + ".asm"], cwd=tmp,
                       capture_output=True, text=True)
    if r.returncode != 0 or "ERROR" in r.stdout:
        print(r.stdout + r.stderr)
        sys.exit("FAIL: could not assemble %s" % name)
    return name + ".prg"


def link(tmp, objs, relax):
    """link with -i -s, return (ok, output text, 64K image)"""
    args = [LINK, "-i", "-s", "-o", "out.hex"] + (["-r"] if relax else []) + objs
    hexfile = os.path.join(tmp, "out.hex")
    if os.path.exists(hexfile):
        os.remove(hexfile)
    r = subprocess.run(args, cwd=tmp, capture_output=True, text=True)
    out = r.stdout + r.stderr
    ok = "Error" not in out and os.path.exists(hexfile)
    image = bytearray(65536)
    if ok:
        for line in open(hexfile):
            line = line.strip()
            if line.startswith(":") and line[7:9] == "00":
                n, a = int(line[1:3], 16), int(line[3:7], 16)
                image[a:a + n] = bytes.fromhex(line[9:9 + 2 * n])
    return ok, out, image


def symbols(out):
    """the public symbols and procs -s printed, name -> address"""
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2:
            try:
                syms[parts[0].lower()] = int(parts[1], 16)
            except ValueError:
                pass
    return syms


# 1. as cts.lib: a page of data, the variables on the next page, and code
#    that points R8 at the page of the variables and then at one of them
LONE_NEXT_TO_OTHER = """
.link .align page
            proc  vt
ring:       ds    256
            public vars
vars:       db    1
            db    2
            public v_b
v_b:        db    3
            public vt_code
vt_code:    ldi   vars.1
            phi   r8
            ldi   v_b.0
            plo   r8
            lbr   vt_end
            nop
vt_end:     sep   r5
            endp
"""

# 2. a proc of one page: the long branch before x is shortened, so x moves
LONE_ONE_PAGE = """
            proc  op
            public op_code
op_code:    lbr   op_x
            nop
            public op_x
op_x:       ldi   op_x.0
            sep   r5
            endp
"""

# 3. a pair of the same label, x moved by a shortened branch before it,
#    in a proc of more than 256 bytes
PAIR = """
.link .align page
            proc  pr
            ds    300
            public pr_code
pr_code:    lbr   pr_x
            nop
            public pr_x
pr_x:       ldi   pr_x.1
            phi   rf
            ldi   pr_x.0
            plo   rf
            sep   r5
            endp
"""

# 4. a lone low byte of x on the second page, with a shortened branch
#    before x but not before the address 256 bytes below it
AMBIGUOUS = """
.link .align page
            proc  am
            ds    200
            public am_code
am_code:    lbr   am_y
            nop
am_y:       sep   r5
            ds    150
            public am_x
am_x:       ldi   am_x.0
            sep   r5
            endp
"""


with tempfile.TemporaryDirectory() as tmp:
    for relax in (False, True):
        mode = "with -r" if relax else "without -r"

        # 1.
        objs = [assemble(tmp, "lone", LONE_NEXT_TO_OTHER, relax)]
        ok, out, image = link(tmp, objs, relax)
        syms = symbols(out)
        check(ok, "a proc with a lone low byte next to another label's high byte links, " + mode)
        if ok:
            code = syms["vt_code"]
            check(image[code + 1] == syms["vars"] >> 8,
                  "  ldi vars.1 is the high byte of vars, " + mode)
            check(image[code + 4] == syms["v_b"] & 0xff,
                  "  ldi v_b.0 is the low byte of v_b, not of vars, " + mode)

        # 2.
        objs = [assemble(tmp, "onepage", LONE_ONE_PAGE, relax)]
        ok, out, image = link(tmp, objs, relax)
        syms = symbols(out)
        check(ok, "a proc of one page with a lone low byte links, " + mode)
        if ok:
            x = syms["op_x"]
            if relax:
                check(x == syms["op_code"] + 3,
                      "  the branch before op_x was shortened, so op_x moved, " + mode)
            check(image[x + 1] == x & 0xff,
                  "  ldi op_x.0 is the low byte of op_x where it is, " + mode)

        # 3.
        objs = [assemble(tmp, "pair", PAIR, relax)]
        ok, out, image = link(tmp, objs, relax)
        syms = symbols(out)
        check(ok, "a proc with a high and low byte pair links, " + mode)
        if ok:
            x = syms["pr_x"]
            if relax:
                check(x == syms["pr_code"] + 3,
                      "  the branch before pr_x was shortened, so pr_x moved, " + mode)
            check(image[x + 1] == x >> 8 and image[x + 4] == x & 0xff,
                  "  ldi pr_x.1 and ldi pr_x.0 are the address of pr_x, " + mode)

        # 4.
        objs = [assemble(tmp, "ambiguous", AMBIGUOUS, relax)]
        ok, out, image = link(tmp, objs, relax)
        syms = symbols(out)
        check(ok, "a proc with a lone low byte that can not be placed links, " + mode)
        if ok:
            x = syms["am_x"]
            check(image[x + 1] == x & 0xff,
                  "  ldi am_x.0 is the low byte of am_x, " + mode)
            if relax:
                check(image[syms["am_code"]] == 0xc0,
                      "  and the branch of the proc was left long, " + mode)
                check("left long" in out,
                      "  and link02 says why, " + mode)

if failures:
    print("%d CHECK(S) FAILED" % failures)
    sys.exit(1)
print("ALL CHECKS PASSED")
