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
#include <stdlib.h>                              // calloc, free, strtod
#include <string.h>                              // strcmp

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corJson/corJsonRender.h"               // corJsonRender
#include "corJson/corJsonRenderSize.h"           // corJsonRenderSize
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
// idType - an NGSI-LD id's type: urn:ngsi-ld:<type>:... ; NULL: the id holds none
//
static char* idType(CorAlloc* allocP, const char* id)
{
  if (strncmp(id, "urn:ngsi-ld:", 12) != 0)
    return NULL;

  const char* t   = id + 12;
  const char* end = strchr(t, ':');

  if ((end == NULL) || (end == t))
    return NULL;

  char* type = corAlloc(allocP, end - t + 1);
  memcpy(type, t, end - t);
  type[end - t] = 0;

  return type;
}



// -----------------------------------------------------------------------------
//
// eventParse - one event: { "event": name, "entity": id | "type": type, "attribute": attribute, "where": q }
//
static bool eventParse(CorAlloc* allocP, CorNode* evNodeP, CepEvent* evP, const char* what, char* why, int whyLen)
{
  if ((evNodeP == NULL) || (evNodeP->type != CorObject))
  {
    snprintf(why, whyLen, "%s - an object", what);
    return false;
  }

  char*    entity    = str(evNodeP, "entity");
  char*    type      = str(evNodeP, "type");
  char*    attribute = str(evNodeP, "attribute");
  char*    name      = str(evNodeP, "event");
  CorNode* whereP    = corTreeLookup(evNodeP, "where");

  if ((entity == NULL) && (type == NULL))
  {
    snprintf(why, whyLen, "%s names no 'entity' and no 'type'", what);
    return false;
  }
  if (attribute == NULL)
  {
    snprintf(why, whyLen, "%s names no 'attribute'", what);
    return false;
  }
  if ((whereP != NULL) && ((whereP->type != CorString) || (whereP->value.s[0] == 0)))
  {
    snprintf(why, whyLen, "%s: 'where' - an NGSI-LD q, a string", what);
    return false;
  }

  //
  // An EntitySelector needs a type: an id alone gives its own - urn:ngsi-ld:<type>:...
  //
  if (type == NULL)
    type = idType(allocP, entity);
  if (type == NULL)
  {
    snprintf(why, whyLen, "%s names an entity whose id holds no type (urn:ngsi-ld:<type>:...) - give 'type' too", what);
    return false;
  }

  evP->name      = corAllocStrdup(allocP, (name != NULL) ? name : attribute);
  evP->entity    = (entity != NULL) ? corAllocStrdup(allocP, entity) : NULL;
  evP->type      = corAllocStrdup(allocP, type);
  evP->attribute = corAllocStrdup(allocP, attribute);
  evP->q         = (whereP != NULL) ? corAllocStrdup(allocP, whereP->value.s) : NULL;
  evP->watch     = -1;

  return true;
}



// -----------------------------------------------------------------------------
//
// durationParse - an ISO 8601 duration, as NGSI-LD's own: P[nD][T[nH][nM][n[.n]S]] -> milliseconds; false: not one,
// or zero
//
bool durationParse(const char* s, long long* msP)
{
  double ms     = 0;
  bool   inTime = false;
  bool   any    = false;

  if ((s == NULL) || (*s != 'P'))
    return false;

  for (++s; *s != 0; )
  {
    if (*s == 'T')
    {
      if (inTime)
        return false;
      inTime = true;
      ++s;
      continue;
    }

    char*  end;
    double n = strtod(s, &end);

    if ((end == s) || (n < 0))
      return false;

    switch (*end)
    {
    case 'D': if (inTime)  return false; ms += n * 86400000.0; break;
    case 'H': if (!inTime) return false; ms += n * 3600000.0;  break;
    case 'M': if (!inTime) return false; ms += n * 60000.0;    break;
    case 'S': if (!inTime) return false; ms += n * 1000.0;     break;
    default:  return false;
    }

    any = true;
    s   = end + 1;
  }

  if ((any == false) || (ms < 1))
    return false;

  *msP = (long long) (ms + 0.5);
  return true;
}



// -----------------------------------------------------------------------------
//
// eventUse - the event named <name> as one the pattern uses: its index in ruleP->eventV (added the first time), -1: no
// such event declared
//
static int eventUse(CepRule* ruleP, CepEvent* declV, int decls, const char* name, char* why, int whyLen)
{
  for (int ix = 0; ix < ruleP->events; ix++)
  {
    if (strcmp(ruleP->eventV[ix].name, name) == 0)
      return ix;
  }

  for (int ix = 0; ix < decls; ix++)
  {
    if (strcmp(declV[ix].name, name) == 0)
    {
      ruleP->eventV[ruleP->events] = declV[ix];
      return ruleP->events++;
    }
  }

  snprintf(why, whyLen, "the pattern names '%s' - no such event in 'events'", name);
  return -1;
}



// -----------------------------------------------------------------------------
//
// patternOne - the pattern is one event
//
static bool patternOne(CorAlloc* allocP, CepRule* ruleP, CepEvent* declV, int decls, const char* name, char* why, int whyLen)
{
  ruleP->eventV = (CepEvent*) corAlloc(allocP, decls * sizeof(CepEvent));
  ruleP->events = 0;
  ruleP->kind   = CepPatternAny;

  return eventUse(ruleP, declV, decls, name, why, whyLen) >= 0;
}



