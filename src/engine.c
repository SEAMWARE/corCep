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
// Every renewInterval seconds (--renew-interval) the subscriptions are renewed - a broker that keeps a
// subscription's owner only for a while (coraine --auth oidc: 24 h) stops notifying, silently, once it runs out.
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
  int           bodyLen;
  bool          event;                           // a notification of an event subscription - the bounded kind
} Item;

static pthread_mutex_t  queueMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   queueCond  = PTHREAD_COND_INITIALIZER;
static Item*            queueHead  = NULL;
static Item*            queueTail  = NULL;
static int              queueEvents = 0;         // event notifications waiting
static long long        queueBytes  = 0;         // their bodies' bytes
static bool             rulesQueued = false;     // a "rules" item waiting - one is enough
static long long        dropped     = 0;         // event notifications dropped since the queue filled up

int queueMax = 10000;                            // --queue-max: event notifications waiting, at most

#define QUEUE_BYTES_MAX  (64LL * 1024 * 1024)    // their bodies, at most

static CepRules*        rules      = NULL;       // the generation running
static CepPatterns*     patterns   = NULL;       // its patterns' state - the host's clock
static bool             stateDirty = false;      // the patterns' state changed since --state was last written

int statsInterval = 10;                          // --stats-interval
int renewInterval = 3600;                        // --renew-interval



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
  long long      droppedWritten;                 // droppedMatches as last written
  long long      droppedValuesWritten;           // droppedValues as last written
  long long      droppedActions;                 // PATCHes dropped (sender.c: not sent within --retry-for, or a full queue), since start
  bool           seen;                           // in the generation running
  time_t         writtenAt;
} Stats;

static Stats* statsList = NULL;



