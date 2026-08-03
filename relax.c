/*
 * relax.c - branch-relaxation (long -> short branch) support for Link/02.
 *
 * Design: a rewrite-and-relink pipeline, not in-place patching. An earlier
 * patch-based approach was replaced after two separate classes of "compute
 * a value early, forget to update it when a later shrink shifts it" bugs.
 *
 *   1. Parse each object file's text into a sequence of segments: raw
 *      passthrough lines (anything outside a proc) and fully-parsed procs
 *      (a byte buffer + an ordered fixup list) for anything inside a
 *      {NAME ... } block.
 *   2. Given a set of "excluded" (must-stay-long) branches, regenerate
 *      every proc's text from scratch: candidate '#' branches not in the
 *      exclusion set are shrunk (opcode -0x90, two operand bytes collapse
 *      to one, marker becomes '<'), and EVERY offset appearing anywhere in
 *      the proc -- byte positions and every fixup's own patch offset and
 *      (for local fixup types) stored target -- is renumbered to reflect
 *      the proc's new, smaller size. This is a static, one-shot text
 *      transform: nothing is computed before the proc's final layout is
 *      known, which is what eliminates the whole "goes stale after a later
 *      shift" bug class.
 *   3. Write the regenerated text to temp files and run it through the
 *      completely unmodified loadFile()/doLink() pipeline, exactly as if
 *      it were the original input. Cross-proc placement and external
 *      symbol resolution never need special-casing, because every proc's
 *      internal layout is already final by the time linking starts.
 *   4. If any shrunk branch comes back "Short branch out of page" (the
 *      pre-existing '<' check in loadFile(), unmodified), that branch is
 *      added to the exclusion set and the whole thing -- regenerate all
 *      files, reset all link state, relink -- runs again. Iterate until a
 *      round produces zero such failures.
 *
 * Only '#' (local, intra-proc) branch candidates are ever shrunk. '!'
 * (external-target long branches, tagged by Asm/02's OT_LBR case the same
 * way) are always left long -- shrinking a branch to an external symbol
 * would need a genuinely different resolution mechanism, since the
 * target's absolute address isn't known until doLink() runs, long after
 * this proc's own layout is fixed. '!' fixups are parsed and renumbered
 * exactly like '?' (plain external W-type reference) and are never
 * eligible for the shrink set.
 */

#include "header.h"

/* 65536, not a guessed "big enough" constant: the CDP1802 has a 16-bit
 * address space, so a single proc's byte content can never legitimately
 * exceed it -- matches word's own domain exactly. A smaller value is not
 * just a lower ceiling: every write derived from an offset into an
 * oversized proc either silently corrupts heap memory (a delayed,
 * hard-to-trace crash) or overruns the stack-allocated scratch buffers in
 * rlxEmitProc, so this bound is load-bearing, not cosmetic. Real-world
 * single-proc sizes vary a lot by source language and compiler -- a
 * compiler that emits one large proc per translation unit can easily
 * exceed what a hand-written assembly codebase's own procs ever reach,
 * so don't assume a smaller value is "big enough" without checking
 * against the largest proc any real caller actually produces. */
#define RLX_MAX_PROC_BYTES  65536
#define RLX_MAX_PROC_FIXUPS 2048
#define RLX_MAX_SEGMENTS    8192
#define RLX_MAX_EXCL        8192
#define RLX_LINE_LEN        1024
/* An absolute safety ceiling, not the round cap that governs a normal
 * build -- see the derived, totalCandidates-based cap computed in
 * runRelaxedLink() (the `maxRounds` local) for that. Kept large-but-
 * finite rather than removed entirely: a build that reaches this ceiling
 * means something is genuinely wrong (a real bug, not "just needs more
 * rounds"), and should fail with a clear report rather than spin
 * unbounded. */
#define RLX_MAX_ROUNDS      100000

typedef struct {
  char type;        /* '#','!','+','^','v','?','/','\\','=','<' */
  word offset;       /* primary (patch) offset, proc-relative, ORIGINAL numbering */
  word lofs;          /* '^' companion low-offset param */
  byte low;             /* '/' low-byte param */
  char name[128];        /* symbol name for '!','?','/','\\','=' */
} RlxFixup;

typedef struct {
  char name[128];
  word size;
  byte bytes[RLX_MAX_PROC_BYTES];
  byte defined[RLX_MAX_PROC_BYTES];
  RlxFixup fixups[RLX_MAX_PROC_FIXUPS];
  int numFixups;
} RlxProc;

typedef struct {
  int isProc;
  char rawLine[RLX_LINE_LEN];
  RlxProc *proc; /* heap-allocated only for isProc segments -- RlxProc is
                  * large (a full proc-sized content buffer plus a fixup
                  * table); embedding one in every segment slot, including
                  * the many plain passthrough lines a typical file has,
                  * multiplies that cost by the segment count and can
                  * exhaust available memory on a real multi-file link
                  * (malloc() failing silently rather than a checked
                  * error). Allocating only where actually needed avoids
                  * this. */
  char parentProc[128]; /* Only meaningful for a !isProc (passthrough)
                          * segment: the name of the proc it was found
                          * inside, or "" if it wasn't inside any proc.
                          * Used only when emitting a LIBRARY pseudo-object
                          * file (see rlxEmitFile's isLibraryFile mode) to
                          * decide whether an in-proc directive like
                          * ".requires" should survive -- it should only if
                          * its own enclosing proc was itself discovered/
                          * kept, exactly mirroring loadFile()'s own
                          * loadModule-gated handling of ".requires" (see
                          * that function's header comment in main.c). A
                          * ".library" line is the one exception: it's
                          * NEVER gated by loadModule in loadFile(), so it
                          * always survives regardless of parentProc. */
} RlxSegment;

typedef struct {
  char origName[1024];
  RlxSegment segs[RLX_MAX_SEGMENTS];
  int numSegs;
} RlxFileData;

typedef struct {
  char fileName[1024];
  char procName[128];
  word offset;
} RlxKey;

static RlxKey rlxExcluded[RLX_MAX_EXCL];
static int rlxNumExcluded = 0;

static RlxKey rlxFailedThisRound[RLX_MAX_EXCL];
static int rlxNumFailedThisRound = 0;

/* Default: exclude only the FIRST branch that failed this round, instead
 * of the whole batch, before retrying. Batch exclusion can over-exclude
 * -- if branches A/B/C all fail together in one round, only A might be
 * the real cause (B/C could have fit once A alone was excluded and
 * everything downstream shifted), but batch exclusion never finds that
 * out, since it always excludes all three at once. One-at-a-time reaches
 * a smaller final binary at the cost of more rounds; on a real multi-
 * hundred-branch program the extra build time has been negligible while
 * the additional shrinkage was real, not just theoretical.
 * RLX_BATCH_EXCLUDE (any value) opts back into the old all-at-once
 * behavior, for a program large enough that round count becomes the
 * bottleneck. See runRelaxedLink(). */
