# Link/02 - General linker for series/02 development tools

This is an update to Mike Riley's LINK/02 relocating linker. It is a relocating linker for objects generated from his Series/X compilers and assemblers, such as Asm/02.

The object format is simple ascii, making it easy for other tool developers to generate object files that can be linked using this linker.

## Original Repo
https://github.com/rileym65/Link-02

## Command Line
```
link [options] obj obj obj ...
link @controlfile
```

## Command Line Switches
```
-b            - Output straight binary
-c            - Output in .cmd format
-e            - Output Elf/OS executable
-i            - Output Intel hex
-I path       - Add path to search list for object files
-h            - Output RCS hex
-q            - Show minimal output
-r            - Enable short-branch relaxation (see below)
-s            - Show public symbols
-S            - Create .sym file for public symbols
-o name       - Specify output filename
-l name       - Specify library to search
-L path       - Add path to search lift for library files
-m            - Output loadable-module format (see below)
-be           - Use big-endian mode
-le           - Use little-endian mode
-v            - Display linker version number
```

## Control file
```
add filename         - add specified object file
library filename     - add library to search
mode big             - set big-endian mode
mode little          - set little-endian mode
mode binary          - set binary output mode
mode cmd             - set .cmd output mode
mode elfos           - set Elf/OS output mode
mode intel           - set Intel hex output mode
mode rcs             - set RCS hex output mode
output filename      - filename for output
```

## Object file structure
```
.big                 - Use big-endian mode
.little              - Use little-endian mode
.requires module     - Requre module to be included
.library filename    - Search specified library for references
.align word          - Align procedure on word boundary (2 bytes)
.align dword         - Align procedure on double word boundary (4 bytes)
.align qword         - Align procedure on quad word boundary (8 bytes)
.align para          - Align procedure on paragraph boundary (16 bytes)
.align 32            - Align procedure on 32-byte boundary
.align 64            - Align procedure on 64-byte boundary
.align 128           - Align procedure on 128-byte boundary
.align page          - Align procedure on page boundary (256 bytes)
:addr bb bb bb ...   - Program bytes
@addr                - Start address
?name addr           - External reference used at addr
=name value          - Public symbol
{name                - Begin relocatable procedure
}                    - End relocatable procedure
/name addr [lofs]    - Add byte at addr with high of name
\name addr           - Add byte at addr with low of name
+addr                - Apply module offset at addr
^addr                - Apply high byte of module offset at addr
vaddr                - Apply low byte of module offset at addr
>inc                 - Increase current address by 'inc'
<addr target         - Apply low byte of module offset at addr
                       Test for short branch out of page
#addr                - Local long-branch operand (same-proc target).
                       Resolved exactly like '+' unless -r is given, in
                       which case it's also a candidate to be shrunk to
                       a 2-byte short branch (opcode -0x90) if the
                       target ends up on the same page. Emitted by an
                       assembler (in place of '+') for a long branch
                       whose target is local to the same procedure.
!name addr           - External long-branch operand. Resolved exactly
                       like '?'; never a candidate for shrinking, since
                       an external target's final address isn't known
                       until the symbol resolves, well after this
                       procedure's own layout must already be final.
```

## Short-branch relaxation (-r)

With `-r`, Link/02 makes repeated internal load/link passes. Each pass
regenerates every procedure's object text from scratch for a given
"must stay long" exclusion set: `#`-tagged branches not in that set get
shortened to their 2-byte form, and every offset in the procedure is
renumbered against the new, smaller size before anything is resolved.
The rewritten text is fed through the ordinary, unmodified object-
loading and linking pipeline. If a shrunk branch turns out not to fit
after all, it's added to the exclusion set and every procedure is
regenerated fresh (not patched in place) -- repeating until a pass
produces zero such failures. `!` branches (external targets) are never
touched, only `#` (local targets), since an external target's page
isn't known until well after this procedure's own layout must be
final.

Only procedures (`{name` ... `}`) are affected -- ORG-mode code always
keeps its original branch encoding. A procedure that needs a hand-
written short branch to always land on a known page can force that
with `.align page` immediately *before* its `{name` record (alignment
appearing anywhere else in the middle of a procedure does not reserve
the padding an assembler's own byte-offset bookkeeping already
assumed, and silently produces wrong addresses).

Environment variables (debugging/tuning aids, none required for normal
use):
```
RLX_BATCH_EXCLUDE  - Any value: on a failed pass, exclude every branch
                     that failed at once instead of the default
                     (exclude only the first one and re-evaluate the
                     rest fresh next pass). Converges in far fewer
                     passes but can leave a few more branches long
                     than necessary.
RLX_MAX_SHRINK n   - Stop shrinking additional branches once n have
                     been shrunk in a pass. Useful for isolating a
                     suspected relaxation bug to a smaller repro.
RLX_KEEP_TEMP      - Any value: keep the intermediate, regenerated
                     per-pass object files (normally deleted) at
                     /tmp/relax_round<N>_<obj-index>.prg.
```

## Loadable-module output (-m)

`-m` produces a relocatable module image intended to be loaded at
runtime to an address not known until load time, then executed from
there. It only has any effect combined with `-b` (straight binary
output) -- `-m` with any other output mode is silently ignored.

The output file layout is:

```
offset 0            - module content begins here (reserve at least
                       4 bytes at the front for this header -- see
                       below); the module's own code/data follow
offset 4            - 2 bytes, big-endian: total code+data size
                       (highest linked address - lowest + 1)
<end of content>    - 2 bytes, big-endian: number of fixups (N)
                     - N * 2 bytes, big-endian: fixup table, one
                       entry per high-byte offset needing relocation
```

The size field at a fixed offset (4) exists because the module's own
total size is self-referential -- nothing in the source can know its
own final linked size ahead of time, so Link/02 patches it in after
linking completes. Any module format that opts into `-m` must reserve
these 2 bytes at that exact offset for this purpose (bytes 0-3 are
otherwise unconstrained by Link/02 itself -- typically a magic number,
version byte, or entry-point jump, whatever the module format's own
loader expects there).

The fixup table records every *high byte* of a full address the
linker resolved during this build (from `#`, `+`, and `^` fixups --
low-byte-only fixups like `v` and short-branch (`<`) targets never
carry an absolute address, so they're never recorded). This assumes
the loader always places the module at a page-aligned load address
(the load base's own low byte is 0) -- under that assumption, adding
the load base to any embedded address only ever changes its high byte,
so recording just the offset of each high byte is enough for the
loader to fully relocate the module: for each listed offset, add the
load base's high byte to whatever byte is already there. Link/02
itself doesn't enforce or depend on the page-alignment assumption
beyond emitting this table -- honoring it is entirely the loader's own
responsibility.

See `tutorial.d` for a full worked example.
