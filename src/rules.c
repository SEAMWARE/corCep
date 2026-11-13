//
// FILE            rules.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The rules: the broker's entities of type Rule (doc/design.md § 3), read whole into a generation of their own.
// A rule that is not right is logged and left out - the others run.
//
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, free
#include <string.h>                              // strcmp

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corTree/corTreeBuilder.h"              // corTreeArray
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corCep.h"



// -----------------------------------------------------------------------------
//
// str - a string member, NULL if absent or not a string
//
static char* str(CorNode* objP, const char* name)
{
  CorNode* nP = corTreeLookup(objP, name);
  return ((nP != NULL) && (nP->type == CorString)) ? nP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// json - a JsonProperty's value in the simplified (keyValues) representation: { "json": <value> }
//
static CorNode* json(CorNode* objP, const char* name)
{
  CorNode* nP = corTreeLookup(objP, name);
  if ((nP == NULL) || (nP->type != CorObject))
    return NULL;
  return corTreeLookup(nP, "json");
}



// -----------------------------------------------------------------------------
//
// count - the children of a container
//
static int count(CorNode* nP)
{
  int n = 0;
  for (CorNode* cP = nP->value.head; cP != NULL; cP = cP->next)
    ++n;
  return n;
}



// -----------------------------------------------------------------------------
//
// eventParse - `when` in P1: one event { entity | type, attribute }
//
static bool eventParse(CorAlloc* allocP, CorNode* whenP, CepEvent* evP, char* why, int whyLen)
{
  if ((whenP == NULL) || (whenP->type != CorObject))
  {
    snprintf(why, whyLen, "'when' - an object");
    return false;
  }

  char* entity    = str(whenP, "entity");
  char* type      = str(whenP, "type");
  char* attribute = str(whenP, "attribute");
  char* name      = str(whenP, "event");

  if ((entity == NULL) && (type == NULL))
  {
    snprintf(why, whyLen, "'when' names no 'entity' and no 'type'");
    return false;
  }
  if (attribute == NULL)
  {
    snprintf(why, whyLen, "'when' names no 'attribute'");
    return false;
  }

  //
  // An EntitySelector needs a type: an id alone gives its own - urn:ngsi-ld:<type>:...
  //
  if ((type == NULL) && (strncmp(entity, "urn:ngsi-ld:", 12) == 0))
  {
    const char* t   = entity + 12;
    const char* end = strchr(t, ':');
    if ((end != NULL) && (end > t))
    {
      type = corAlloc(allocP, end - t + 1);
      memcpy(type, t, end - t);
      type[end - t] = 0;
    }
  }
  if (type == NULL)
  {
    snprintf(why, whyLen, "'when' names an entity whose id holds no type (urn:ngsi-ld:<type>:...) - give 'type' too");
    return false;
  }

  evP->name      = corAllocStrdup(allocP, (name != NULL) ? name : attribute);
  evP->entity    = (entity != NULL) ? corAllocStrdup(allocP, entity) : NULL;
  evP->type      = (type   != NULL) ? corAllocStrdup(allocP, type)   : NULL;
  evP->attribute = corAllocStrdup(allocP, attribute);
  evP->watch     = -1;

  return true;
}



// -----------------------------------------------------------------------------
//
// actionParse - one of `then`: { "set": {...}, "entity": id } | { "toggle": attribute, "entity": id }
//
static bool actionParse(CorAlloc* allocP, CorNode* aP, CepAction* actP, char* why, int whyLen)
{
  if (aP->type != CorObject)
  {
    snprintf(why, whyLen, "an action - an object");
    return false;
  }

  char*    entity = str(aP, "entity");
  CorNode* setP   = corTreeLookup(aP, "set");
  char*    toggle = str(aP, "toggle");

  if (entity == NULL)
  {
    snprintf(why, whyLen, "an action names no 'entity'");
    return false;
  }
  if ((setP != NULL) == (toggle != NULL))
  {
    snprintf(why, whyLen, "an action is one of 'set' and 'toggle'");
    return false;
  }

  actP->entity = corAllocStrdup(allocP, entity);

  if (setP != NULL)
  {
    if ((setP->type != CorObject) || (setP->value.head == NULL))
    {
      snprintf(why, whyLen, "'set' - an object of attributes and values");
      return false;
    }
    actP->kind = CepActionSet;
    actP->set  = setP;                                 // lives in the generation's allocator
  }
  else
  {
    actP->kind      = CepActionToggle;
    actP->attribute = corAllocStrdup(allocP, toggle);
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// watchOf - the watch an event needs: one subscription per (entity or type, attribute)
//
static int watchOf(CepRules* rP, CepEvent* evP, int max)
{
  for (int ix = 0; ix < rP->watches; ix++)
  {
    CepWatch* wP = &rP->watchV[ix];
    if ((strcmp(wP->attribute, evP->attribute) == 0) &&
        (((wP->entity != NULL) && (evP->entity != NULL) && (strcmp(wP->entity, evP->entity) == 0)) ||
         ((wP->entity == NULL) && (evP->entity == NULL) && (strcmp(wP->type, evP->type) == 0))))
      return ix;
  }

  if (rP->watches >= max)
    return -1;

  CepWatch* wP  = &rP->watchV[rP->watches];
  wP->entity    = evP->entity;
  wP->type      = evP->type;
  wP->attribute = evP->attribute;

  return rP->watches++;
}



// -----------------------------------------------------------------------------
//
// rulesLoad - every Rule entity (enabled ones), NULL: the broker did not answer
//
CepRules* rulesLoad(void)
{
  CepRules* rP = calloc(1, sizeof(CepRules));
  if (rP == NULL)
    return NULL;

  corAllocBufferInit(&rP->alloc, NULL, 0, 256 * 1024, NULL, "corCep rules");

  CepResponse resp;
  bool got = brokerRequest(&rP->alloc, CorVerbGet, "/entities", "type=Rule&options=keyValues&limit=1000", NULL, &resp);

  if (got && nonexistentTenant(&resp))
  {
    //
    // A tenant with nothing in it yet: no rules. corCep's own Rule subscription makes the tenant (a POST
    // /subscriptions creates it), but a broker may not - zero rules either way, and the first Rule made is notified.
    //
    resp.status = 200;
    resp.body   = corTreeArray(&rP->alloc, NULL);
  }
  else if ((got == false) || (resp.status != 200))
  {
    logLine("the rules: GET /entities?type=Rule - %s", (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    rulesFree(rP);
    return NULL;
  }

  if ((resp.body == NULL) || (resp.body->type != CorArray))
  {
    logLine("the rules: the broker's answer is not an array");
    rulesFree(rP);
    return NULL;
  }

  int n = count(resp.body);
  rP->ruleV  = (CepRule*)  corAlloc(&rP->alloc, (n + 1) * sizeof(CepRule));
  rP->watchV = (CepWatch*) corAlloc(&rP->alloc, (n + 1) * sizeof(CepWatch));

  for (CorNode* eP = resp.body->value.head; eP != NULL; eP = eP->next)
  {
    char     why[256] = { 0 };
    CepRule* ruleP    = &rP->ruleV[rP->rules];
    char*    id       = str(eP, "id");
    CorNode* enabledP = corTreeLookup(eP, "enabled");
    CorNode* thenP    = json(eP, "then");

    if (id == NULL)
      continue;

    if ((enabledP != NULL) && (enabledP->type == CorBoolean) && (enabledP->value.b == false))
      continue;

    memset(ruleP, 0, sizeof(CepRule));
    ruleP->id   = corAllocStrdup(&rP->alloc, id);
    ruleP->name = corAllocStrdup(&rP->alloc, (str(eP, "name") != NULL) ? str(eP, "name") : id);

    if (eventParse(&rP->alloc, json(eP, "when"), &ruleP->event, why, sizeof(why)) == false)
      goto bad;

    if ((thenP == NULL) || (thenP->type != CorArray) || (thenP->value.head == NULL))
    {
      snprintf(why, sizeof(why), "'then' - a list of actions");
      goto bad;
    }

    ruleP->actionV = (CepAction*) corAlloc(&rP->alloc, count(thenP) * sizeof(CepAction));
    for (CorNode* aP = thenP->value.head; aP != NULL; aP = aP->next)
    {
      memset(&ruleP->actionV[ruleP->actions], 0, sizeof(CepAction));
      if (actionParse(&rP->alloc, aP, &ruleP->actionV[ruleP->actions], why, sizeof(why)) == false)
        goto bad;
      ++ruleP->actions;
    }

    ruleP->event.watch = watchOf(rP, &ruleP->event, n + 1);
    ++rP->rules;
    continue;

  bad:
    logLine("rule %s left out: %s", id, why);
  }

  logLine("%d rule%s, %d subscription%s", rP->rules, (rP->rules == 1) ? "" : "s", rP->watches, (rP->watches == 1) ? "" : "s");
  return rP;
}



// -----------------------------------------------------------------------------
//
// rulesFree -
//
void rulesFree(CepRules* rulesP)
{
  if (rulesP == NULL)
    return;

  corAllocBufferReset(&rulesP->alloc, false);
  free(rulesP);
}