static int rlxOneAtATime = 1;

static int rlxKeyEq(RlxKey *a, RlxKey *b) {
  return strcmp(a->fileName, b->fileName) == 0 &&
         strcmp(a->procName, b->procName) == 0 &&
         a->offset == b->offset;
}

static int rlxIsExcluded(char *file, char *proc, word off) {
  int i;
  RlxKey k;
  strcpy(k.fileName, file);
  strcpy(k.procName, proc);
  k.offset = off;
  for (i = 0; i < rlxNumExcluded; i++)
    if (rlxKeyEq(&rlxExcluded[i], &k)) return 1;
  return 0;
}

void rlxRecordFailure(char *origFile, char *procName, word origOffset) {
  RlxKey k;
  int i;
  if (origFile == NULL || procName == NULL) return;
  strcpy(k.fileName, origFile);
  strcpy(k.procName, procName);
  k.offset = origOffset;
  for (i = 0; i < rlxNumFailedThisRound; i++)
    if (rlxKeyEq(&rlxFailedThisRound[i], &k)) return;
  if (rlxNumFailedThisRound >= RLX_MAX_EXCL) return;
  rlxFailedThisRound[rlxNumFailedThisRound++] = k;
}

/* ---- Library-code relaxation: discovery pass ----
 *
 * -r's regenerate-and-relink pipeline only ever handled OBJECT files --
 * library (.lib) files were always loaded through main.c's own unmodified
 * loadFile()/selective-scan path, so library-sourced branches were never
 * shrink candidates even when object-file code right next to them got
 * shrunk. Since library code is often the bulk of a real program's
 * instruction count, this leaves a lot of -r's benefit unrealized.
 *
 * Fix: before the relaxation round loop starts, run one ordinary,
 * completely unmodified link (rlxRunDiscoveryPass(), below) so the
 * existing loadFile()/doLink()/library-scan machinery determines the real
 * closure of library procs THIS program needs -- exactly what a plain,
 * non-relaxed build would determine. While that pass runs, the two
 * existing "Linking %s from library" print sites in loadFile() (main.c)
 * also call rlxRecordDiscovered() (only when rlxDiscovering is set, so
 * this has zero effect outside a -r build) to capture the (library file,
 * proc name) pair. The resulting rlxDiscovered[] list is then used to
 * build "library pseudo-object files" -- the referenced library files,
 * re-parsed with the same rlxParseFile() used for objects, but filtered
 * at emission time (see rlxEmitFile()'s isLibraryFile mode) to include
 * ONLY discovered procs -- merged into the same files[]/tmpPaths[]/
 * origNames[] arrays object files already use, so the existing round
 * loop, exclusion tracking, and rlxLinkOnce() all apply completely
 * unchanged.
 *
 * Discovery MUST run with rlxActive == 0 (i.e. strictly before
 * runRelaxedLink() sets it to 1): loadFile()'s '<' handler branches on
 * rlxActive to decide whether to expect the extra fields a relaxation-
 * generated '<' line carries (full target, original proc name/offset) --
 * a discovery-pass load is reading ORIGINAL, unregenerated object/library
 * text, exactly what a plain build would see, and a genuine hand-written
 * short branch in that text has none of those extra fields. */

#define RLX_MAX_DISCOVERED 8192   /* matches RLX_MAX_EXCL/RLX_MAX_SEGMENTS */

typedef struct {
  char libFile[1024];
  char procName[128];
} RlxDiscoveredProc;

static RlxDiscoveredProc rlxDiscovered[RLX_MAX_DISCOVERED];
static int rlxNumDiscovered = 0;

/* Called from loadFile() (main.c), only while rlxDiscovering is set. */
void rlxRecordDiscovered(char *libFile, char *procName) {
  int i;
  if (libFile == NULL || procName == NULL) return;
  for (i = 0; i < rlxNumDiscovered; i++)
    if (strcmp(rlxDiscovered[i].libFile, libFile) == 0 &&
        strcmp(rlxDiscovered[i].procName, procName) == 0)
      return;
  if (rlxNumDiscovered >= RLX_MAX_DISCOVERED) {
    /* A warning, not a silent truncation -- omitting a proc here means
     * omitting real, needed code from the library pseudo-object file
     * built later, not just losing a relaxation opportunity. */
    printf("Warning: more than %d distinct library procs referenced -- "
           "some library code will not be relaxation-eligible\n",
           RLX_MAX_DISCOVERED);
    return;
  }
  strcpy(rlxDiscovered[rlxNumDiscovered].libFile, libFile);
  strcpy(rlxDiscovered[rlxNumDiscovered].procName, procName);
  rlxNumDiscovered++;
}

static int rlxIsDiscovered(char *libFile, char *procName) {
  int i;
  for (i = 0; i < rlxNumDiscovered; i++)
    if (strcmp(rlxDiscovered[i].libFile, libFile) == 0 &&
        strcmp(rlxDiscovered[i].procName, procName) == 0)
      return 1;
  return 0;
}

/* Collapses rlxDiscovered[] down to the distinct set of library file
 * names referenced (in first-seen order). Returned pointers alias
 * rlxDiscovered[]'s own (static, program-lifetime) buffers -- never
 * freed, matching this file's existing convention for rlxExcluded[]/
 * rlxFailedThisRound[] never being freed either. */
static int rlxUniqueLibFiles(char **out, int max) {
  int i, j, n = 0;
  for (i = 0; i < rlxNumDiscovered; i++) {
    int dup = 0;
    for (j = 0; j < n; j++)
      if (strcmp(out[j], rlxDiscovered[i].libFile) == 0) { dup = 1; break; }
    if (!dup) {
      if (n >= max) continue; /* can't happen given RLX_MAX_DISCOVERED
                                * sizing at every call site, but guard
                                * rather than overrun regardless. */
      out[n++] = rlxDiscovered[i].libFile;
    }
  }
  return n;
}

/* ---- Parsing ---- */

/* Both of these guard a fixed-size array. A silent truncation here would
 * produce a WRONG binary that appears to link successfully -- far worse
 * than a clean, diagnosable fatal error, so both fail loudly rather than
 * dropping content. */
