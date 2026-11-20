//
// FILE            dryrun.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /dry-run - rules replayed against made-up events, nothing written (README "Dry run", doc/design.md § 9).
// For the rule editors: what would fire, when, on which entities - and what an 'if' stopped.
//
// A replay is PURE: the broker is not asked. The rules are parsed as the running ones are (rulesParse), the
// patterns run on a state and a clock of their own (patternsNew with a virtual wall clock), the events are fed in
// time order and the deadlines between them run on that clock. 'where' (the subscription's q), 'if' and a group
// target's q are evaluated by corNgsild's matcher - the broker's - against the entities of the request, each event
// changing its entity's attribute first. Actions are reported, not applied: a light switched by a rule does not
// change the state the next rule sees, and steps are not stepped.
//
// The caller's own token (Authorization: Bearer) - a JWT checked against --jwks, with the --editor-role in the
// --roles-claim. /dry-run is off (404) without --jwks.
//
// Runs on the listener's thread: bounded - 100 rules, 10000 events, 1000 fires, 31 days.
//
#if CEP_WITH_DRY_RUN

#define _GNU_SOURCE                              // strptime, timegm

#include <pthread.h>                             // pthread_create, pthread_cond_*
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // qsort
#include <string.h>                              // strcmp, strlen, memcpy
#include <strings.h>                             // strncasecmp
#include <time.h>                                // time, timegm, gmtime_r

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corHttp/CorHttp.h"                     // CorHttpConn, corHttpResponse*
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corJson/corJsonRender.h"               // corJsonRender
#include "corJson/corJsonRenderSize.h"           // corJsonRenderSize
#include "corNgsild/ldEntityMatch.h"             // ldEntityMatchQ
#include "corNgsild/ldQParse.h"                  // ldQParse
#include "corRest/corRestFetch.h"                // corRestFetch
#include "corRest/corRestJwk.h"                  // corRestJwkLoad, corRestJwkCount
#include "corRest/corRestJwt.h"                  // corRestJwtVerify, corRestJwtCheckString
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, corTreeChildAdd, ...
#include "corTree/corTreeChildCount.h"           // corTreeChildCount
#include "corTree/corTreeChildReplace.h"         // corTreeChildReplace
#include "corTree/corTreeClone.h"                // corTreeClone
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corCep.h"



#define DR_RULES_MAX    100
#define DR_EVENTS_MAX   10000
#define DR_FIRES_MAX    1000
#define DR_FOR_MAX_MS   (31LL * 86400 * 1000)
#define DR_FOR_DEFAULT  (3600LL * 1000)          // after the last event: an hour, for the deadlines (absent, windows)



// -----------------------------------------------------------------------------
//
// The token check's configuration - set once by dryRunInit
//
static const char*      jwks        = NULL;      // NULL: /dry-run is off
static const char*      issuer      = NULL;
static const char*      audience    = NULL;
static const char*      rolesClaim  = NULL;
static const char*      editorRole  = NULL;



// -----------------------------------------------------------------------------
//
// dryRunInit - /dry-run on, its keys fetched: false (why says why) if they cannot be - corCep does not start
//
bool dryRunInit(const char* jwksArg, const char* issuerArg, const char* audienceArg, const char* rolesClaimArg,
                const char* editorRoleArg, char* why, int whyLen)
{
  jwks       = jwksArg;
  issuer     = ((issuerArg   != NULL) && (issuerArg[0]   != 0)) ? issuerArg   : NULL;
  audience   = ((audienceArg != NULL) && (audienceArg[0] != 0)) ? audienceArg : NULL;
  rolesClaim = rolesClaimArg;
  editorRole = editorRoleArg;

  if ((rolesClaim == NULL) || (rolesClaim[0] == 0) || (editorRole == NULL) || (editorRole[0] == 0))
  {
    snprintf(why, whyLen, "--roles-claim and --editor-role: neither may be empty");
    return false;
  }

  logLine("/dry-run on: %d key%s from %s, role '%s' in '%s'", corRestJwkCount(), (corRestJwkCount() == 1) ? "" : "s",
          jwks, editorRole, rolesClaim);
  return true;
}



