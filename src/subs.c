//
// FILE            subs.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corCep's subscriptions, all with the id prefix urn:ngsi-ld:Subscription:corcep-:
//   corcep-rules  every Rule entity - a rule made, changed or deleted reloads the rules
//   corcep-<n>    one per watch (entity or type, attribute, q) - its notifications are events
// The Rule subscription is in the rules tenant; each watch's is in every data tenant, the same id in each.
// The notifications go to <notifyBase>/notify/<secret>/<rules | n | n/tenant> - n alone: the default tenant.
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
  int    no;                                     // corcep-<no> - the same id in each data tenant
  bool   used;
  bool*  inV;                                    // made in tenantV[ix] (tenants.c - a tenant keeps its index), malloc'd
  int    inN;
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
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "else"));
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "author"));
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "enabled"));
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "name"));
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "location"));         // sunrise / sunset: the rule's place
    corTreeChildAdd(watchedP, corTreeString(allocP, NULL, "tenant"));           // the tenants it runs in
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
// inTenant - " (tenant <t>)" for a log line - "" with --tenant (one tenant: nothing to tell apart)
//
static const char* inTenant(const char* tenant)
{
  static __thread char text[128];

  if (cep.single)
    return "";

  snprintf(text, sizeof(text), " (tenant %s)", TENANT_TEXT(tenant));
  return text;
}



