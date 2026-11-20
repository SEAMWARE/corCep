//
// FILE            state.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// --state <file>: the patterns' state over a restart (doc/design.md § 6) - open matches, deadlines, window values.
// Written (patternsSave) by the engine's thread at most once a second when something changed, and when corCep stops
// (SIGTERM, SIGINT); read back at start - each tenant's (patternsLoad) when the tenant is made, before it adopts the
// rules. Version 2: a section per tenant; a state of corCep 0.1 (version 1, one tenant) is read as its tenant's.
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
#include "corTree/corTreeLookup.h"               // corTreeLookup

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
// What stateLoad read - kept till each tenant is made (stateLoadTenant): a tenant is known only once the broker
// lists it. The tables outlive the tree: a name sent once points into them.
//
static CorAlloc      loadAlloc;
static CorBinTables  loadTables;
static bool          loadUp   = false;
static char*         loadText = NULL;
static CorNode*      loadTopP = NULL;            // version 2: { version, tenants: [ { tenant, <patternsSave> } ], steps }; 1: 0.1's
static int           loadSteps = 0;



// -----------------------------------------------------------------------------
//
// stateLegacyTenant - the tenant a state of corCep 0.1 (one tenant, not named in it) was of
//
const char* stateLegacyTenant(void)
{
  return (cep.single && (cep.rulesTenant != NULL)) ? cep.rulesTenant : "";
}



// -----------------------------------------------------------------------------
//
// stateLoad - the state file read; its steps kept for the rules (steppersLoad), each tenant's patterns for when it
// is made. None yet, or one that is not a state: the patterns start empty (logged)
//
void stateLoad(void)
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

  const char* error = NULL;
  CorNode*    topP  = NULL;

  corAllocBufferInit(&loadAlloc, NULL, 0, 256 * 1024, NULL, "corCep state read");
  loadUp   = (size > (long) sizeof(magic)) && (memcmp(text, magic, sizeof(magic)) == 0) && corTreeBinTablesInit(&loadTables, 4096, 4096);
  loadText = text;

  if (loadUp)
    topP = corTreeBinDecode(&text[sizeof(magic)], size - sizeof(magic), NULL, &loadTables, &loadAlloc, &error);

  CorNode* versionP = (topP != NULL) ? corTreeLookup(topP, "version") : NULL;
  long long version = ((versionP != NULL) && (versionP->type == CorInt)) ? versionP->value.i : 0;

  if ((version != 1) && (version != 2))
  {
    logLine("state: %s - not a corCep state (version 1 or 2)%s%s, starting empty", stateFile, (error != NULL) ? ": " : "", (error != NULL) ? error : "");
    stateRelease();
    return;
  }

  loadTopP  = topP;
  loadSteps = steppersLoad(topP, nowMs(), wallNow());
  if (cep.single == false)                       // --tenant: one line, once its tenant is read (stateLoadTenant)
    logLine("state: %s - read%s, %d step%s", stateFile, (version == 1) ? " (corCep 0.1's)" : "", loadSteps, (loadSteps == 1) ? "" : "s");
}



// -----------------------------------------------------------------------------
//
// stateLoadTenant - what the state file had for <tenant>, into psP; the number of rules read (0: nothing)
//
int stateLoadTenant(CepPatterns* psP, const char* tenant)
{
  CorNode* entryP = NULL;

  if (loadTopP == NULL)
    return 0;

  CorNode* tenantsP = corTreeLookup(loadTopP, "tenants");

  if (tenantsP == NULL)                          // 0.1's: one tenant's
    entryP = (strcmp(tenant, stateLegacyTenant()) == 0) ? loadTopP : NULL;
  else if (tenantsP->type == CorArray)
  {
    for (CorNode* eP = tenantsP->value.head; (eP != NULL) && (entryP == NULL); eP = eP->next)
    {
      CorNode* nameP = corTreeLookup(eP, "tenant");
      if ((nameP != NULL) && (nameP->type == CorString) && (strcmp(nameP->value.s, tenant) == 0))
        entryP = eP;
    }
  }

  int n = (entryP != NULL) ? patternsLoad(psP, entryP, nowMs(), wallNow()) : 0;
  if (n < 0)
    n = 0;

  if (cep.single)
    logLine("state: %s - %d rule%s' matches and deadlines, %d step%s read", stateFile, n, (n == 1) ? "" : "s", loadSteps, (loadSteps == 1) ? "" : "s");
  else if (n > 0)
    logLine("state: tenant %s - %d rule%s' matches and deadlines read", TENANT_TEXT(tenant), n, (n == 1) ? "" : "s");

  return n;
}



// -----------------------------------------------------------------------------
//
// stateRelease -
//
void stateRelease(void)
{
  if (loadUp)
    corTreeBinTablesRelease(&loadTables);
  if (loadText != NULL)
    corAllocBufferReset(&loadAlloc, false);
  free(loadText);

  loadUp   = false;
  loadText = NULL;
  loadTopP = NULL;
}



// -----------------------------------------------------------------------------
//
// stateSave - the state written; false: not (logged once until a write works again)
//
bool stateSave(void)
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
  CorNode*  topP    = corTreeObject(&alloc, NULL);
  CorNode*  listP   = corTreeArray(&alloc, "tenants");

  corTreeChildAdd(topP, corTreeInteger(&alloc, "version", 2));
  for (int ix = 0; ix < tenants; ix++)           // each tenant's patterns
  {
    CorNode* entryP = patternsSave(tenantV[ix].patterns, &alloc, nowMono, nowWall);
    corTreeChildAdd(entryP, corTreeString(&alloc, "tenant", tenantV[ix].name));
    corTreeChildAdd(listP, entryP);
  }
  corTreeChildAdd(topP, listP);
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
