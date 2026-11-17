//
// FILE            vars.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// ${...} - an action's values naming the match (doc/design.md § 3.2):
//
//   ${entity}            the entity the rule fired for (a schedule fires for none)
//   ${<event>.entity}    the entity of the latest <event> the rule saw - per entity, for a rule on a type
//   ${<event>.value}     that event's value - the notified attribute's, any JSON
//   ${<event>.<agg>}     a window rule: its aggregate now (avg, min, max, sum or count - the rule's own)
//
// In an action's 'entity', 'q', the values of 'set' and 'create' (and the id of a 'delete'). A string that is ONE
// ${...} and nothing else becomes the value itself, its type kept (a number stays a number); in a longer string the
// value is written into the text - a string without its quotes.
//
// The values come from the devices - anyone who can write an entity writes them - so they are checked before use:
// into an entity id, a value with no space, quote, <, >, {, }, |, \, ^, `; into a q, a string only between quotes
// and without a quote or a backslash in it, else a number or a boolean. One that does not pass: the action fails,
// lastError says why.
//
// The latest values are kept here (CepVars), by rule id, partition and event name - in memory: after a restart an
// event's values come with its next event (${entity} is the match's, as before).
//
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, free
#include <string.h>                              // strcmp, strdup, strncmp, strchr, strlen

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corTree/corTreeBuilder.h"              // corTreeString, corTreeChildAdd
#include "corTree/corTreeClone.h"                // corTreeClone
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corCep.h"



// -----------------------------------------------------------------------------
//
// CepLast - the latest event of a name, for a rule and a partition
//
typedef struct CepLast
{
  struct CepLast*  next;
  char*            ruleId;
  char*            key;                          // the partition - "" for a rule that is not partitioned
  char*            event;
  char*            entity;
  char*            value;                        // JSON text
} CepLast;

struct CepVars
{
  CepLast*  list;
};



static const char* aggName[] = { "avg", "min", "max", "sum", "count" };   // CepWindowAgg's order



// -----------------------------------------------------------------------------
//
// varsNew, varsFree -
//
CepVars* varsNew(void)
{
  return calloc(1, sizeof(CepVars));
}

static void lastFree(CepLast* lP)
{
  free(lP->ruleId);
  free(lP->key);
  free(lP->event);
  free(lP->entity);
  free(lP->value);
  free(lP);
}

void varsFree(CepVars* vP)
{
  if (vP == NULL)
    return;

  while (vP->list != NULL)
  {
    CepLast* lP = vP->list;
    vP->list = lP->next;
    lastFree(lP);
  }
  free(vP);
}



// -----------------------------------------------------------------------------
//
// lastOf - the record; NULL: none yet
//
static CepLast* lastOf(CepVars* vP, const char* ruleId, const char* key, const char* event)
{
  for (CepLast* lP = vP->list; lP != NULL; lP = lP->next)
  {
    if ((strcmp(lP->ruleId, ruleId) == 0) && (strcmp(lP->key, key) == 0) && (strcmp(lP->event, event) == 0))
      return lP;
  }
  return NULL;
}



// -----------------------------------------------------------------------------
//
// varsNote - an event came: the latest of <event> for <rule> in <key>
//
void varsNote(CepVars* vP, const char* ruleId, const char* key, const char* event, const char* entity, const char* valueJson)
{
  CepLast* lP = lastOf(vP, ruleId, key, event);

  if (lP == NULL)
  {
    lP = calloc(1, sizeof(CepLast));
    if (lP == NULL)
      return;
    lP->ruleId = strdup(ruleId);
    lP->key    = strdup(key);
    lP->event  = strdup(event);
    if ((lP->ruleId == NULL) || (lP->key == NULL) || (lP->event == NULL))
    {
      lastFree(lP);
      return;
    }
    lP->next = vP->list;
    vP->list = lP;
  }

  free(lP->entity);
  free(lP->value);
  lP->entity = strdup(entity);
  lP->value  = strdup(valueJson);
}