// -----------------------------------------------------------------------------
//
// sweep - the subscriptions of an earlier run removed from the tenant at work (their notifications would go to an
// old secret); keepRules: not the Rule subscription (--tenant: the rules and the data in one tenant)
//
static bool sweep(CorAlloc* allocP, bool keepRules)
{
  CepResponse resp;
  bool        got = brokerRequest(allocP, CorVerbGet, "/subscriptions", "limit=1000", NULL, &resp);

  if (got && nonexistentTenant(&resp))
    return true;                                 // a tenant with nothing in it yet: no subscriptions to remove

  if ((got == false) || (resp.status != 200))
  {
    logLine("GET /subscriptions%s: %s", inTenant(cepTenantNow), (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    return false;
  }

  if ((resp.body != NULL) && (resp.body->type == CorArray))
  {
    for (CorNode* sP = resp.body->value.head; sP != NULL; sP = sP->next)
    {
      CorNode* idP = corTreeLookup(sP, "id");

      if ((idP == NULL) || (idP->type != CorString) || (strncmp(idP->value.s, CEP_SUB_PREFIX, sizeof(CEP_SUB_PREFIX) - 1) != 0))
        continue;
      if (keepRules && (strcmp(idP->value.s, CEP_SUB_RULES) == 0))
        continue;
      subDelete(allocP, idP->value.s);
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// subsInit - in the rules tenant: the subscriptions of an earlier run removed, the Rule subscription made
//
bool subsInit(void)
{
  CorAlloc alloc;

  corAllocBufferInit(&alloc, NULL, 0, 64 * 1024, NULL, "corCep subsInit");

  const char* before = tenantUse(cep.rulesTenant);
  bool        ok     = sweep(&alloc, false) && subCreate(&alloc, CEP_SUB_RULES, NULL, "Rule", NULL, NULL, "rules");
  tenantUse(before);

  corAllocBufferReset(&alloc, false);
  return ok;
}



// -----------------------------------------------------------------------------
//
// subRenew - a PATCH that changes nothing, made with corCep's token of now: a broker that keeps a subscription's
// owner only for a while (coraine --auth oidc: --oidcOwnerMaxAge, 24 h) keeps it again from now. An owner run out
// is not an error the broker reports - its notifications just stop coming. Returns the status (0: no answer).
//
static int subRenew(CorAlloc* allocP, const char* id)
{
  char        path[512];
  CorNode*    bodyP = corTreeObject(allocP, NULL);
  CepResponse resp;

  corTreeChildAdd(bodyP, corTreeBoolean(allocP, "isActive", true));
  snprintf(path, sizeof(path), "/subscriptions/%s", urlEncode(allocP, id));

  if (brokerRequest(allocP, CorVerbPatch, path, NULL, bodyP, &resp) == false)
  {
    logLine("renewing subscription %s%s: %s", id, inTenant(cepTenantNow), resp.error);
    return 0;
  }

  if ((resp.status != 204) && (resp.status != 404))
    logLine("renewing subscription %s%s: %d %s", id, inTenant(cepTenantNow), resp.status, (resp.text != NULL) ? resp.text : "no body");

  return resp.status;
}



// -----------------------------------------------------------------------------
//
// subsRenew - every subscription of corCep renewed (--renew-interval), in every tenant. One the broker no longer
// has is made again: the Rule subscription here, an event one by the rules read again - true: read them.
//
bool subsRenew(void)
{
  CorAlloc alloc;
  bool     reload = false;

  corAllocBufferInit(&alloc, NULL, 0, 64 * 1024, NULL, "corCep subsRenew");

  const char* before = tenantUse(cep.rulesTenant);
  if (subRenew(&alloc, CEP_SUB_RULES) == 404)
  {
    logLine("subscription %s gone - made again", CEP_SUB_RULES);
    subCreate(&alloc, CEP_SUB_RULES, NULL, "Rule", NULL, NULL, "rules");
    reload = true;                               // a rule changed while it was gone: read now
  }

  for (int ix = 0; ix < subN; ix++)
  {
    char id[128];

    snprintf(id, sizeof(id), "%s%d", CEP_SUB_PREFIX, subV[ix].no);
    for (int tx = 0; (tx < subV[ix].inN) && (tx < tenants); tx++)
    {
      if (subV[ix].inV[tx] == false)
        continue;

      tenantUse(tenantV[tx].name);
      if (subRenew(&alloc, id) == 404)
      {
        logLine("subscription %s%s gone - made again", id, inTenant(tenantV[tx].name));
        subV[ix].inV[tx] = false;                // subsApply makes it again
        reload           = true;
      }
    }
  }
  tenantUse(before);

  corAllocBufferReset(&alloc, false);
  return reload;
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
// subInTenants - a watch's subscription made in each data tenant it is not in yet; false: one failed
//
static bool subInTenants(CorAlloc* allocP, Sub* sP, CepWatch* wP)
{
  bool ok = true;

  if (sP->inN < tenants)
  {
    bool* vP = realloc(sP->inV, tenants * sizeof(bool));
    if (vP == NULL)
      return false;
    for (int tx = sP->inN; tx < tenants; tx++)
      vP[tx] = false;
    sP->inV = vP;
    sP->inN = tenants;
  }

  for (int tx = 0; tx < tenants; tx++)
  {
    if (sP->inV[tx] == true)
      continue;

    char id[128];
    char which[128];
    const char* name = tenantV[tx].name;

    snprintf(id, sizeof(id), "%s%d", CEP_SUB_PREFIX, sP->no);
    if (name[0] == 0)
      snprintf(which, sizeof(which), "%d", sP->no);                 // the default tenant: as corCep 0.1
    else
      snprintf(which, sizeof(which), "%d/%s", sP->no, name);

    tenantUse(name);
    if (subCreate(allocP, id, wP->entity, wP->type, wP->attribute, wP->q, which) == true)
      sP->inV[tx] = true;
    else
      ok = false;
  }
  tenantUse(NULL);

  return ok;
}



// -----------------------------------------------------------------------------
//
// subsApply - every watch of the generation gets its subscription in every data tenant: the one it had, or a new
// one; the subscriptions no watch has any more are deleted - after, so no event is lost in between. A tenant seen
// for the first time is swept first - an earlier run's subscriptions in it removed.
//
bool subsApply(CepRules* rulesP)
{
  static bool* sweptV = NULL;
  static int   swept  = 0;
  CorAlloc     alloc;
  bool         ok = true;

  corAllocBufferInit(&alloc, NULL, 0, 64 * 1024, NULL, "corCep subsApply");

  if (swept < tenants)
  {
    bool* vP = realloc(sweptV, tenants * sizeof(bool));
    if (vP != NULL)
    {
      for (int tx = swept; tx < tenants; tx++)
        vP[tx] = false;
      sweptV = vP;
      swept  = tenants;
    }
  }
  for (int tx = 0; tx < swept; tx++)
  {
    if (sweptV[tx] == true)
      continue;

    tenantUse(tenantV[tx].name);
    if (cep.single)                              // the rules tenant itself: subsInit swept it
      sweptV[tx] = true;
    else
      sweptV[tx] = sweep(&alloc, false);         // 0.1's Rule subscription too: the rules are in the rules tenant now
  }
  tenantUse(NULL);

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

    if (ix == subN)                              // a new watch: a number of its own
    {
      if (subN == subMax)
      {
        int  max = (subMax == 0) ? 16 : subMax * 2;
        Sub* vP  = realloc(subV, max * sizeof(Sub));

        if (vP == NULL)
        {
          wP->sub = -1;
          ok      = false;
          continue;
        }
        subV   = vP;
        subMax = max;
      }
      memset(&subV[subN], 0, sizeof(Sub));
      subV[subN].key = strdup(key);
      subV[subN].no  = nextNo++;
      ++subN;
    }

    subV[ix].used = true;
    wP->sub       = subV[ix].no;

    if (subInTenants(&alloc, &subV[ix], wP) == false)
      ok = false;
  }

  //
  // The ones no watch has any more - in every tenant they were made in
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
    for (int tx = 0; (tx < subV[ix].inN) && (tx < tenants); tx++)
    {
      if (subV[ix].inV[tx] == false)
        continue;
      tenantUse(tenantV[tx].name);
      subDelete(&alloc, id);
    }
    tenantUse(NULL);
    free(subV[ix].key);
    free(subV[ix].inV);
  }
  subN = kept;

  corAllocBufferReset(&alloc, false);
  return ok;
}



// -----------------------------------------------------------------------------
//
// subsFree - the table of the watches' subscriptions freed (cep.so's close; the subscriptions themselves stay in
// the broker: the next start removes them, subsInit and the sweep of each tenant)
//
void subsFree(void)
{
  for (int ix = 0; ix < subN; ix++)
  {
    free(subV[ix].key);
    free(subV[ix].inV);
  }
  free(subV);
  subV   = NULL;
  subN   = 0;
  subMax = 0;
}
