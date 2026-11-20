//
// FILE            tenants.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The data tenants (doc/design.md, decision 2 as amended 2026-11-19): the rules live in the rules tenant
// (--rules-tenant), and a rule runs in every data tenant - the default tenant and each other one the broker has,
// new ones too - unless its 'tenant' names some. An event in a tenant is matched, counted and acted on in that
// tenant only: each tenant has its own patterns' state and ${...} values, its own subscriptions (subs.c).
//
// Where the list comes from:
//   --tenants a,b       fixed - against a broker that cannot list its tenants ("" the default tenant)
//   cep.so              coraine's tenantDataVisit, in process (tenantVisitHook) - the reserved tenants left out
//   the standalone      GET /admin/tenants (coraine's admin API) - not there: the default tenant alone, said once
//
// Refreshed every TENANTS_REFRESH_S and at each reload of the rules (engine.c). A tenant is never dropped while
// corCep runs: one gone from the broker's list keeps its state (a broker's tenant is not deleted in NGSI-LD).
//
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, realloc, free
#include <string.h>                              // strcmp, strdup, strchr

#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset

#include "corCep.h"
#include "cepTransport.h"



CepTenant* tenantV = NULL;
int        tenants = 0;
static int tenantMax = 0;

void (*tenantVisitHook)(void (*fn)(const char* name, void* ctx), void* ctx) = NULL;

__thread const char* cepTenantNow = NULL;



// -----------------------------------------------------------------------------
//
// tenantUse -
//
const char* tenantUse(const char* tenant)
{
  const char* before = cepTenantNow;

  cepTenantNow = tenant;
  return before;
}



