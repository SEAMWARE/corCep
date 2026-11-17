//
// FILE            keys.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The keys tokens and rule signatures are checked against - one key set (corRestJwk holds one), made of up to two
// JWK Sets: --jwks (the rule editors' tokens, for /dry-run) and --author-jwks (the keys the editors sign the rules
// with, section 7 of doc/design.md). Each a URL or a file; fetched at start, then every --jwks-refresh, and at once
// when a token names a key the set does not have (its issuer rotated its keys). A fetch that fails keeps the keys
// in use.
//
// What a key may sign is not decided here: a token's issuer is - /dry-run wants --issuer, a rule's signature
// --author-issuer and a "rule" claim (author.c).
//
#include <pthread.h>                             // pthread_*
#include <stdio.h>                               // snprintf
#include <string.h>                              // strlen
#include <time.h>                                // clock_gettime

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corJson/corJsonRender.h"               // corJsonRender
#include "corJson/corJsonRenderSize.h"           // corJsonRenderSize
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeArray, corTreeChildAdd
#include "corTree/corTreeLookup.h"               // corTreeLookup
#include "corRest/corRestFetch.h"                // corRestFetch
#include "corRest/corRestJwk.h"                  // corRestJwkLoad

#include "corCep.h"



static const char*      whereV[2]    = { NULL, NULL };
static const char*      optionV[2]   = { NULL, NULL };   // the option naming each - for a message
static int              wheres       = 0;
static int              refreshS     = 300;

static pthread_mutex_t  refreshMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   refreshCond  = PTHREAD_COND_INITIALIZER;
static bool             refreshNow   = false;



// -----------------------------------------------------------------------------
//
// keysFetch - every set fetched, their keys installed as one set; false: not (the keys in use stay) - why says why
//
static bool keysFetch(char* why, int whyLen)
{
  CorAlloc alloc;
  CorJson  json;
  CorNode* allP  = NULL;
  CorNode* keysP = NULL;
  bool     ok    = true;

  corAllocBufferInit(&alloc, NULL, 0, 64 * 1024, NULL, "corCep jwks");
  corJsonCreate(&json, &alloc);

  allP  = corTreeObject(&alloc, NULL);
  keysP = corTreeArray(&alloc, "keys");
  corTreeChildAdd(allP, keysP);

  for (int ix = 0; (ix < wheres) && ok; ix++)
  {
    char     fetchWhy[256];
    char*    text = corRestFetch(whereV[ix], &alloc, fetchWhy, sizeof(fetchWhy));
    CorNode* setP = (text != NULL) ? corJsonParse(&json, text) : NULL;
    CorNode* kP   = (setP != NULL) ? corTreeLookup(setP, "keys") : NULL;

    if ((kP == NULL) || (kP->type != CorArray))
    {
      snprintf(why, whyLen, "%s %s: %s", optionV[ix], whereV[ix], (text == NULL) ? fetchWhy : "not a JWK Set");
      ok = false;
      break;
    }

    while (kP->value.head != NULL)               // moved, not added: corTreeChildAdd keeps the rest of a list
    {
      CorNode* keyP = kP->value.head;
      kP->value.head = keyP->next;
      keyP->next     = NULL;
      corTreeChildAdd(keysP, keyP);
    }
  }

  if (ok)
  {
    char* text = corAlloc(&alloc, corJsonRenderSize(&json, allP) + 1);
    corJsonRender(&json, allP, text);
    ok = corRestJwkLoad(text, NULL, why, whyLen);
  }

  corAllocBufferReset(&alloc, false);
  return ok;
}



// -----------------------------------------------------------------------------
//
// refresher - the keys fetched again every --jwks-refresh, and at once when asked (keysRefreshNow)
//
static void* refresher(void* argP)
{
  (void) argP;

  while (true)
  {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += refreshS;

    pthread_mutex_lock(&refreshMutex);
    while ((refreshNow == false) && (pthread_cond_timedwait(&refreshCond, &refreshMutex, &until) == 0))
      ;
    refreshNow = false;
    pthread_mutex_unlock(&refreshMutex);

    char why[300];
    if (keysFetch(why, sizeof(why)) == false)
      logLine("the keys not fetched again - the ones in use kept: %s", why);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// keysRefreshNow - a token named a key not in the set: fetched again, now (the refresher's own pace: once each ask)
//
void keysRefreshNow(void)
{
  pthread_mutex_lock(&refreshMutex);
  refreshNow = true;
  pthread_cond_signal(&refreshCond);
  pthread_mutex_unlock(&refreshMutex);
}



// -----------------------------------------------------------------------------
//
// keysInit - the sets (NULL / "": not given) fetched, the refresher started; false: why - corCep does not start
//
bool keysInit(const char* jwks, const char* authorJwks, int refresh, char* why, int whyLen)
{
  if ((jwks != NULL) && (jwks[0] != 0))
  {
    optionV[wheres]  = "--jwks";
    whereV[wheres++] = jwks;
  }
  if ((authorJwks != NULL) && (authorJwks[0] != 0))
  {
    optionV[wheres]  = "--author-jwks";
    whereV[wheres++] = authorJwks;
  }
  refreshS = refresh;

  if (wheres == 0)
    return true;

  if (keysFetch(why, whyLen) == false)
    return false;

  pthread_t tid;
  if (pthread_create(&tid, NULL, refresher, NULL) != 0)
  {
    snprintf(why, whyLen, "no thread for the key refresh");
    return false;
  }
  pthread_detach(tid);

  return true;
}