// -----------------------------------------------------------------------------
//
// engineQueue - an item for the engine
//
// Bounded: --queue-max event notifications, QUEUE_BYTES_MAX of their bodies. Full: the OLDEST event notifications
// waiting are dropped to make room - an event handled late is worse than not handled (a light switched by a press
// of a minute ago), the newest are what the place is now. Logged when the dropping starts, and how many once the
// queue has drained to half. Never refused: a notification answered with an error makes the broker skip the
// subscription's notifications for its endpoint's cooldown (NGSI-LD § 5.2.15 - coraine: 30 s by default).
//
// A "rules" notification (a reload) and the sender's "action" reports are never dropped; a reload already waiting
// is not queued again.
//
void engineQueue(const char* which, const char* body, int bodyLen)
{
  bool event = (strcmp(which, "rules") != 0) && (strcmp(which, "action") != 0);

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
  iP->bodyLen       = bodyLen;
  iP->event         = event;

  Item* droppedList = NULL;                      // freed after the lock

  pthread_mutex_lock(&queueMutex);

  if ((event == false) && (strcmp(which, "rules") == 0) && rulesQueued)
  {
    pthread_mutex_unlock(&queueMutex);
    free(iP->body);
    free(iP);
    return;
  }

  if (event)
  {
    Item** prevPP = &queueHead;
    Item*  lastP  = NULL;                        // the item before *prevPP - the tail, if the last goes

    while (((queueEvents >= queueMax) || (queueBytes + bodyLen > QUEUE_BYTES_MAX)) && (*prevPP != NULL))
    {
      Item* xP = *prevPP;

      if (xP->event == false)
      {
        lastP  = xP;
        prevPP = &xP->next;
        continue;
      }

      *prevPP = xP->next;
      if (queueTail == xP)
        queueTail = lastP;
      --queueEvents;
      queueBytes -= xP->bodyLen;
      if (dropped++ == 0)
        logLine("the event queue is full (%d notifications) - the oldest dropped till it drains", queueMax);
      xP->next    = droppedList;
      droppedList = xP;
    }
  }

  if (queueTail == NULL)
    queueHead = iP;
  else
    queueTail->next = iP;
  queueTail = iP;
  if (event)
  {
    ++queueEvents;
    queueBytes += bodyLen;
  }
  else if (strcmp(which, "rules") == 0)
    rulesQueued = true;
  pthread_cond_signal(&queueCond);
  pthread_mutex_unlock(&queueMutex);

  while (droppedList != NULL)
  {
    Item* xP    = droppedList;
    droppedList = xP->next;
    free(xP->body);
    free(xP);
  }
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

  long long droppedNow = 0;
  if (iP->event)
  {
    --queueEvents;
    queueBytes -= iP->bodyLen;
    if ((dropped > 0) && (queueEvents <= queueMax / 2))   // half drained: the dropping is over
    {
      droppedNow = dropped;
      dropped    = 0;
    }
  }
  else if (strcmp(iP->which, "rules") == 0)
    rulesQueued = false;
  pthread_mutex_unlock(&queueMutex);

  if (droppedNow > 0)
    logLine("the event queue has room again - %lld notification%s dropped while it was full", droppedNow, (droppedNow == 1) ? "" : "s");

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
// failed - the reason of a broker request that did not do what it should
//
void brokerFailed(char* why, int whyLen, const char* what, CepResponse* respP)
{
  snprintf(why, whyLen, "%s: %s", what, (respP->status != 0) ? ((respP->text != NULL) ? respP->text : "no body") : respP->error);
}



// -----------------------------------------------------------------------------
//
// targetsGet - an action's targets, as keyValues entities: one entity (GET it, if its value is needed), or every
// entity of a type that matches q. NULL: the broker did not answer (why says so).
//
static CorNode* targetsGet(CorAlloc* allocP, CepAction* aP, bool values, char* why, int whyLen)
{
  CepResponse resp;
  char        path[1024];
  char        query[2048];

  if (aP->entity != NULL)
  {
    CorNode* listP = corTreeArray(allocP, NULL);

    if (values == false)
    {
      CorNode* eP = corTreeObject(allocP, NULL);
      corTreeChildAdd(eP, corTreeString(allocP, "id", aP->entity));
      corTreeChildAdd(listP, eP);
      return listP;
    }

    snprintf(path,  sizeof(path),  "/entities/%s", urlEncode(allocP, aP->entity));
    snprintf(query, sizeof(query), "attrs=%s&options=keyValues", urlEncode(allocP, aP->attribute));

    if ((brokerRequest(allocP, CorVerbGet, path, query, NULL, &resp) == false) || (resp.status != 200) || (resp.body == NULL))
    {
      brokerFailed(why, whyLen, aP->entity, &resp);
      return NULL;
    }

    corTreeChildAdd(listP, resp.body);
    return listP;
  }

  snprintf(query, sizeof(query), "type=%s%s%s&options=keyValues&limit=1000", urlEncode(allocP, aP->type),
           (aP->q != NULL) ? "&q=" : "", (aP->q != NULL) ? urlEncode(allocP, aP->q) : "");

  if ((brokerRequest(allocP, CorVerbGet, "/entities", query, NULL, &resp) == false) || (resp.status != 200) || (resp.body == NULL) || (resp.body->type != CorArray))
  {
    brokerFailed(why, whyLen, "GET /entities", &resp);
    return NULL;
  }

  return resp.body;
}



// -----------------------------------------------------------------------------
//
// attrsPatch - PATCH /entities/<id>/attrs with { <name>: <value> }, as a Property - queued, to the sender
// (sender.c); a failure comes back later, an "action" item (actionReport)
//
static void attrsPatch(CorAlloc* allocP, const char* ruleId, int action, const char* id, CorNode* bodyP)
{
  senderPatch(ruleId, action, id, jsonText(allocP, bodyP));
}



// -----------------------------------------------------------------------------
//
// numberNode - a step's value as JSON: an integer when it is one
//
static CorNode* numberNode(CorAlloc* allocP, const char* name, double v)
{
  if ((v == (double) (long long) v) && (v > -9e15) && (v < 9e15))
    return corTreeInteger(allocP, name, (long long) v);
  return corTreeFloat(allocP, name, v);
}



// -----------------------------------------------------------------------------
//
// Steppers - the steps running: a number, 'by' every 'every', until an event, a bound, or the time limit
//
typedef struct Stepper
{
  struct Stepper*  next;
  char*            ruleId;
  char*            key;                          // the partition (the event's entity, "" - the rule is not partitioned)
  int              untilSub;                     // the subscription of the 'until' event (stable across reloads), -1: none
  char*            attribute;
  double           by, min, max;
  bool             hasMin, hasMax;
  long long        everyMs, nextAt, endAt;
  int              n;
  char**           idV;
  double*          valueV;
} Stepper;

static Stepper* stepperList    = NULL;
static Stepper* stepperPending = NULL;          // read from --state, waiting for their rules (steppersAdopt)



// -----------------------------------------------------------------------------
//
// stepperFree -
//
static void stepperFree(Stepper* sP)
{
  for (int ix = 0; ix < sP->n; ix++)
    free(sP->idV[ix]);
  free(sP->idV);
  free(sP->valueV);
  free(sP->ruleId);
  free(sP->key);
  free(sP->attribute);
  free(sP);
}



// -----------------------------------------------------------------------------
//
// StepperKey - which steppers: a rule's step of an attribute in a partition, or the steps an 'until' stops
//
typedef struct StepperKey
{
  const char*  ruleId;                           // NULL: by 'until'
  const char*  key;
  const char*  attribute;
  int          untilSub;
  const char*  entity;                           // the entity the 'until' event came from
} StepperKey;



// -----------------------------------------------------------------------------
//
// steppersDrop - the steppers <kP> names stop
//
static void steppersDrop(const StepperKey* kP)
{
  Stepper** prevPP = &stepperList;

  while (*prevPP != NULL)
  {
    Stepper* sP = *prevPP;
    bool     drop;

    if (kP->ruleId != NULL)
      drop = (strcmp(sP->ruleId, kP->ruleId) == 0) && (strcmp(sP->key, kP->key) == 0) && (strcmp(sP->attribute, kP->attribute) == 0);
    else
      drop = (sP->untilSub == kP->untilSub) && ((sP->key[0] == 0) || (strcmp(sP->key, kP->entity) == 0));

    if (drop)
    {
      *prevPP = sP->next;
      stepperFree(sP);
    }
    else
      prevPP = &sP->next;
  }
}



// -----------------------------------------------------------------------------
//
// stepperStart - a step: its targets' values now, the first step at once; one already running for the rule, the
// partition and the attribute is replaced
//
static bool stepperStart(CorAlloc* allocP, CepRule* ruleP, CepAction* aP, const char* entity, char* why, int whyLen)
{
  CorNode* listP = targetsGet(allocP, aP, true, why, whyLen);
  if (listP == NULL)
    return false;

  const char* key = ruleP->partitioned ? entity : "";

  StepperKey same = { ruleP->id, key, aP->attribute, -1, NULL };
  steppersDrop(&same);

  Stepper* sP = calloc(1, sizeof(Stepper));
  int      n  = 0;

  for (CorNode* eP = listP->value.head; eP != NULL; eP = eP->next)
    ++n;

  if ((sP == NULL) || ((sP->idV = calloc(n + 1, sizeof(char*))) == NULL) || ((sP->valueV = calloc(n + 1, sizeof(double))) == NULL))
  {
    if (sP != NULL)
    {
      free(sP->idV);
      free(sP);
    }
    snprintf(why, whyLen, "step: out of memory");
    return false;
  }

  for (CorNode* eP = listP->value.head; eP != NULL; eP = eP->next)
  {
    CorNode* idP = corTreeLookup(eP, "id");
    CorNode* vP  = corTreeLookup(eP, aP->attribute);

    if ((idP == NULL) || (idP->type != CorString))
      continue;
    if ((vP == NULL) || ((vP->type != CorInt) && (vP->type != CorFloat)))
    {
      logLine("rule %s: step - %s has no number '%s', left out", ruleP->id, idP->value.s, aP->attribute);
      continue;
    }

    sP->idV[sP->n]    = strdup(idP->value.s);
    sP->valueV[sP->n] = (vP->type == CorInt) ? (double) vP->value.i : vP->value.f;
    ++sP->n;
  }

  long long now = nowMs();

  sP->ruleId    = strdup(ruleP->id);
  sP->key       = strdup(key);
  sP->untilSub  = ((aP->until >= 0) && (rules != NULL)) ? rules->watchV[ruleP->untilV[aP->until].watch].sub : -1;
  sP->attribute = strdup(aP->attribute);
  sP->by        = aP->by;
  sP->min       = aP->min;
  sP->max       = aP->max;
  sP->hasMin    = aP->hasMin;
  sP->hasMax    = aP->hasMax;
  sP->everyMs   = aP->everyMs;
  sP->nextAt    = now;
  sP->endAt     = now + aP->forMs;
  sP->next      = stepperList;
  stepperList   = sP;

  return true;
}



// -----------------------------------------------------------------------------
//
// steppersRun - the steps due; returns the next one's time, 0: none
//
static long long steppersRun(CorAlloc* allocP, long long now)
{
  long long next   = 0;
  Stepper** prevPP = &stepperList;

  while (*prevPP != NULL)
  {
    Stepper* sP   = *prevPP;
    bool     done = false;

    if (now >= sP->endAt)
    {
      logLine("rule %s: step of %s stopped - its time limit", sP->ruleId, sP->attribute);
      done = true;
    }
    else if (now >= sP->nextAt)
    {
      int atBound = 0;

      for (int ix = 0; ix < sP->n; ix++)
      {
        double v = sP->valueV[ix] + sP->by;

        if (sP->hasMax && (v >= sP->max)) { v = sP->max; ++atBound; }
        if (sP->hasMin && (v <= sP->min)) { v = sP->min; ++atBound; }

        if (v != sP->valueV[ix])
        {
          CorNode* bodyP = corTreeObject(allocP, NULL);
          CorNode* pP    = corTreeObject(allocP, sP->attribute);

          corTreeChildAdd(pP, corTreeString(allocP, "type", "Property"));
          corTreeChildAdd(pP, numberNode(allocP, "value", v));
          corTreeChildAdd(bodyP, pP);
          attrsPatch(allocP, sP->ruleId, 0, sP->idV[ix], bodyP);
          sP->valueV[ix] = v;
          stateDirty     = true;
        }
      }

      sP->nextAt += sP->everyMs;
      if (sP->nextAt <= now)
        sP->nextAt = now + sP->everyMs;

      if (atBound == sP->n)
      {
        logLine("rule %s: step of %s stopped - %s", sP->ruleId, sP->attribute, (sP->n == 0) ? "no target" : "at its bound");
        done = true;
      }
    }

    if (done)
    {
      *prevPP    = sP->next;
      stateDirty = true;
      stepperFree(sP);
      continue;
    }

    if ((next == 0) || (sP->nextAt < next))
      next = sP->nextAt;
    if (sP->endAt < next)
      next = sP->endAt;
    prevPP = &sP->next;
  }

  return next;
}



// -----------------------------------------------------------------------------
//
// actionRun - one action on each of its targets; false: one target or more failed (why: the first reason)
//
static bool actionRun(CorAlloc* allocP, CepRule* ruleP, int action, CepAction* aP, const char* entity, char* why, int whyLen)
{
  if (aP->kind == CepActionStep)
    return stepperStart(allocP, ruleP, aP, entity, why, whyLen);

  CorNode* listP = targetsGet(allocP, aP, aP->kind == CepActionToggle, why, whyLen);
  if (listP == NULL)
    return false;

  bool ok = true;

  for (CorNode* eP = listP->value.head; eP != NULL; eP = eP->next)
  {
    CorNode* idP   = corTreeLookup(eP, "id");
    CorNode* bodyP = corTreeObject(allocP, NULL);

    if ((idP == NULL) || (idP->type != CorString))
      continue;

    if (aP->kind == CepActionSet)
    {
      for (CorNode* vP = aP->set->value.head; vP != NULL; vP = vP->next)
        corTreeChildAdd(bodyP, property(allocP, vP->name, vP));
    }
    else
    {
      CorNode* vP = corTreeLookup(eP, aP->attribute);
      if ((vP == NULL) || (vP->type != CorBoolean))
      {
        if (ok)
          snprintf(why, whyLen, "toggle %s: %s is not a boolean", idP->value.s, aP->attribute);
        ok = false;
        continue;
      }
      corTreeChildAdd(bodyP, property(allocP, aP->attribute, corTreeBoolean(allocP, aP->attribute, !vP->value.b)));
    }

    attrsPatch(allocP, ruleP->id, action, idP->value.s, bodyP);
  }

  return ok;
}



// -----------------------------------------------------------------------------
//
// condHolds - an 'if': does the broker have an entity that matches? Asked when the rule is about to fire: the
// broker evaluates the q, corCep keeps no copy of the entities
//
static bool condHolds(CorAlloc* allocP, CepRule* ruleP, CepCondition* cP)
{
  char        query[2048];
  CepResponse resp;

  snprintf(query, sizeof(query), "type=%s%s%s&q=%s&options=keyValues&limit=1", urlEncode(allocP, cP->type),
           (cP->entity != NULL) ? "&id=" : "", (cP->entity != NULL) ? urlEncode(allocP, cP->entity) : "", urlEncode(allocP, cP->q));

  if ((brokerRequest(allocP, CorVerbGet, "/entities", query, NULL, &resp) == false) || (resp.status != 200) || (resp.body == NULL) || (resp.body->type != CorArray))
  {
    char why[512];
    brokerFailed(why, sizeof(why), "GET /entities", &resp);
    logLine("rule %s: 'if' %s - not known, taken as false: %s", ruleP->id, cP->q, why);
    return false;
  }

  return resp.body->value.head != NULL;
}



// -----------------------------------------------------------------------------
//
// condFailing - a condition, leaf or group: NULL when it holds, else the condition that does not - for an 'all'
// its first member that does not hold (the rest not asked), for an 'any' the group itself (each asked until one
// holds)
//
static CepCondition* condFailing(CorAlloc* allocP, CepRule* ruleP, CepCondition* cP)
{
  if (cP->kind == CondLeaf)
    return (condHolds(allocP, ruleP, cP) == true) ? NULL : cP;

  for (int ix = 0; ix < cP->kids; ix++)
  {
    CepCondition* failP = condFailing(allocP, ruleP, &cP->kidV[ix]);

    if ((cP->kind == CondAll) && (failP != NULL))
      return failP;
    if ((cP->kind == CondAny) && (failP == NULL))
      return NULL;
  }

  return (cP->kind == CondAll) ? NULL : cP;
}



// -----------------------------------------------------------------------------
//
// condText - a condition in words, for the log: a leaf its q; a group "all(a; b)", "any(a | b)"
//
void condText(CepCondition* cP, char* buf, int bufLen)
{
  if (cP->kind == CondLeaf)
  {
    snprintf(buf, bufLen, "%s", cP->q);
    return;
  }

  int len = snprintf(buf, bufLen, "%s(", (cP->kind == CondAll) ? "all" : "any");

  for (int ix = 0; (ix < cP->kids) && (len < bufLen - 1); ix++)
  {
    if (ix > 0)
      len += snprintf(&buf[len], bufLen - len, "%s", (cP->kind == CondAll) ? "; " : " | ");
    if (len < bufLen - 1)
    {
      condText(&cP->kidV[ix], &buf[len], bufLen - len);
      len += strlen(&buf[len]);
    }
  }
  if (len < bufLen - 1)
    snprintf(&buf[len], bufLen - len, ")");
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
// statsWrite - POST /entities/<rule>/attrs: fired, firedAt, droppedMatches, droppedValues, lastError (append: made
// the first time, replaced after)
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

  long long dropped = patternsDropped(patterns, sP->id);
  if (dropped > 0)
  {
    CorNode* dP = corTreeObject(allocP, "droppedMatches");

    corTreeChildAdd(dP, corTreeString(allocP, "type", "Property"));
    corTreeChildAdd(dP, corTreeInteger(allocP, "value", dropped));
    corTreeChildAdd(bodyP, dP);
  }
  sP->droppedWritten = dropped;

  long long droppedValues = patternsDroppedValues(patterns, sP->id);
  if (droppedValues > 0)
  {
    CorNode* dP = corTreeObject(allocP, "droppedValues");

    corTreeChildAdd(dP, corTreeString(allocP, "type", "Property"));
    corTreeChildAdd(dP, corTreeInteger(allocP, "value", droppedValues));
    corTreeChildAdd(bodyP, dP);
  }
  sP->droppedValuesWritten = droppedValues;

  if (sP->droppedActions > 0)
  {
    CorNode* dP = corTreeObject(allocP, "droppedActions");

    corTreeChildAdd(dP, corTreeString(allocP, "type", "Property"));
    corTreeChildAdd(dP, corTreeInteger(allocP, "value", sP->droppedActions));
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
static void statsFlush(CorAlloc* allocP, bool force)
{
  time_t now = time(NULL);

  for (Stats* sP = statsList; sP != NULL; sP = sP->next)
  {
    if ((patternsDropped(patterns, sP->id) != sP->droppedWritten) || (patternsDroppedValues(patterns, sP->id) != sP->droppedValuesWritten))
      sP->dirty = true;                          // a drop is news without a fire too

    if ((sP->dirty == true) && (sP->seen == true) && (force || (now - sP->writtenAt >= statsInterval)))
      statsWrite(allocP, sP);
  }
}



// -----------------------------------------------------------------------------
//
// actionReport - the sender's word on a PATCH that failed, or was dropped: "<rule>\n<action>\n<dropped>\n<why>"
//
static void actionReport(char* text)
{
  char* ruleId  = text;
  char* actionS = strchr(ruleId, '\n');
  char* droppedS = (actionS != NULL) ? strchr(actionS + 1, '\n') : NULL;
  char* why      = (droppedS != NULL) ? strchr(droppedS + 1, '\n') : NULL;

  if (why == NULL)
    return;
  *actionS++  = 0;
  *droppedS++ = 0;
  *why++      = 0;

  int    action = atoi(actionS);
  Stats* sP     = NULL;

  for (Stats* xP = statsList; (xP != NULL) && (sP == NULL); xP = xP->next)
  {
    if (strcmp(xP->id, ruleId) == 0)
      sP = xP;
  }

  if (action == 0)
    logLine("rule %s: step - %s", ruleId, why);
  else
    logLine("rule %s: action %d - %s", ruleId, action, why);

  if (sP == NULL)                                // the rule is gone meanwhile
    return;

  if (action == 0)
    snprintf(sP->lastError, sizeof(sP->lastError), "step: %s", why);
  else
    snprintf(sP->lastError, sizeof(sP->lastError), "action %d: %s", action, why);
  nowIso(sP->lastErrorAt, sizeof(sP->lastErrorAt));
  if (atoi(droppedS) == 1)
    ++sP->droppedActions;
  sP->dirty = true;
}



// -----------------------------------------------------------------------------
//
// ruleFire - its actions, in order; one failing does not stop the others
//
static void ruleFire(CorAlloc* allocP, CepRule* ruleP, const char* entity)
{
  int    nFailed = 0;
  Stats* sP      = statsOf(ruleP);

  for (int ix = 0; ix < ruleP->conds; ix++)
  {
    CepCondition* failP = condFailing(allocP, ruleP, &ruleP->condV[ix]);

    if (failP != NULL)
    {
      char text[512];
      condText(failP, text, sizeof(text));
      logLine("rule %s: 'if' %s does not hold - not fired (%s)", ruleP->id, text, entity);
      return;
    }
  }

  for (int ix = 0; ix < ruleP->actions; ix++)
  {
    CepAction* aP       = &ruleP->actionV[ix];
    char       why[512] = { 0 };
    bool       ok       = actionRun(allocP, ruleP, ix + 1, aP, entity, why, sizeof(why));

    if (ok == false)
    {
      logLine("rule %s: action %d - %s", ruleP->id, ix + 1, why);
      ++nFailed;

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

  logLine("rule %s fired (%s)%s", ruleP->id, entity, (nFailed > 0) ? " - an action failed" : "");
}



// -----------------------------------------------------------------------------
//
// fireCb - the patterns' way to fire a rule
//
static void fireCb(CepRule* ruleP, const char* entity, void* ctx)
{
  stateDirty = true;
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

    if (rules->watchV[n].sub >= 0)                                          // an 'until' event stops its steps
    {
      StepperKey until = { NULL, NULL, NULL, rules->watchV[n].sub, idP->value.s };
      steppersDrop(&until);
    }

    CorNode* vP = corTreeLookup(eP, wP->attribute);                       // keyValues: the value itself
    double   v  = 0;
    bool     number = (vP->type == CorInt) || (vP->type == CorFloat);

    if (number)
      v = (vP->type == CorInt) ? (double) vP->value.i : vP->value.f;
    patternsEvent(patterns, rules, n, idP->value.s, number ? &v : NULL, nowMs(), fireCb, allocP);
    stateDirty = true;
  }
}



// -----------------------------------------------------------------------------
//
// steppersSave - the steps running, for --state: what each has done so far; what it does (by, bounds, every,
// until) is the rule's, read again from it after a restart. Times: the wall clock, ms.
//
CorNode* steppersSave(CorAlloc* aP, long long nowMono, long long nowWall)
{
  long long toWall = nowWall - nowMono;
  CorNode*  vP     = corTreeArray(aP, "steps");

  for (Stepper* sP = stepperList; sP != NULL; sP = sP->next)
  {
    CorNode* oP = corTreeObject(aP, NULL);
    CorNode* tV = corTreeArray(aP, "targets");

    corTreeChildAdd(oP, corTreeString(aP, "rule", sP->ruleId));
    corTreeChildAdd(oP, corTreeString(aP, "key", sP->key));
    corTreeChildAdd(oP, corTreeString(aP, "attribute", sP->attribute));
    corTreeChildAdd(oP, corTreeInteger(aP, "nextAt", sP->nextAt + toWall));
    corTreeChildAdd(oP, corTreeInteger(aP, "endAt", sP->endAt + toWall));
    for (int ix = 0; ix < sP->n; ix++)
    {
      CorNode* targetP = corTreeArray(aP, NULL);
      corTreeChildAdd(targetP, corTreeString(aP, NULL, sP->idV[ix]));
      corTreeChildAdd(targetP, corTreeFloat(aP, NULL, sP->valueV[ix]));
      corTreeChildAdd(tV, targetP);
    }
    corTreeChildAdd(oP, tV);
    corTreeChildAdd(vP, oP);
  }

  return vP;
}



// -----------------------------------------------------------------------------
//
// steppersLoad - the steps saved, kept aside till the rules are read (steppersAdopt); the number read
//
int steppersLoad(CorNode* topP, long long nowMono, long long nowWall)
{
  long long toMono = nowMono - nowWall;
  CorNode*  vP     = corTreeLookup(topP, "steps");
  int       n      = 0;

  for (CorNode* oP = ((vP != NULL) && (vP->type == CorArray)) ? vP->value.head : NULL; oP != NULL; oP = oP->next)
  {
    CorNode* ruleP   = corTreeLookup(oP, "rule");
    CorNode* keyP    = corTreeLookup(oP, "key");
    CorNode* attrP   = corTreeLookup(oP, "attribute");
    CorNode* nextP   = corTreeLookup(oP, "nextAt");
    CorNode* endP    = corTreeLookup(oP, "endAt");
    CorNode* tV      = corTreeLookup(oP, "targets");

    if ((ruleP == NULL) || (ruleP->type != CorString) || (keyP == NULL) || (keyP->type != CorString) ||
        (attrP == NULL) || (attrP->type != CorString) || (nextP == NULL) || (nextP->type != CorInt) ||
        (endP == NULL) || (endP->type != CorInt) || (tV == NULL) || (tV->type != CorArray))
      continue;

    int targets = 0;
    for (CorNode* tP = tV->value.head; tP != NULL; tP = tP->next)
      ++targets;

    Stepper* sP = calloc(1, sizeof(Stepper));
    if ((sP == NULL) || ((sP->idV = calloc(targets + 1, sizeof(char*))) == NULL) || ((sP->valueV = calloc(targets + 1, sizeof(double))) == NULL))
    {
      if (sP != NULL)
      {
        free(sP->idV);
        free(sP);
      }
      break;
    }

    for (CorNode* tP = tV->value.head; tP != NULL; tP = tP->next)
    {
      CorNode* idP = (tP->type == CorArray) ? tP->value.head : NULL;
      CorNode* xP  = (idP != NULL) ? idP->next : NULL;

      if ((xP == NULL) || (idP->type != CorString) || ((xP->type != CorFloat) && (xP->type != CorInt)))
        continue;
      sP->idV[sP->n]    = strdup(idP->value.s);
      sP->valueV[sP->n] = (xP->type == CorInt) ? (double) xP->value.i : xP->value.f;
      ++sP->n;
    }

    sP->ruleId     = strdup(ruleP->value.s);
    sP->key        = strdup(keyP->value.s);
    sP->attribute  = strdup(attrP->value.s);
    sP->nextAt     = nextP->value.i + toMono;
    sP->endAt      = endP->value.i + toMono;
    sP->untilSub   = -1;
    sP->next       = stepperPending;
    stepperPending = sP;
    ++n;
  }

  return n;
}



// -----------------------------------------------------------------------------
//
// steppersAdopt - the steps read from --state go on, with what their rule's step action says now; a rule, or a step
// of that attribute in it, gone meanwhile: the step stops
//
static void steppersAdopt(void)
{
  while (stepperPending != NULL)
  {
    Stepper*   sP    = stepperPending;
    CepRule*   ruleP = NULL;
    CepAction* aP    = NULL;

    stepperPending = sP->next;

    for (int ix = 0; (ix < rules->rules) && (ruleP == NULL); ix++)
    {
      if (strcmp(rules->ruleV[ix].id, sP->ruleId) == 0)
        ruleP = &rules->ruleV[ix];
    }
    for (int ix = 0; (ruleP != NULL) && (ix < ruleP->actions) && (aP == NULL); ix++)
    {
      CepAction* xP = &ruleP->actionV[ix];
      if ((xP->kind == CepActionStep) && (strcmp(xP->attribute, sP->attribute) == 0))
        aP = xP;
    }

    if (aP == NULL)
    {
      logLine("rule %s: step of %s not continued - %s", sP->ruleId, sP->attribute, (ruleP == NULL) ? "the rule is gone" : "the rule steps it no more");
      stepperFree(sP);
      continue;
    }

    sP->untilSub = (aP->until >= 0) ? rules->watchV[ruleP->untilV[aP->until].watch].sub : -1;
    sP->by       = aP->by;
    sP->min      = aP->min;
    sP->max      = aP->max;
    sP->hasMin   = aP->hasMin;
    sP->hasMax   = aP->hasMax;
    sP->everyMs  = aP->everyMs;
    sP->next     = stepperList;
    stepperList  = sP;
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
  patternsAdopt(patterns, rules, nowMs());
  stateDirty = true;

  Stepper** stepPP = &stepperList;                     // the steps of rules gone stop
  while (*stepPP != NULL)
  {
    Stepper* tP    = *stepPP;
    bool     found = false;

    for (int ix = 0; (ix < rules->rules) && (found == false); ix++)
      found = (strcmp(rules->ruleV[ix].id, tP->ruleId) == 0);

    if (found)
      stepPP = &tP->next;
    else
    {
      *stepPP = tP->next;
      stepperFree(tP);
    }
  }

  steppersAdopt();

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

  patterns = patternsNew(0);
  if (patterns == NULL)
  {
    logLine("out of memory for the patterns' state");
    exit(1);
  }

  stateLoad(patterns);                           // before the first rules: their states found again by id
  rulesReload();

  long long renewedAt = nowMs();
  long long savedAt   = 0;

  while (true)
  {
    //
    // Stopping (SIGTERM, SIGINT - seen within a second): the state and the statistics written first
    //
    if (cepStopping)
    {
      int left = senderDrain(2000);              // the PATCHes made go out first - 2 s at most
      if (left > 0)
        logLine("stopping: %d action%s not sent", left, (left == 1) ? "" : "s");
      stateSave(patterns);
      statsFlush(&alloc, true);
      logLine("stopped (signal %d)%s", (int) cepStopping, (stateFile != NULL) ? " - the state written" : "");
      exit(0);
    }

    //
    // The deadlines passed (absences, superseded fires waiting) fire; the wait: till the next one, a second at most
    //
    long long now  = nowMs();

    //
    // The subscriptions renewed - a broker that keeps their owner only for a while would stop notifying
    //
    if ((renewInterval > 0) && (now - renewedAt >= (long long) renewInterval * 1000))
    {
      renewedAt = now;
      if (subsRenew() == true)
        rulesReload();
    }

    long long next = patternsTimers(patterns, rules, now, fireCb, &alloc);
    long long step = steppersRun(&alloc, nowMs());

    if ((step != 0) && ((next == 0) || (step < next)))
      next = step;
    long long wait = ((next != 0) && (next - now < 1000)) ? next - now : 1000;

    if (stateDirty && (stateFile != NULL) && (nowMs() - savedAt >= 1000))
    {
      stateSave(patterns);
      stateDirty = false;
      savedAt    = nowMs();
    }

    statsFlush(&alloc, false);
    corAllocBufferReset(&alloc, true);

    Item* iP = dequeue((wait > 0) ? wait : 1);

    if (iP == NULL)                              // a tick, or a deadline
      continue;

    if (strcmp(iP->which, "rules") == 0)
      rulesReload();
    else if (strcmp(iP->which, "action") == 0)
      actionReport(iP->body);
    else
      eventHandle(&alloc, atoi(iP->which), iP->body);

    free(iP->body);
    free(iP);
    statsFlush(&alloc, false);
    corAllocBufferReset(&alloc, true);
  }
}