// -----------------------------------------------------------------------------
//
// varsKeep - the records of rules gone dropped (a reload)
//
void varsKeep(CepVars* vP, CepRules* rulesP)
{
  CepLast** prevPP = &vP->list;

  while (*prevPP != NULL)
  {
    CepLast* lP    = *prevPP;
    bool     found = false;

    for (int ix = 0; (rulesP != NULL) && (ix < rulesP->rules) && (found == false); ix++)
      found = (strcmp(rulesP->ruleV[ix].id, lP->ruleId) == 0);

    if (found)
      prevPP = &lP->next;
    else
    {
      *prevPP = lP->next;
      lastFree(lP);
    }
  }
}



// -----------------------------------------------------------------------------
//
// varNameCheck - one name, between ${ and }: true if the rule can give it a value
//
static bool varNameCheck(CepRule* ruleP, const char* name, int len, char* why, int whyLen)
{
  if ((len == 6) && (strncmp(name, "entity", 6) == 0))
    return true;

  const char* dot = memchr(name, '.', len);
  if (dot == NULL)
  {
    snprintf(why, whyLen, "'${%.*s}' - ${entity}, or ${<event>.entity|value|avg|min|max|sum|count}", len, name);
    return false;
  }

  int         evLen    = dot - name;
  const char* field    = dot + 1;
  int         fieldLen = len - evLen - 1;
  int         ev       = -1;

  for (int ix = 0; ix < ruleP->events; ix++)
  {
    if (((int) strlen(ruleP->eventV[ix].name) == evLen) && (strncmp(ruleP->eventV[ix].name, name, evLen) == 0))
      ev = ix;
  }
  if (ev == -1)
  {
    snprintf(why, whyLen, "'${%.*s}' - no event '%.*s' in the rule's pattern", len, name, evLen, name);
    return false;
  }

  if (((fieldLen == 6) && (strncmp(field, "entity", 6) == 0)) || ((fieldLen == 5) && (strncmp(field, "value", 5) == 0)))
    return true;

  for (int ax = 0; ax < (int) (sizeof(aggName) / sizeof(aggName[0])); ax++)
  {
    if (((int) strlen(aggName[ax]) == fieldLen) && (strncmp(field, aggName[ax], fieldLen) == 0))
    {
      if ((ruleP->kind == CepPatternWindow) && ((int) ruleP->aggregate == ax))
        return true;
      snprintf(why, whyLen, "'${%.*s}' - %s: a window rule's own aggregate only", len, name, aggName[ax]);
      return false;
    }
  }

  snprintf(why, whyLen, "'${%.*s}' - '%.*s': entity, value, or a window's aggregate", len, name, fieldLen, field);
  return false;
}



