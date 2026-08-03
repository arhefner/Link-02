#ifndef _HEADER_H
#define _HEADER_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define LIBPATH "C:\\Users\\Public\\Series02\\"
#else
#include <strings.h>
#include <unistd.h>
#define O_BINARY 0
#define LIBPATH "/usr/local/lib/"
#endif

#ifdef MAIN
#define LINK
#else
#define LINK extern
#endif

#define BM_BINARY 1
#define BM_ELFOS 2
#define BM_CMD 3
#define BM_RCS 4
#define BM_INTEL 5

typedef unsigned char byte;
typedef unsigned short word;

LINK word address;
LINK byte memory[65536];
LINK byte map[65536];
LINK word lowest;
LINK word highest;
LINK char **objects;
LINK int numObjects;
LINK int outMode;
LINK char outName[1024];
LINK word startAddress;
LINK char **symbols;
LINK word *values;
LINK int numSymbols;
LINK int showSymbols;
LINK int quiet;
LINK char **references;
LINK word *addresses;
LINK byte *lows;
LINK char *types;
LINK int numReferences;
LINK int inProc;
LINK word offset;
LINK char addressMode;
LINK char **libraries;
LINK int numLibraries;
LINK int libScan;
LINK int loadModule;
LINK char **requires;
LINK char *requireAdded;
LINK int numRequires;
LINK char **incPath;
LINK int numIncPath;
LINK char **libPath;
LINK int numLibPath;
//grw - added support for symbol map file
LINK FILE *symFile;
LINK char symName[64];
LINK int createSym;
//arh - add support for Elf/OS header generation
LINK int buildMonth;
LINK int buildDay;
LINK int buildYear;
LINK int buildHour;
LINK int buildMinute;
LINK int buildSecond;
LINK int buildNumber;

/* Branch-relaxation support (see relax.c). doRelax is set by the -r
 * command-line flag. rlxActive/rlxCurOrigFile are used to let loadFile()'s
 * existing '<' (short-branch) error path report failures back to relax.c
 * without relax.c needing to duplicate any of loadFile()'s own parsing. */
LINK int doRelax;
LINK int rlxActive;
LINK char rlxCurOrigFile[1024];

/* Set (only) while relax.c's own discovery pass (an ordinary, unmodified
 * link run before any relaxation round) is in progress -- lets loadFile()
 * report, via rlxRecordDiscovered(), exactly which (library file, proc
 * name) pairs the real link actually pulled in, at the same two points it
 * already prints "Linking %s from library". This is how relax.c learns
 * which library procs are relaxation-eligible without loadFile()'s own
 * selective-inclusion logic needing to change at all. */
LINK int rlxDiscovering;
void rlxRecordDiscovered(char *libFile, char *procName);

/* Set by loadFile()'s '<' handler whenever a short-branch out-of-page
 * error is detected AND there's no way to recover from it -- either
 * because -r isn't active at all (no retry mechanism exists outside
 * relax.c), or because it's a genuine hand-written short branch rather
 * than one of relax.c's own shrink candidates (nothing to add to an
 * exclusion set for -- it was never a candidate). Previously such an
 * error only ever printed a message and link02 wrote output anyway,
 * silently producing a corrupted binary; checked by both main()'s
 * plain link path and relax.c's round loop, each aborting before any
 * output is written once this is set. */
LINK int shortBranchFatal;

/* Loadable-module output mode (-m). Appends a load-time fixup table
 * after the ordinary binary content: a flat list of high-byte offsets,
 * nothing else. Every fixup this linker already tracks resolves to a
 * 2-byte address written high-byte-first (-m output assumes big-endian
 * mode, -be) -- if the loader guarantees the module is always loaded at
 * a page-aligned address (load base's low byte is 0), adding the load
 * base to any embedded address only ever changes its high byte, so
 * that's the only thing worth recording. Low-byte-only fixups ('L'/
 * 'v') and short-branch ('<') targets never carry an absolute address
 * at all and are never recorded. This page-alignment assumption is the
 * loader's own contract, not something Link/02 itself enforces or
 * depends on beyond emitting this table -- whatever loads a -m module
 * at runtime is responsible for placing it on a page boundary and
 * adding its own load base's high byte to each recorded offset. */
LINK int moduleMode;
LINK word *moduleFixups;
LINK int numModuleFixups;
void addModuleFixup(word address);

/* -m output always patches the module's own total code+data size (2
 * bytes, big-endian, "highest - lowest + 1") into the output file at
 * this fixed offset, since that size is inherently self-referential --
 * nothing in the assembled source can know its own final linked size
 * ahead of time. Part of the -m format's own contract: any module
 * format that opts into -m must reserve these 2 bytes for it, at this
 * exact offset, for the loader to read back at load time. */
#define MODULE_SIZE_FIELD_OFFSET 4

FILE *findInputFile(char *filename, int isLibrary);
int loadFile(char *filename);
int doLink();
char *getHex(char *line, word *value);
int findSymbol(char *name);
word readMem(word address);
void writeMem(word address, word value);
void addReference(char *name, word value, char typ, byte low);

int runRelaxedLink();
void rlxRecordFailure(char *origFile, char *procName, word origOffset);

#endif
