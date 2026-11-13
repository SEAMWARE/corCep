//
// FILE            engine.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The engine: the notifications, one at a time, in the order they came. A Rule notification reloads the
// rules; an event notification fires the rules of its watch. Actions run here, in order - two actions on one
// entity are never reordered.
//
// Each rule's statistics - fired (a count), firedAt, lastError - are written to its Rule entity, at most once
// every statsInterval seconds (--stats-interval): on a tick of the engine's loop, once a second.
//
#include <pthread.h>                             // pthread_mutex_t, pthread_cond_t
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // malloc, free, atoi
#include <string.h>                              // strcmp, memcpy
#include <time.h>                                // clock_gettime, gmtime_r, strftime

#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, ...
#include "corTree/corTreeClone.h"                // corTreeClone
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corCep.h"



// -----------------------------------------------------------------------------
//
// The queue - filled by the listener's thread, emptied by the engine's
//
typedef struct Item
{
  struct Item*  next;
  char          which[32];
  char*         body;                            // NUL-terminated, malloc'd
} Item;

static pthread_mutex_t  queueMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   queueCond  = PTHREAD_COND_INITIALIZER;
static Item*            queueHead  = NULL;
static Item*            queueTail  = NULL;

static CepRules*        rules      = NULL;       // the generation running

int statsInterval = 10;                          // --stats-interval



// -----------------------------------------------------------------------------
//
// Stats - a rule's statistics, by its id: kept across generations (a reload does not reset them)
//
typedef struct Stats
{
  struct Stats*  next;
  char*          id;                             // malloc'd
  long long      fired;
  char           firedAt[32];                    // "" - not fired since corCep started
  char           lastError[384];                 // "" - none since corCep started
  char           lastErrorAt[32];
  bool           dirty;                          // changed since written
  bool           seen;                           // in the generation running
  time_t         writtenAt;
} Stats;

static Stats* statsList = NULL;



// -----------------------------------------------------------------------------
//
// engineQueue -
//
void engineQueue(const char* which, const char* body, int bodyLen)
{
  Item* iP = malloc(sizeof(Item));
  if (iP == NULL)
    return;

  iP->next = NULL;
  snprintf(iP->which, sizeof(iP->which), "%s", which);
  iP->body = malloc(bodyLen + 1);
  if (iP->body == NULL)
  {
    free(iP);
    return;
  }
  memcpy(iP->body, body, bodyLen);
  iP->body[bodyLen] = 0;

  pthread_mutex_lock(&queueMutex);
  if (queueTail == NULL)
    queueHead = iP;
  else
    queueTail->next = iP;
  queueTail = iP;
  pthread_cond_signal(&queueCond);
  pthread_mutex_unlock(&queueMutex);
}



// -----------------------------------------------------------------------------
//
// dequeue - waits for the next item, at most <ms> milliseconds: NULL - a tick
//
static Item* dequeue(long long ms)
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

  pthread_mutex_lock(&queueMutex);
  while (queueHead == NULL)
  {
    if (pthread_cond_timedwait(&queueCond, &queueMutex, &until) != 0)
    {
      pthread_mutex_unlock(&queueMutex);
      return NULL;
    }
  }

  Item* iP  = queueHead;
  queueHead = iP->next;
  if (queueHead == NULL)
    queueTail = NULL;
  pthread_mutex_unlock(&queueMutex);

  return iP;
}



// -----------------------------------------------------------------------------
//
// property - { "type": "Property", "value": <a copy of valueP> }
//
static CorNode* property(CorAlloc* allocP, const char* name, CorNode* valueP)
{
  CorNode* pP = corTreeObject(allocP, name);
  CorNode* vP = corTreeClone(allocP, valueP);

  vP->name = (char*) "value";
  corTreeChildAdd(pP, corTreeString(allocP, "type", "Property"));
  corTreeChildAdd(pP, vP);

  return pP;
}