static void rlxCheckFixupCapacity(char *filename, RlxProc *proc) {
  if (proc->numFixups >= RLX_MAX_PROC_FIXUPS) {
    printf("Error: proc '%s' in %s has more than %d fixups (internal "
           "limit RLX_MAX_PROC_FIXUPS) -- rebuild link02 with a larger "
           "limit\n", proc->name, filename, RLX_MAX_PROC_FIXUPS);
    exit(1);
  }
}

static void rlxCheckSegmentCapacity(char *filename, RlxFileData *fd) {
  if (fd->numSegs >= RLX_MAX_SEGMENTS) {
    printf("Error: %s has more than %d segments (internal limit "
           "RLX_MAX_SEGMENTS) -- rebuild link02 with a larger limit\n",
           filename, RLX_MAX_SEGMENTS);
    exit(1);
  }
}

static int rlxParseFile(char *filename, RlxFileData *fd, int isLibrary) {
  FILE *f;
  char buffer[RLX_LINE_LEN];
  char *line;
  int pos;
  char token[256];
  word value, addr, lofs, low;
  RlxProc *proc = NULL;
  word curpos = 0;
  int inProc = 0;
  RlxSegment *seg;

  /* isLibrary selects which search path findInputFile() consults after
   * trying the current directory (-L for a library, -I for an object
   * file), exactly matching loadFile()'s own convention (findInputFile(f,
   * libScan != 0)). Object files (runRelaxedLink()'s own objects[i] loop)
   * always pass 0. Library files discovered by relax.c's own discovery
   * pass (see rlxRunDiscoveryPass()/rlxUniqueLibFiles()) pass 1, so a
   * library found via -L (not just the current directory) resolves here
   * the same way it would in a plain, non-relaxed link. Before isLibrary
   * existed at all, this was a bare fopen(filename, "r") with no fallback
   * to any search path, so an object file that wasn't in the current
   * directory failed here even though a plain build of the same command
   * line found it fine via -I. */
  f = findInputFile(filename, isLibrary);
  if (f == NULL) {
    printf("Could not open input file: %s\n", filename);
    return -1;
  }
  strcpy(fd->origName, filename);
  fd->numSegs = 0;

  while (fgets(buffer, RLX_LINE_LEN - 1, f) != NULL) {
    line = buffer;
    if (*line == '{') {
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      rlxCheckSegmentCapacity(filename, fd);
      seg = &fd->segs[fd->numSegs];
      seg->isProc = 1;
      seg->parentProc[0] = 0; /* unused for an isProc segment -- proc->name
                                * is the name that matters here */
      seg->proc = (RlxProc *)malloc(sizeof(RlxProc));
      proc = seg->proc;
      strcpy(proc->name, token);
      proc->numFixups = 0;
      memset(proc->defined, 0, sizeof(proc->defined));
      curpos = 0;
      inProc = 1;
      /* Claim this segment slot NOW, not at the matching '}' below --
       * see the unconditional passthrough branch's own comment (bottom
       * of this loop) for why: an unrecognized line encountered WHILE
       * inProc (e.g. a ".library" directive positioned inside a proc)
       * falls through to that passthrough branch, which writes into
       * fd->segs[fd->numSegs] -- if numSegs weren't already bumped
       * here, that write would land on THIS SAME slot (still "pending"
       * until close), silently overwriting isProc/proc back to a raw-
       * text segment and corrupting this proc's own representation.
       * Bumping it immediately means any such line gets its own,
       * later, non-colliding slot instead. */
      fd->numSegs++;
    } else if (*line == '}') {
      if (proc != NULL) {
        proc->size = curpos;
      }
      inProc = 0;
      proc = NULL;
    } else if (inProc && *line == ':') {
      line++;
      line = getHex(line, &addr);
      curpos = addr;
      while (*line != 0) {
        while (*line > 0 && *line <= ' ') line++;
        if (*line != 0) {
          line = getHex(line, &value);
          /* curpos is a word (0-65535), the same domain
           * RLX_MAX_PROC_BYTES=65536 now covers exactly -- the compiler
           * correctly flags this comparison as always-true. Kept anyway:
           * cheap, correct defense-in-depth against a future shrink of
           * that constant silently reintroducing an out-of-bounds
           * write. */
          if (curpos < RLX_MAX_PROC_BYTES) {
            proc->bytes[curpos] = value & 0xff;
            proc->defined[curpos] = 1;
          }
          curpos++;
        }
      }
    } else if (inProc && *line == '>') {
      line++;
      line = getHex(line, &value);
      curpos += value;
    } else if (inProc && (*line == '#' || *line == '+')) {
      char t = *line;
      line++;
      line = getHex(line, &addr);
      rlxCheckFixupCapacity(filename, proc);
      proc->fixups[proc->numFixups].type = t;
      proc->fixups[proc->numFixups].offset = addr;
      proc->numFixups++;
    } else if (inProc && *line == '^') {
      line++;
      line = getHex(line, &addr);
      while (*line == ' ') line++;
      getHex(line, &lofs);
      rlxCheckFixupCapacity(filename, proc);
      proc->fixups[proc->numFixups].type = '^';
      proc->fixups[proc->numFixups].offset = addr;
      proc->fixups[proc->numFixups].lofs = lofs;
      proc->numFixups++;
    } else if (inProc && *line == 'v') {
      line++;
      line = getHex(line, &addr);
      rlxCheckFixupCapacity(filename, proc);
      proc->fixups[proc->numFixups].type = 'v';
      proc->fixups[proc->numFixups].offset = addr;
      proc->numFixups++;
    } else if (inProc && *line == '<') {
      /* Not expected pre-relax in this codebase (no hand-written short
       * branches exist here), but handle passthrough defensively: treat
       * the stored byte as an opaque already-short local target and just
       * renumber the patch offset, leaving the byte value untouched. */
      line++;
      line = getHex(line, &addr);
      rlxCheckFixupCapacity(filename, proc);
      proc->fixups[proc->numFixups].type = '<';
      proc->fixups[proc->numFixups].offset = addr;
      proc->numFixups++;
    } else if (inProc && (*line == '?' || *line == '!' || *line == '\\')) {
      char t = *line;
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      while (*line == ' ') line++;
      getHex(line, &value);
      rlxCheckFixupCapacity(filename, proc);
      proc->fixups[proc->numFixups].type = t;
      strcpy(proc->fixups[proc->numFixups].name, token);
      proc->fixups[proc->numFixups].offset = value;
      proc->numFixups++;
    } else if (inProc && *line == '/') {
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      while (*line == ' ') line++;
      line = getHex(line, &value);
      while (*line == ' ') line++;
      getHex(line, &low);
      rlxCheckFixupCapacity(filename, proc);
      proc->fixups[proc->numFixups].type = '/';
      strcpy(proc->fixups[proc->numFixups].name, token);
      proc->fixups[proc->numFixups].offset = value;
      proc->fixups[proc->numFixups].low = low & 0xff;
      proc->numFixups++;
    } else if (inProc && *line == '=') {
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      while (*line == ' ') line++;
      getHex(line, &value);
      rlxCheckFixupCapacity(filename, proc);
      proc->fixups[proc->numFixups].type = '=';
      strcpy(proc->fixups[proc->numFixups].name, token);
      proc->fixups[proc->numFixups].offset = value;
      proc->numFixups++;
    } else {
      /* Passthrough: anything outside a proc (.big, @start, etc.), or an
       * unrecognized/unsupported line inside one (e.g. ".library" --
       * see the '{' branch above for why this slot is now guaranteed
       * not to collide with an still-open proc's own segment). Stored
       * verbatim, so it re-emits at the SAME point in segment order --
       * for a still-open proc, that's right after that proc's own
       * regenerated {...} block closes (rlxEmitProc always finishes
       * the whole proc before this loop moves to the next segment),
       * not literally inside it; harmless for a line like ".library"
       * whose meaning doesn't depend on proc-nesting -- confirmed by
       * comparing against the unmodified, non-relaxed loadFile() path,
       * which links the same input fine with it positioned either
       * way. */
      rlxCheckSegmentCapacity(filename, fd);
      seg = &fd->segs[fd->numSegs];
      seg->isProc = 0;
      strcpy(seg->rawLine, buffer);
      if (inProc && proc != NULL)
        strcpy(seg->parentProc, proc->name);
      else
        seg->parentProc[0] = 0;
      fd->numSegs++;
    }
  }
  fclose(f);
  return 0;
}

