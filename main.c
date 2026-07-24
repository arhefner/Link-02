#define MAIN

#include "header.h"

#define NAME_AND_VERSION  "Link/02 v1.9"

struct tm *localtime_r(const time_t *timer, struct tm *buf)
{
  if (buf != NULL)
  {
    memcpy(buf, localtime(timer), sizeof(struct tm));
  }

  return buf;
}

char *getHex(char *line, word *value) {
  *value = 0;
  while ((*line >= '0' && *line <= '9') || (*line >= 'a' && *line <= 'f') ||
         (*line >= 'A' && *line <= 'F')) {
    if (*line >= '0' && *line <= '9')
      *value = (*value << 4) + (*line - '0');
    else if (*line >= 'a' && *line <= 'f')
      *value = (*value << 4) + (*line - 87);
    else if (*line >= 'A' && *line <= 'F')
      *value = (*value << 4) + (*line - 65);
    line++;
  }
  return line;
}

int findSymbol(char *name) {
  int i;
  for (i = 0; i < numSymbols; i++)
    if (strcmp(symbols[i], name) == 0) return i;
  return -1;
}

word readMem(word address) {
  word ret;
  if (addressMode == 'L')
    ret = memory[address] + (memory[address + 1] << 8);
  else
    ret = memory[address + 1] + (memory[address] << 8);
  return ret;
}

void writeMem(word address, word value) {
  if (addressMode == 'L') {
    memory[address] = value & 0xff;
    memory[address + 1] = (value >> 8) & 0xff;
  } else {
    memory[address + 1] = value & 0xff;
    memory[address] = (value >> 8) & 0xff;
  }
  map[address] = 1;
  map[address + 1] = 1;
}

void addReference(char *name, word value, char typ, byte low) {
  numReferences++;
  if (numReferences == 1) {
    references = (char **)malloc(sizeof(char *));
    addresses = (word *)malloc(sizeof(word));
    types = (char *)malloc(sizeof(char));
    lows = (byte *)malloc(sizeof(byte));
  } else {
    references = (char **)realloc(references, sizeof(char *) * numReferences);
    addresses = (word *)realloc(addresses, sizeof(word) * numReferences);
    types = (char *)realloc(types, sizeof(char) * numReferences);
    lows = (byte *)realloc(lows, sizeof(byte) * numReferences);
  }
  references[numReferences - 1] = (char *)malloc(strlen(name) + 1);
  strcpy(references[numReferences - 1], name);
  addresses[numReferences - 1] = value;
  lows[numReferences - 1] = low;
  types[numReferences - 1] = typ;
}

void addLibrary(char *name) {
  numLibraries++;
  if (numLibraries == 1)
    libraries = (char **)malloc(sizeof(char *));
  else
    libraries = (char **)realloc(libraries, sizeof(char *) * numLibraries);
  libraries[numLibraries - 1] = (char *)malloc(strlen(name) + 1);
  strcpy(libraries[numLibraries - 1], name);
}

void addObject(char *name) {
  numObjects++;
  if (numObjects == 1)
    objects = (char **)malloc(sizeof(char *));
  else
    objects = (char **)realloc(objects, sizeof(char *) * numObjects);
  objects[numObjects - 1] = (char *)malloc(strlen(name) + 1);
  strcpy(objects[numObjects - 1], name);
}

word adjust(word address, char *bound) {
  if (strncmp(bound, "word", 4) == 0) {
    address = (address + 1) & 0xfffe;
  } else if (strncmp(bound, "dword", 5) == 0) {
    address = (address + 3) & 0xfffc;
  } else if (strncmp(bound, "qword", 5) == 0) {
    address = (address + 7) & 0xfff8;
  } else if (strncmp(bound, "para", 4) == 0) {
    address = (address + 15) & 0xfff0;
  } else if (strncmp(bound, "32", 2) == 0) {
    address = (address + 31) & 0xffe0;
  } else if (strncmp(bound, "64", 2) == 0) {
    address = (address + 63) & 0xffc0;
  } else if (strncmp(bound, "128", 3) == 0) {
    address = (address + 127) & 0xff80;
  } else if (strncmp(bound, "page", 4) == 0) {
    address = (address + 255) & 0xff00;
  } else {
    printf("Error: Unrecognized alignment: %s\n", bound);
  }

  return address;
}

