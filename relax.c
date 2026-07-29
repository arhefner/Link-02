/*
 * relax.c - branch-relaxation (long -> short branch) support for Link/02.
 *
 * Design (per project owner, replacing an earlier patch-based approach that
 * had two rounds of "compute a value early, forget to fix it up when a
 * later shrink shifts it" bugs):
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
 * would need a genuinely different resolution mechanism (the target's
 * absolute address isn't known until doLink() runs, long after this
 * proc's own layout is fixed), and in this project's own kernel externally
 * targeted long branches are a small minority (14 of 339) confined to one
 * file (kernel.asm's dispatch table). '!' fixups are parsed and
 * renumbered exactly like '?' (plain external W-type reference) and are
 * never eligible for the shrink set.
 */

#include "header.h"

#define RLX_MAX_PROC_BYTES  8192
#define RLX_MAX_PROC_FIXUPS 2048
#define RLX_MAX_SEGMENTS    8192
#define RLX_MAX_EXCL        8192
#define RLX_LINE_LEN        1024
#define RLX_MAX_ROUNDS      200

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
                  * ~300KB (8192-byte content buffer, 2048-entry fixup
                  * table); embedding it in every segment slot, including
                  * the hundreds of plain passthrough lines per file,
                  * blew past 19GB across the real kernel's object files
                  * and made malloc() fail silently (caught as a NULL
                  * deref segfault, not a checked error -- first real
                  * multi-file run after the synthetic single-proc
                  * tests). */
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

/* Default since 2026-07-15 (was an opt-in experiment before that):
 * exclude only the FIRST branch that failed this round, instead of the
 * whole batch, before retrying. Batch exclusion can over-exclude -- if
 * branches A/B/C all fail together in one round, only A might be the
 * real cause (B/C could have fit once A alone was excluded and
 * everything downstream shifted), but batch exclusion never finds that
 * out, since it always excludes all three at once. Measured on the
 * ELF-DOS kernel (346 candidate branches): one-at-a-time shrinks 24
 * more of them than batch exclusion (252 vs 228) for about 0.1 extra
 * seconds of build time (95 rounds vs 4) -- a real win at negligible
 * cost, not just a theoretical one. RLX_BATCH_EXCLUDE (any value) opts
 * back into the old all-at-once behavior, in case a future, much
 * larger program ever makes the round count a real problem. See
 * runRelaxedLink(). */
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
       * inProc (e.g. a ".library" directive positioned inside a proc,
       * found 2026-07-27 via a real downstream link failure) falls
       * through to that passthrough branch, which writes into
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
      proc->fixups[proc->numFixups].type = t;
      proc->fixups[proc->numFixups].offset = addr;
      proc->numFixups++;
    } else if (inProc && *line == '^') {
      line++;
      line = getHex(line, &addr);
      while (*line == ' ') line++;
      getHex(line, &lofs);
      proc->fixups[proc->numFixups].type = '^';
      proc->fixups[proc->numFixups].offset = addr;
      proc->fixups[proc->numFixups].lofs = lofs;
      proc->numFixups++;
    } else if (inProc && *line == 'v') {
      line++;
      line = getHex(line, &addr);
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
       * whose meaning doesn't depend on proc-nesting (confirmed: the
       * unmodified, non-relaxed loadFile() path already links this
       * project's own reported repro fine with it positioned either
       * way). */
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
 * the proc has already been written -- confirmed by inspecting real
 * kernel .prg output (kernel/file.prg's dir_create proc: every ':' line
 * precedes a single trailing block of '#'/'^'/'v' markers). Those
 * handlers require their patch position's placeholder byte to already
 * be sitting in memory[] (from the ':' line) before they run '+'-style
 * resolution against it -- so this rewrite must preserve that ordering,
 * not just byte-for-byte content. ('?'/'!'/'/'/'\\', the EXTERNAL types,
 * don't touch memory[] until doLink() runs later and so are order-
 * -insensitive, but are emitted after the content too, for simplicity
 * and to match the source format.) Getting this backwards was caught by
 * a synthetic test: it produced a bogus "collision" warning and, more
 * seriously, would resolve a kept-long branch against a stale value
 * whenever its true target happened to be nonzero. */