/* ---- Rewrite/renumber engine ---- */

static word rlxRemap(word p, word *removed, int numRemoved) {
  int i, count = 0;
  for (i = 0; i < numRemoved; i++)
    if (removed[i] <= p) count++;
  return (word)(p - count);
}

/* Regenerate a proc's text (as a single string, appended to *out) given
 * the set of original '#' offsets chosen to shrink this round.
 *
 * Emission order matters: Asm/02 itself only ever flushes '#'/'+'/'^'/'v'
 * (the LOCAL fixup types, resolved by loadFile() via a direct memory
 * read at load time) at OT_ENDP, i.e. after every ':' content line for
 * the proc has already been written. Those handlers require their patch
 * position's placeholder byte to already be sitting in memory[] (from
 * the ':' line) before they run '+'-style resolution against it -- so
 * this rewrite must preserve that ordering, not just byte-for-byte
 * content. ('?'/'!'/'/'/'\\', the EXTERNAL types, don't touch memory[]
 * until doLink() runs later and so are order-insensitive, but are
 * emitted after the content too, for simplicity and to match the source
 * format.) Getting this backwards produces a bogus "collision" warning
 * and, more seriously, resolves a kept-long branch against a stale
 * value whenever its true target happens to be nonzero. */
/* nbytes/ndefined used to be plain stack locals -- harmless at the
 * original RLX_MAX_PROC_BYTES=8192 (16KB combined) but a needlessly
 * large ~128KB stack frame at the new 65536 value (see that constant's
 * own comment). static is safe here: rlxEmitProc is never called
 * concurrently or recursively (this is a single-threaded, sequential
 * regenerate-one-proc-at-a-time pipeline), and ndefined is explicitly
 * memset(0) at the top of every call regardless, so nothing depends on
 * either buffer's content surviving -- or NOT surviving -- between
 * calls. */
static byte rlxEmitNbytes[RLX_MAX_PROC_BYTES];
static byte rlxEmitNdefined[RLX_MAX_PROC_BYTES];
static int rlxEmitPartner[RLX_MAX_PROC_FIXUPS];