/* findInputFile: search for 'filename' the exact way loadFile() has
 * always searched for one -- first as given (relative to the current
 * directory), then via the -L library search path (isLibrary != 0)
 * or the -I include/object search path (isLibrary == 0), in the
 * order each -I/-L was given on the command line. Split out of
 * loadFile() itself (2026-07-23) so rlxParseFile() (relax.c, used for
 * a -r relaxation pass) can search the same paths for an object file
 * instead of only ever trying the current directory -- previously a
 * -r build failed with "Could not open input file" for any object
 * file that wasn't in the current directory, even though a plain
 * (non-relaxed) build of the exact same command line found it fine
 * via -I. Deliberately does NOT print any error message itself: the
 * two real messages ("Could not open library file: %s" / "Could not
 * open input file: %s") differ by caller, and rlxParseFile() is only
 * ever called for object files (isLibrary always 0 there -- see its
 * own header comment; a -r pass still loads *library* files via a
 * direct loadFile() call in rlxLinkOnce(), which already searches
 * libPath correctly, so this fix's real scope is object files only).
 * Returns an open FILE*, or NULL if every attempt failed.
 */
FILE *findInputFile(char *filename, int isLibrary) {
  char buffer[1024];
  char path[2048];
  FILE *file;
  int i;

  file = fopen(filename, "r");
  if (file != NULL) return file;

  if (isLibrary) {
    strcpy(buffer, LIBPATH);
    strcat(buffer, filename);
    file = fopen(buffer, "r");
    if (file != NULL) return file;

    for (i = 0; i < numLibPath; i++) {
      strcpy(path, libPath[i]);
      if (path[strlen(path) - 1] != '/') strcat(path, "/");
      strcat(path, filename);
      file = fopen(path, "r");
      if (file != NULL) return file;
    }
  } else {
    for (i = 0; i < numIncPath; i++) {
      strcpy(path, incPath[i]);
      if (path[strlen(path) - 1] != '/') strcat(path, "/");
      strcat(path, filename);
      file = fopen(path, "r");
      if (file != NULL) return file;
    }
  }

  return NULL;
}

