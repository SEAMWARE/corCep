//
// FILE            listener.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The notifications - corHttp on a thread of its own. A notification is answered at once (204) and queued
// for the engine; nothing is decided here. Only POST /notify/<secret>/<which> is taken - and POST /dry-run (dryrun.c).
//
#include <pthread.h>                             // pthread_create
#include <stdio.h>                               // snprintf
#include <string.h>                              // strncmp, strlen

#include "corHttp/CorHttp.h"                     // corHttp*

#include "corCep.h"



static CorHttpServer server;



// -----------------------------------------------------------------------------
//
// requestCb -
//
static void requestCb(CorHttpConn* connP)
{
#if CEP_WITH_DRY_RUN
  if ((connP->method.len == 4) && (strncmp(connP->method.s, "POST", 4) == 0) &&
      (connP->path.len == 8) && (strncmp(connP->path.s, "/dry-run", 8) == 0))
  {
    dryRunRequest(connP);
    return;
  }
#endif

  char prefix[64];
  int  prefixLen = snprintf(prefix, sizeof(prefix), "/notify/%s/", cep.secret);

  if ((connP->method.len != 4) || (strncmp(connP->method.s, "POST", 4) != 0) ||
      (connP->path.len <= prefixLen) || (strncmp(connP->path.s, prefix, prefixLen) != 0))
  {
    corHttpResponseStatus(connP, 404);
    return;
  }

  char which[32];
  int  whichLen = connP->path.len - prefixLen;

  if (whichLen >= (int) sizeof(which))
  {
    corHttpResponseStatus(connP, 404);
    return;
  }
  memcpy(which, connP->path.s + prefixLen, whichLen);
  which[whichLen] = 0;

  engineQueue(which, connP->body.s, connP->body.len);   // never refused: a failure would stop the broker notifying (cooldown)
  corHttpResponseStatus(connP, 204);
}



// -----------------------------------------------------------------------------
//
// serve -
//
static void* serve(void* argP)
{
  (void) argP;
  corHttpServe(&server);
  logLine("the listener stopped");
  return NULL;
}



// -----------------------------------------------------------------------------
//
// listenerStart -
//
bool listenerStart(void)
{
  if (corHttpInit(&server, cep.port, 64, requestCb) != CorHttpOk)
  {
    logLine("the listener: port %d could not be opened", cep.port);
    return false;
  }

  server.maxRequestSize = 4 * 1024 * 1024;

  pthread_t tid;
  if (pthread_create(&tid, NULL, serve, NULL) != 0)
  {
    logLine("the listener: no thread");
    return false;
  }
  pthread_detach(tid);

  return true;
}