static void rlxEmitProc(FILE *out, char *origFile, RlxProc *orig,
                         word *shrink, int numShrink) {
  word removed[RLX_MAX_PROC_FIXUPS];
  byte *nbytes = rlxEmitNbytes;
  byte *ndefined = rlxEmitNdefined;
  int *partner = rlxEmitPartner;
  char fixupText[RLX_MAX_PROC_FIXUPS][80];
  int numFixupText = 0;
  word newSize;
  int i, j;
  word p, np;

  for (i = 0; i < numShrink; i++) removed[i] = shrink[i];

  /* Pair up every '^'/'v' fixup ONCE, before the main emission loop
   * below touches any of them, rather than having 'v' guess at its
   * partner while walking past it. A '^'/'v' pair is always emitted as
   * two strictly adjacent entries (which one comes first depends on the
   * compiled instruction's own byte order), but a 'v' checking only its
   * immediate neighbors can be fooled when it sits between two
   * UNRELATED '^' fixups -- e.g. "^A vB ^C", where vB's true partner is
   * ^A (to its left) but ^C (to its right, really the start of a
   * completely different pair) looks just as adjacent. A single greedy
   * left-to-right sweep avoids this ambiguity entirely: claim the
   * leftmost unclaimed '^'/'v' (or 'v'/'^') adjacent pair first, mark
   * both entries used, and never reconsider a claimed entry -- so once
   * A and B are claimed as a pair, C is no longer a candidate partner
   * for B, and is free to pair correctly with whatever follows it. */
  for (i = 0; i < orig->numFixups; i++) partner[i] = -1;
  for (i = 0; i + 1 < orig->numFixups; i++) {
    if (partner[i] != -1) continue;
    if ((orig->fixups[i].type == '^' && orig->fixups[i + 1].type == 'v') ||
        (orig->fixups[i].type == 'v' && orig->fixups[i + 1].type == '^')) {
      partner[i] = i + 1;
      partner[i + 1] = i;
    }
  }

  newSize = rlxRemap(orig->size, removed, numShrink);
  /* ndefined is now `byte *`, not an array -- sizeof(ndefined) would give
   * the pointer's own size (8 on a 64-bit build), not the buffer's, since
   * the static-buffer change above. Use the real static array's size
   * explicitly instead. */
  memset(ndefined, 0, sizeof(rlxEmitNdefined));

  for (p = 0; p < orig->size; p++) {
    int isRemoved = 0;
    for (i = 0; i < numShrink; i++)
      if (removed[i] == p) { isRemoved = 1; break; }
    if (isRemoved) continue;
    if (!orig->defined[p]) continue;
    np = rlxRemap(p, removed, numShrink);
    {
      byte v = orig->bytes[p];
      for (i = 0; i < numShrink; i++)
        if (removed[i] == (word)(p + 1)) { v = (byte)(v - 0x90); break; }
      /* np is a word -- see the matching comment on the ':' parse-side
       * check above for why this is now always true, and why it's kept
       * anyway. */
      if (np < RLX_MAX_PROC_BYTES) {
        nbytes[np] = v;
        ndefined[np] = 1;
      }
    }
  }

  for (i = 0; i < orig->numFixups; i++) {
    RlxFixup *fx = &orig->fixups[i];
    word newOff = rlxRemap(fx->offset, removed, numShrink);

    if (fx->type == '#') {
      int shrinking = 0;
      /* fx->offset+1 is computed as int (word promotes), so this catches
       * the one theoretical edge RLX_MAX_PROC_BYTES=65536 doesn't rule
       * out by construction: a 2-byte field whose low byte would sit at
       * proc-relative offset 65536, i.e. a fixup positioned at the very
       * last valid byte (65535) of a proc that uses the FULL 16-bit
       * address space. Not achievable by any real, valid 1802 program
       * (there's no address 65536 to reference), so this can only mean
       * malformed/corrupted .prg input -- fail loudly rather than read
       * or write one byte past the end of a fixed buffer. */
      if ((int)fx->offset + 1 >= RLX_MAX_PROC_BYTES) {
        printf("Error: proc '%s' in %s: fixup offset %04x has no room "
               "for its 2-byte field within the 16-bit address space\n",
               orig->name, origFile, fx->offset);
        exit(1);
      }
      for (j = 0; j < numShrink; j++)
        if (shrink[j] == fx->offset) { shrinking = 1; break; }
      {
        word targetOrig = (word)((orig->bytes[fx->offset] << 8) |
                                  orig->bytes[fx->offset + 1]);
        word targetNew = rlxRemap(targetOrig, removed, numShrink);
        if (shrinking) {
          /* fx->offset (X) is itself a removed position -- remap(X)
           * collapses onto the opcode's own new slot (X-1's new
           * position), since remap()'s "how many removed positions are
           * <= p" count includes X itself when p==X. The single
           * remaining operand byte actually lands where the ORIGINAL
           * low byte (X+1, never itself removed) maps to -- one past
           * the shrunk opcode. Using remap(X+1) here also means this
           * write lands exactly on top of the general copy loop's own
           * (stale, unrenumbered) placeholder for that position, rather
           * than colliding with the opcode's slot. */
          word shortOff = rlxRemap((word)(fx->offset + 1), removed, numShrink);
          nbytes[shortOff] = targetNew & 0xff;
          ndefined[shortOff] = 1;
          /* The full (unmasked) target field is required -- see
           * loadFile()'s '<' handler for why the stored byte alone
           * can't be trusted for either validation or resolution once a
           * proc exceeds 256 bytes. The proc name/offset fields after it
           * are used only by the error path, to map a failure back to
           * this exact original branch. */
          sprintf(fixupText[numFixupText++], "<%04x %04x %s %04x\n", shortOff,
                  targetNew, orig->name, fx->offset);
        } else {
          nbytes[newOff] = (targetNew >> 8) & 0xff;
          nbytes[newOff + 1] = targetNew & 0xff;
          ndefined[newOff] = 1;
          ndefined[newOff + 1] = 1;
          sprintf(fixupText[numFixupText++], "#%04x\n", newOff);
        }
      }
    } else if (fx->type == '+') {
      /* Same theoretical edge as the '#' case just above -- see its own
       * comment. */
      if ((int)fx->offset + 1 >= RLX_MAX_PROC_BYTES) {
        printf("Error: proc '%s' in %s: fixup offset %04x has no room "
               "for its 2-byte field within the 16-bit address space\n",
               orig->name, origFile, fx->offset);
        exit(1);
      }
      word targetOrig = (word)((orig->bytes[fx->offset] << 8) |
                                orig->bytes[fx->offset + 1]);
      word targetNew = rlxRemap(targetOrig, removed, numShrink);
      nbytes[newOff] = (targetNew >> 8) & 0xff;
      nbytes[newOff + 1] = targetNew & 0xff;
      ndefined[newOff] = 1;
      ndefined[newOff + 1] = 1;
      sprintf(fixupText[numFixupText++], "+%04x\n", newOff);
    } else if (fx->type == '^') {
      word targetOrig = (word)((orig->bytes[fx->offset] << 8) | fx->lofs);
      word targetNew = rlxRemap(targetOrig, removed, numShrink);
      nbytes[newOff] = (targetNew >> 8) & 0xff;
      ndefined[newOff] = 1;
      sprintf(fixupText[numFixupText++], "^%04x %02x\n", newOff,
              targetNew & 0xff);
    } else if (fx->type == 'v') {
      word targetNew;
      /* 'v' by itself carries no high-byte information -- its target
       * always comes from its '^' partner, identified up front by the
       * greedy pairing pass above (see its own header comment for why a
       * simple "check my neighbor" lookup isn't reliable on its own). */
      if (partner[i] >= 0) {
        RlxFixup *capFx = &orig->fixups[partner[i]];
        word targetOrig = (word)((orig->bytes[capFx->offset] << 8) |
                                  capFx->lofs);
        targetNew = rlxRemap(targetOrig, removed, numShrink);
      } else {
        /* No '^' partner found at all -- degrade gracefully (only
         * correct if the proc is under 256 bytes), rather than failing
         * outright. */
        targetNew = rlxRemap(orig->bytes[fx->offset], removed, numShrink);
      }
      nbytes[newOff] = targetNew & 0xff;
      ndefined[newOff] = 1;
      sprintf(fixupText[numFixupText++], "v%04x\n", newOff);
    } else if (fx->type == '?' || fx->type == '!' || fx->type == '\\') {
      sprintf(fixupText[numFixupText++], "%c%s %04x\n", fx->type, fx->name,
              newOff);
    } else if (fx->type == '/') {
      sprintf(fixupText[numFixupText++], "/%s %04x %02x\n", fx->name, newOff,
              fx->low);
    } else if (fx->type == '=') {
      sprintf(fixupText[numFixupText++], "=%s %04x\n", fx->name, newOff);
    } else if (fx->type == '<') {
      /* A genuine, hand-written short branch (asm02-emitted directly,
       * not one of our own '#'-shrink products). Must NOT simply copy
       * the stored byte through unchanged -- that's only correct if
       * nothing between the proc's start and this target ever shrinks;
       * if any '#'-candidate before the target shrinks, the target's
       * true proc-relative position moves but a copied-through stored
       * byte wouldn't, silently pointing the branch at the wrong place
       * once resolved. Reconstruct the original target from the stored
       * byte and remap it exactly like any other local target instead.
       *
       * The stored byte is still the ONLY information available for the
       * target -- unlike the '#'-shrink case, there are no unmasked
       * high bits to recover, since asm02 itself only ever stored one
       * byte for a genuine short branch in the first place. This means
       * a hand-written short branch whose target's own proc-relative
       * offset is >= 256 was never representable correctly even before
       * relaxation existed, and still isn't -- a pre-existing, inherent
       * limit of the '<' mechanism itself, not something relaxation
       * introduces or could fix. Only matters for a proc large enough
       * to need it; most hand-written short branches are written inside
       * small, tight loops well under that size. */
      word targetOrig = orig->bytes[fx->offset];
      word targetNew = rlxRemap(targetOrig, removed, numShrink);
      nbytes[newOff] = targetNew & 0xff;
      ndefined[newOff] = 1;
      sprintf(fixupText[numFixupText++], "<%04x %04x\n", newOff,
              targetNew & 0xff);
    }
  }

  fprintf(out, "{%s\n", orig->name);

  /* Emit byte content as contiguous runs, defined vs. gap. */
  p = 0;
  while (p < newSize) {
    if (ndefined[p]) {
      word start = p;
      int col = 0;
      fprintf(out, ":%04x", start);
      while (p < newSize && ndefined[p]) {
        fprintf(out, " %02x", nbytes[p]);
        p++;
        col++;
        if (col == 16 && p < newSize && ndefined[p]) {
          fprintf(out, "\n:%04x", p);
          col = 0;
        }
      }
      fprintf(out, "\n");
    } else {
      word start = p;
      while (p < newSize && !ndefined[p]) p++;
      fprintf(out, ">%04x\n", (word)(p - start));
    }
  }

  for (i = 0; i < numFixupText; i++) fputs(fixupText[i], out);

  fprintf(out, "}\n");
}

