//
// FILE            sender.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The actions' requests - PATCHes of attributes, entities created (POST) and deleted - sent by a thread of their
// own, off the engine's: a broker slow or away no longer holds up the patterns (doc/design.md § 5, § 6).
//
// - A queue per target entity: two actions on one light go out in the order they were made, never reordered - the
//   next one waits for the one before to be done.
// - Retried with back-off (0.25 s, doubled, 5 s at most) for --retry-for (default 30 s) when the broker did not
//   answer, or answered 408, 429, 5xx; then dropped - a light switched minutes late is worse than not switched.
//   Any other answer is final: a 404 is not retried - a DELETE's 404 is done (the entity is gone, as asked).
// - A target's queue holds QUEUE_MAX at most: beyond, the oldest is dropped.
// - Dropped, or failed: told to the engine (engineQueue "action") - the rule's lastError and droppedActions are the
//   engine's.
//
// The targets take turns: one request at a time, the one due first.
//
#include <pthread.h>                             // pthread_create, pthread_mutex_t, pthread_cond_t
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // malloc, calloc, free
#include <string.h>                              // strcmp, strdup, strlen
#include <time.h>                                // clock_gettime

#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset

#include "corCep.h"



#define QUEUE_MAX     100                        // requests waiting per target - the oldest dropped beyond
#define BACKOFF_FIRST 250                        // ms
#define BACKOFF_MAX   5000                       // ms



// -----------------------------------------------------------------------------
//
// Patch - one request waiting - a PATCH, a POST or a DELETE
//
typedef struct Patch
{
  struct Patch*  next;
  char*          ruleId;
  int            action;                         // 1..: the rule's action; 0: a step
  CorRestVerb    verb;                           // PATCH (attributes), POST (create), DELETE
  char*          body;                           // JSON text; NULL: none (DELETE)
  char*          author;                         // the rule's signed 'author' - NULL: the author's rights not checked
  long long      madeAt;                         // nowMs()
} Patch;



// -----------------------------------------------------------------------------
//
// Target - an entity's queue
//
typedef struct Target
{
  struct Target*  next;
  char*           id;
  Patch*          head;
  Patch*          tail;
  int             len;
  long long       dueAt;                         // its head is sent at or after this (a retry's back-off)
  long long       backoffMs;                     // the next back-off; 0: the head not tried yet
  bool            sending;                       // its head is out - the sender works without the lock
} Target;



static pthread_mutex_t  mutex       = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   cond        = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   emptyCond   = PTHREAD_COND_INITIALIZER;
static Target*          targetList  = NULL;
static int              waiting     = 0;         // requests in all the queues, the one out included
static long long        retryForMs  = 30000;



// -----------------------------------------------------------------------------
//
// patchFree -
//
static void patchFree(Patch* pP)
{
  free(pP->ruleId);
  free(pP->body);
  free(pP->author);
  free(pP);
}



// -----------------------------------------------------------------------------
//
// report - an action failed, or dropped: to the engine, as "<rule>\n<action>\n<dropped 0|1>\n<why>"
//
static void report(const char* ruleId, int action, bool dropped, const char* why)
{
  char text[1024];
  int  len = snprintf(text, sizeof(text), "%s\n%d\n%d\n%s", ruleId, action, dropped ? 1 : 0, why);

  if (len >= (int) sizeof(text))
    len = sizeof(text) - 1;
  engineQueue("action", text, len);
}



// -----------------------------------------------------------------------------
//
// verbName -
//
static const char* verbName(CorRestVerb verb)
{
  return (verb == CorVerbPost) ? "POST" : (verb == CorVerbDelete) ? "DELETE" : "PATCH";
}