// -----------------------------------------------------------------------------
//
// problem - an error answer, application/problem+json
//
static void problem(CorHttpConn* connP, int status, const char* title, const char* detail)
{
  CorAlloc* aP    = &connP->alloc;
  CorNode*  objP  = corTreeObject(aP, NULL);
  CorJson   json;

  corTreeChildAdd(objP, corTreeString(aP, "title", title));
  corTreeChildAdd(objP, corTreeInteger(aP, "status", status));
  if (detail != NULL)
    corTreeChildAdd(objP, corTreeString(aP, "detail", detail));

  corJsonCreate(&json, aP);
  char* body = corAlloc(aP, corJsonRenderSize(&json, objP) + 1);
  corJsonRender(&json, objP, body);

  corHttpResponseStatus(connP, status);
  corHttpResponseHeader(connP, "Content-Type", "application/problem+json");
  if (status == 401)
    corHttpResponseHeader(connP, "WWW-Authenticate", "Bearer");
  corHttpResponseBody(connP, body, strlen(body));
}



// -----------------------------------------------------------------------------
//
// claimPath - a claim by a dot-separated path: realm_access.roles
//
static CorNode* claimPath(CorAlloc* aP, CorNode* claimsP, const char* path)
{
  char*    copy = corAllocStrdup(aP, path);
  CorNode* nP   = claimsP;

  for (char* part = copy; (part != NULL) && (nP != NULL); )
  {
    char* dot = strchr(part, '.');
    if (dot != NULL)
      *dot = 0;
    nP   = (nP->type == CorObject) ? corTreeLookup(nP, part) : NULL;
    part = (dot != NULL) ? dot + 1 : NULL;
  }

  return nP;
}