/* Debug/bisection aid: RLX_MAX_SHRINK=N caps the total number of branches
 * allowed to shrink across the whole link (deterministic candidate
 * order), so a failure that only appears with -r can be binary-searched
 * between "goes through the full rewrite-and-relink machinery but
 * shrinks nothing" (N=0, should behave identically to flag-off if the
 * bug is specifically in shrinking) and "fully relaxed" (unset/large N).
 * Reset to the configured budget at the start of every round by
 * runRelaxedLink() -- it must apply consistently per round, not
 * cumulatively across them. */
static int rlxShrinkBudget = -1;   /* -1 = unlimited */
static int rlxShrinkRemaining = -1;
static int rlxActualShrinkCount = 0; /* candidates actually placed in a
                                       * shrink[] set this round -- the
                                       * budget cap above can make this
                                       * fewer than (candidates minus
                                       * excluded), so it's tracked
                                       * separately for accurate reporting. */

/* isLibraryFile: 0 for an object file, emitted exactly as before with no
 * filtering. 1 for a library pseudo-object file (see the discovery-pass
 * header comment above rlxDiscovered[]) -- only procs actually present in
 * rlxDiscovered[] are emitted at all (an undiscovered proc's whole {...}
 * block is skipped, never a shrink candidate, never even written), and a
 * passthrough line found INSIDE a proc (in practice, ".requires" -- see
 * RlxSegment.parentProc's own comment) is only kept if its enclosing proc
 * was itself discovered/kept. A ".library" line is never gated this way
 * -- loadFile() itself never gates it on loadModule either (main.c,
 * confirmed: unlike ".requires", ".library" has no such check), so it
 * always survives regardless of which proc (if any) it was found inside. */
static void rlxEmitFile(char *tmpPath, RlxFileData *fd, int isLibraryFile) {
  FILE *out;
  int i, n;
  word shrink[RLX_MAX_PROC_FIXUPS];

  out = fopen(tmpPath, "w");
  if (out == NULL) {
    printf("Error: could not create temp file %s\n", tmpPath);
    exit(1);
  }
  for (i = 0; i < fd->numSegs; i++) {
    RlxSegment *seg = &fd->segs[i];
    if (!seg->isProc) {
      if (isLibraryFile && seg->parentProc[0] != 0) {
        int isLibDirective = (strncmp(seg->rawLine, ".library ", 9) == 0);
        if (!isLibDirective &&
            !rlxIsDiscovered(fd->origName, seg->parentProc))
          continue;
      }
      fputs(seg->rawLine, out);
      if (strlen(seg->rawLine) == 0 ||
          seg->rawLine[strlen(seg->rawLine) - 1] != '\n')
        fputc('\n', out);
    } else {
      RlxProc *proc = seg->proc;
      int j;
      if (isLibraryFile && !rlxIsDiscovered(fd->origName, proc->name))
        continue;
      n = 0;
      for (j = 0; j < proc->numFixups; j++)
        if (proc->fixups[j].type == '#' &&
            !rlxIsExcluded(fd->origName, proc->name, proc->fixups[j].offset)) {
          if (rlxShrinkBudget >= 0) {
            if (rlxShrinkRemaining <= 0) continue;
            rlxShrinkRemaining--;
          }
          rlxActualShrinkCount++;
          shrink[n++] = proc->fixups[j].offset;
        }
      rlxEmitProc(out, fd->origName, proc, shrink, n);
    }
  }
  fclose(out);
}

/* ---- Link-state reset + one round's worth of load+link ---- */

static void rlxResetLinkState() {
  int i;
  for (i = 0; i < numSymbols; i++) free(symbols[i]);
  if (numSymbols > 0) { free(symbols); free(values); }
  numSymbols = 0;

  for (i = 0; i < numReferences; i++) free(references[i]);
  if (numReferences > 0) {
    free(references);
    free(addresses);
    free(types);
    free(lows);
  }
  numReferences = 0;

  for (i = 0; i < numRequires; i++) free(requires[i]);
  if (numRequires > 0) { free(requires); free(requireAdded); }
  numRequires = 0;

  /* moduleFixups (-m) must be reset here too, for the exact same
   * reason numReferences/numSymbols are: rlxLinkOnce() runs once per
   * relaxation round (plus once more for the discovery pass), and
   * only the FINAL round's resolution is what ends up in the output
   * -- without this, fixup offsets from every earlier, now-discarded
   * round (including the discovery pass, which never even runs
   * relaxation) would silently accumulate into a wrong, bloated table. */
  if (numModuleFixups > 0) free(moduleFixups);
  moduleFixups = NULL;
  numModuleFixups = 0;

  memset(memory, 0, sizeof(memory));
  memset(map, 0, sizeof(map));
  address = 0;
  lowest = 0xffff;
  highest = 0x0000;
  startAddress = 0xffff;
  libScan = 0;
  loadModule = -1;
}