// -----------------------------------------------------------------------------
//
// enqueue - a request on <entity>, at the end of its queue
//
static void enqueue(const char* ruleId, int action, const char* entity, CorRestVerb verb, const char* body, const char* author)
{
  Patch* pP = calloc(1, sizeof(Patch));

  if ((pP == NULL) || ((pP->ruleId = strdup(ruleId)) == NULL) || ((body != NULL) && ((pP->body = strdup(body)) == NULL)) ||
      ((author != NULL) && ((pP->author = strdup(author)) == NULL)))
  {
    if (pP != NULL)
      patchFree(pP);
    report(ruleId, action, true, "out of memory");
    return;
  }
  pP->action = action;
  pP->verb   = verb;
  pP->madeAt = nowMs();

  pthread_mutex_lock(&mutex);

  Target* tP = targetList;
  while ((tP != NULL) && (strcmp(tP->id, entity) != 0))
    tP = tP->next;

  if (tP == NULL)
  {
    tP = calloc(1, sizeof(Target));
    if ((tP == NULL) || ((tP->id = strdup(entity)) == NULL))
    {
      free(tP);
      pthread_mutex_unlock(&mutex);
      report(ruleId, action, true, "out of memory");
      patchFree(pP);
      return;
    }
    tP->next   = targetList;
    targetList = tP;
  }

  //
  // Full: the oldest not out is dropped
  //
  Patch* droppedP = NULL;
  if (tP->len >= QUEUE_MAX)
  {
    Patch** prevPP = tP->sending ? &tP->head->next : &tP->head;
    droppedP = *prevPP;
    if (droppedP != NULL)
    {
      *prevPP = droppedP->next;
      if (tP->tail == droppedP)
        tP->tail = (prevPP == &tP->head) ? NULL : tP->head;
      --tP->len;
      --waiting;
      if (prevPP == &tP->head)
      {
        tP->backoffMs = 0;
        tP->dueAt     = 0;
      }
    }
  }

  if (tP->tail == NULL)
    tP->head = pP;
  else
    tP->tail->next = pP;
  tP->tail = pP;
  ++tP->len;
  ++waiting;

  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);

  if (droppedP != NULL)
  {
    char why[600];
    snprintf(why, sizeof(why), "%s %s: dropped - %d waiting for it already", verbName(droppedP->verb), entity, QUEUE_MAX);
    report(droppedP->ruleId, droppedP->action, true, why);
    patchFree(droppedP);
  }
}



// -----------------------------------------------------------------------------
//
// senderPatch, senderCreate, senderDelete - a PATCH of <entity>'s attributes, <entity> created (body: the whole
// entity, POST /entities), <entity> deleted - each at the end of <entity>'s queue: a create, a PATCH, a delete of
// one entity go out in the order they were made
//
void senderPatch(const char* ruleId, int action, const char* entity, const char* body, const char* author)
{
  enqueue(ruleId, action, entity, CorVerbPatch, body, author);
}

void senderCreate(const char* ruleId, int action, const char* entity, const char* body, const char* author)
{
  enqueue(ruleId, action, entity, CorVerbPost, body, author);
}

void senderDelete(const char* ruleId, int action, const char* entity, const char* author)
{
  enqueue(ruleId, action, entity, CorVerbDelete, NULL, author);
}



// -----------------------------------------------------------------------------
//
// retriable - an answer worth trying again: none at all, 408, 429, 5xx
//
static bool retriable(bool answered, int status)
{
  return (answered == false) || (status == 408) || (status == 429) || (status >= 500);
}



// -----------------------------------------------------------------------------
//
// nextDue - the target whose head is due first (not out); NULL: none. *dueAtP: its time
//
static Target* nextDue(long long* dueAtP)
{
  Target* bestP = NULL;

  for (Target* tP = targetList; tP != NULL; tP = tP->next)
  {
    if ((tP->head == NULL) || tP->sending)
      continue;
    if ((bestP == NULL) || (tP->dueAt < bestP->dueAt))
      bestP = tP;
  }

  if (bestP != NULL)
    *dueAtP = bestP->dueAt;
  return bestP;
}