// -----------------------------------------------------------------------------
//
// hasRole - the roles claim holds the role: an array of strings, or a string of them, space-separated
//
static bool hasRole(CorNode* rolesP, const char* role)
{
  if (rolesP == NULL)
    return false;

  if (rolesP->type == CorArray)
  {
    for (CorNode* rP = rolesP->value.head; rP != NULL; rP = rP->next)
    {
      if ((rP->type == CorString) && (strcmp(rP->value.s, role) == 0))
        return true;
    }
    return false;
  }

  if (rolesP->type != CorString)
    return false;

  int len = strlen(role);
  for (const char* s = rolesP->value.s; *s != 0; )
  {
    while (*s == ' ')
      ++s;
    const char* end = s;
    while ((*end != 0) && (*end != ' '))
      ++end;
    if ((end - s == len) && (strncmp(s, role, len) == 0))
      return true;
    s = end;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// tokenCheck - the caller's token: 0 - it may; 401 / 403 - it may not, detail says why
//
static int tokenCheck(CorHttpConn* connP, char* detail, int detailLen)
{
  const char* header = corHttpHeader(connP, "Authorization");

  if ((header == NULL) || (strncasecmp(header, "Bearer ", 7) != 0))
  {
    snprintf(detail, detailLen, "no bearer token - Authorization: Bearer <token>");
    return 401;
  }

  const char* token = header + 7;
  while (*token == ' ')
    ++token;

  CorRestJwtExpect expect  = { issuer, audience, (int64_t) time(NULL), 60 };
  CorNode*         claimsP = NULL;
  CorRestJwtCheck  check   = corRestJwtVerify(token, &expect, &connP->alloc, &claimsP);

  if (check != CorRestJwtOk)
  {
    if (check == CorRestJwtUnknownKey)
      keysRefreshNow();
    snprintf(detail, detailLen, "the token: %s", corRestJwtCheckString(check));
    return 401;
  }

  if (corTreeLookup(claimsP, "rule") != NULL)    // a rule's signature (author.c) - not a token for /dry-run
  {
    snprintf(detail, detailLen, "the token: a rule's signature, not an access token");
    return 401;
  }

  if (hasRole(claimPath(&connP->alloc, claimsP, rolesClaim), editorRole) == false)
  {
    snprintf(detail, detailLen, "the token has no role '%s' (in '%s')", editorRole, rolesClaim);
    return 403;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// DrEntity - an entity of the state the replay sees: the request's 'entities', changed by the events
//
typedef struct DrEntity
{
  struct DrEntity*  next;
  const char*       id;
  const char*       type;
  CorNode*          nodeP;                       // normalized: each attribute { "type": ..., "value" | "object": ... }
} DrEntity;



// -----------------------------------------------------------------------------
//
// DrEvent - an event of the request
//
typedef struct DrEvent
{
  long long    at;                               // ms from the start
  int          ix;                               // its place in the request - the sort keeps it for equal times
  const char*  entity;
  const char*  type;
  const char*  attribute;
  CorNode*     valueP;
} DrEvent;



// -----------------------------------------------------------------------------
//
// Dr - a replay
//
typedef struct Dr
{
  CorAlloc*     aP;
  CepRules*     rulesP;
  CepPatterns*  psP;
  CepVars*      varsP;                           // the events' latest values, for ${...}
  long long     startMs;                         // the wall clock at 'now' 0
  long long     now;                             // ms from the start
  DrEntity*     entities;                        // in the request's order, then the ones the events made
  DrEntity*     entitiesTail;
  CorNode*      firesP;
  int           fires;
  bool          truncated;
} Dr;



// -----------------------------------------------------------------------------
//
// isAttrObject - an attribute in the normalized form: an object with an NGSI-LD attribute type
//
static bool isAttrObject(CorNode* nP)
{
  static const char* typeV[] = { "Property", "Relationship", "GeoProperty", "LanguageProperty", "VocabProperty",
                                 "JsonProperty", "ListProperty", "ListRelationship", NULL };

  if (nP->type != CorObject)
    return false;

  CorNode* tP = corTreeLookup(nP, "type");
  if ((tP == NULL) || (tP->type != CorString))
    return false;

  for (int ix = 0; typeV[ix] != NULL; ix++)
  {
    if (strcmp(tP->value.s, typeV[ix]) == 0)
      return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// propertyOf - a value as a Property: { "type": "Property", "value": <value> }
//
static CorNode* propertyOf(CorAlloc* aP, const char* name, CorNode* valueP)
{
  CorNode* attrP = corTreeObject(aP, name);
  CorNode* vP    = corTreeClone(aP, valueP);

  vP->name = (char*) "value";
  corTreeChildAdd(attrP, corTreeString(aP, "type", "Property"));
  corTreeChildAdd(attrP, vP);

  return attrP;
}



// -----------------------------------------------------------------------------
//
// entityLink - an entity at the end of the state
//
static void entityLink(Dr* drP, DrEntity* dP)
{
  dP->next = NULL;
  if (drP->entitiesTail == NULL)
    drP->entities = dP;
  else
    drP->entitiesTail->next = dP;
  drP->entitiesTail = dP;
}



// -----------------------------------------------------------------------------
//
// entityAdd - an entity of the request into the state: normalized or keyValues (a plain value is a Property's)
//
static const char* entityAdd(Dr* drP, CorNode* eP)
{
  CorNode* idP   = corTreeLookup(eP, "id");
  CorNode* typeP = corTreeLookup(eP, "type");

  if ((eP->type != CorObject) || (idP == NULL) || (idP->type != CorString) || (typeP == NULL) || (typeP->type != CorString))
    return "an entity: an object with a string 'id' and 'type'";

  DrEntity* dP = (DrEntity*) corAlloc(drP->aP, sizeof(DrEntity));
  dP->id    = idP->value.s;
  dP->type  = typeP->value.s;
  dP->nodeP = corTreeObject(drP->aP, NULL);

  for (CorNode* aP = eP->value.head; aP != NULL; aP = aP->next)
  {
    if ((strcmp(aP->name, "@context") == 0))
      continue;
    if ((strcmp(aP->name, "id") == 0) || (strcmp(aP->name, "type") == 0) || isAttrObject(aP))
      corTreeChildAdd(dP->nodeP, corTreeClone(drP->aP, aP));
    else
      corTreeChildAdd(dP->nodeP, propertyOf(drP->aP, aP->name, aP));
  }

  entityLink(drP, dP);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// entityOf - an entity of the state; made (with its id and type, no attributes) the first time an event names it
//
static DrEntity* entityOf(Dr* drP, const char* id, const char* type)
{
  for (DrEntity* dP = drP->entities; dP != NULL; dP = dP->next)
  {
    if (strcmp(dP->id, id) == 0)
      return dP;
  }

  DrEntity* dP = (DrEntity*) corAlloc(drP->aP, sizeof(DrEntity));
  dP->id    = id;
  dP->type  = (type != NULL) ? type : "";
  dP->nodeP = corTreeObject(drP->aP, NULL);
  corTreeChildAdd(dP->nodeP, corTreeString(drP->aP, "id", id));
  corTreeChildAdd(dP->nodeP, corTreeString(drP->aP, "type", dP->type));

  entityLink(drP, dP);
  return dP;
}



// -----------------------------------------------------------------------------
//
// qMatches - an entity of the state matches a q (NULL: every entity does); a q that does not parse matches none
//
static bool qMatches(Dr* drP, DrEntity* dP, const char* q)
{
  if (q == NULL)
    return true;

  LdQNode* qP = ldQParse(q, drP->aP);
  return (qP != NULL) && ldEntityMatchQ(dP->nodeP, qP);
}



// -----------------------------------------------------------------------------
//
// selects - an entity of the state is one an 'if' leaf or a group target names: the entity, or one of the type
//
static bool selects(DrEntity* dP, const char* entity, const char* type)
{
  if (entity != NULL)
    return strcmp(dP->id, entity) == 0;
  return (type != NULL) && (strcmp(dP->type, type) == 0);
}



// -----------------------------------------------------------------------------
//
// condFailingHere - condFailing (engine.c) against the state: NULL when the condition holds, else the one that does not
//
static CepCondition* condFailingHere(Dr* drP, CepCondition* cP)
{
  if (cP->kind == CondLeaf)
  {
    for (DrEntity* dP = drP->entities; dP != NULL; dP = dP->next)
    {
      if (selects(dP, cP->entity, cP->type) && qMatches(drP, dP, cP->q))
        return NULL;
    }
    return cP;
  }

  for (int ix = 0; ix < cP->kids; ix++)
  {
    CepCondition* failP = condFailingHere(drP, &cP->kidV[ix]);

    if ((cP->kind == CondAll) && (failP != NULL))
      return failP;
    if ((cP->kind == CondAny) && (failP == NULL))
      return NULL;
  }

  return (cP->kind == CondAll) ? NULL : cP;
}



// -----------------------------------------------------------------------------
//
// isoOf - ms since the epoch as ISO 8601, UTC, with ms
//
static char* isoOf(CorAlloc* aP, long long ms)
{
  time_t    t = (time_t) (ms / 1000);
  struct tm tm;
  char      s[24];
  char*     buf = corAlloc(aP, 32);

  gmtime_r(&t, &tm);
  strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%S", &tm);
  snprintf(buf, 32, "%s.%03lldZ", s, ms % 1000);

  return buf;
}



// -----------------------------------------------------------------------------
//
// actionNode - an action as it would run: what, and on which entities of the state
//
static CorNode* actionNode(Dr* drP, CepAction* aP)
{
  CorAlloc* allocP = drP->aP;
  CorNode*  objP   = corTreeObject(allocP, NULL);
  CorNode*  targP  = corTreeArray(allocP, "entities");

  switch (aP->kind)
  {
  case CepActionSet:
    {
      CorNode* setP = corTreeClone(allocP, aP->set);
      setP->name = (char*) "set";
      corTreeChildAdd(objP, setP);
    }
    break;

  case CepActionToggle:
    corTreeChildAdd(objP, corTreeString(allocP, "toggle", aP->attribute));
    break;

  case CepActionStep:
    {
      CorNode* stepP = corTreeObject(allocP, "step");
      corTreeChildAdd(stepP, corTreeString(allocP, "attribute", aP->attribute));
      corTreeChildAdd(stepP, corTreeFloat(allocP, "by", aP->by));
      corTreeChildAdd(objP, stepP);
    }
    break;

  case CepActionCreate:                          // the entity as written - an id corCep makes is not shown, none is made
    {
      CorNode* createP = corTreeClone(allocP, aP->set);
      CorNode* idP     = corTreeLookup(aP->set, "id");

      createP->name = (char*) "create";
      corTreeChildAdd(objP, createP);
      if (idP != NULL)
        corTreeChildAdd(targP, corTreeString(allocP, NULL, idP->value.s));
      corTreeChildAdd(objP, targP);
      return objP;
    }

  case CepActionDelete:
    corTreeChildAdd(objP, corTreeBoolean(allocP, "delete", true));
    break;
  }

  if (aP->entity != NULL)
    corTreeChildAdd(targP, corTreeString(allocP, NULL, aP->entity));
  else
  {
    corTreeChildAdd(objP, corTreeString(allocP, "type", aP->type));
    if (aP->q != NULL)
      corTreeChildAdd(objP, corTreeString(allocP, "q", aP->q));

    for (DrEntity* dP = drP->entities; dP != NULL; dP = dP->next)
    {
      if (selects(dP, NULL, aP->type) && qMatches(drP, dP, aP->q))
        corTreeChildAdd(targP, corTreeString(allocP, NULL, dP->id));
    }
  }

  corTreeChildAdd(objP, targP);
  return objP;
}



// -----------------------------------------------------------------------------
//
// fireCb - a rule done: its 'if' looked at in the state, its actions reported
//
static void fireCb(CepRule* ruleP, const char* entity, void* ctx)
{
  Dr*       drP    = (Dr*) ctx;
  CorAlloc* allocP = drP->aP;

  if (drP->fires >= DR_FIRES_MAX)
  {
    drP->truncated = true;
    return;
  }
  ++drP->fires;

  CorNode* fP = corTreeObject(allocP, NULL);
  corTreeChildAdd(fP, corTreeInteger(allocP, "at", drP->now));
  corTreeChildAdd(fP, corTreeString(allocP, "time", isoOf(allocP, drP->startMs + drP->now)));
  corTreeChildAdd(fP, corTreeString(allocP, "rule", ruleP->id));
  if (entity != NULL)
    corTreeChildAdd(fP, corTreeString(allocP, "entity", entity));
  else
    corTreeChildAdd(fP, corTreeNull(allocP, "entity"));

  int first = 0;
  int end   = ruleP->thens;

  for (int ix = 0; ix < ruleP->conds; ix++)
  {
    CepCondition* failP = condFailingHere(drP, &ruleP->condV[ix]);

    if (failP != NULL)
    {
      char text[512];
      char why[600];

      condText(failP, text, sizeof(text));
      snprintf(why, sizeof(why), "'if' %s does not hold", text);

      if (ruleP->thens == ruleP->actions)          // no else: not fired
      {
        corTreeChildAdd(fP, corTreeBoolean(allocP, "fired", false));
        corTreeChildAdd(fP, corTreeString(allocP, "why", corAllocStrdup(allocP, why)));
        corTreeChildAdd(drP->firesP, fP);
        return;
      }

      corTreeChildAdd(fP, corTreeBoolean(allocP, "else", true));
      corTreeChildAdd(fP, corTreeString(allocP, "why", corAllocStrdup(allocP, why)));
      first = ruleP->thens;
      end   = ruleP->actions;
      break;
    }
  }

  CorNode* actionsP = corTreeArray(allocP, "actions");
  for (int ix = first; ix < end; ix++)
  {
    CepAction* aP = &ruleP->actionV[ix];
    CepAction  resolved;
    char       why[600];

    if (aP->vars && (varsAction(allocP, drP->varsP, drP->psP, ruleP, entity, aP, &resolved, why, sizeof(why)) == false))
    {
      CorNode* xP = corTreeObject(allocP, NULL);       // as corCep would: the action fails, the others run
      corTreeChildAdd(xP, corTreeString(allocP, "failed", corAllocStrdup(allocP, why)));
      corTreeChildAdd(actionsP, xP);
      continue;
    }
    corTreeChildAdd(actionsP, actionNode(drP, aP->vars ? &resolved : aP));
  }

  corTreeChildAdd(fP, corTreeBoolean(allocP, "fired", true));
  corTreeChildAdd(fP, actionsP);
  corTreeChildAdd(drP->firesP, fP);
}



// -----------------------------------------------------------------------------
//
// timersUntil - the deadlines up to <until> (ms from the start), each run at its own time
//
static void timersUntil(Dr* drP, long long until)
{
  while (drP->truncated == false)
  {
    long long next = patternsTimers(drP->psP, drP->rulesP, drP->now, fireCb, drP);

    if ((next == 0) || (next > until))
      break;
    drP->now = (next > drP->now) ? next : drP->now + 1;
  }

  if (until > drP->now)
    drP->now = until;
}



// -----------------------------------------------------------------------------
//
// eventFeed - an event: its entity's attribute changed, then each watch it matches (and whose 'where' holds) fed
//
static void eventFeed(Dr* drP, DrEvent* evP)
{
  DrEntity* dP = entityOf(drP, evP->entity, evP->type);
  if ((evP->type != NULL) && (dP->type[0] == 0))
    dP->type = evP->type;

  CorNode* oldP = corTreeLookup(dP->nodeP, evP->attribute);
  CorNode* newP = isAttrObject(evP->valueP) ? corTreeClone(drP->aP, evP->valueP) : propertyOf(drP->aP, evP->attribute, evP->valueP);

  newP->name = (char*) evP->attribute;
  if (oldP != NULL)
    corTreeChildReplace(dP->nodeP, oldP, newP);
  else
    corTreeChildAdd(dP->nodeP, newP);

  CorNode* vP     = isAttrObject(evP->valueP) ? corTreeLookup(evP->valueP, "value") : evP->valueP;
  double   v      = 0;
  bool     number = (vP != NULL) && ((vP->type == CorInt) || (vP->type == CorFloat));

  if (number)
    v = (vP->type == CorInt) ? (double) vP->value.i : vP->value.f;

  for (int n = 0; n < drP->rulesP->watches; n++)
  {
    CepWatch* wP = &drP->rulesP->watchV[n];

    if (strcmp(wP->attribute, evP->attribute) != 0)
      continue;
    if ((wP->entity != NULL) ? (strcmp(wP->entity, dP->id) != 0) : ((wP->type == NULL) || (strcmp(wP->type, dP->type) != 0)))
      continue;
    if (qMatches(drP, dP, wP->q) == false)
      continue;

    for (int rx = 0; rx < drP->rulesP->rules; rx++)        // the latest values for ${...} - as corCep keeps them
    {
      CepRule* ruleP = &drP->rulesP->ruleV[rx];

      for (int ex = 0; ruleP->vars && (ex < ruleP->events); ex++)
      {
        if ((ruleP->eventV[ex].watch == n) && (vP != NULL))
          varsNote(drP->varsP, ruleP->id, ruleP->partitioned ? dP->id : "", ruleP->eventV[ex].name, dP->id, jsonText(drP->aP, vP));
      }
    }

    patternsEvent(drP->psP, drP->rulesP, n, dP->id, number ? &v : NULL, drP->now, fireCb, drP);
  }
}



// -----------------------------------------------------------------------------
//
// atParse - an event's time: ms (a number), or an ISO 8601 duration from the start ("PT0S" too)
//
static bool atParse(CorNode* atP, long long* msP)
{
  if (atP == NULL)
    return false;

  if ((atP->type == CorInt) && (atP->value.i >= 0))
  {
    *msP = atP->value.i;
    return true;
  }

  if (atP->type != CorString)
    return false;

  if ((strcmp(atP->value.s, "PT0S") == 0) || (strcmp(atP->value.s, "P0D") == 0))
  {
    *msP = 0;
    return true;
  }

  return durationParse(atP->value.s, msP);
}



// -----------------------------------------------------------------------------
//
// isoParse - 2026-11-17T18:00:00Z, 2026-11-17T18:00:00.250+01:00 -> ms since the epoch; false: not one
//
static bool isoParse(const char* s, long long* msP)
{
  struct tm   tm = { 0 };
  const char* rest = strptime(s, "%Y-%m-%dT%H:%M:%S", &tm);

  if (rest == NULL)
    return false;

  long long ms = 0;
  if (*rest == '.')
  {
    int digits = 0;
    for (++rest; (*rest >= '0') && (*rest <= '9'); ++rest, ++digits)
    {
      if (digits < 3)
        ms = ms * 10 + (*rest - '0');
    }
    for (; digits < 3; ++digits)
      ms *= 10;
  }

  long long offsetS = 0;
  if ((*rest == '+') || (*rest == '-'))
  {
    int h, m;
    if (sscanf(rest + 1, "%2d:%2d", &h, &m) != 2)
      return false;
    offsetS = (h * 3600 + m * 60) * ((*rest == '+') ? 1 : -1);
    rest += 6;
  }
  else if (*rest == 'Z')
    ++rest;
  else
    return false;

  if (*rest != 0)
    return false;

  *msP = ((long long) timegm(&tm) - offsetS) * 1000 + ms;
  return true;
}



// -----------------------------------------------------------------------------
//
// eventCompare - by time, then by place in the request
//
static int eventCompare(const void* a, const void* b)
{
  const DrEvent* aP = (const DrEvent*) a;
  const DrEvent* bP = (const DrEvent*) b;

  if (aP->at != bP->at)
    return (aP->at < bP->at) ? -1 : 1;
  return aP->ix - bP->ix;
}



// -----------------------------------------------------------------------------
//
// LeftOut - the rules the parser left out, for the answer
//
typedef struct LeftOut
{
  CorAlloc*  aP;
  CorNode*   arrayP;
} LeftOut;

static void leftOutCb(const char* id, const char* why, void* ctx)
{
  LeftOut* lP = (LeftOut*) ctx;
  CorNode* oP = corTreeObject(lP->aP, NULL);

  if (id != NULL)
    corTreeChildAdd(oP, corTreeString(lP->aP, "rule", corAllocStrdup(lP->aP, id)));
  corTreeChildAdd(oP, corTreeString(lP->aP, "why", corAllocStrdup(lP->aP, why)));
  corTreeChildAdd(lP->arrayP, oP);
}



// -----------------------------------------------------------------------------
//
// replay - the request's body replayed; NULL and 'why' if it is not one
//
static CorNode* replay(CorAlloc* aP, CorNode* bodyP, char* why, int whyLen)
{
  if ((bodyP == NULL) || (bodyP->type != CorObject))
  {
    snprintf(why, whyLen, "the body: a JSON object - { \"rules\": [...], \"events\": [...] }");
    return NULL;
  }

  CorNode* rulesP    = corTreeLookup(bodyP, "rules");
  CorNode* eventsP   = corTreeLookup(bodyP, "events");
  CorNode* entitiesP = corTreeLookup(bodyP, "entities");
  CorNode* startP    = corTreeLookup(bodyP, "start");
  CorNode* forP      = corTreeLookup(bodyP, "for");

  if ((rulesP == NULL) || (rulesP->type != CorArray) || (rulesP->value.head == NULL))
  {
    snprintf(why, whyLen, "'rules': a list of Rule entities (keyValues), one at least");
    return NULL;
  }
  if (corTreeChildCount(rulesP) > DR_RULES_MAX)
  {
    snprintf(why, whyLen, "'rules': %d at most", DR_RULES_MAX);
    return NULL;
  }
  if ((eventsP != NULL) && (eventsP->type != CorArray))
  {
    snprintf(why, whyLen, "'events': a list");
    return NULL;
  }
  if ((entitiesP != NULL) && (entitiesP->type != CorArray))
  {
    snprintf(why, whyLen, "'entities': a list");
    return NULL;
  }

  Dr dr = { 0 };
  dr.aP = aP;

  //
  // The start - the wall clock at 0, for the schedules
  //
  if (startP == NULL)
  {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    dr.startMs = (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  }
  else if ((startP->type != CorString) || (isoParse(startP->value.s, &dr.startMs) == false) || (dr.startMs <= 0))
  {
    snprintf(why, whyLen, "'start': an ISO 8601 date-time after 1970 (2026-11-17T18:00:00Z)");
    return NULL;
  }

  //
  // The state
  //
  for (CorNode* eP = (entitiesP != NULL) ? entitiesP->value.head : NULL; eP != NULL; eP = eP->next)
  {
    const char* bad = entityAdd(&dr, eP);
    if (bad != NULL)
    {
      snprintf(why, whyLen, "'entities': %s", bad);
      return NULL;
    }
  }

  //
  // The events, in time order
  //
  int      events = (eventsP != NULL) ? corTreeChildCount(eventsP) : 0;
  DrEvent* evV    = (DrEvent*) corAlloc(aP, (events + 1) * sizeof(DrEvent));

  if (events > DR_EVENTS_MAX)
  {
    snprintf(why, whyLen, "'events': %d at most", DR_EVENTS_MAX);
    return NULL;
  }

  int ix = 0;
  for (CorNode* eP = (eventsP != NULL) ? eventsP->value.head : NULL; eP != NULL; eP = eP->next, ix++)
  {
    DrEvent* evP   = &evV[ix];
    CorNode* idP   = corTreeLookup(eP, "entity");
    CorNode* typeP = corTreeLookup(eP, "type");
    CorNode* attrP = corTreeLookup(eP, "attribute");

    evP->ix     = ix;
    evP->valueP = corTreeLookup(eP, "value");

    if ((eP->type != CorObject) || (idP == NULL) || (idP->type != CorString) || (attrP == NULL) || (attrP->type != CorString) ||
        (evP->valueP == NULL) || ((typeP != NULL) && (typeP->type != CorString)))
    {
      snprintf(why, whyLen, "'events' [%d]: { \"at\", \"entity\", \"type\" (optional), \"attribute\", \"value\" }", ix);
      return NULL;
    }
    if (atParse(corTreeLookup(eP, "at"), &evP->at) == false)
    {
      snprintf(why, whyLen, "'events' [%d]: 'at' - ms from the start, or an ISO 8601 duration (PT0.5S)", ix);
      return NULL;
    }

    evP->entity    = idP->value.s;
    evP->type      = (typeP != NULL) ? typeP->value.s : NULL;
    evP->attribute = attrP->value.s;
  }
  qsort(evV, events, sizeof(DrEvent), eventCompare);

  //
  // The end
  //
  long long end = ((events > 0) ? evV[events - 1].at : 0) + DR_FOR_DEFAULT;
  if (forP != NULL)
  {
    long long forMs;
    if ((forP->type != CorString) || (durationParse(forP->value.s, &forMs) == false))
    {
      snprintf(why, whyLen, "'for': an ISO 8601 duration (PT1H)");
      return NULL;
    }
    end = forMs;
  }
  if (end > DR_FOR_MAX_MS)
  {
    snprintf(why, whyLen, "the replay lasts 31 days at most ('for', or the last event + 1 hour)");
    return NULL;
  }

  //
  // The rules
  //
  LeftOut leftOut = { aP, corTreeArray(aP, "leftOut") };

  dr.rulesP = rulesParse(rulesP, (time_t) (dr.startMs / 1000), leftOutCb, &leftOut);
  dr.psP    = patternsNew(dr.startMs, NULL);
  dr.varsP  = varsNew();
  dr.firesP = corTreeArray(aP, "fires");

  if ((dr.rulesP == NULL) || (dr.psP == NULL) || (dr.varsP == NULL))
  {
    rulesFree(dr.rulesP);
    patternsFree(dr.psP);
    varsFree(dr.varsP);
    snprintf(why, whyLen, "out of memory");
    return NULL;
  }

  //
  // The replay
  //
  patternsAdopt(dr.psP, dr.rulesP, 0);

  for (ix = 0; (ix < events) && (evV[ix].at <= end) && (dr.truncated == false); ix++)
  {
    timersUntil(&dr, evV[ix].at);
    eventFeed(&dr, &evV[ix]);
  }
  timersUntil(&dr, end);

  rulesFree(dr.rulesP);
  patternsFree(dr.psP);
  varsFree(dr.varsP);

  CorNode* outP = corTreeObject(aP, NULL);
  corTreeChildAdd(outP, corTreeString(aP, "start", isoOf(aP, dr.startMs)));
  corTreeChildAdd(outP, corTreeInteger(aP, "end", end));
  corTreeChildAdd(outP, dr.firesP);
  corTreeChildAdd(outP, leftOut.arrayP);
  corTreeChildAdd(outP, corTreeBoolean(aP, "truncated", dr.truncated));

  return outP;
}



// -----------------------------------------------------------------------------
//
// dryRunRequest - POST /dry-run
//
void dryRunRequest(CorHttpConn* connP)
{
  if (jwks == NULL)
  {
    problem(connP, 404, "Not Found", "/dry-run is off - corCep runs without --jwks");
    return;
  }

  char detail[512];
  int  status = tokenCheck(connP, detail, sizeof(detail));

  if (status != 0)
  {
    problem(connP, status, (status == 401) ? "Unauthorized" : "Forbidden", detail);
    return;
  }

  CorAlloc* aP   = &connP->alloc;
  char*     text = corAlloc(aP, connP->body.len + 1);

  memcpy(text, connP->body.s, connP->body.len);
  text[connP->body.len] = 0;

  CorJson  json;
  corJsonCreate(&json, aP);
  CorNode* bodyP = (connP->body.len > 0) ? corJsonParse(&json, text) : NULL;
  CorNode* outP  = replay(aP, bodyP, detail, sizeof(detail));

  if (outP == NULL)
  {
    problem(connP, 400, "Bad Request", detail);
    return;
  }

  char* body = corAlloc(aP, corJsonRenderSize(&json, outP) + 1);
  corJsonRender(&json, outP, body);

  corHttpResponseStatus(connP, 200);
  corHttpResponseHeader(connP, "Content-Type", "application/json");
  corHttpResponseBody(connP, body, strlen(body));
}

#endif  // CEP_WITH_DRY_RUN
