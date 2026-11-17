//
// FILE            state.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// --state <file>: the patterns' state over a restart (doc/design.md § 6) - open matches, deadlines, window values.
// Written (patternsSave) by the engine's thread at most once a second when something changed, and when corCep stops
// (SIGTERM, SIGINT); read back (patternsLoad) at start, before the first rules are adopted.
//
// The cor format (corTree/corTreeBin.h - a tree, binary, names numbered by first appearance), behind 8 bytes:
// "corCepS" and a NUL. Written whole to <file>.tmp, flushed to the disk (fsync), renamed over <file>: a crash while
// writing leaves the last state written, never half of one.
//
#include <errno.h>                               // errno
#include <fcntl.h>                               // open
#include <stdio.h>                               // fopen, fread, rename, snprintf
#include <stdlib.h>                              // malloc, free
#include <string.h>                              // strerror, memcmp
#include <time.h>                                // clock_gettime
#include <unistd.h>                              // write, fsync, close

#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset
#include "corTree/corTreeBin.h"                  // corTreeBinEncode, corTreeBinDecode, CorBinTables
#include "corTree/corTreeBuilder.h"              // corTreeChildAdd

#include "corCep.h"



const char* stateFile = NULL;                    // --state; NULL: the state is not kept

static const char magic[8] = "corCepS";          // the file's first 8 bytes - the NUL included



// -----------------------------------------------------------------------------
//
// wallNow - the wall clock, ms since the epoch
//
static long long wallNow(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// stateLoad - the state file read into psP; none yet, or one that is not a state: the patterns start empty (logged)
//
void stateLoad(CepPatterns* psP)
{
  if (stateFile == NULL)
    return;

  FILE* fP = fopen(stateFile, "r");
  if (fP == NULL)
  {
    logLine("state: %s - %s, starting empty", stateFile, (errno == ENOENT) ? "none yet" : strerror(errno));
    return;
  }

  fseek(fP, 0, SEEK_END);
  long size = ftell(fP);
  fseek(fP, 0, SEEK_SET);

  char* text = (size > 0) ? malloc(size) : NULL;
  if ((text == NULL) || (fread(text, 1, size, fP) != (size_t) size))
  {
    logLine("state: %s - not read, starting empty", stateFile);
    fclose(fP);
    free(text);
    return;
  }
  fclose(fP);

  CorAlloc      alloc;
  CorBinTables  tables;
  const char*   error = NULL;
  CorNode*      topP  = NULL;
  int           n     = -1;

  corAllocBufferInit(&alloc, NULL, 0, 256 * 1024, NULL, "corCep state read");
  bool tablesUp = (size > (long) sizeof(magic)) && (memcmp(text, magic, sizeof(magic)) == 0) && corTreeBinTablesInit(&tables, 4096, 4096);

  if (tablesUp)                                  // the tables outlive the tree: a name sent once points into them
    topP = corTreeBinDecode(&text[sizeof(magic)], size - sizeof(magic), NULL, &tables, &alloc, &error);

  long long nowMono = nowMs();
  long long nowWall = wallNow();

  if (topP != NULL)
    n = patternsLoad(psP, topP, nowMono, nowWall);

  if (n < 0)
    logLine("state: %s - not a corCep state (version 1)%s%s, starting empty", stateFile, (error != NULL) ? ": " : "", (error != NULL) ? error : "");
  else
  {
    int steps = steppersLoad(topP, nowMono, nowWall);
    logLine("state: %s - %d rule%s' matches and deadlines, %d step%s read", stateFile, n, (n == 1) ? "" : "s", steps, (steps == 1) ? "" : "s");
  }

  if (tablesUp)
    corTreeBinTablesRelease(&tables);
  corAllocBufferReset(&alloc, false);
  free(text);
}



// -----------------------------------------------------------------------------
//
// stateSave - the state written; false: not (logged once until a write works again)
//
bool stateSave(CepPatterns* psP)
{
  static bool failing = false;

  if (stateFile == NULL)
    return true;

  CorAlloc      alloc;
  CorBinTables  tables;
  CorBinBuffer  out    = { NULL, 0, 0 };
  bool          ok     = false;

  corAllocBufferInit(&alloc, NULL, 0, 256 * 1024, NULL, "corCep state write");

  long long nowMono = nowMs();
  long long nowWall = wallNow();
  CorNode*  topP    = patternsSave(psP, &alloc, nowMono, nowWall);

  corTreeChildAdd(topP, steppersSave(&alloc, nowMono, nowWall));

  if (corTreeBinTablesInit(&tables, 4096, 4096))
  {
    ok = corTreeBinEncode(topP, NULL, &tables, &out);
    corTreeBinTablesRelease(&tables);
  }
  corAllocBufferReset(&alloc, false);

  if (ok == false)
  {
    free(out.buf);
    if (failing == false)
      logLine("state: %s - not written: out of memory", stateFile);
    failing = true;
    return false;
  }

  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s.tmp", stateFile);

  int  fd   = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  int  err  = (fd >= 0) ? 0 : errno;

  ok = (fd >= 0) && (write(fd, magic, sizeof(magic)) == (ssize_t) sizeof(magic));
  if ((fd >= 0) && (ok == false))
    err = errno;

  for (int done = 0; ok && (done < out.len); )
  {
    ssize_t n = write(fd, &out.buf[done], out.len - done);
    if (n <= 0)
    {
      err = errno;
      ok  = false;
    }
    else
      done += n;
  }

  if (ok && (fsync(fd) != 0))
  {
    err = errno;
    ok  = false;
  }
  if (fd >= 0)
    close(fd);
  if (ok && (rename(tmp, stateFile) != 0))
  {
    err = errno;
    ok  = false;
  }

  free(out.buf);

  if ((ok == false) && (failing == false))
    logLine("state: %s - not written: %s", stateFile, strerror(err));
  else if (ok && failing)
    logLine("state: %s - written again", stateFile);
  failing = !ok;

  return ok;
}