// -----------------------------------------------------------------------------
//
// scheduleParse - { "cron": "<fields>" } or { "at": "HH:MM[:SS]" | "sunrise" | "sunset", "offset": "[-]<duration>" }
//
// The location of sunrise / sunset is the Rule entity's - rulesLoad reads it (locationParse).
//
static bool scheduleParse(CorAlloc* allocP, CorNode* patternP, CorNode* kindP, CepRule* ruleP, char* why, int whyLen)
{
  ruleP->eventV = (CepEvent*) corAlloc(allocP, sizeof(CepEvent));      // none - never fed
  ruleP->events = 0;

  if (kindP->type != CorString)
  {
    snprintf(why, whyLen, "'%s' - a string", kindP->name);
    return false;
  }

  if (strcmp(kindP->name, "cron") == 0)
  {
    ruleP->kind = CepPatternCron;
    if (cronParse(kindP->value.s, &ruleP->cron, why, whyLen) == false)
      return false;
  }
  else
  {
    const char* at = kindP->value.s;
    int         h, m, s = 0;
    char        end;

    ruleP->kind = CepPatternAt;

    if (strcmp(at, "sunrise") == 0)
      ruleP->atKind = CepAtSunrise;
    else if (strcmp(at, "sunset") == 0)
      ruleP->atKind = CepAtSunset;
    else if (((sscanf(at, "%2d:%2d:%2d%c", &h, &m, &s, &end) == 3) || ((sscanf(at, "%2d:%2d%c", &h, &m, &end) == 2) && (strlen(at) == 5))) &&
             (h >= 0) && (h <= 23) && (m >= 0) && (m <= 59) && (s >= 0) && (s <= 59))
    {
      ruleP->atKind   = CepAtClock;
      ruleP->atSecond = h * 3600 + m * 60 + s;
    }
    else
    {
      snprintf(why, whyLen, "'at' - \"HH:MM\", \"HH:MM:SS\", \"sunrise\" or \"sunset\": '%s'", at);
      return false;
    }

    CorNode* offsetP = corTreeLookup(patternP, "offset");
    if (offsetP != NULL)
    {
      long long ms;
      bool      minus = (offsetP->type == CorString) && (offsetP->value.s[0] == '-');

      if ((offsetP->type != CorString) || (durationParse(offsetP->value.s + (minus ? 1 : 0), &ms) == false))
      {
        snprintf(why, whyLen, "'offset' - an ISO 8601 duration, '-' before it for earlier (\"-PT30M\")");
        return false;
      }
      ruleP->offsetS = (minus ? -ms : ms) / 1000;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// locationParse - a sunrise / sunset rule's place: the Rule entity's 'location', a GeoJSON Point (keyValues)
//
static bool locationParse(CorAlloc* allocP, CorNode* entityP, CepRule* ruleP, char* why, int whyLen)
{
  CorNode* locP    = corTreeLookup(entityP, "location");
  CorNode* typeP   = (locP != NULL) && (locP->type == CorObject) ? corTreeLookup(locP, "type")        : NULL;
  CorNode* coordsP = (locP != NULL) && (locP->type == CorObject) ? corTreeLookup(locP, "coordinates") : NULL;
  CorNode* lonP    = ((coordsP != NULL) && (coordsP->type == CorArray)) ? coordsP->value.head : NULL;
  CorNode* latP    = (lonP != NULL) ? lonP->next : NULL;

  if ((typeP == NULL) || (typeP->type != CorString) || (strcmp(typeP->value.s, "Point") != 0) ||
      (lonP == NULL) || ((lonP->type != CorInt) && (lonP->type != CorFloat)) ||
      (latP == NULL) || ((latP->type != CorInt) && (latP->type != CorFloat)))
  {
    snprintf(why, whyLen, "'at': '%s' needs the Rule's 'location' - a GeoProperty, a Point", (ruleP->atKind == CepAtSunrise) ? "sunrise" : "sunset");
    return false;
  }

  ruleP->lon = (lonP->type == CorInt) ? (double) lonP->value.i : lonP->value.f;
  ruleP->lat = (latP->type == CorInt) ? (double) latP->value.i : latP->value.f;

  if ((ruleP->lat < -90) || (ruleP->lat > 90) || (ruleP->lon < -180) || (ruleP->lon > 180))
  {
    snprintf(why, whyLen, "'location' - [ longitude -180..180, latitude -90..90 ]");
    return false;
  }

  //
  // The place is part of what the rule's state hangs on: another place - the next time computed afresh
  //
  char   place[64];
  size_t len = strlen(ruleP->whenText);

  snprintf(place, sizeof(place), "|%.6f,%.6f", ruleP->lon, ruleP->lat);
  char* text = corAlloc(allocP, len + strlen(place) + 1);
  memcpy(text, ruleP->whenText, len);
  strcpy(text + len, place);
  ruleP->whenText = text;

  return true;
}



// -----------------------------------------------------------------------------
//
// windowParse - the parameters of a window: 'for', and ONE aggregate with its bounds
//
static bool windowParse(CorNode* patternP, CepRule* ruleP, char* why, int whyLen)
{
  static const char* aggs[]   = { "avg", "min", "max", "sum", "count", NULL };
  static const char* bounds[] = { "gt", "ge", "lt", "le", NULL };
  CorNode*           forP     = corTreeLookup(patternP, "for");
  CorNode*           aggP     = NULL;

  ruleP->kind = CepPatternWindow;

  if ((forP == NULL) || (forP->type != CorString) || (durationParse(forP->value.s, &ruleP->forMs) == false))
  {
    snprintf(why, whyLen, "'window' needs 'for' - an ISO 8601 duration (\"PT1H\")");
    return false;
  }

  for (int ix = 0; aggs[ix] != NULL; ix++)
  {
    CorNode* nP = corTreeLookup(patternP, aggs[ix]);

    if (nP == NULL)
      continue;
    if (aggP != NULL)
    {
      snprintf(why, whyLen, "'window': one aggregate - '%s' and '%s'", aggP->name, nP->name);
      return false;
    }
    aggP             = nP;
    ruleP->aggregate = (CepWindowAgg) ix;
  }

  if (aggP == NULL)
  {
    snprintf(why, whyLen, "'window' needs an aggregate - avg, min, max, sum or count");
    return false;
  }

  if ((aggP->type != CorObject) || (aggP->value.head == NULL))
  {
    snprintf(why, whyLen, "'%s' - { \"gt\" | \"ge\" | \"lt\" | \"le\": <number>, ... }", aggP->name);
    return false;
  }

  ruleP->bounds = 0;
  for (CorNode* bP = aggP->value.head; bP != NULL; bP = bP->next)
  {
    int b = -1;

    for (int ix = 0; bounds[ix] != NULL; ix++)
    {
      if (strcmp(bP->name, bounds[ix]) == 0)
        b = ix;
    }

    if ((b == -1) || ((bP->type != CorInt) && (bP->type != CorFloat)))
    {
      snprintf(why, whyLen, "'%s' - { \"gt\" | \"ge\" | \"lt\" | \"le\": <number>, ... }", aggP->name);
      return false;
    }

    ruleP->boundV[b] = (bP->type == CorInt) ? (double) bP->value.i : bP->value.f;
    ruleP->bounds   |= 1u << b;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// patternParse - { "<kind>": ..., <its parameters> } - the vocabulary of doc/design.md § 3.1:
//
//   { "any":    [ <names> ] }                               any of them
//   { "seq":    [ <names> ], "within": <duration> }         in this order (a name may repeat), all within
//   { "all":    [ <names> ], "within": <duration> }         each of them, in any order, all within
//   { "repeat": <name>, "min": <n>, "within": <duration> }  n times within
//   { "absent": <name>, "for": <duration> }                 none for that long
//   { "absent": <name>, "for": <duration>, "after": <name> }  none for that long after the 'after' event - it
//                                                            starts the deadline, <name> clears it
//   { "window": <name>, "for": <duration>, "<aggregate>": { "<bound>": <number>, ... } }
//                                                           an aggregate of the last 'for' of values: avg, min, max,
//                                                           sum or count - one; bounds gt, ge, lt, le - all must hold
//   { "cron": "<min> <hour> <dom> <month> <dow>" }          a cron schedule (6 fields: the second first) - no events
//   { "at": "HH:MM[:SS]" | "sunrise" | "sunset", "offset": <signed duration> }
//                                                           every day; the sun at the Rule's 'location' - no events
// "supersedes" (seq, all, repeat): the
// ids of the rules this one supersedes - each of their fires waits 'within', and a match of this one cancels it.
//
static bool patternParse(CorAlloc* allocP, CorNode* patternP, CepRule* ruleP, CepEvent* declV, int decls, char* why, int whyLen)
{
  static const char* kinds[] = { "any", "seq", "all", "repeat", "absent", "window", "at", "cron", NULL };
  CorNode*           kindP   = NULL;

  for (CorNode* mP = patternP->value.head; (mP != NULL) && (kindP == NULL); mP = mP->next)
  {
    for (int ix = 0; kinds[ix] != NULL; ix++)
    {
      if (strcmp(mP->name, kinds[ix]) == 0)
      {
        kindP = mP;
        break;
      }
    }
  }

  if (kindP == NULL)
  {
    snprintf(why, whyLen, "'pattern' names no kind (any, seq, all, repeat, absent, window, cron, at)");
    return false;
  }

  const char* kind = kindP->name;

  if ((strcmp(kind, "at") == 0) || (strcmp(kind, "cron") == 0))
    return scheduleParse(allocP, patternP, kindP, ruleP, why, whyLen);

  ruleP->eventV = (CepEvent*) corAlloc(allocP, decls * sizeof(CepEvent));
  ruleP->events = 0;

  //
  // The names: a list (any, seq, all) or one (repeat, absent)
  //
  bool list = (strcmp(kind, "any") == 0) || (strcmp(kind, "seq") == 0) || (strcmp(kind, "all") == 0);

  if (list)
  {
    if ((kindP->type != CorArray) || (kindP->value.head == NULL))
    {
      snprintf(why, whyLen, "'%s' - a list of event names", kind);
      return false;
    }

    int n = count(kindP);
    ruleP->stepV = (int*) corAlloc(allocP, n * sizeof(int));
    ruleP->steps = 0;

    for (CorNode* nP = kindP->value.head; nP != NULL; nP = nP->next)
    {
      if (nP->type != CorString)
      {
        snprintf(why, whyLen, "'%s' - a list of event names", kind);
        return false;
      }

      int before = ruleP->events;
      int ix     = eventUse(ruleP, declV, decls, nP->value.s, why, whyLen);

      if (ix < 0)
        return false;

      if ((ix < before) && (strcmp(kind, "seq") != 0))     // only a sequence may name an event twice
      {
        snprintf(why, whyLen, "'%s' names '%s' twice", kind, nP->value.s);
        return false;
      }
      ruleP->stepV[ruleP->steps++] = ix;
    }
  }
  else
  {
    if (kindP->type != CorString)
    {
      snprintf(why, whyLen, "'%s' - an event name", kind);
      return false;
    }
    if (eventUse(ruleP, declV, decls, kindP->value.s, why, whyLen) < 0)
      return false;
  }

  //
  // The parameters
  //
  CorNode* withinP = corTreeLookup(patternP, "within");
  CorNode* forP    = corTreeLookup(patternP, "for");
  CorNode* minP    = corTreeLookup(patternP, "min");

  if ((corTreeLookup(patternP, "after") != NULL) && (strcmp(kind, "absent") != 0))
  {
    snprintf(why, whyLen, "'after' - with 'absent' only");
    return false;
  }

  if (strcmp(kind, "any") == 0)
    ruleP->kind = CepPatternAny;
  else if (strcmp(kind, "window") == 0)
    return windowParse(patternP, ruleP, why, whyLen);
  else if (strcmp(kind, "absent") == 0)
  {
    CorNode* afterP = corTreeLookup(patternP, "after");

    ruleP->kind    = CepPatternAbsent;
    ruleP->afterEv = -1;
    if ((forP == NULL) || (forP->type != CorString) || (durationParse(forP->value.s, &ruleP->forMs) == false))
    {
      snprintf(why, whyLen, "'absent' needs 'for' - an ISO 8601 duration (\"PT10M\")");
      return false;
    }
    if (afterP != NULL)
    {
      if ((afterP->type != CorString) || (strcmp(afterP->value.s, kindP->value.s) == 0))
      {
        snprintf(why, whyLen, "'absent': 'after' - the name of another event, the one that starts the deadline");
        return false;
      }

      ruleP->afterEv = eventUse(ruleP, declV, decls, afterP->value.s, why, whyLen);
      if (ruleP->afterEv < 0)
        return false;
    }
  }
  else
  {
    ruleP->kind = (strcmp(kind, "seq") == 0) ? CepPatternSeq : (strcmp(kind, "all") == 0) ? CepPatternAll : CepPatternRepeat;

    if ((withinP == NULL) || (withinP->type != CorString) || (durationParse(withinP->value.s, &ruleP->withinMs) == false))
    {
      snprintf(why, whyLen, "'%s' needs 'within' - an ISO 8601 duration (\"PT0.5S\")", kind);
      return false;
    }

    if (ruleP->kind == CepPatternRepeat)
    {
      if ((minP == NULL) || (minP->type != CorInt) || (minP->value.i < 2) || (minP->value.i > 1000))
      {
        snprintf(why, whyLen, "'repeat' needs 'min' - a number, 2 to 1000");
        return false;
      }
      ruleP->min = (int) minP->value.i;
    }
    else if (ruleP->steps < 2)
    {
      snprintf(why, whyLen, "'%s' - two steps or more", kind);
      return false;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// whenParse - `when`, in one of two forms:
//
//   { "entity" | "type", "attribute", "where" }                one event - the short form
//   { "events": [ <event>, ... ], "pattern": <pattern> }       named events, combined by the pattern
//
// The pattern: "<event>" (that one), or an object (patternParse); no pattern with one event: that event.
//
static bool whenParse(CorAlloc* allocP, CorNode* whenP, CepRule* ruleP, char* why, int whyLen)
{
  if ((whenP == NULL) || (whenP->type != CorObject))
  {
    snprintf(why, whyLen, "'when' - an object");
    return false;
  }

  CorNode* eventsP  = corTreeLookup(whenP, "events");
  CorNode* patternP = corTreeLookup(whenP, "pattern");

  bool schedule = (patternP != NULL) && (patternP->type == CorObject) &&
                  ((corTreeLookup(patternP, "cron") != NULL) || (corTreeLookup(patternP, "at") != NULL));

  if ((eventsP == NULL) && schedule)                                       // a schedule: no events needed
    return patternParse(allocP, patternP, ruleP, NULL, 0, why, whyLen);

  if (eventsP == NULL)
  {
    if (patternP != NULL)
    {
      snprintf(why, whyLen, "'when': a 'pattern' needs 'events'");
      return false;
    }

    ruleP->eventV = (CepEvent*) corAlloc(allocP, sizeof(CepEvent));
    ruleP->events = 1;
    ruleP->declV  = ruleP->eventV;
    ruleP->decls  = 1;
    return eventParse(allocP, whenP, &ruleP->eventV[0], "'when'", why, whyLen);
  }

  if ((eventsP->type != CorArray) || (eventsP->value.head == NULL))
  {
    snprintf(why, whyLen, "'when': 'events' - a list of events");
    return false;
  }

  //
  // Every event declared - parsed, and named once only
  //
  int       n       = count(eventsP);
  CepEvent* declV   = (CepEvent*) corAlloc(allocP, n * sizeof(CepEvent));
  int       decls   = 0;

  for (CorNode* eP = eventsP->value.head; eP != NULL; eP = eP->next)
  {
    char what[64];
    snprintf(what, sizeof(what), "event %d of 'events'", decls + 1);

    if (eventParse(allocP, eP, &declV[decls], what, why, whyLen) == false)
      return false;

    for (int ix = 0; ix < decls; ix++)
    {
      if (strcmp(declV[ix].name, declV[decls].name) == 0)
      {
        snprintf(why, whyLen, "'events': two events named '%s'", declV[decls].name);
        return false;
      }
    }
    ++decls;
  }

  ruleP->declV = declV;
  ruleP->decls = decls;

  //
  // The pattern
  //
  if (patternP == NULL)
  {
    if (decls != 1)
    {
      snprintf(why, whyLen, "'when': %d events need a 'pattern' - { \"any\": [ ... ] }", decls);
      return false;
    }
    return patternOne(allocP, ruleP, declV, decls, declV[0].name, why, whyLen);
  }

  if (patternP->type == CorString)
    return patternOne(allocP, ruleP, declV, decls, patternP->value.s, why, whyLen);

  if (patternP->type != CorObject)
  {
    snprintf(why, whyLen, "'pattern' - an event name or an object");
    return false;
  }

  return patternParse(allocP, patternP, ruleP, declV, decls, why, whyLen);
}



// -----------------------------------------------------------------------------
//
// numberOf - a number member: true and *vP if present and a number
//
static bool numberOf(CorNode* objP, const char* name, double* vP)
{
  CorNode* nP = corTreeLookup(objP, name);

  if (nP == NULL)
    return false;
  if (nP->type == CorInt)
    *vP = (double) nP->value.i;
  else if (nP->type == CorFloat)
    *vP = nP->value.f;
  else
    return false;

  return true;
}



// -----------------------------------------------------------------------------
//
// untilOf - the event a step stops at: declared in 'events', not used by the pattern; its index in untilV
//
static int untilOf(CorAlloc* allocP, CepRule* ruleP, const char* name, char* why, int whyLen)
{
  for (int ix = 0; ix < ruleP->events; ix++)
  {
    if (strcmp(ruleP->eventV[ix].name, name) == 0)
    {
      snprintf(why, whyLen, "'until' names '%s', an event of the pattern - it would fire the rule too", name);
      return -1;
    }
  }

  for (int ix = 0; ix < ruleP->untils; ix++)
  {
    if (strcmp(ruleP->untilV[ix].name, name) == 0)
      return ix;
  }

  for (int ix = 0; ix < ruleP->decls; ix++)
  {
    if (strcmp(ruleP->declV[ix].name, name) == 0)
    {
      if (ruleP->untilV == NULL)
        ruleP->untilV = (CepEvent*) corAlloc(allocP, ruleP->decls * sizeof(CepEvent));
      ruleP->untilV[ruleP->untils] = ruleP->declV[ix];
      return ruleP->untils++;
    }
  }

  snprintf(why, whyLen, "'until' names '%s' - no such event in 'events'", name);
  return -1;
}



// -----------------------------------------------------------------------------
//
// createParse - { "create": { "id"?, "type", "<attribute>": <value>, ... } } - no target besides
//
static bool createParse(CorAlloc* allocP, CorNode* aP, CorNode* createP, CepAction* actP, char* why, int whyLen)
{
  if ((corTreeLookup(aP, "entity") != NULL) || (corTreeLookup(aP, "type") != NULL) || (corTreeLookup(aP, "q") != NULL) || (corTreeLookup(aP, "until") != NULL))
  {
    snprintf(why, whyLen, "'create' makes its entity - no 'entity', 'type', 'q', 'until' besides (the type goes inside)");
    return false;
  }
  if (createP->type != CorObject)
  {
    snprintf(why, whyLen, "'create' - an object: the entity, \"type\" and its attributes' values");
    return false;
  }

  CorNode* idP   = corTreeLookup(createP, "id");
  CorNode* typeP = corTreeLookup(createP, "type");

  if ((typeP == NULL) || (typeP->type != CorString) || (typeP->value.s[0] == 0))
  {
    snprintf(why, whyLen, "'create' - the entity needs a \"type\", a string");
    return false;
  }
  if ((idP != NULL) && ((idP->type != CorString) || (strchr(idP->value.s, ':') == NULL)))
  {
    snprintf(why, whyLen, "'create' - \"id\" is a URI (urn:ngsi-ld:...); leave it out and corCep makes one each time");
    return false;
  }

  actP->kind  = CepActionCreate;
  actP->set   = createP;                         // lives in the generation's allocator
  actP->until = -1;
  (void) allocP;
  return true;
}



// -----------------------------------------------------------------------------
//
// actionParse - one of `then`. The target: "entity": <id>, or "type": <type> with an optional "q" - every entity
// of the type that matches. What is done, one of:
//
//   "set":    { "<attribute>": <value>, ... }          PATCHed as Properties
//   "toggle": "<attribute>"                            a boolean: read, the other value PATCHed
//   "step":   { "attribute", "by", "every", "min", "max", "for" }, "until": "<event>"
//             a number: 'by' added every 'every' - until the event, a bound, or 'for' (default PT30S)
//   "delete": true                                     the target deleted - one entity, or the group
//   "delete": "<id>"                                   the same, "entity" written in it
//
// And one without a target - it makes it:
//
//   "create": { "id"?, "type", "<attribute>": <value>, ... }
//             POST /entities, the values as Properties; no "id": corCep makes one each time -
//             urn:ngsi-ld:<type>:<rule's last part>-<ms>-<n>
//
static bool actionParse(CorAlloc* allocP, CepRule* ruleP, CorNode* aP, CepAction* actP, char* why, int whyLen)
{
  if (aP->type != CorObject)
  {
    snprintf(why, whyLen, "an action - an object");
    return false;
  }

  char*    entity = str(aP, "entity");
  char*    type   = str(aP, "type");
  char*    q      = str(aP, "q");
  CorNode* setP   = corTreeLookup(aP, "set");
  char*    toggle = str(aP, "toggle");
  CorNode* stepP  = corTreeLookup(aP, "step");
  char*    until  = str(aP, "until");
  CorNode* createP = corTreeLookup(aP, "create");
  CorNode* deleteP = corTreeLookup(aP, "delete");

  if ((setP != NULL) + (toggle != NULL) + (stepP != NULL) + (createP != NULL) + (deleteP != NULL) != 1)
  {
    snprintf(why, whyLen, "an action is one of 'set', 'toggle', 'step', 'create' and 'delete'");
    return false;
  }

  if (createP != NULL)
    return createParse(allocP, aP, createP, actP, why, whyLen);

  if ((deleteP != NULL) && (deleteP->type == CorString))       // { "delete": "<id>" } - the id is the target
  {
    if ((entity != NULL) || (type != NULL) || (q != NULL))
    {
      snprintf(why, whyLen, "'delete': \"<id>\" names its target - no 'entity', 'type', 'q' besides");
      return false;
    }
    entity = deleteP->value.s;
  }
  else if ((deleteP != NULL) && ((deleteP->type != CorBoolean) || (deleteP->value.b == false)))
  {
    snprintf(why, whyLen, "'delete' - true (the target: 'entity', or 'type' and 'q'), or an entity id");
    return false;
  }

  if ((entity == NULL) == (type == NULL))
  {
    snprintf(why, whyLen, "an action names its target: 'entity', or 'type' (and 'q') - one of them");
    return false;
  }
  if ((q != NULL) && (type == NULL))
  {
    snprintf(why, whyLen, "an action's 'q' - with 'type' only");
    return false;
  }
  if ((until != NULL) && (stepP == NULL))
  {
    snprintf(why, whyLen, "'until' - with 'step' only");
    return false;
  }

  actP->entity = (entity != NULL) ? corAllocStrdup(allocP, entity) : NULL;
  actP->type   = (type   != NULL) ? corAllocStrdup(allocP, type)   : NULL;
  actP->q      = (q      != NULL) ? corAllocStrdup(allocP, q)      : NULL;
  actP->until  = -1;

  if (deleteP != NULL)
  {
    actP->kind = CepActionDelete;
    return true;
  }

  if (setP != NULL)
  {
    if ((setP->type != CorObject) || (setP->value.head == NULL))
    {
      snprintf(why, whyLen, "'set' - an object of attributes and values");
      return false;
    }
    actP->kind = CepActionSet;
    actP->set  = setP;                                 // lives in the generation's allocator
    return true;
  }

  if (toggle != NULL)
  {
    actP->kind      = CepActionToggle;
    actP->attribute = corAllocStrdup(allocP, toggle);
    return true;
  }

  //
  // step
  //
  char* attribute = (stepP->type == CorObject) ? str(stepP, "attribute") : NULL;
  char* every     = (stepP->type == CorObject) ? str(stepP, "every")     : NULL;
  char* forS      = (stepP->type == CorObject) ? str(stepP, "for")       : NULL;

  if ((attribute == NULL) || (numberOf(stepP, "by", &actP->by) == false) || (actP->by == 0))
  {
    snprintf(why, whyLen, "'step' needs 'attribute' and 'by' - a number, not 0");
    return false;
  }
  if ((every == NULL) || (durationParse(every, &actP->everyMs) == false) || (actP->everyMs < 50))
  {
    snprintf(why, whyLen, "'step' needs 'every' - an ISO 8601 duration, PT0.05S or more");
    return false;
  }
  actP->forMs = 30000;
  if ((forS != NULL) && (durationParse(forS, &actP->forMs) == false))
  {
    snprintf(why, whyLen, "'step': 'for' - an ISO 8601 duration");
    return false;
  }

  actP->kind      = CepActionStep;
  actP->attribute = corAllocStrdup(allocP, attribute);
  actP->hasMin    = numberOf(stepP, "min", &actP->min);
  actP->hasMax    = numberOf(stepP, "max", &actP->max);

  if (until != NULL)
  {
    actP->until = untilOf(allocP, ruleP, until, why, whyLen);
    if (actP->until < 0)
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// condGroup - the members of a group (a JSON list, or the list of "all" / "any") into condP->kidV
//
static bool condNode(CorAlloc* allocP, CorNode* cP, CepCondition* condP, int depth, char* why, int whyLen);

static bool condGroup(CorAlloc* allocP, CorNode* listP, CepCondition* condP, int depth, char* why, int whyLen)
{
  condP->kids = 0;
  condP->kidV = (CepCondition*) corAlloc(allocP, count(listP) * sizeof(CepCondition));

  for (CorNode* kP = listP->value.head; kP != NULL; kP = kP->next)
  {
    if (condNode(allocP, kP, &condP->kidV[condP->kids], depth + 1, why, whyLen) == false)
      return false;
    condP->kids++;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// condNode - one condition: a leaf { "entity" | "type", "q" }, a group { "all": [...] } / { "any": [...] }, or a
// list (all of it) - groups nest
//
static bool condNode(CorAlloc* allocP, CorNode* cP, CepCondition* condP, int depth, char* why, int whyLen)
{
  memset(condP, 0, sizeof(*condP));

  if (depth > 16)
  {
    snprintf(why, whyLen, "'if' - groups nested more than 16 deep");
    return false;
  }

  if (cP->type == CorArray)
  {
    if (cP->value.head == NULL)
    {
      snprintf(why, whyLen, "'if' - an empty list inside a condition");
      return false;
    }
    condP->kind = CondAll;
    return condGroup(allocP, cP, condP, depth, why, whyLen);
  }

  if (cP->type != CorObject)
  {
    snprintf(why, whyLen, "'if' - { \"entity\" or \"type\", \"q\" }, { \"all\": [...] }, { \"any\": [...] }, or a list of them");
    return false;
  }

  CorNode* allP = corTreeLookup(cP, "all");
  CorNode* anyP = corTreeLookup(cP, "any");
  char*    entity = str(cP, "entity");
  char*    type   = str(cP, "type");
  char*    q      = str(cP, "q");

  if ((allP != NULL) || (anyP != NULL))
  {
    const char* name = (allP != NULL) ? "all" : "any";
    CorNode*    lP   = (allP != NULL) ? allP : anyP;

    if ((allP != NULL) && (anyP != NULL))
    {
      snprintf(why, whyLen, "'if' - \"all\" and \"any\" in one condition: one of them, the other a condition inside it");
      return false;
    }
    if ((entity != NULL) || (type != NULL) || (q != NULL) || (count(cP) != 1))
    {
      snprintf(why, whyLen, "'if' - \"%s\" with other members: a group holds its conditions only", name);
      return false;
    }
    if ((lP->type != CorArray) || (lP->value.head == NULL))
    {
      snprintf(why, whyLen, "'if' - \"%s\" is a list of one condition at least", name);
      return false;
    }
    condP->kind = (allP != NULL) ? CondAll : CondAny;
    return condGroup(allocP, lP, condP, depth, why, whyLen);
  }

  if (((entity == NULL) && (type == NULL)) || (q == NULL))
  {
    snprintf(why, whyLen, "'if' - { \"entity\" or \"type\", \"q\" }, { \"all\": [...] }, { \"any\": [...] }, or a list of them");
    return false;
  }
  if (type == NULL)
    type = idType(allocP, entity);
  if (type == NULL)
  {
    snprintf(why, whyLen, "'if' names an entity whose id holds no type (urn:ngsi-ld:<type>:...) - give 'type' too");
    return false;
  }

  condP->kind   = CondLeaf;
  condP->entity = (entity != NULL) ? corAllocStrdup(allocP, entity) : NULL;
  condP->type   = corAllocStrdup(allocP, type);
  condP->q      = corAllocStrdup(allocP, q);

  return true;
}



// -----------------------------------------------------------------------------
//
// condParse - 'if': a condition (condNode), or a list of them - each must hold. A condition is a leaf
// { "entity" | "type", "q" } or a group { "all": [...] } / { "any": [...] }, groups nested in groups
//
static bool condParse(CorAlloc* allocP, CorNode* whenP, CepRule* ruleP, char* why, int whyLen)
{
  CorNode* ifP = corTreeLookup(whenP, "if");

  if (ifP == NULL)
    return true;

  int n = (ifP->type == CorArray) ? count(ifP) : 1;
  ruleP->condV = (CepCondition*) corAlloc(allocP, n * sizeof(CepCondition));

  for (CorNode* cP = (ifP->type == CorArray) ? ifP->value.head : ifP; cP != NULL; cP = (ifP->type == CorArray) ? cP->next : NULL)
  {
    if (condNode(allocP, cP, &ruleP->condV[ruleP->conds], 0, why, whyLen) == false)
      return false;
    ruleP->conds++;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// same - two strings, either NULL
//
static bool same(const char* a, const char* b)
{
  return ((a == NULL) && (b == NULL)) || ((a != NULL) && (b != NULL) && (strcmp(a, b) == 0));
}



// -----------------------------------------------------------------------------
//
// watchOf - the watch an event needs: one subscription per (entity or type, attribute, q)
//
static int watchOf(CepRules* rP, CepEvent* evP)
{
  for (int ix = 0; ix < rP->watches; ix++)
  {
    CepWatch* wP = &rP->watchV[ix];
    if ((strcmp(wP->attribute, evP->attribute) == 0) && same(wP->q, evP->q) &&
        (((wP->entity != NULL) && same(wP->entity, evP->entity)) || ((wP->entity == NULL) && (evP->entity == NULL) && (strcmp(wP->type, evP->type) == 0))))
      return ix;
  }

  if (rP->watches == rP->watchMax)
  {
    int       max = (rP->watchMax == 0) ? 16 : rP->watchMax * 2;
    CepWatch* wV  = (CepWatch*) corAlloc(&rP->alloc, max * sizeof(CepWatch));

    if (rP->watches > 0)
      memcpy(wV, rP->watchV, rP->watches * sizeof(CepWatch));
    rP->watchV   = wV;
    rP->watchMax = max;
  }

  CepWatch* wP  = &rP->watchV[rP->watches];
  wP->entity    = evP->entity;
  wP->type      = evP->type;
  wP->attribute = evP->attribute;
  wP->q         = evP->q;
  wP->sub       = -1;

  return rP->watches++;
}



// -----------------------------------------------------------------------------
//
// whenExtras - what follows from `when` beside the pattern: 'supersedes', the partitioning, the text
//
static bool whenExtras(CorAlloc* allocP, CorNode* whenP, CepRule* ruleP, char* why, int whyLen)
{
  CorNode* supP = corTreeLookup(whenP, "supersedes");

  if (supP != NULL)
  {
    if ((ruleP->kind != CepPatternSeq) && (ruleP->kind != CepPatternAll) && (ruleP->kind != CepPatternRepeat))
    {
      snprintf(why, whyLen, "'supersedes' - with a 'seq', 'all' or 'repeat' pattern only (it waits their 'within')");
      return false;
    }
    if ((supP->type != CorArray) || (supP->value.head == NULL))
    {
      snprintf(why, whyLen, "'supersedes' - a list of Rule ids");
      return false;
    }

    ruleP->supersedeV = (char**) corAlloc(allocP, count(supP) * sizeof(char*));
    for (CorNode* nP = supP->value.head; nP != NULL; nP = nP->next)
    {
      if (nP->type != CorString)
      {
        snprintf(why, whyLen, "'supersedes' - a list of Rule ids");
        return false;
      }
      ruleP->supersedeV[ruleP->supersedes++] = corAllocStrdup(allocP, nP->value.s);
    }
  }

  //
  // Every event a type selector (no entity id): a match per entity - two switches' presses are not one double press
  //
  ruleP->partitioned = (ruleP->kind != CepPatternCron) && (ruleP->kind != CepPatternAt);
  for (int ix = 0; ix < ruleP->events; ix++)
  {
    if (ruleP->eventV[ix].entity != NULL)
      ruleP->partitioned = false;
  }

  CorJson json;
  corJsonCreate(&json, allocP);
  ruleP->whenText = corAlloc(allocP, corJsonRenderSize(&json, whenP) + 1);
  corJsonRender(&json, whenP, ruleP->whenText);

  return true;
}



// -----------------------------------------------------------------------------
//
// rulesParse - Rule entities (keyValues, as the broker gives them) into a generation; the disabled ones skipped,
// the bad ones left out - each told to leftOut (NULL: logged). <now>: a schedule must fire after it. The
// generation's strings are its own; arrayP may go after.
//
static CepRules* rulesParseX(CorNode* arrayP, time_t now, CepLeftOutFn leftOut, void* ctx, bool signedOnly)
{
  CepRules* rP = calloc(1, sizeof(CepRules));
  if (rP == NULL)
    return NULL;

  corAllocBufferInit(&rP->alloc, NULL, 0, 256 * 1024, NULL, "corCep rules");

  //
  // A copy in the generation's own allocator: the rules point into it (names, q, set values)
  //
  CorJson  cj;
  corJsonCreate(&cj, &rP->alloc);
  char*    text = corAlloc(&rP->alloc, corJsonRenderSize(&cj, arrayP) + 1);
  corJsonRender(&cj, arrayP, text);
  CorNode* bodyP = corJsonParse(&cj, text);

  if ((bodyP == NULL) || (bodyP->type != CorArray))
  {
    rulesFree(rP);
    return NULL;
  }

  int n = count(bodyP);
  rP->ruleV  = (CepRule*)  corAlloc(&rP->alloc, (n + 1) * sizeof(CepRule));

  for (CorNode* eP = bodyP->value.head; eP != NULL; eP = eP->next)
  {
    char     why[256] = { 0 };
    CepRule* ruleP    = &rP->ruleV[rP->rules];
    char*    id       = str(eP, "id");
    CorNode* enabledP = corTreeLookup(eP, "enabled");
    CorNode* thenP    = json(eP, "then");

    if (id == NULL)
    {
      if (leftOut != NULL)
        leftOut(NULL, "a rule without an 'id'", ctx);
      continue;
    }

    if ((enabledP != NULL) && (enabledP->type == CorBoolean) && (enabledP->value.b == false))
      continue;

    memset(ruleP, 0, sizeof(CepRule));
    ruleP->id   = corAllocStrdup(&rP->alloc, id);
    ruleP->name = corAllocStrdup(&rP->alloc, (str(eP, "name") != NULL) ? str(eP, "name") : id);

    //
    // --pdp: the author's signature first (author.c) - a rule changed without its editor does not run
    //
    if (signedOnly && (authorVerify(&rP->alloc, str(eP, "author"), json(eP, "when"), json(eP, "then"), &ruleP->author, why, sizeof(why)) == false))
      goto bad;

    if (whenParse(&rP->alloc, json(eP, "when"), ruleP, why, sizeof(why)) == false)
      goto bad;

    if (whenExtras(&rP->alloc, json(eP, "when"), ruleP, why, sizeof(why)) == false)
      goto bad;

    if (condParse(&rP->alloc, json(eP, "when"), ruleP, why, sizeof(why)) == false)
      goto bad;

    if ((ruleP->kind == CepPatternAt) && (ruleP->atKind != CepAtClock) && (locationParse(&rP->alloc, eP, ruleP, why, sizeof(why)) == false))
      goto bad;

    if (((ruleP->kind == CepPatternCron) || (ruleP->kind == CepPatternAt)) && (scheduleNext(ruleP, now) == 0))
    {
      snprintf(why, sizeof(why), "the schedule never fires (within 5 years for 'cron', a year for 'at')");
      goto bad;
    }

    CorNode* firedP = corTreeLookup(eP, "fired");
    if ((firedP != NULL) && (firedP->type == CorInt))
      ruleP->firedBase = firedP->value.i;

    if ((thenP == NULL) || (thenP->type != CorArray) || (thenP->value.head == NULL))
    {
      snprintf(why, sizeof(why), "'then' - a list of actions");
      goto bad;
    }

    ruleP->actionV = (CepAction*) corAlloc(&rP->alloc, count(thenP) * sizeof(CepAction));
    for (CorNode* aP = thenP->value.head; aP != NULL; aP = aP->next)
    {
      memset(&ruleP->actionV[ruleP->actions], 0, sizeof(CepAction));
      CepAction* actP = &ruleP->actionV[ruleP->actions];

      if (actionParse(&rP->alloc, ruleP, aP, actP, why, sizeof(why)) == false)
        goto bad;

      //
      // ${...}: each one a value the rule can give - the target, the q, the values
      //
      if (((actP->entity != NULL) && (varsCheck(ruleP, actP->entity, &actP->vars, why, sizeof(why)) == false)) ||
          ((actP->q      != NULL) && (varsCheck(ruleP, actP->q,      &actP->vars, why, sizeof(why)) == false)) ||
          ((actP->set    != NULL) && (varsCheckTree(ruleP, actP->set, &actP->vars, why, sizeof(why)) == false)))
        goto bad;
      if (actP->vars && (actP->kind == CepActionStep))
      {
        snprintf(why, sizeof(why), "a 'step' with ${...} in its target - not yet");
        goto bad;
      }
      if (actP->vars)
        ruleP->vars = true;
      ++ruleP->actions;
    }

    for (int ix = 0; ix < ruleP->events; ix++)
      ruleP->eventV[ix].watch = watchOf(rP, &ruleP->eventV[ix]);
    for (int ix = 0; ix < ruleP->untils; ix++)
      ruleP->untilV[ix].watch = watchOf(rP, &ruleP->untilV[ix]);
    ++rP->rules;
    continue;

  bad:
    if (leftOut != NULL)
      leftOut(id, why, ctx);
    else
      logLine("rule %s left out: %s", id, why);
  }

  return rP;
}



// -----------------------------------------------------------------------------
//
// rulesParse - the rules of Rule entities, unsigned ones too (/dry-run: drafts)
//
CepRules* rulesParse(CorNode* arrayP, time_t now, CepLeftOutFn leftOut, void* ctx)
{
  return rulesParseX(arrayP, now, leftOut, ctx, false);
}



// -----------------------------------------------------------------------------
//
// rulesLoad - every Rule entity (enabled ones), NULL: the broker did not answer
//
CepRules* rulesLoad(void)
{
  CorAlloc alloc;
  corAllocBufferInit(&alloc, NULL, 0, 256 * 1024, NULL, "corCep rules read");

  CepResponse resp;
  bool got = brokerRequest(&alloc, CorVerbGet, "/entities", "type=Rule&options=keyValues&limit=1000", NULL, &resp);

  if (got && nonexistentTenant(&resp))
  {
    //
    // A tenant with nothing in it yet: no rules. corCep's own Rule subscription makes the tenant (a POST
    // /subscriptions creates it), but a broker may not - zero rules either way, and the first Rule made is notified.
    //
    resp.status = 200;
    resp.body   = corTreeArray(&alloc, NULL);
  }
  else if ((got == false) || (resp.status != 200))
  {
    logLine("the rules: GET /entities?type=Rule - %s", (resp.status != 0) ? ((resp.text != NULL) ? resp.text : "no body") : resp.error);
    corAllocBufferReset(&alloc, false);
    return NULL;
  }

  if ((resp.body == NULL) || (resp.body->type != CorArray))
  {
    logLine("the rules: the broker's answer is not an array");
    corAllocBufferReset(&alloc, false);
    return NULL;
  }

  CepRules* rP = rulesParseX(resp.body, time(NULL), NULL, NULL, authorOn());
  corAllocBufferReset(&alloc, false);

  if (rP != NULL)
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
