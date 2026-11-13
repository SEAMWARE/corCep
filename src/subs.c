//
// FILE            subs.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corCep's subscriptions, all with the id prefix urn:ngsi-ld:Subscription:corcep-:
//   corcep-rules  every Rule entity - a rule made, changed or deleted reloads the rules
//   corcep-<n>    one per watch (entity or type, attribute, q) - its notifications are events
// The notifications go to <notifyBase>/notify/<secret>/<rules | n>.
//
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // realloc, free
#include <string.h>                              // strncmp, strcmp, strdup

#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, ...
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corCep.h"



//
// The event subscriptions made - kept across rule generations: a watch that stays keeps its subscription (and its
// number), so a reload loses no event. Only the watches no rule has any more are deleted.
//
typedef struct Sub
{
  char*  key;                                    // "<entity or type>|<attribute>|<q>", malloc'd
  int    no;                                     // corcep-<no>
  bool   used;
} Sub;

static Sub*  subV   = NULL;
static int   subN   = 0;
static int   subMax = 0;
static int   nextNo = 0;



// -----------------------------------------------------------------------------
//
// subDelete -
//
static void subDelete(CorAlloc* allocP, const char* id)
{
  char        path[512];
  CepResponse resp;

  snprintf(path, sizeof(path), "/subscriptions/%s", urlEncode(allocP, id));
  if ((brokerRequest(allocP, CorVerbDelete, path, NULL, NULL, &resp) == false) || ((resp.status != 204) && (resp.status != 404)))
    logLine("DELETE subscription %s: %s", id, (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
}



// -----------------------------------------------------------------------------
//
// subCreate - entities [{ id | type }], watchedAttributes [attribute], q, keyValues
//
// The Rule subscription (attribute NULL) watches what makes a rule - not the statistics corCep writes to it
// (fired, firedAt, lastError): a write of those must not read the rules again.
//
static bool subCreate(CorAlloc* allocP, const char* id, const char* entity, const char* type, const char* attribute, const char* q, const char* which)
{
  CorNode* subP      = corTreeObject(allocP, NULL);
  CorNode* entitiesP = corTreeArray(allocP, "entities");
  CorNode* selP      = corTreeObject(allocP, NULL);
  CorNode* notifP    = corTreeObject(allocP, "notification");
  CorNode* endpointP = corTreeObject(allocP, "endpoint");
  char     uri[1024];
  char     description[512];

  snprintf(uri, sizeof(uri), "%s/notify/%s/%s", cep.notifyBase, cep.secret, which);
  snprintf(description, sizeof(description), "corCep: %s %s%s%s", (entity != NULL) ? entity : type, (attribute != NULL) ? attribute : "(the rules)",
           (q != NULL) ? " where " : "", (q != NULL) ? q : "");

  corTreeChildAdd(subP, corTreeString(allocP, "id", id));
  corTreeChildAdd(subP, corTreeString(allocP, "type", "Subscription"));
  corTreeChildAdd(subP, corTreeString(allocP, "description", description));

  if (entity != NULL)
    corTreeChildAdd(selP, corTreeString(allocP, "id", entity));
  corTreeChildAdd(selP, corTreeString(allocP, "type", type));
  corTreeChildAdd(entitiesP, selP);
  corTreeChildAdd(subP, entitiesP);

  //
  // The Rule subscription (no attribute): a rule deleted is news too - NGSI-LD's default triggers are only
  // entityCreated and entityUpdated
  //
  if (attribute == NULL)
  {
    CorNode* watchedP  = corTreeArray(allocP, "watchedAttributes");
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "when"));
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "then"));
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "enabled"));
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "name"));
    corTreeChildAdd(subP, watchedP);

    CorNode* triggersP = corTreeArray(allocP, "notificationTrigger");
    corTreeChildAdd(triggersP, corTreeString(allocP, NULL, "entityCreated"));
    corTreeChildAdd(triggersP, corTreeString(allocP, NULL, "entityUpdated"));
    corTreeChildAdd(triggersP, corTreeString(allocP, NULL, "entityDeleted"));
    corTreeChildAdd(subP, triggersP);
  }

  if (attribute != NULL)
  {
    CorNode* watchedP = corTreeArray(allocP, "watchedAttributes");
    CorNode* attrsP   = corTreeArray(allocP, "attributes");
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, attribute));
    corTreeChildAdd(attrsP,   corTreeString(allocP, NULL, attribute));
    corTreeChildAdd(subP, watchedP);
    corTreeChildAdd(notifP, attrsP);

    if (q != NULL)
      corTreeChildAdd(subP, corTreeString(allocP, "q", q));
  }

  corTreeChildAdd(notifP, corTreeString(allocP, "format", "keyValues"));
  corTreeChildAdd(endpointP, corTreeString(allocP, "uri", uri));
  corTreeChildAdd(endpointP, corTreeString(allocP, "accept", "application/json"));
  corTreeChildAdd(notifP, endpointP);
  corTreeChildAdd(subP, notifP);

  CepResponse resp;
  if ((brokerRequest(allocP, CorVerbPost, "/subscriptions", NULL, subP, &resp) == false) || (resp.status != 201))
  {
    logLine("POST subscription %s (%s): %s", id, description, (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// subsInit - the subscriptions of an earlier run removed (their notifications would go to an old secret), the
// Rule subscription made
//
bool subsInit(void)
{
  CorAlloc    alloc;
  CepResponse resp;

  corAllocBufferInit(&alloc, NULL, 0, 64 * 1024, NULL, "corCep subsInit");

  bool got = brokerRequest(&alloc, CorVerbGet, "/subscriptions", "limit=1000", NULL, &resp);

  if (got && nonexistentTenant(&resp))
    resp.body = NULL;                            // a tenant with nothing in it yet: no subscriptions to remove
  else if ((got == false) || (resp.status != 200))
  {
    logLine("GET /subscriptions: %s", (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    corAllocBufferReset(&alloc, false);
    return false;
  }

  if ((resp.body != NULL) && (resp.body->type == CorArray))
  {
    for (CorNode* sP = resp.body->value.head; sP != NULL; sP = sP->next)
    {
      CorNode* idP = corTreeLookup(sP, "id");
      if ((idP != NULL) && (idP->type == CorString) && (strncmp(idP->value.s, CEP_SUB_PREFIX, sizeof(CEP_SUB_PREFIX) - 1) == 0))
        subDelete(&alloc, idP->value.s);
    }
  }

  bool ok = subCreate(&alloc, CEP_SUB_RULES, NULL, "Rule", NULL, NULL, "rules");

  corAllocBufferReset(&alloc, false);
  return ok;
}



// -----------------------------------------------------------------------------
//
// watchKey -
//
static void watchKey(CepWatch* wP, char* key, int keyLen)
{
  snprintf(key, keyLen, "%s%s|%s|%s", (wP->entity != NULL) ? "id:" : "type:", (wP->entity != NULL) ? wP->entity : wP->type, wP->attribute,
           (wP->q != NULL) ? wP->q : "");
}



// -----------------------------------------------------------------------------
//
// subsApply - every watch of the generation gets its subscription: the one it had, or a new one; the
// subscriptions no watch has any more are deleted - after, so no event is lost in between
//
bool subsApply(CepRules* rulesP)
{
  CorAlloc alloc;
  bool     ok = true;

  corAllocBufferInit(&alloc, NULL, 0, 64 * 1024, NULL, "corCep subsApply");

  for (int ix = 0; ix < subN; ix++)
    subV[ix].used = false;

  for (int n = 0; n < rulesP->watches; n++)
  {
    CepWatch* wP = &rulesP->watchV[n];
    char      key[1024];
    int       ix;

    watchKey(wP, key, sizeof(key));
    for (ix = 0; ix < subN; ix++)
    {
      if (strcmp(subV[ix].key, key) == 0)
        break;
    }

    if (ix < subN)
    {
      subV[ix].used = true;
      wP->sub       = subV[ix].no;
      continue;
    }

    char id[128];
    char which[16];
    int  no = nextNo++;

    snprintf(id, sizeof(id), "%s%d", CEP_SUB_PREFIX, no);
    snprintf(which, sizeof(which), "%d", no);

    if (subCreate(&alloc, id, wP->entity, wP->type, wP->attribute, wP->q, which) == false)
    {
      wP->sub = -1;
      ok      = false;
      continue;
    }

    if (subN == subMax)
    {
      subMax = (subMax == 0) ? 16 : subMax * 2;
      subV   = realloc(subV, subMax * sizeof(Sub));
    }
    subV[subN].key  = strdup(key);
    subV[subN].no   = no;
    subV[subN].used = true;
    ++subN;
    wP->sub = no;
  }

  //
  // The ones no watch has any more
  //
  int kept = 0;
  for (int ix = 0; ix < subN; ix++)
  {
    if (subV[ix].used == true)
    {
      subV[kept++] = subV[ix];
      continue;
    }

    char id[128];
    snprintf(id, sizeof(id), "%s%d", CEP_SUB_PREFIX, subV[ix].no);
    subDelete(&alloc, id);
    free(subV[ix].key);
  }
  subN = kept;

  corAllocBufferReset(&alloc, false);
  return ok;
}