int loadFile(char *filename) {
  int i;
  int j;
  char buffer[1024];
  char token[256];
  int pos;
  int flag;
  FILE *file;
  word value;
  word addr;
  word lofs;
  word low;
  char *line;

  //grw - suppress linking messages when creating sym file
  if (!quiet && !createSym && (libScan == 0)) printf("Linking: %s\n", filename);
  inProc = 0;
  offset = 0;
  file = findInputFile(filename, libScan != 0);
  if (file == NULL) {
    if (libScan != 0) {
      printf("Could not open library file: %s\n", filename);
    } else {
      printf("Could not open input file: %s\n", filename);
    }
    return -1;
  }
  while (fgets(buffer, 1023, file) != NULL) {
    line = buffer;
    if (strncmp(line, ".big", 4) == 0)
      addressMode = 'B';
    else if (strncmp(line, ".little", 7) == 0)
      addressMode = 'L';
    else if (strncmp(line, ".align ", 7) == 0) {
      line += 7;
      while (*line == ' ') line++;
      if (inProc) {
        offset = adjust(offset, line);
      } else {
        address = adjust(address, line);
      }
    } else if (strncmp(line, ".ver", 4) == 0) {
      address -= 4;
      memory[address++] = buildMonth;
      memory[address++] = buildDay;
      memory[address++] = (buildYear >> 8) & 0xff;
      memory[address++] = buildYear & 0xff;
    } else if (strncmp(line, ".ever", 5) == 0) {
      address -= 6;
      memory[address++] = buildMonth | 0x80;
      memory[address++] = buildDay;
      memory[address++] = (buildYear >> 8) & 0xff;
      memory[address++] = buildYear & 0xff;
      memory[address++] = (buildNumber >> 8) & 0xff;
      memory[address++] = buildNumber & 0xff;
    } else if (strncmp(line, ".eever", 6) == 0) {
      address -= 9;
      memory[address++] = buildMonth | 0xc0;
      memory[address++] = buildDay;
      memory[address++] = (buildYear >> 8) & 0xff;
      memory[address++] = buildYear & 0xff;
      memory[address++] = buildHour;
      memory[address++] = buildMinute;
      memory[address++] = buildSecond;
      memory[address++] = (buildNumber >> 8) & 0xff;
      memory[address++] = buildNumber & 0xff;
    } else if (strncmp(line, ".library ", 9) == 0) {
      line += 9;
      while (*line == ' ') line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      addLibrary(token);
    } else if (strncmp(line, ".requires ", 10) == 0 && loadModule != 0) {
      line += 10;
      while (*line == ' ') line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      flag = 0;
      for (i = 0; i < numRequires; i++)
        if (strcmp(token, requires[i]) == 0) flag = -1;
      for (i = 0; i < numSymbols; i++)
        if (strcmp(token, symbols[i]) == 0) {
          flag = -1;
        }
      if (flag == 0) {
        numRequires++;
        if (numRequires == 1) {
          requires = (char **)malloc(sizeof(char *));
          requireAdded = (char *)malloc(sizeof(char));
        } else {
          requires = (char **)realloc(requires, sizeof(char *) * numRequires);
          requireAdded =
              (char *)realloc(requireAdded, sizeof(char) * numRequires);
        }
        requires[numRequires - 1] = (char *)malloc(strlen(token) + 1);
        strcpy(requires[numRequires - 1], token);
        requireAdded[numRequires - 1] = 'N';
      }
    } else if (*line == '>' && loadModule != 0) {
      line++;
      line = getHex(line, &value);
      address += value;
    } else if (*line == ':' && loadModule != 0) {
      line++;
      line = getHex(line, &address);
      if (inProc) address += offset;
      while (*line != 0) {
        while (*line > 0 && *line <= ' ') line++;
        if (*line != 0) {
          line = getHex(line, &value);
          if (address < lowest) lowest = address;
          if (address > highest) highest = address;
          if (map[address] != 0) {
            printf("Error: Collision at %04x\n", address);
          }
          memory[address] = value & 0xff;
          map[address++] = 1;
        }
      }
    } else if (*line == '@') {
      line = buffer + 1;
      getHex(line, &startAddress);
    } else if ((*line == '+' || *line == '#') && loadModule != 0) {
      /* '#' (local long-branch operand, tagged by Asm/02's OT_LBR case) is
       * resolved identically to plain '+' whenever -r doesn't intervene
       * to shrink it first -- both are a full-word local target needing
       * the proc base added in. */
      line++;
      line = getHex(line, &addr);
      value = readMem(addr + offset);
      value += offset;
      writeMem(addr + offset, value);
    } else if (*line == '^' && loadModule != 0) {
      line++;
      line = getHex(line, &addr);
      value = (memory[addr + offset] << 8) + offset;
      while (*line == ' ') line++;
      getHex(line, &lofs);
      value += lofs;
      memory[addr + offset] = (value >> 8) & 0xff;
    } else if (*line == 'v' && loadModule != 0) {
      line++;
      line = getHex(line, &addr);
      value = memory[addr + offset] + offset;
      memory[addr + offset] = value & 0xff;
    } else if (*line == '<' && loadModule != 0) {
      line++;
      line = getHex(line, &addr);
      if (rlxActive) {
        /* Relaxation-generated '<' lines carry the FULL (unmasked)
         * proc-relative target as an explicit field, followed by the
         * original proc name and original, pre-shrink offset (for
         * failure reporting). This is NOT optional the way the trailing
         * identity fields are: a single stored byte can only ever
         * represent a target whose own local offset is < 256, so
         * memory[addr+offset] alone (the plain, non-relax '<' path
         * below) cannot even correctly VALIDATE a target from a large
         * proc, let alone resolve it -- the high bits of the true
         * target are simply gone once they've been masked into one
         * byte. Reading the full value back out of the text sidesteps
         * that loss entirely. */
        word fullTarget;
        char origProc[128];
        word origOff;
        int p = 0;
        while (*line == ' ') line++;
        line = getHex(line, &fullTarget);
        value = fullTarget + offset;
        if (((addr + offset) & 0xff00) != (value & 0xff00)) {
          while (*line == ' ') line++;
          while (*line != 0 && *line > ' ') origProc[p++] = *line++;
          origProc[p] = 0;
          while (*line == ' ') line++;
          if (p > 0 && *line != 0) {
            /* Expected during relaxation iteration -- this exact branch
             * will be excluded and the round retried, so no per-branch
             * "Error:" here; printing one made routine iteration look
             * like a failing link. runRelaxedLink()'s own per-round
             * summary (and, if it ever comes to that, the "did not
             * converge" message) already carries the real signal. */
            getHex(line, &origOff);
            rlxRecordFailure(rlxCurOrigFile, origProc, origOff);
          } else {
            /* No original-branch identity to record -- this '<' wasn't
             * one of relax.c's own shrink candidates (a genuine hand-
             * written short branch instead), so there's nothing an
             * exclude-and-retry round could do about it. Fatal --
             * surface it, unlike the expected "will retry" case above. */
            printf("Error: Short branch out of page at %04x\n", addr + offset);
            shortBranchFatal = 1;
          }
        }
      } else {
        value = memory[addr + offset] + offset;
        if (((addr + offset) & 0xff00) != (value & 0xff00)) {
          printf("Error: Short branch out of page at %04x\n", addr + offset);
          shortBranchFatal = 1;
        }
      }
      memory[addr + offset] = value & 0xff;
    } else if (*line == '=' && loadModule != 0) {
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      while (*line == ' ') line++;
      getHex(line, &value);
      if (inProc) value += offset;
      for (i = 0; i < numSymbols; i++)
        if (strcmp(token, symbols[i]) == 0) {
          printf("Error: Duplicate symbol: %s\n", token);
          fclose(file);
          return -1;
        }
      numSymbols++;
      if (numSymbols == 1) {
        symbols = (char **)malloc(sizeof(char *));
        values = (word *)malloc(sizeof(word));
      } else {
        symbols = (char **)realloc(symbols, sizeof(char *) * numSymbols);
        values = (word *)realloc(values, sizeof(word) * numSymbols);
      }
      symbols[numSymbols - 1] = (char *)malloc(strlen(token) + 1);
      strcpy(symbols[numSymbols - 1], token);
      values[numSymbols - 1] = value;
      for (i = 0; i < numRequires; i++)
        if (strcmp(token, requires[i]) == 0) {
          requireAdded[i] = 'Y';
        }
    } else if ((*line == '?' || *line == '!') && loadModule != 0) {
      /* '!' (external-target long-branch operand, tagged by Asm/02's
       * OT_LBR case) is resolved identically to plain '?' -- both are a
       * full-word external reference. '!' branches are never shrunk by
       * -r (see relax.c's header comment), so this is the only path
       * that ever resolves them. */
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      while (*line == ' ') line++;
      getHex(line, &value);
      if (inProc) value += offset;
      addReference(token, value, 'W', 0);
    } else if (*line == '/' && loadModule != 0) {
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      while (*line == ' ') line++;
      line = getHex(line, &value);
      if (inProc) value += offset;
      while (*line == ' ') line++;
      getHex(line, &low);
      addReference(token, value, 'H', low & 0xff);
    } else if (*line == '\\' && loadModule != 0) {
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      while (*line == ' ') line++;
      getHex(line, &value);
      if (inProc) value += offset;
      addReference(token, value, 'L', 0);
    } else if (*line == '{') {
      line++;
      pos = 0;
      while (*line != 0 && *line > ' ') token[pos++] = *line++;
      token[pos] = 0;
      if (libScan != 0) {
        int s;
        s = findSymbol(token);
        if (s == -1) {
          for (i = 0; i < numReferences; i++)
            if (strcmp(references[i], token) == 0) {
              loadModule = -1;
              //grw - suppress linking messages when creating sym
              // file
              if (!createSym) printf("Linking %s from library\n", token);
              i = numReferences;
            }
          if (loadModule == 0) {
            for (i = 0; i < numRequires; i++)
              if (requireAdded[i] == 'N' && strcmp(requires[i], token) == 0) {
                loadModule = -1;
                requireAdded[i] = 'Y';
                //grw - suppress linking messages when creating
                // sym file
                if (!createSym) printf("Linking %s from library\n", token);
              }
          }
        }
      }
      if (loadModule != 0) {
        value = address;
        for (i = 0; i < numSymbols; i++)
          if (strcmp(token, symbols[i]) == 0) {
            printf("Error: Duplicate symbol: %s\n", token);
            fclose(file);
            return -1;
          }
        inProc = -1;
        offset = address;
        numSymbols++;
        if (numSymbols == 1) {
          symbols = (char **)malloc(sizeof(char *));
          values = (word *)malloc(sizeof(word));
        } else {
          symbols = (char **)realloc(symbols, sizeof(char *) * numSymbols);
          values = (word *)realloc(values, sizeof(word) * numSymbols);
        }
        symbols[numSymbols - 1] = (char *)malloc(strlen(token) + 1);
        strcpy(symbols[numSymbols - 1], token);
        values[numSymbols - 1] = value;
        for (i = 0; i < numRequires; i++)
          if (strcmp(token, requires[i]) == 0) {
            requireAdded[i] = 'Y';
          }
      }
    } else if (*line == '}') {
      inProc = 0;
      offset = 0;
      if (libScan != 0) loadModule = 0;
    }
  }
  fclose(file);
  return 0;
}