// -----------------------------------------------------------------------------
//
// tenantOf -
//
CepTenant* tenantOf(const char* name)
{
  if (name == NULL)
    name = "";

  for (int ix = 0; ix < tenants; ix++)
  {
    if (strcmp(tenantV[ix].name, name) == 0)
      return &tenantV[ix];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// tenantAdd - a data tenant seen for the first time: its patterns (and what --state had for it), its ${...}
// values; the rules of the generation running adopted. NULL: out of memory, or the rules tenant
//
static CepTenant* tenantAdd(const char* name, CepRules* rulesP, long long now)
{
  if ((cep.single == false) && (cep.rulesTenant != NULL) && (strcmp(name, cep.rulesTenant) == 0))
    return NULL;                                 // the rules' own tenant is never a data tenant (--tenant T: it is the only one)

  if (tenants == tenantMax)
  {
    int        max = (tenantMax == 0) ? 8 : tenantMax * 2;
    CepTenant* vP  = realloc(tenantV, max * sizeof(CepTenant));

    if (vP == NULL)
      return NULL;
    tenantV   = vP;
    tenantMax = max;
  }

  CepTenant* tP = &tenantV[tenants];

  memset(tP, 0, sizeof(CepTenant));
  tP->name     = strdup(name);
  tP->vars     = varsNew();
  tP->seen     = true;

  if ((tP->name == NULL) || (tP->vars == NULL))
  {
    free(tP->name);
    if (tP->vars != NULL)
      varsFree(tP->vars);
    return NULL;
  }

  tP->patterns = patternsNew(0, tP->name);       // the name stays where it is: tenantV may move, the string does not
  if (tP->patterns == NULL)
  {
    free(tP->name);
    varsFree(tP->vars);
    return NULL;
  }

  stateLoadTenant(tP->patterns, name);           // what --state had for it (logged there)

  ++tenants;

  if (rulesP != NULL)
    patternsAdopt(tP->patterns, rulesP, now);

  logLine("tenant %s: the rules run in it", TENANT_TEXT(name));
  return tP;
}



// -----------------------------------------------------------------------------
//
// Visit - what tenantsRefresh gathers: the names seen, a growing list
//
typedef struct Visit
{
  char**  nameV;
  int     names;
  int     max;
} Visit;

static void visitAdd(const char* name, void* ctx)
{
  Visit* vP = (Visit*) ctx;

  if (vP->names == vP->max)
  {
    int    max = (vP->max == 0) ? 16 : vP->max * 2;
    char** nV  = realloc(vP->nameV, max * sizeof(char*));

    if (nV == NULL)
      return;
    vP->nameV = nV;
    vP->max   = max;
  }

  vP->nameV[vP->names] = strdup(name);
  if (vP->nameV[vP->names] != NULL)
    ++vP->names;
}



// -----------------------------------------------------------------------------
//
// adminTenants - GET /admin/tenants (coraine's admin API: an array of the names, the default tenant not in it, the
// reserved ones neither); false: no such API - the default tenant alone
//
static bool adminTenants(Visit* vP)
{
  static bool told = false;
  CorAlloc    alloc;
  CepResponse resp;

  corAllocBufferInit(&alloc, NULL, 0, 16 * 1024, NULL, "corCep tenants");

  const char* before = tenantUse(NULL);
  bool        got    = brokerAdminGet(&alloc, "/admin/tenants", &resp);
  tenantUse(before);

  if ((got == false) || (resp.status != 200) || (resp.body == NULL) || (resp.body->type != CorArray))
  {
    if (told == false)
      logLine("the broker's tenants not known (GET /admin/tenants: %s) - the default tenant only; --tenants names them",
              (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    told = true;
    corAllocBufferReset(&alloc, false);
    return false;
  }

  told = false;
  visitAdd("", vP);
  for (CorNode* nP = resp.body->value.head; nP != NULL; nP = nP->next)
  {
    if (nP->type == CorString)
      visitAdd(nP->value.s, vP);
  }

  corAllocBufferReset(&alloc, false);
  return true;
}



// -----------------------------------------------------------------------------
//
// tenantsRefresh - the data tenants as the broker has them now (or as --tenants says); the new ones made. Returns
// how many are new: their subscriptions are still to be made (engine.c applies the watches again)
//
int tenantsRefresh(CepRules* rulesP, long long now)
{
  Visit visit = { NULL, 0, 0 };

  if (cep.tenants != NULL)                       // --tenants (or --tenant): fixed
  {
    char* list = strdup(cep.tenants);
    char* save = NULL;

    if ((list != NULL) && ((list[0] == ',') || (list[0] == 0)))
      visitAdd("", &visit);                      // ",a" - a leading empty name: the default tenant; "": it alone
    for (char* name = (list != NULL) ? strtok_r(list, ",", &save) : NULL; name != NULL; name = strtok_r(NULL, ",", &save))
      visitAdd(name, &visit);
    free(list);
  }
  else if (tenantVisitHook != NULL)              // in coraine
    tenantVisitHook(visitAdd, &visit);
  else if (adminTenants(&visit) == false)
    visitAdd("", &visit);

  for (int ix = 0; ix < tenants; ix++)
    tenantV[ix].seen = false;

  int added = 0;

  for (int ix = 0; ix < visit.names; ix++)
  {
    CepTenant* tP = tenantOf(visit.nameV[ix]);

    if (tP == NULL)
    {
      tP = tenantAdd(visit.nameV[ix], rulesP, now);
      if (tP != NULL)
        ++added;
    }
    if (tP != NULL)
      tP->seen = true;
    free(visit.nameV[ix]);
  }
  free(visit.nameV);

  return added;
}



// -----------------------------------------------------------------------------
//
// tenantsFree - every tenant's state freed (a plugin's close, once the engine has stopped)
//
void tenantsFree(void)
{
  for (int ix = 0; ix < tenants; ix++)
  {
    free(tenantV[ix].name);
    patternsFree(tenantV[ix].patterns);
    varsFree(tenantV[ix].vars);
  }

  free(tenantV);
  tenantV   = NULL;
  tenants   = 0;
  tenantMax = 0;
}