static void rlxEmitProc(FILE *out, char *origFile, RlxProc *orig,
                         word *shrink, int numShrink) {
  word removed[RLX_MAX_PROC_FIXUPS];
  byte nbytes[RLX_MAX_PROC_BYTES];
  byte ndefined[RLX_MAX_PROC_BYTES];
  char fixupText[RLX_MAX_PROC_FIXUPS][80];
  int numFixupText = 0;
  word newSize;
  int i, j;
  word p, np;
  int haveLastCaret = 0;
  word lastCaretTargetNew = 0;

  for (i = 0; i < numShrink; i++) removed[i] = shrink[i];

  newSize = rlxRemap(orig->size, removed, numShrink);
  memset(ndefined, 0, sizeof(ndefined));

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
      haveLastCaret = 1;
      lastCaretTargetNew = targetNew;
    } else if (fx->type == 'v') {
      word targetNew;
      if (haveLastCaret) {
        targetNew = lastCaretTargetNew;
      } else {
        /* Not expected (every 'v' in this codebase is immediately
         * preceded by its pairing '^'), but degrade gracefully: the raw
         * low byte alone is only correct if the proc is under 256 bytes. */
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
       * not one of our own '#'-shrink products) -- first real instance
       * found 2026-07-11 in progs/mr.asm's readlp loop. BUG FIXED THE
       * SAME DAY: this originally just copied the stored byte through
       * unchanged, which is only correct if NOTHING between the proc's
       * start and this target ever shrinks -- if any '#'-candidate
       * before the target shrinks, the target's true proc-relative
       * position moves but the stored byte doesn't, silently pointing
       * the branch at the wrong place once resolved. Caught immediately
       * via direct byte verification (the standing project practice):
       * mr_receive's own readlp branch resolved to an address hundreds
       * of bytes past the end of the program. Fixed by reconstructing
       * the original target from the stored byte and remapping it
       * exactly like any other local target.
       *
       * The stored byte is still the ONLY information available for the
       * target -- unlike the '#'-shrink case, there are no unmasked
       * high bits to recover, since asm02 itself only ever stored one
       * byte for a genuine short branch in the first place. This means
       * a hand-written short branch whose target's own proc-relative
       * offset is >= 256 was never representable correctly even before
       * relaxation existed, and still isn't -- a pre-existing, inherent
       * limit of the '<' mechanism itself, not something introduced or
       * fixable here. Fine for now: proc sizes in this codebase are
       * nowhere near that (mr_receive itself is ~160 bytes). */
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
  (void)origFile;
}

/* Debug/bisection aid: RLX_MAX_SHRINK=N caps the total number of branches
 * allowed to shrink across the whole kernel (deterministic candidate
 * order), so a hardware failure with -r can be binary-searched between
 * "goes through the full rewrite-and-relink machinery but shrinks
 * nothing" (N=0, should behave identically to flag-off if the bug is
 * specifically in shrinking) and "fully relaxed" (unset/large N). Reset
 * to the configured budget at the start of every round by
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

  for (round = 0; round < RLX_MAX_ROUNDS; round++) {
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
    if (rlxOneAtATime) {
      if (rlxNumExcluded < RLX_MAX_EXCL)
        rlxExcluded[rlxNumExcluded++] = rlxFailedThisRound[0];
    } else {
      for (i = 0; i < rlxNumFailedThisRound; i++) {
        if (rlxNumExcluded < RLX_MAX_EXCL)
          rlxExcluded[rlxNumExcluded++] = rlxFailedThisRound[i];
      }
    }
    if (round == RLX_MAX_ROUNDS - 1) {
      printf("Error: branch relaxation did not converge after %d rounds\n",
             RLX_MAX_ROUNDS);
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