int doLink() {
  int i;
  int j;
  int s;
  int errors;
  word address;
  word v;
  int resolved;
  errors = 0;
  resolved = 0;
  i = 0;
  //  for (i=0; i<numReferences; i++) {
  while (i < numReferences) {
    s = findSymbol(references[i]);
    if (s < 0) {
      i++;
    } else {
      resolved++;
      address = addresses[i];
      if (types[i] == 'W') {
        v = readMem(address) + values[s];
        writeMem(address, v);
      }
      if (types[i] == 'H') {
        v = ((memory[address] << 8) + values[s] + lows[i]) >> 8;
        memory[address] = v & 0xff;
      }
      if (types[i] == 'L') {
        v = memory[address] + values[s];
        memory[address] = v & 0xff;
      }
      free(references[i]);
      for (j = i; j < numReferences - 1; j++) {
        references[j] = references[j + 1];
        addresses[j] = addresses[j + 1];
        types[j] = types[j + 1];
        lows[j] = lows[j + 1];
      }
      numReferences--;
      if (numReferences > 0) {
        references =
            (char **)realloc(references, sizeof(char *) * numReferences);
        addresses = (word *)realloc(addresses, sizeof(word) * numReferences);
        types = (char *)realloc(types, sizeof(char) * numReferences);
        lows = (byte *)realloc(lows, sizeof(byte) * numReferences);
      } else {
        free(references);
        free(addresses);
        free(types);
        free(lows);
      }
    }
  }
  return resolved;
}

