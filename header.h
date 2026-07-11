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