// -----------------------------------------------------------------------------
//
// senderThread - the requests due, one at a time
//
static void* senderThread(void* arg)
{
  CorAlloc alloc;
  char     buf[16 * 1024];

  (void) arg;
  corAllocBufferInit(&alloc, buf, sizeof(buf), 64 * 1024, NULL, "corCep sender");

  pthread_mutex_lock(&mutex);
  while (true)
  {
    long long dueAt = 0;
    Target*   tP    = nextDue(&dueAt);

    if (tP == NULL)
    {
      pthread_cond_wait(&cond, &mutex);
      continue;
    }

    long long now = nowMs();
    if (dueAt > now)
    {
      struct timespec until;
      long long       ms = dueAt - now;

      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec  += ms / 1000;
      until.tv_nsec += (ms % 1000) * 1000000;
      if (until.tv_nsec >= 1000000000)
      {
        until.tv_sec  += 1;
        until.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&cond, &mutex, &until);
      continue;
    }

    //
    // The head goes out - without the lock: senderPatch adds behind it meanwhile
    //
    Patch* pP   = tP->head;
    char*  id   = tP->id;
    tP->sending = true;
    pthread_mutex_unlock(&mutex);

    char        path[1024];
    CepResponse resp = { 0 };

    if (pP->verb == CorVerbPost)
      snprintf(path, sizeof(path), "/entities");
    else if (pP->verb == CorVerbDelete)
      snprintf(path, sizeof(path), "/entities/%s", urlEncode(&alloc, id));
    else
      snprintf(path, sizeof(path), "/entities/%s/attrs", urlEncode(&alloc, id));

    //
    // The author's rights (author.c): the PDP asked first - a no is final, no answer is tried again, as the broker's
    //
    char why[600] = { 0 };
    int  may      = 1;

    if (pP->author != NULL)
    {
      char ldPath[1100];                         // as the PEP hands it over: decoded (APISIX's uri)

      if (pP->verb == CorVerbPost)
        snprintf(ldPath, sizeof(ldPath), "/ngsi-ld/v1/entities");
      else
        snprintf(ldPath, sizeof(ldPath), "/ngsi-ld/v1/entities/%s%s", id, (pP->verb == CorVerbDelete) ? "" : "/attrs");
      may = authorMay(&alloc, pP->author, verbName(pP->verb), ldPath, pP->body, why, sizeof(why));
    }

    bool answered = (may == 1) && brokerRequestText(&alloc, pP->verb, path, NULL, pP->body, &resp);
    bool done     = answered && ((resp.status == 204) || ((pP->verb == CorVerbPost) && (resp.status == 201)) ||
                                 ((pP->verb == CorVerbDelete) && (resp.status == 404)));   // gone already: what was asked

    if (may == 0)
      resp.status = 403;                         // final: not retriable
    else if (may == -1)
      resp.status = 0;                           // no answer: retriable
    else if (done == false)
    {
      char what[512];
      snprintf(what, sizeof(what), "%s %s", verbName(pP->verb), id);
      brokerFailed(why, sizeof(why), what, &resp);
    }
    corAllocBufferReset(&alloc, true);

    pthread_mutex_lock(&mutex);
    tP->sending = false;
    now         = nowMs();

    bool retry = (done == false) && (may != 0) && retriable(answered, resp.status);   // a no from the PDP is final
    if (retry)
    {
      long long backoff = (tP->backoffMs == 0) ? BACKOFF_FIRST : tP->backoffMs;

      if (now + backoff - pP->madeAt <= retryForMs)
      {
        tP->dueAt     = now + backoff;
        tP->backoffMs = (backoff * 2 > BACKOFF_MAX) ? BACKOFF_MAX : backoff * 2;
        continue;                                // the head stays - the targets behind it wait
      }
    }

    //
    // Done - sent, failed for good, or out of time: the head leaves
    //
    tP->head = pP->next;
    if (tP->head == NULL)
      tP->tail = NULL;
    --tP->len;
    --waiting;
    tP->dueAt     = 0;
    tP->backoffMs = 0;

    if (tP->head == NULL)                        // an empty queue goes - a group's targets come and go
    {
      Target** prevPP = &targetList;
      while (*prevPP != tP)
        prevPP = &(*prevPP)->next;
      *prevPP = tP->next;
      free(tP->id);
      free(tP);
    }

    if (waiting == 0)
      pthread_cond_broadcast(&emptyCond);

    pthread_mutex_unlock(&mutex);

    if (done == false)
    {
      char text[700];

      if (retry)
        snprintf(text, sizeof(text), "%s - dropped, not sent within %lld s", why, retryForMs / 1000);
      else
        snprintf(text, sizeof(text), "%s", why);
      report(pP->ruleId, pP->action, retry, text);
    }
    patchFree(pP);

    pthread_mutex_lock(&mutex);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// senderStart - the sender's thread; retryForS: --retry-for (0: no retry)
//
bool senderStart(int retryForS)
{
  pthread_t tid;

  retryForMs = (long long) retryForS * 1000;
  if (pthread_create(&tid, NULL, senderThread, NULL) != 0)
    return false;
  pthread_detach(tid);
  return true;
}



// -----------------------------------------------------------------------------
//
// senderDrain - at a stop: waits till every request is sent (or dropped), ms at most; the number still waiting
//
int senderDrain(long long ms)
{
  struct timespec until;

  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec  += ms / 1000;
  until.tv_nsec += (ms % 1000) * 1000000;
  if (until.tv_nsec >= 1000000000)
  {
    until.tv_sec  += 1;
    until.tv_nsec -= 1000000000;
  }

  pthread_mutex_lock(&mutex);
  while (waiting > 0)
  {
    if (pthread_cond_timedwait(&emptyCond, &mutex, &until) != 0)
      break;
  }
  int left = waiting;
  pthread_mutex_unlock(&mutex);

  return left;
}