void outputBinary() {
  int file;
  file = open(outName, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
  write(file, memory + lowest, (highest - lowest) + 1);
  close(file);
}

void outputElfos() {
  int file;
  word load;
  word size;
  word exec;
  char header[6];
  exec = startAddress;
  load = lowest;
  size = (highest - lowest) + 1;
  header[0] = (load >> 8) & 0xff;
  header[1] = load & 0xff;
  header[2] = (size >> 8) & 0xff;
  header[3] = size & 0xff;
  header[4] = (exec >> 8) & 0xff;
  header[5] = exec & 0xff;
  file = open(outName, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
  write(file, header, 6);
  write(file, memory + lowest, (highest - lowest) + 1);
  close(file);
}

void outputIntel() {
  int i;
  FILE *file;
  byte buffer[16];
  char line[256];
  char tmp[5];
  byte count;
  word outAddress;
  word checksum;
  int address;
  int high;
  address = lowest;
  high = highest;
  outAddress = lowest;
  count = 0;
  file = fopen(outName, "w");
  while (address <= high) {
    if (map[address] == 1) {
      if (count == 0) outAddress = address;
      buffer[count++] = memory[address];
      if (count == 16) {
        strcpy(line, ":");
        sprintf(tmp, "%02x", count);
        strcat(line, tmp);
        checksum = count;
        sprintf(tmp, "%04x", outAddress);
        strcat(line, tmp);
        checksum += ((outAddress >> 8) & 0xff);
        checksum += (outAddress & 0xff);
        strcat(line, "00");
        for (i = 0; i < count; i++) {
          sprintf(tmp, "%02x", buffer[i]);
          strcat(line, tmp);
          checksum += buffer[i];
        }
        checksum = (checksum ^ 0xffff) + 1;
        sprintf(tmp, "%02x", checksum & 0xff);
        strcat(line, tmp);
        fprintf(file, "%s\n", line);
        count = 0;
      }
    } else if (count > 0) {
      strcpy(line, ":");
      sprintf(tmp, "%02x", count);
      strcat(line, tmp);
      checksum = count;
      sprintf(tmp, "%04x", outAddress);
      strcat(line, tmp);
      checksum += ((outAddress >> 8) & 0xff);
      checksum += (outAddress & 0xff);
      strcat(line, "00");
      for (i = 0; i < count; i++) {
        sprintf(tmp, "%02x", buffer[i]);
        strcat(line, tmp);
        checksum += buffer[i];
      }
      checksum = (checksum ^ 0xffff) + 1;
      sprintf(tmp, "%02x", checksum & 0xff);
      strcat(line, tmp);
      fprintf(file, "%s\n", line);
      count = 0;
    }
    address++;
  }
  if (count > 0) {
    strcpy(line, ":");
    sprintf(tmp, "%02x", count);
    strcat(line, tmp);
    checksum = count;
    sprintf(tmp, "%04x", outAddress);
    strcat(line, tmp);
    checksum += ((outAddress >> 8) & 0xff);
    checksum += (outAddress & 0xff);
    strcat(line, "00");
    for (i = 0; i < count; i++) {
      sprintf(tmp, "%02x", buffer[i]);
      strcat(line, tmp);
      checksum += buffer[i];
    }
    checksum = (checksum ^ 0xffff) + 1;
    sprintf(tmp, "%02x", checksum & 0xff);
    strcat(line, tmp);
    fprintf(file, "%s\n", line);
    count = 0;
  }
  if (startAddress != 0xffff) {
    sprintf(line, ":040000050000%02x%02x", (startAddress >> 8) & 0xff,
            startAddress & 0xff);
    checksum = 4 + 5 + ((startAddress >> 8) & 0xff) + (startAddress & 0xff);
    checksum = (checksum ^ 0xffff) + 1;
    sprintf(tmp, "%02x", checksum & 0xff);
    strcat(line, tmp);
    fprintf(file, "%s\n", line);
  }
  fprintf(file, ":00000001FF\n");
  fclose(file);
}

void outputRcs() {
  int i;
  FILE *file;
  byte buffer[16];
  char line[256];
  char tmp[5];
  byte count;
  word outAddress;
  int address;
  int high;
  address = lowest;
  high = highest;
  outAddress = lowest;
  count = 0;
  file = fopen(outName, "w");
  while (address <= high) {
    if (map[address] == 1) {
      if (count == 0) outAddress = address;
      buffer[count++] = memory[address];
      if (count == 16) {
        strcpy(line, ":");
        sprintf(tmp, "%04x", outAddress);
        strcat(line, tmp);
        for (i = 0; i < count; i++) {
          sprintf(tmp, " %02x", buffer[i]);
          strcat(line, tmp);
        }
        fprintf(file, "%s\n", line);
        count = 0;
      }
    } else if (count > 0) {
      strcpy(line, ":");
      sprintf(tmp, "%04x", outAddress);
      strcat(line, tmp);
      for (i = 0; i < count; i++) {
        sprintf(tmp, " %02x", buffer[i]);
        strcat(line, tmp);
      }
      fprintf(file, "%s\n", line);
      count = 0;
    }
    address++;
  }
  if (count > 0) {
    strcpy(line, ":");
    sprintf(tmp, "%04x", outAddress);
    strcat(line, tmp);
    for (i = 0; i < count; i++) {
      sprintf(tmp, " %02x", buffer[i]);
      strcat(line, tmp);
    }
    fprintf(file, "%s\n", line);
    count = 0;
  }
  if (startAddress != 0xffff) {
    sprintf(line, "@%04x", startAddress);
    fprintf(file, "%s\n", line);
  }
  fclose(file);
}

void readControlFile(char *filename) {
  FILE *file;
  char line[1024];
  char *pchar;
  file = fopen(filename, "r");
  if (file == NULL) {
    printf("Could not open %s\n", filename);
    exit(1);
  }
  while ((pchar = fgets(line, 1023, file)) != NULL) {
    while (strlen(line) > 0 && line[strlen(line) - 1] <= ' ')
      line[strlen(line) - 1] = 0;
    if (strncasecmp(line, "mode ", 5) == 0) {
      pchar = line + 5;
      while (*pchar == ' ') pchar++;
      if (strcasecmp(pchar, "binary") == 0) outMode = BM_BINARY;
      if (strcasecmp(pchar, "cmd") == 0) outMode = BM_CMD;
      if (strcasecmp(pchar, "elfos") == 0) outMode = BM_ELFOS;
      if (strcasecmp(pchar, "intel") == 0) outMode = BM_INTEL;
      if (strcasecmp(pchar, "rcs") == 0) outMode = BM_RCS;
      if (strcasecmp(pchar, "big") == 0) addressMode = 'B';
      if (strcasecmp(pchar, "little") == 0) addressMode = 'L';
    }
    if (strncasecmp(line, "output ", 7) == 0) {
      pchar = line + 7;
      while (*pchar == ' ') pchar++;
      strcpy(outName, pchar);
    }
    if (strncasecmp(line, "add ", 4) == 0) {
      pchar = line + 4;
      addObject(pchar);
    }
    if (strncasecmp(line, "library ", 8) == 0) {
      pchar = line + 8;
      addLibrary(pchar);
    }
  }
}

void sortSymbols() {
  char flag;
  int i;
  word t;
  char *c;
  flag = 1;
  while (flag == 1) {
    flag = 0;
    for (i = 0; i < numSymbols - 1; i++)
      if (values[i] > values[i + 1]) {
        c = symbols[i];
        symbols[i] = symbols[i + 1];
        symbols[i + 1] = c;
        t = values[i];
        values[i] = values[i + 1];
        values[i + 1] = t;
        flag = 1;
      }
  }
}

int main(int argc, char **argv) {
  int i;
  time_t tv;
  struct tm dt;
  char *pchar;
  int resolved;
  char tmp[1024];
  char buffer[33];
  FILE *buildFile;
  lowest = 0xffff;
  highest = 0x0000;
  numObjects = 0;
  startAddress = 0xffff;
  showSymbols = 0;
  quiet = 0;
  createSym = 0;
  numSymbols = 0;
  numReferences = 0;
  numLibraries = 0;
  numRequires = 0;
  addressMode = 'L';
  numLibPath = 0;
  numIncPath = 0;
  doRelax = 0;
  rlxActive = 0;
  shortBranchFatal = 0;
  tv = time(NULL);
  localtime_r(&tv, &dt);
  buildMonth = dt.tm_mon + 1;
  buildDay = dt.tm_mday;
  buildYear = dt.tm_year + 1900;
  buildHour = dt.tm_hour;
  buildMinute = dt.tm_min;
  buildSecond = dt.tm_sec;

  strcpy(outName, "");
  outMode = BM_BINARY;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-b") == 0)
      outMode = BM_BINARY;
    else if (strcmp(argv[i], "-c") == 0)
      outMode = BM_CMD;
    else if (strcmp(argv[i], "-e") == 0)
      outMode = BM_ELFOS;
    else if (strcmp(argv[i], "-i") == 0)
      outMode = BM_INTEL;
    else if (strcmp(argv[i], "-h") == 0)
      outMode = BM_RCS;
    else if (strcmp(argv[i], "-q") == 0)
      quiet = -1;
    else if (strcmp(argv[i], "-s") == 0)
      showSymbols = -1;
    else if (strcmp(argv[i], "-be") == 0)
      addressMode = 'B';
    else if (strcmp(argv[i], "-le") == 0)
      addressMode = 'L';
    else if (strcmp(argv[i], "-r") == 0)
      doRelax = -1;
    else if (strcmp(argv[i], "-I") == 0) {
      i++;
      numIncPath++;
      if (numIncPath == 1)
        incPath = (char **)malloc(sizeof(char *));
      else
        incPath = (char **)realloc(incPath, sizeof(char *) * numIncPath);
      incPath[numIncPath - 1] = (char *)malloc(strlen(argv[i]) + 1);
      strcpy(incPath[numIncPath - 1], argv[i]);
    } else if (strcmp(argv[i], "-L") == 0) {
      i++;
      numLibPath++;
      if (numLibPath == 1)
        libPath = (char **)malloc(sizeof(char *));
      else
        libPath = (char **)realloc(libPath, sizeof(char *) * numLibPath);
      libPath[numLibPath - 1] = (char *)malloc(strlen(argv[i]) + 1);
      strcpy(libPath[numLibPath - 1], argv[i]);
    } else if (strcmp(argv[i], "-o") == 0) {
      i++;
      strcpy(outName, argv[i]);
    } else if (argv[i][0] == '@') {
      readControlFile(argv[i] + 1);
    } else if (strcmp(argv[i], "-l") == 0) {
      i++;
      addLibrary(argv[i]);
    } else if (strcmp(argv[i], "-v") == 0) {
      printf("%s\n", NAME_AND_VERSION);
      printf("by Michael H. Riley\n");
      printf("with contributions by:\n");
      printf("  Tony Hefner\n");
      printf("  Gaston Williams\n");
      exit(1);
    }
    //grw - add option for Symbol file
    else if (strcmp(argv[i], "-S") == 0) {
      createSym = -1;
    } else {
      addObject(argv[i]);
    }
  }

  if (!quiet) {
    printf("%s\n", NAME_AND_VERSION);
  }

  if (numObjects == 0) {
    printf("No object files specified\n");
    exit(1);
  }
  if (strlen(outName) == 0) {
    strcpy(outName, objects[0]);
    pchar = strchr(outName, '.');
    if (pchar != NULL) *pchar = 0;
    if (outMode == BM_BINARY) strcat(outName, ".bin");
    if (outMode == BM_CMD) strcat(outName, ".cmd");
    if (outMode == BM_ELFOS) strcat(outName, ".elfos");
    if (outMode == BM_INTEL) strcat(outName, ".intel");
    if (outMode == BM_RCS) strcat(outName, ".hex");
  }

  strcpy(tmp, outName);
  pchar = strchr(tmp, '.');
  if (pchar != NULL) *pchar = 0;
  strcat(tmp, ".lkb");
  buildFile = fopen(tmp, "r");
  if (buildFile == NULL)
  {
    buildNumber = 1;
  }
  else
  {
    fgets(buffer, 32, buildFile);
    buildNumber = atoi(buffer) + 1;
    fclose(buildFile);
  }
  buildFile = fopen(tmp, "w");
  fprintf(buildFile, "%d\n", buildNumber);
  fclose(buildFile);

  //grw - create symbol file name from outName
  if (createSym) {
    //grw - symName is up to 64 characters including ".sym"
    if (strlen(outName) > 60) {
      strncpy(symName, outName, 60);
      symName[60] = 0;
    } else
      strcpy(symName, outName);

    for (i = 0; i < strlen(symName); i++)
      if (symName[i] == '.') symName[i] = 0;
    strcat(symName, ".sym");
  }
  for (i = 0; i < 65536; i++) {
    memory[i] = 0;
    map[i] = 0;
  }
  address = 0;
  libScan = 0;
  loadModule = -1;
  if (doRelax) {
    /* Branch relaxation: relax.c drives its own load/doLink rounds
     * internally (see runRelaxedLink()'s header comment) and leaves the
     * global link state exactly as if the block below had run once,
     * successfully, on the final (all in-range) rewritten text. */
    runRelaxedLink();
  } else {
  for (i = 0; i < numObjects; i++) {
    if (loadFile(objects[i]) < 0) {
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
  }
  if (shortBranchFatal) {
    printf("Errors during link.  Aborting output\n");
    exit(1);
  }
  if (numReferences > 0) {
    for (i = 0; i < numReferences; i++) {
      printf("Error: Symbol %s not found\n", references[i]);
    }
    printf("Errors during link.  Aborting output\n");
    exit(1);
  } else {
    if (!quiet) {
      printf("Writing: %s\n", outName);
    }
    switch (outMode) {
      case BM_BINARY:
        outputBinary();
        break;
      case BM_ELFOS:
        outputElfos();
        break;
      case BM_INTEL:
        outputIntel();
        break;
      case BM_RCS:
        outputRcs();
        break;
    }
  }

  if (!quiet) {
    printf("Lowest address : %04x\n", lowest);
    printf("Highest address: %04x\n", highest);
    printf("Public symbols : %d\n", numSymbols);
    if (startAddress != 0xffff) printf("Start address  : %04x\n", startAddress);
  }

  if (showSymbols) {
    sortSymbols();
    for (i = 0; i < numSymbols; i++)
      printf("%-20s %04x\n", symbols[i], values[i]);
  }

  if (createSym) {
    symFile = fopen(symName, "w");
    if (symFile != NULL) {
      sortSymbols();
      for (i = 0; i < numSymbols; i++)
        fprintf(symFile, "%-20s %04x\n", symbols[i], values[i]);
      fclose(symFile);
    } else
      printf("Error opening symbol map file %s.\n", symName);
  }

  return 0;
}
