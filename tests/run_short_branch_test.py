#!/usr/bin/env python3
"""
run_short_branch_test.py - regression test for hand-written short branches
more than 256 bytes into a proc.

asm02 stores only the low byte of a short branch's target in a '<' fixup.
The target is on the page of the branch's own operand byte, so its high
byte is the operand's.  link02 once added the low byte alone to the
proc's base, as if every target were on the proc's first page, and so
rejected every good short branch past the first 256 bytes of a proc,
with or without -r.

Checks, each without and with -r:

  1. A page-aligned proc with short branches past its first page links,
     and the branches point at their targets.
  2. A proc that is not page-aligned, placed so that its branches stay
     on one page, links, with the targets relocated.
  3. A proc placed so that a branch and its target end up on different
     pages is still rejected.
  4. With -r to both tools, a long branch before a short branch's target
     is shrunk, which moves the target, and the short branch follows it.

Exits 0 and prints "ALL CHECKS PASSED" on success; exits 1 with a
descriptive message on any failure.

Usage: python3 tests/run_short_branch_test.py [asm02] [link02]
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
    ok = "Short branch out of page" not in out and os.path.exists(hexfile)
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


def short_target(image, operand):
    """the target of the short branch whose operand byte is at operand"""
    return (operand & 0xff00) | image[operand]


# A proc whose code starts past its first page: a loop of short branches.
BIG = """
{align}
            proc  {name}
            ds    300
            public {name}_loop
{name}_loop: ldi  0
            bz    {name}_done
            br    {name}_loop
            public {name}_done
{name}_done: sep  r5
            endp
"""

# A proc of n bytes, to put the next proc where it is wanted.
FILLER = """
            proc  filler
            ds    {n}
            sep   r5
            endp
"""


def big(name, aligned):
    return BIG.format(name=name, align=".link .align page" if aligned else "")


def branches_ok(image, syms, name):
    """both short branches of BIG point at their labels"""
    loop = syms[name + "_loop"]
    done = syms[name + "_done"]
    bz = loop + 2          # ldi 0 is two bytes, then bz's operand at +1
    br = loop + 4
    return (short_target(image, bz + 1) == done and
            short_target(image, br + 1) == loop)


with tempfile.TemporaryDirectory() as tmp:
    for relax in (False, True):
        mode = "with -r" if relax else "without -r"

        # 1. page-aligned, branches past the first page
        objs = [assemble(tmp, "aligned", big("al", True))]
        ok, out, image = link(tmp, objs, relax)
        syms = symbols(out)
        check(ok, "aligned proc with short branches past its first page links, " + mode)
        if ok:
            check(branches_ok(image, syms, "al"),
                  "aligned proc's short branches point at their targets, " + mode)

        # 2. not aligned, placed so that the branches stay on one page:
        #    a 16-byte filler puts the loop at 0x13c, its targets at 0x13c/0x142
        objs = [assemble(tmp, "fill16", FILLER.format(n=15)),
                assemble(tmp, "unaligned", big("un", False))]
        ok, out, image = link(tmp, objs, relax)
        syms = symbols(out)
        check(ok, "unaligned proc whose branches stay on one page links, " + mode)
        if ok:
            check(syms["un"] % 256 != 0, "  (and the proc is really not page-aligned)")
            check(branches_ok(image, syms, "un"),
                  "unaligned proc's short branches are relocated to their targets, " + mode)

        # 3. not aligned, placed so that a branch's operand is on one page
        #    and its target on the next: 0xd0 bytes put the loop at 0x1fc,
        #    bz's operand at 0x1ff and done at 0x202
        objs = [assemble(tmp, "fillcross", FILLER.format(n=0xcf)),
                assemble(tmp, "cross", big("cr", False))]
        ok, out, image = link(tmp, objs, relax)
        check(not ok, "a short branch whose target lands on another page is rejected, " + mode)

    # 4. -r shrinks a long branch before the target, which moves it
    objs = [assemble(tmp, "shrink", """
.link .align page
            proc  sh
            ds    300
            public sh_loop
sh_loop:    ldi   0
            bz    sh_done
            lbr   sh_skip
sh_skip:    nop
            public sh_done
sh_done:    br    sh_loop
            endp
""", relax=True)]
    ok, out, image = link(tmp, objs, True)
    syms = symbols(out)
    check(ok, "short branch past the first page links when -r shrinks a branch before its target")
    if ok:
        loop, done = syms["sh_loop"], syms["sh_done"]
        check(image[loop + 4] == 0x30,
              "  the long branch was shrunk to a short one (BR)")
        check(done == loop + 7, "  so the target moved down by one byte")
        check(short_target(image, loop + 3) == done,
              "  and the short branch points at the target's new address")
        check(short_target(image, done + 1) == loop,
              "  and the branch back points at the loop")

if failures:
    print("%d CHECK(S) FAILED" % failures)
    sys.exit(1)
print("ALL CHECKS PASSED")
