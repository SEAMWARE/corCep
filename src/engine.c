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
#include <pthread.h>                             // pthread_mutex_t, pthread_cond_t
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // malloc, free, atoi
#include <string.h>                              // strcmp, memcpy

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
// dequeue - waits for the next item
//
static Item* dequeue(void)
{
  pthread_mutex_lock(&queueMutex);
  while (queueHead == NULL)
    pthread_cond_wait(&queueCond, &queueMutex);

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
// ruleFire - its actions, in order; one failing does not stop the others
//
static void ruleFire(CorAlloc* allocP, CepRule* ruleP, const char* entity)
{
  int failed = 0;

  for (int ix = 0; ix < ruleP->actions; ix++)
  {
    CepAction* aP       = &ruleP->actionV[ix];
    char       why[512] = { 0 };
    bool       ok       = (aP->kind == CepActionSet) ? actionSet(allocP, aP, why, sizeof(why)) : actionToggle(allocP, aP, why, sizeof(why));

    if (ok == false)
    {
      logLine("rule %s: action %d - %s", ruleP->id, ix + 1, why);
      ++failed;
    }
  }

  logLine("rule %s fired (%s)%s", ruleP->id, entity, (failed > 0) ? " - an action failed" : "");
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

    for (int ix = 0; ix < rules->rules; ix++)
    {
      if (rules->ruleV[ix].event.watch == n)
        ruleFire(allocP, &rules->ruleV[ix], idP->value.s);
    }
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
    Item* iP = dequeue();

    if (strcmp(iP->which, "rules") == 0)
      rulesReload();
    else
      eventHandle(&alloc, atoi(iP->which), iP->body);

    free(iP->body);
    free(iP);
    corAllocBufferReset(&alloc, true);
  }
}