/* Runs one full load+resolve+link pass over the given (temp-file-path,
 * original-name) pairs. Mirrors main()'s own object/library loop exactly.
 * origNames[i] is set into rlxCurOrigFile immediately before paths[i] is
 * actually loaded -- it MUST happen here, not back when the temp files
 * were written, because that's what the '<' handler reads at the moment
 * it detects a failure. Getting this wrong doesn't crash anything: it
 * just silently attributes every failure to whichever file this loop
 * happened to be on last, so exclusions recorded from one round never
 * match the real branch on the next -- the relaxation loop still
 * "succeeds" at reaching its round cap, just without ever converging.
 * Returns 1 on a fully-resolved link, 0 if unresolved symbols remain. */
static int rlxLinkOnce(char **paths, char **origNames, int numPaths) {
  int i, resolved;
  rlxResetLinkState();
  for (i = 0; i < numPaths; i++) {
    strcpy(rlxCurOrigFile, origNames[i]);
    if (loadFile(paths[i]) < 0) {
      printf("Errors: aborting link\n");
      exit(1);
    }
  }
  doLink();
  resolved = 1;
  while (numReferences > 0 && resolved != 0) {
    libScan = -1;
    resolved = 0;
    for (i = 0; i < numLibraries; i++) {
      loadModule = 0;
      if (loadFile(libraries[i]) < 0) {
        printf("Errors: aborting link\n");
        exit(1);
      }
      resolved += doLink();
    }
  }
  return numReferences == 0;
}

/* Runs one ordinary, fully unmodified link over the real object files
 * (objects[]/numObjects, both globals set by main()'s own -I/-l/object-
 * file argument parsing well before doRelax is ever consulted) with
 * rlxDiscovering set, so every "Linking %s from library" event loadFile()
 * would normally just print also gets recorded into rlxDiscovered[] (see
 * that struct's own header comment). Must run before runRelaxedLink()
 * sets rlxActive = 1 -- see the same header comment for why. */
static void rlxRunDiscoveryPass() {
  rlxDiscovering = 1;
  rlxNumDiscovered = 0;
  if (!rlxLinkOnce(objects, objects, numObjects)) {
    int i;
    for (i = 0; i < numReferences; i++)
      printf("Error: Symbol %s not found\n", references[i]);
    printf("Errors during link.  Aborting output\n");
    exit(1);
  }
  rlxDiscovering = 0;
}

/* ---- Cross-platform temp-file support ----
 *
 * /tmp is a Unix-only convention -- Windows has no such fixed path;
 * TEMP/TMP (checked in that order, matching Windows' own documented
 * lookup order for %TEMP%/%TMP%) are the real per-user/per-machine
 * temp locations there instead. TMPDIR is the closest Unix analogue
 * (not always set, hence the /tmp fallback -- unchanged behavior from
 * before this existed). Falls back to "." only if the platform's own
 * usual environment variables are entirely absent, which in practice
 * should only happen in a stripped-down environment missing them.
 */
#if defined(_WIN32) || defined(_WIN64)
#define RLX_PATHSEP "\\"
#else
#define RLX_PATHSEP "/"
#endif

static const char *rlxTempDir() {
  char *dir;
#if defined(_WIN32) || defined(_WIN64)
  dir = getenv("TEMP");
  if (dir == NULL) dir = getenv("TMP");
  if (dir == NULL) dir = ".";
#else
  dir = getenv("TMPDIR");
  if (dir == NULL) dir = "/tmp";
#endif
  return dir;
}

/* Portable file copy, used only by the RLX_KEEP_TEMP debug path below
 * -- replaces a previous system("cp %s %s") call, which depended on a
 * Unix-only external command and would have needed a second,
 * separately-tested Windows equivalent (e.g. "copy") instead of one
 * implementation that works everywhere this program already runs. */
static void rlxCopyFile(const char *src, const char *dst) {
  FILE *in, *out;
  char buf[4096];
  size_t n;
  in = fopen(src, "rb");
  if (in == NULL) return;
  out = fopen(dst, "wb");
  if (out == NULL) { fclose(in); return; }
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
}

/* ---- Outer driver, called from main() when -r is given ---- */