// -----------------------------------------------------------------------------
//
// actionSet - PATCH /entities/<id>/attrs { <attribute>: { type: Property, value }, ... }
//
static bool actionSet(CorAlloc* allocP, CepAction* aP, char* why, int whyLen)
{
  CorNode* bodyP = corTreeObject(allocP, NULL);
  char     path[1024];

  for (CorNode* vP = aP->set->value.head; vP != NULL; vP = vP->next)
    corTreeChildAdd(bodyP, property(allocP, vP->name, vP));

  snprintf(path, sizeof(path), "/entities/%s/attrs", urlEncode(allocP, aP->entity));

  CepResponse resp;
  if ((brokerRequest(allocP, CorVerbPatch, path, NULL, bodyP, &resp) == false) || (resp.status != 204))
  {
    snprintf(why, whyLen, "PATCH %s: %s", aP->entity, (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// actionToggle - the boolean read, the other value PATCHed
//
static bool actionToggle(CorAlloc* allocP, CepAction* aP, char* why, int whyLen)
{
  char        path[1024];
  char        query[512];
  CepResponse resp;

  snprintf(path,  sizeof(path),  "/entities/%s", urlEncode(allocP, aP->entity));
  snprintf(query, sizeof(query), "attrs=%s&options=keyValues", urlEncode(allocP, aP->attribute));

  if ((brokerRequest(allocP, CorVerbGet, path, query, NULL, &resp) == false) || (resp.status != 200) || (resp.body == NULL))
  {
    snprintf(why, whyLen, "GET %s: %s", aP->entity, (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    return false;
  }

  CorNode* vP = corTreeLookup(resp.body, aP->attribute);
  if ((vP == NULL) || (vP->type != CorBoolean))
  {
    snprintf(why, whyLen, "toggle %s: %s is not a boolean", aP->entity, aP->attribute);
    return false;
  }

  CorNode* nowP  = corTreeBoolean(allocP, aP->attribute, !vP->value.b);
  CorNode* bodyP = corTreeObject(allocP, NULL);
  corTreeChildAdd(bodyP, property(allocP, aP->attribute, nowP));

  snprintf(path, sizeof(path), "/entities/%s/attrs", urlEncode(allocP, aP->entity));
  if ((brokerRequest(allocP, CorVerbPatch, path, NULL, bodyP, &resp) == false) || (resp.status != 204))
  {
    snprintf(why, whyLen, "PATCH %s: %s", aP->entity, (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// nowIso - the time, UTC, ISO 8601 with milliseconds
//
static void nowIso(char* buf, int bufLen)
{
  struct timespec ts;
  struct tm       tm;
  char            s[24];

  clock_gettime(CLOCK_REALTIME, &ts);
  gmtime_r(&ts.tv_sec, &tm);
  strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%S", &tm);
  snprintf(buf, bufLen, "%s.%03ldZ", s, ts.tv_nsec / 1000000);
}



// -----------------------------------------------------------------------------
//
// statsOf - a rule's statistics; made at its first sight, the count going on from the Rule entity's 'fired'
//
static Stats* statsOf(CepRule* ruleP)
{
  for (Stats* sP = statsList; sP != NULL; sP = sP->next)
  {
    if (strcmp(sP->id, ruleP->id) == 0)
      return sP;
  }

  Stats* sP = calloc(1, sizeof(Stats));
  if (sP == NULL)
    return NULL;

  sP->id    = strdup(ruleP->id);
  sP->fired = ruleP->firedBase;
  sP->next  = statsList;
  statsList = sP;

  return sP;
}



// -----------------------------------------------------------------------------
//
// statsWrite - POST /entities/<rule>/attrs: fired, firedAt, lastError (append: made the first time, replaced after)
//
static void statsWrite(CorAlloc* allocP, Stats* sP)
{
  CorNode* bodyP  = corTreeObject(allocP, NULL);
  CorNode* firedP = corTreeObject(allocP, "fired");

  corTreeChildAdd(firedP, corTreeString(allocP, "type", "Property"));
  corTreeChildAdd(firedP, corTreeInteger(allocP, "value", sP->fired));
  corTreeChildAdd(bodyP, firedP);

  if (sP->firedAt[0] != 0)
  {
    CorNode* atP = corTreeObject(allocP, "firedAt");
    CorNode* vP  = corTreeObject(allocP, "value");

    corTreeChildAdd(vP, corTreeString(allocP, "@type", "DateTime"));
    corTreeChildAdd(vP, corTreeString(allocP, "@value", sP->firedAt));
    corTreeChildAdd(atP, corTreeString(allocP, "type", "Property"));
    corTreeChildAdd(atP, vP);
    corTreeChildAdd(bodyP, atP);
  }

  long long dropped = patternsDropped(sP->id);
  if (dropped > 0)
  {
    CorNode* dP = corTreeObject(allocP, "droppedMatches");

    corTreeChildAdd(dP, corTreeString(allocP, "type", "Property"));
    corTreeChildAdd(dP, corTreeInteger(allocP, "value", dropped));
    corTreeChildAdd(bodyP, dP);
  }

  if (sP->lastError[0] != 0)
  {
    CorNode* errP = corTreeObject(allocP, "lastError");

    corTreeChildAdd(errP, corTreeString(allocP, "type", "Property"));
    corTreeChildAdd(errP, corTreeString(allocP, "value", sP->lastError));
    corTreeChildAdd(errP, corTreeString(allocP, "observedAt", sP->lastErrorAt));
    corTreeChildAdd(bodyP, errP);
  }

  char        path[1024];
  CepResponse resp;

  snprintf(path, sizeof(path), "/entities/%s/attrs", urlEncode(allocP, sP->id));
  if ((brokerRequest(allocP, CorVerbPost, path, NULL, bodyP, &resp) == false) || ((resp.status != 204) && (resp.status != 404)))
    logLine("rule %s: its statistics not written - %s", sP->id, (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);

  sP->dirty     = false;
  sP->writtenAt = time(NULL);
}



// -----------------------------------------------------------------------------
//
// statsFlush - the statistics changed and not written for statsInterval seconds
//
static void statsFlush(CorAlloc* allocP)
{
  time_t now = time(NULL);

  for (Stats* sP = statsList; sP != NULL; sP = sP->next)
  {
    if ((sP->dirty == true) && (sP->seen == true) && (now - sP->writtenAt >= statsInterval))
      statsWrite(allocP, sP);
  }
}



// -----------------------------------------------------------------------------
//
// ruleFire - its actions, in order; one failing does not stop the others
//
static void ruleFire(CorAlloc* allocP, CepRule* ruleP, const char* entity)
{
  int    failed = 0;
  Stats* sP     = statsOf(ruleP);

  for (int ix = 0; ix < ruleP->actions; ix++)
  {
    CepAction* aP       = &ruleP->actionV[ix];
    char       why[512] = { 0 };
    bool       ok       = (aP->kind == CepActionSet) ? actionSet(allocP, aP, why, sizeof(why)) : actionToggle(allocP, aP, why, sizeof(why));

    if (ok == false)
    {
      logLine("rule %s: action %d - %s", ruleP->id, ix + 1, why);
      ++failed;

      if (sP != NULL)
      {
        snprintf(sP->lastError, sizeof(sP->lastError), "action %d: %s", ix + 1, why);
        nowIso(sP->lastErrorAt, sizeof(sP->lastErrorAt));
      }
    }
  }

  if (sP != NULL)
  {
    ++sP->fired;
    nowIso(sP->firedAt, sizeof(sP->firedAt));
    sP->dirty = true;
  }

  logLine("rule %s fired (%s)%s", ruleP->id, entity, (failed > 0) ? " - an action failed" : "");
}



// -----------------------------------------------------------------------------
//
// fireCb - the patterns' way to fire a rule
//
static void fireCb(CepRule* ruleP, const char* entity, void* ctx)
{
  ruleFire((CorAlloc*) ctx, ruleP, (entity != NULL) ? entity : "-");
}



// -----------------------------------------------------------------------------
//
// eventHandle - a notification of subscription <sub>: for each entity in it holding the attribute, the rules of the watch
//
static void eventHandle(CorAlloc* allocP, int sub, char* body)
{
  int n = -1;                                    // the watch of the subscription

  for (int ix = 0; (rules != NULL) && (ix < rules->watches); ix++)
  {
    if (rules->watchV[ix].sub == sub)
    {
      n = ix;
      break;
    }
  }

  if (n == -1)
    return;                                      // a subscription no rule has any more

  CorJson  json;
  corJsonCreate(&json, allocP);
  CorNode* notifP = corJsonParse(&json, body);
  CorNode* dataP  = (notifP != NULL) ? corTreeLookup(notifP, "data") : NULL;

  if ((dataP == NULL) || (dataP->type != CorArray))
  {
    logLine("a notification without 'data' - ignored");
    return;
  }

  CepWatch* wP = &rules->watchV[n];
  for (CorNode* eP = dataP->value.head; eP != NULL; eP = eP->next)
  {
    CorNode* idP = corTreeLookup(eP, "id");
    if ((idP == NULL) || (idP->type != CorString) || (corTreeLookup(eP, wP->attribute) == NULL))
      continue;

    patternsEvent(rules, n, idP->value.s, nowMs(), fireCb, allocP);
  }
}



// -----------------------------------------------------------------------------
//
// rulesReload - a new generation; the old one stays if the broker did not answer
//
static void rulesReload(void)
{
  CepRules* newP = rulesLoad();
  if (newP == NULL)
    return;

  subsApply(newP);
  rulesFree(rules);
  rules = newP;
  patternsAdopt(rules, nowMs());

  //
  // The statistics of the rules running; those of a rule gone (deleted, disabled, left out) are dropped
  //
  for (Stats* sP = statsList; sP != NULL; sP = sP->next)
    sP->seen = false;
  for (int ix = 0; ix < rules->rules; ix++)
  {
    Stats* sP = statsOf(&rules->ruleV[ix]);
    if (sP != NULL)
      sP->seen = true;
  }

  Stats** prevPP = &statsList;
  while (*prevPP != NULL)
  {
    Stats* sP = *prevPP;
    if (sP->seen == false)
    {
      *prevPP = sP->next;
      free(sP->id);
      free(sP);
    }
    else
      prevPP = &sP->next;
  }
}



// -----------------------------------------------------------------------------
//
// engineRun -
//
void engineRun(void)
{
  CorAlloc alloc;
  char     buf[32 * 1024];

  corAllocBufferInit(&alloc, buf, sizeof(buf), 256 * 1024, NULL, "corCep event");

  rulesReload();

  while (true)
  {
    //
    // The deadlines passed (absences, superseded fires waiting) fire; the wait: till the next one, a second at most
    //
    long long now  = nowMs();
    long long next = patternsTimers(rules, now, fireCb, &alloc);
    long long wait = ((next != 0) && (next - now < 1000)) ? next - now : 1000;

    statsFlush(&alloc);
    corAllocBufferReset(&alloc, true);

    Item* iP = dequeue((wait > 0) ? wait : 1);

    if (iP == NULL)                              // a tick, or a deadline
      continue;

    if (strcmp(iP->which, "rules") == 0)
      rulesReload();
    else
      eventHandle(&alloc, atoi(iP->which), iP->body);

    free(iP->body);
    free(iP);
    statsFlush(&alloc);
    corAllocBufferReset(&alloc, true);
  }
}