// -----------------------------------------------------------------------------
//
// varsCheck - the ${...} of a string: each one the rule can give; *usedP set when there is one. False: why
//
bool varsCheck(CepRule* ruleP, const char* s, bool* usedP, char* why, int whyLen)
{
  for (const char* p = strstr(s, "${"); p != NULL; p = strstr(p, "${"))
  {
    const char* end = strchr(p + 2, '}');
    if (end == NULL)
    {
      snprintf(why, whyLen, "'%s' - a '${' without its '}'", s);
      return false;
    }
    if (varNameCheck(ruleP, p + 2, end - p - 2, why, whyLen) == false)
      return false;
    *usedP = true;
    p = end + 1;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// varsCheckTree - varsCheck on every string value of a tree (set, create)
//
bool varsCheckTree(CepRule* ruleP, CorNode* nodeP, bool* usedP, char* why, int whyLen)
{
  if (nodeP->type == CorString)
    return varsCheck(ruleP, nodeP->value.s, usedP, why, whyLen);

  if ((nodeP->type == CorObject) || (nodeP->type == CorArray))
  {
    for (CorNode* kP = nodeP->value.head; kP != NULL; kP = kP->next)
    {
      if (varsCheckTree(ruleP, kP, usedP, why, whyLen) == false)
        return false;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// VarCtx - what a ${...} is resolved against: the fire
//
typedef struct VarCtx
{
  CepVars*      vars;
  CepPatterns*  psP;
  CepRule*      ruleP;
  const char*   entity;                          // the entity the rule fired for, NULL: none
  CorAlloc*     allocP;
} VarCtx;



// -----------------------------------------------------------------------------
//
// varValue - the JSON text of a name; NULL: no value (why says so)
//
static const char* varValue(VarCtx* cP, const char* name, int len, char* why, int whyLen)
{
  if ((len == 6) && (strncmp(name, "entity", 6) == 0))
  {
    if (cP->entity == NULL)
    {
      snprintf(why, whyLen, "${entity} - the rule fired for no entity (a schedule)");
      return NULL;
    }
    int   n = strlen(cP->entity) + 3;
    char* t = corAlloc(cP->allocP, n);
    snprintf(t, n, "\"%s\"", cP->entity);
    return t;
  }

  const char* dot   = memchr(name, '.', len);
  int         evLen = dot - name;
  char        event[256];
  char        field[16];

  snprintf(event, sizeof(event), "%.*s", evLen, name);
  snprintf(field, sizeof(field), "%.*s", len - evLen - 1, dot + 1);

  const char* key = (cP->ruleP->partitioned && (cP->entity != NULL)) ? cP->entity : "";

  if ((strcmp(field, "entity") != 0) && (strcmp(field, "value") != 0))   // a window's aggregate
  {
    double a;
    if (patternsAggregate(cP->psP, cP->ruleP, key, &a) == false)
    {
      snprintf(why, whyLen, "${%s} - the window has no value", event);
      return NULL;
    }
    char* t = corAlloc(cP->allocP, 40);
    snprintf(t, 40, "%.17g", a);
    return t;
  }

  CepLast* lP = lastOf(cP->vars, cP->ruleP->id, key, event);
  if (lP == NULL)
  {
    snprintf(why, whyLen, "${%s.%s} - no '%s' event yet", event, field, event);
    return NULL;
  }

  if (strcmp(field, "value") == 0)
    return lP->value;

  int   n = strlen(lP->entity) + 3;
  char* t = corAlloc(cP->allocP, n);
  snprintf(t, n, "\"%s\"", lP->entity);
  return t;
}



// -----------------------------------------------------------------------------
//
// unquoted - a JSON value's text as written into a string: a string without its quotes (and its escapes as they
// are - an id or a q has none that pass the checks); others as they are
//
static const char* unquoted(CorAlloc* allocP, const char* json, bool* stringP)
{
  int len = strlen(json);

  *stringP = (len >= 2) && (json[0] == '"') && (json[len - 1] == '"');
  if (*stringP == false)
    return json;

  char* s = corAlloc(allocP, len - 1);
  memcpy(s, &json[1], len - 2);
  s[len - 2] = 0;
  return s;
}



// -----------------------------------------------------------------------------
//
// VarUse - where a string goes: the checks its values must pass
//
typedef enum VarUse
{
  VarUseText,                                    // a value in set / create - anything
  VarUseId,                                      // an entity id
  VarUseQ                                        // an NGSI-LD q
} VarUse;



// -----------------------------------------------------------------------------
//
// textSubst - a string with its ${...} written into it; NULL: a value missing or not allowed (why)
//
static char* textSubst(VarCtx* cP, const char* s, VarUse use, char* why, int whyLen)
{
  int   size = strlen(s) + 1;
  char* out  = corAlloc(cP->allocP, size);
  int   len  = 0;

  out[0] = 0;

  for (const char* p = s; *p != 0; )
  {
    const char* var = strstr(p, "${");
    const char* end = (var != NULL) ? strchr(var + 2, '}') : NULL;

    if ((var == NULL) || (end == NULL))
      var = end = p + strlen(p);

    //
    // The text before it
    //
    int pre = var - p;
    if (len + pre + 1 > size)
    {
      size = (len + pre + 1) * 2;
      char* bigger = corAlloc(cP->allocP, size);
      memcpy(bigger, out, len + 1);
      out = bigger;
    }
    memcpy(&out[len], p, pre);
    len += pre;
    out[len] = 0;

    if (*var == 0)
      break;

    //
    // The value
    //
    const char* json = varValue(cP, var + 2, end - var - 2, why, whyLen);
    if (json == NULL)
      return NULL;

    bool        isString;
    const char* text = unquoted(cP->allocP, json, &isString);

    if (use == VarUseId)
    {
      if (strpbrk(text, " \t\r\n\"<>{}|\\^`") != NULL)
      {
        snprintf(why, whyLen, "${%.*s} = %s - not for an entity id", (int) (end - var - 2), var + 2, json);
        return NULL;
      }
    }
    else if (use == VarUseQ)
    {
      bool quoted = (var > s) && (var[-1] == '"') && (end[1] == '"');
      bool scalar = (isString == false) && (json[0] != '{') && (json[0] != '[') && (strcmp(json, "null") != 0);

      if (isString ? ((quoted == false) || (strpbrk(text, "\"\\") != NULL)) : (scalar == false))
      {
        snprintf(why, whyLen, "${%.*s} = %s - not for the q (a string goes between quotes, without a quote in it; else a number or a boolean)",
                 (int) (end - var - 2), var + 2, json);
        return NULL;
      }
    }

    int tlen = strlen(text);
    if (len + tlen + 1 > size)
    {
      size = (len + tlen + 1) * 2;
      char* bigger = corAlloc(cP->allocP, size);
      memcpy(bigger, out, len + 1);
      out = bigger;
    }
    memcpy(&out[len], text, tlen);
    len += tlen;
    out[len] = 0;

    p = end + 1;
  }

  return out;
}



// -----------------------------------------------------------------------------
//
// treeSubst - a copy of a tree (set, create) with its strings' ${...} resolved; NULL: why
//
static CorNode* treeSubst(VarCtx* cP, CorNode* nodeP, char* why, int whyLen)
{
  if (nodeP->type == CorString)
  {
    const char* s   = nodeP->value.s;
    int         len = strlen(s);
    const char* end = (strncmp(s, "${", 2) == 0) ? strchr(s + 2, '}') : NULL;

    if ((end != NULL) && (end == &s[len - 1]))     // one ${...}, nothing else: the value itself, its type kept
    {
      const char* json = varValue(cP, s + 2, len - 3, why, whyLen);
      if (json == NULL)
        return NULL;

      CorJson j;
      char*   copy = corAllocStrdup(cP->allocP, json);   // the parse works in place
      corJsonCreate(&j, cP->allocP);
      CorNode* vP = corJsonParse(&j, copy);
      if (vP == NULL)
      {
        snprintf(why, whyLen, "${%.*s} - its value is not JSON", len - 3, s + 2);
        return NULL;
      }
      vP->name = nodeP->name;
      return vP;
    }

    char* text = textSubst(cP, s, VarUseText, why, whyLen);
    return (text != NULL) ? corTreeString(cP->allocP, nodeP->name, text) : NULL;
  }

  if ((nodeP->type != CorObject) && (nodeP->type != CorArray))
    return corTreeClone(cP->allocP, nodeP);

  CorNode* outP = (nodeP->type == CorObject) ? corTreeObject(cP->allocP, nodeP->name) : corTreeArray(cP->allocP, nodeP->name);
  for (CorNode* kP = nodeP->value.head; kP != NULL; kP = kP->next)
  {
    CorNode* xP = treeSubst(cP, kP, why, whyLen);
    if (xP == NULL)
      return NULL;
    corTreeChildAdd(outP, xP);
  }

  return outP;
}



// -----------------------------------------------------------------------------
//
// varsAction - a copy of an action with its ${...} resolved for this fire; false: why
//
bool varsAction(CorAlloc* allocP, CepVars* vP, CepPatterns* psP, CepRule* ruleP, const char* entity, CepAction* aP, CepAction* outP, char* why, int whyLen)
{
  VarCtx ctx = { vP, psP, ruleP, entity, allocP };

  *outP = *aP;

  if ((aP->entity != NULL) && ((outP->entity = textSubst(&ctx, aP->entity, VarUseId, why, whyLen)) == NULL))
    return false;
  if ((aP->q != NULL) && ((outP->q = textSubst(&ctx, aP->q, VarUseQ, why, whyLen)) == NULL))
    return false;
  if ((aP->set != NULL) && ((outP->set = treeSubst(&ctx, aP->set, why, whyLen)) == NULL))
    return false;

  if (aP->kind == CepActionCreate)               // a create's id: an id
  {
    CorNode* idP = corTreeLookup(outP->set, "id");
    if ((idP != NULL) && ((idP->type != CorString) || (strpbrk(idP->value.s, " \t\r\n\"<>{}|\\^`") != NULL) || (strchr(idP->value.s, ':') == NULL)))
    {
      snprintf(why, whyLen, "create: \"id\" is not an entity id after its ${...}");
      return false;
    }
  }

  return true;
}