int runRelaxedLink() {
  RlxFileData *files;
  char **tmpPaths;
  char **origNames;
  const char *tmpDir;
  int i, round;
  int maxRounds;
  int origQuiet;
  int totalCandidates, totalShrunk;
  static char *libFiles[RLX_MAX_DISCOVERED];
  int numLibFiles;
  int numAllFiles;

  /* Discovery pass -- see rlxRunDiscoveryPass()'s own header comment.
   * Must happen before rlxActive is set below, and before files[]/
   * tmpPaths[]/origNames[] are sized, since numLibFiles isn't known
   * until this returns. */
  rlxRunDiscoveryPass();
  numLibFiles = rlxUniqueLibFiles(libFiles, RLX_MAX_DISCOVERED);
  numAllFiles = numObjects + numLibFiles;

  files = (RlxFileData *)malloc(sizeof(RlxFileData) * numAllFiles);
  tmpPaths = (char **)malloc(sizeof(char *) * numAllFiles);
  origNames = (char **)malloc(sizeof(char *) * numAllFiles);

  tmpDir = rlxTempDir();
  for (i = 0; i < numObjects; i++) {
    if (rlxParseFile(objects[i], &files[i], 0) < 0) {
      printf("Errors: aborting link\n");
      exit(1);
    }
    /* sized to the ACTUAL temp dir length, not a fixed guess -- a
     * real-world Windows %TEMP% (e.g.
     * C:\Users\SomeLongUserName\AppData\Local\Temp) can easily run
     * past what the old fixed 64-byte buffer (sized for "/tmp/..."
     * alone) had room for. */
    tmpPaths[i] = (char *)malloc(strlen(tmpDir) + 64);
    sprintf(tmpPaths[i], "%s%slink02_relax_%d_%d.prg", tmpDir,
            RLX_PATHSEP, (int)getpid(), i);
    origNames[i] = files[i].origName;
  }
  for (i = 0; i < numLibFiles; i++) {
    int idx = numObjects + i;
    if (rlxParseFile(libFiles[i], &files[idx], 1) < 0) {
      printf("Errors: aborting link\n");
      exit(1);
    }
    tmpPaths[idx] = (char *)malloc(strlen(tmpDir) + 64);
    sprintf(tmpPaths[idx], "%s%slink02_relax_%d_%d.prg", tmpDir,
            RLX_PATHSEP, (int)getpid(), idx);
    origNames[idx] = files[idx].origName;
  }

  totalCandidates = 0;
  for (i = 0; i < numAllFiles; i++) {
    int s;
    int isLib = (i >= numObjects);
    for (s = 0; s < files[i].numSegs; s++)
      if (files[i].segs[s].isProc) {
        int j;
        RlxProc *proc = files[i].segs[s].proc;
        if (isLib && !rlxIsDiscovered(files[i].origName, proc->name))
          continue;
        for (j = 0; j < proc->numFixups; j++)
          if (proc->fixups[j].type == '#') totalCandidates++;
      }
  }

  origQuiet = quiet;
  rlxActive = 1;

  {
    char *envMax = getenv("RLX_MAX_SHRINK");
    rlxShrinkBudget = envMax ? atoi(envMax) : -1;
  }
  rlxOneAtATime = getenv("RLX_BATCH_EXCLUDE") == NULL;

  /* Round cap derived from totalCandidates, not a fixed guess. This is a
   * real, provable upper bound, not a "should be big enough" estimate: a
   * round only continues (rlxNumFailedThisRound > 0) if at least one
   * branch failed, and rlxEmitFile() only ever offers a branch as a
   * shrink candidate if it's NOT already in rlxExcluded[] -- so a round
   * that doesn't converge is GUARANTEED to add at least one branch to
   * rlxExcluded[] that was never there before (one-at-a-time picks
   * exactly one; batch-exclude picks every failure that round, all of
   * them also new by the same argument). rlxExcluded[] only ever grows,
   * never shrinks, and can never hold more than totalCandidates entries
   * (every '#' fixup is a candidate at most once) -- so the round loop
   * MUST converge within totalCandidates rounds of net exclusion
   * progress, plus one final round to confirm zero failures. A program
   * with a large candidate pool (library code pulls in far more
   * candidates than a small, self-contained object file) can genuinely
   * need several hundred rounds -- a fixed round cap sized for a small
   * program will cut off a larger one before it converges, which reads
   * as a mysterious "does not converge" failure rather than what it
   * actually is (needed more rounds than were allowed). RLX_MAX_ROUNDS
   * itself is kept only as an absolute sanity ceiling below, in case
   * this proof has a flaw not yet found. */
  {
    int derivedCap = totalCandidates + 2;
    maxRounds = derivedCap < RLX_MAX_ROUNDS ? derivedCap : RLX_MAX_ROUNDS;
  }

  for (round = 0; round < maxRounds; round++) {
    int ok;
    rlxNumFailedThisRound = 0;
    rlxShrinkRemaining = rlxShrinkBudget;
    rlxActualShrinkCount = 0;
    for (i = 0; i < numAllFiles; i++) {
      rlxEmitFile(tmpPaths[i], &files[i], i >= numObjects);
    }
    if (getenv("RLX_KEEP_TEMP") != NULL) {
      for (i = 0; i < numAllFiles; i++) {
        char *keep = (char *)malloc(strlen(tmpDir) + 64);
        sprintf(keep, "%s%srelax_round%d_%d.prg", tmpDir, RLX_PATHSEP,
                round, i);
        rlxCopyFile(tmpPaths[i], keep);
        free(keep);
      }
    }
    quiet = -1;
    ok = rlxLinkOnce(tmpPaths, origNames, numAllFiles);
    quiet = origQuiet;
    if (shortBranchFatal) {
      /* A '<' failed with no original-branch identity to exclude and
       * retry -- a genuine hand-written short branch that's out of
       * range at its actual linked position, not one of relax.c's own
       * shrink candidates. No amount of further rounds fixes this;
       * main()'s own shortBranchFatal check would catch it too, but
       * exiting here avoids pointlessly iterating first. */
      printf("Errors during link.  Aborting output\n");
      exit(1);
    }
    if (rlxNumFailedThisRound == 0) {
      if (!ok) {
        /* Unresolved external symbols -- a real link error, unrelated to
         * relaxation. Report it the normal way. */
        int i2;
        for (i2 = 0; i2 < numReferences; i2++)
          printf("Error: Symbol %s not found\n", references[i2]);
        printf("Errors during link.  Aborting output\n");
        exit(1);
      }
      break;
    }
    /* Silently declining to record an exclusion here (if rlxNumExcluded
     * reached RLX_MAX_EXCL) would break the very convergence guarantee
     * maxRounds's own derivation above depends on: a round that fails
     * MUST grow rlxExcluded[] by at least one previously-unseen entry,
     * or the same branch just keeps failing forever with nothing
     * tracking it as excluded. Fail loudly instead -- this can only mean
     * totalCandidates itself exceeded RLX_MAX_EXCL, a real, reportable
     * limit, not silent non-progress. */
    if (rlxOneAtATime) {
      if (rlxNumExcluded >= RLX_MAX_EXCL) {
        printf("Error: more than %d branches need to stay long (internal "
               "limit RLX_MAX_EXCL) -- rebuild link02 with a larger "
               "limit\n", RLX_MAX_EXCL);
        exit(1);
      }
      rlxExcluded[rlxNumExcluded++] = rlxFailedThisRound[0];
    } else {
      for (i = 0; i < rlxNumFailedThisRound; i++) {
        if (rlxNumExcluded >= RLX_MAX_EXCL) {
          printf("Error: more than %d branches need to stay long "
                 "(internal limit RLX_MAX_EXCL) -- rebuild link02 with a "
                 "larger limit\n", RLX_MAX_EXCL);
          exit(1);
        }
        rlxExcluded[rlxNumExcluded++] = rlxFailedThisRound[i];
      }
    }
    if (round == maxRounds - 1) {
      /* Per the derivation above this loop, this should be unreachable
       * whenever maxRounds came from derivedCap (the totalCandidates-
       * based bound) rather than the absolute RLX_MAX_ROUNDS ceiling --
       * reaching it means either totalCandidates itself was implausibly
       * huge (hit the ceiling) or the convergence proof has a real flaw.
       * Either way this is a genuine internal error, not "just needs
       * more rounds", so it's reported as such rather than suggesting a
       * retry. */
      printf("Error: branch relaxation did not converge after %d rounds "
             "(%d candidates) -- this should not happen; please report it\n",
             maxRounds, totalCandidates);
      exit(1);
    }
  }

  rlxActive = 0;

  totalShrunk = rlxActualShrinkCount;
  if (!origQuiet)
    printf("Relaxation: %d of %d local long branches shortened (%d rounds)\n",
           totalShrunk, totalCandidates, round + 1);

  for (i = 0; i < numAllFiles; i++) {
    remove(tmpPaths[i]);
    free(tmpPaths[i]);
  }
  free(tmpPaths);
  free(files);

  return 0;
}
