//
// FILE            pattern.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The patterns over time (doc/design.md § 3.1, § 5): each rule's open matches and timers. The engine's thread only.
//
//   seq     a partial match per first step seen, advanced step by step; done when the last step comes within
//           'within' of the first
//   all     the same, the steps in any order
//   repeat  the times of the last events within 'within'; done at 'min' of them
//   absent  a deadline, moved on by each event; done when it passes - once, until the next event
//
// Semantics, as SQL:2016 row pattern recognition's defaults:
//   - an event goes to the OLDEST partial match that can take it; one that took it starts no new match
//   - a match done clears the open matches of its partition (AFTER MATCH SKIP PAST LAST ROW): the events it used
//     are not used again
//   - a rule whose events are all type selectors keeps its matches per ENTITY (PARTITION BY the entity id): two
//     switches' presses are not one double press. A rule naming entities by id has one partition.
//   - the time of an event is when its notification reaches corCep
//
// 'supersedes': a rule (seq / all / repeat) superseding a single-event rule makes that rule's fire WAIT the
// superseding rule's 'within'; the superseding rule matching in that time cancels it, and the event it matched
// on does not start a new wait. A double press then fires the double-press rule only, and a single press fires
// 'within' late.
//
// A rule's state survives a reload of the rules if its 'when' is the same; a changed 'when' starts afresh.
//
#include <stdlib.h>                              // calloc, free, malloc
#include <string.h>                              // strcmp, strdup, memmove
#include <time.h>                                // clock_gettime

#include "corCep.h"



#define PARTIALS_MAX  32                         // open matches per partition - the oldest dropped beyond, and counted



// -----------------------------------------------------------------------------
//
// Partial - an open match of a seq or all
//
typedef struct Partial
{
  long long           start;                     // its first event's time
  int                 step;                      // seq: the steps matched
  unsigned long long  seen;                      // all: the steps matched, a bit each
} Partial;



// -----------------------------------------------------------------------------
//
// Part - a rule's state in one partition (the entity id; "" - the rule is not partitioned)
//
typedef struct Part
{
  struct Part*  next;
  char*         key;
  Partial       partialV[PARTIALS_MAX];          // seq, all - oldest first
  int           partials;
  long long*    timeV;                           // repeat - the times of the last events, oldest first ('min' of them at most)
  int           times;
  long long     absentAt;                        // absent - the deadline, 0: none
  char*         absentEntity;
  long long     pendingAt;                       // a superseded rule - its fire waiting, 0: none
  char*         pendingEntity;
} Part;



// -----------------------------------------------------------------------------
//
// State - a rule's, by its id
//
typedef struct State
{
  struct State*  next;
  char*          id;
  char*          whenText;
  Part*          parts;
  long long      dropped;                        // partial matches dropped, since corCep started
  bool           seen;
} State;

static State*   stateList  = NULL;
static State**  ruleStateV = NULL;               // the generation's rules' states, by rule index
static int      ruleStates = 0;



// -----------------------------------------------------------------------------
//
// nowMs -
//
long long nowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// partsFree -
//
static void partsFree(State* sP)
{
  while (sP->parts != NULL)
  {
    Part* pP  = sP->parts;
    sP->parts = pP->next;
    free(pP->key);
    free(pP->timeV);
    free(pP->absentEntity);
    free(pP->pendingEntity);
    free(pP);
  }
}



// -----------------------------------------------------------------------------
//
// partOf - a rule's state in a partition, made the first time
//
static Part* partOf(State* sP, const char* key)
{
  for (Part* pP = sP->parts; pP != NULL; pP = pP->next)
  {
    if (strcmp(pP->key, key) == 0)
      return pP;
  }

  Part* pP = calloc(1, sizeof(Part));
  if (pP == NULL)
    return NULL;

  pP->key   = strdup(key);
  pP->next  = sP->parts;
  sP->parts = pP;

  return pP;
}



// -----------------------------------------------------------------------------
//
// setStr - a malloc'd string replaced
//
static void setStr(char** pP, const char* s)
{
  free(*pP);
  *pP = (s != NULL) ? strdup(s) : NULL;
}



// -----------------------------------------------------------------------------
//
// patternsAdopt - a new generation of rules: the state of each rule found by id - kept if its 'when' is the
// same, started afresh if not; the state of rules gone dropped. An 'absent' rule on an entity id starts its
// timer now: no event at all for 'for' is an absence too.
//
void patternsAdopt(CepRules* rulesP, long long now)
{
  for (State* sP = stateList; sP != NULL; sP = sP->next)
    sP->seen = false;

  free(ruleStateV);
  ruleStates = 0;
  ruleStateV = calloc((rulesP->rules > 0) ? rulesP->rules : 1, sizeof(State*));
  if (ruleStateV == NULL)
    return;
  ruleStates = rulesP->rules;

  for (int ix = 0; ix < rulesP->rules; ix++)
  {
    CepRule* ruleP = &rulesP->ruleV[ix];
    State*   sP;

    for (sP = stateList; sP != NULL; sP = sP->next)
    {
      if (strcmp(sP->id, ruleP->id) == 0)
        break;
    }

    bool fresh = false;

    if (sP == NULL)
    {
      sP = calloc(1, sizeof(State));
      if (sP == NULL)
        continue;
      sP->id       = strdup(ruleP->id);
      sP->whenText = strdup(ruleP->whenText);
      sP->next     = stateList;
      stateList    = sP;
      fresh        = true;
    }
    else if (strcmp(sP->whenText, ruleP->whenText) != 0)
    {
      partsFree(sP);
      setStr(&sP->whenText, ruleP->whenText);
      fresh = true;
    }

    sP->seen        = true;
    ruleStateV[ix]  = sP;

    if (fresh && (ruleP->kind == CepPatternAbsent) && (ruleP->partitioned == false))
    {
      Part* pP = partOf(sP, "");
      if (pP != NULL)
      {
        pP->absentAt = now + ruleP->forMs;
        setStr(&pP->absentEntity, ruleP->eventV[0].entity);
      }
    }
  }

  State** prevPP = &stateList;
  while (*prevPP != NULL)
  {
    State* sP = *prevPP;
    if (sP->seen == false)
    {
      *prevPP = sP->next;
      partsFree(sP);
      free(sP->id);
      free(sP->whenText);
      free(sP);
    }
    else
      prevPP = &sP->next;
  }
}



// -----------------------------------------------------------------------------
//
// partialAdd - a new open match; the oldest dropped (and counted) if the partition holds the most already
//
static void partialAdd(State* sP, Part* pP, long long now, int step, unsigned long long seen)
{
  if (pP->partials == PARTIALS_MAX)
  {
    memmove(&pP->partialV[0], &pP->partialV[1], (PARTIALS_MAX - 1) * sizeof(Partial));
    --pP->partials;
    ++sP->dropped;
  }

  pP->partialV[pP->partials].start = now;
  pP->partialV[pP->partials].step  = step;
  pP->partialV[pP->partials].seen  = seen;
  ++pP->partials;
}



// -----------------------------------------------------------------------------
//
// expire - the open matches whose first event is more than 'within' ago
//
static void expire(CepRule* ruleP, Part* pP, long long now)
{
  int kept = 0;

  for (int ix = 0; ix < pP->partials; ix++)
  {
    if (now - pP->partialV[ix].start <= ruleP->withinMs)
      pP->partialV[kept++] = pP->partialV[ix];
  }
  pP->partials = kept;
}



// -----------------------------------------------------------------------------
//
// feedSeq - event <ev> to a seq; true: a match done
//
static bool feedSeq(State* sP, CepRule* ruleP, Part* pP, int ev, long long now)
{
  expire(ruleP, pP, now);

  for (int ix = 0; ix < pP->partials; ix++)
  {
    Partial* mP = &pP->partialV[ix];

    if (ruleP->stepV[mP->step] != ev)
      continue;

    if (++mP->step == ruleP->steps)
    {
      pP->partials = 0;                          // AFTER MATCH SKIP PAST LAST ROW
      return true;
    }
    return false;                                // taken - starts no new match
  }

  if (ruleP->stepV[0] == ev)
    partialAdd(sP, pP, now, 1, 0);

  return false;
}



// -----------------------------------------------------------------------------
//
// feedAll - event <ev> to an all; true: a match done
//
static bool feedAll(State* sP, CepRule* ruleP, Part* pP, int ev, long long now)
{
  unsigned long long full = (ruleP->steps >= 64) ? ~0ULL : ((1ULL << ruleP->steps) - 1);
  int                step = -1;

  for (int ix = 0; ix < ruleP->steps; ix++)
  {
    if (ruleP->stepV[ix] == ev)
      step = ix;
  }
  if (step == -1)
    return false;

  expire(ruleP, pP, now);

  for (int ix = 0; ix < pP->partials; ix++)
  {
    Partial* mP = &pP->partialV[ix];

    if ((mP->seen & (1ULL << step)) != 0)
      continue;

    mP->seen |= (1ULL << step);
    if (mP->seen == full)
    {
      pP->partials = 0;
      return true;
    }
    return false;
  }

  partialAdd(sP, pP, now, 0, 1ULL << step);
  return false;
}



// -----------------------------------------------------------------------------
//
// feedRepeat - an event to a repeat; true: 'min' of them within 'within'
//
static bool feedRepeat(CepRule* ruleP, Part* pP, long long now)
{
  if (pP->timeV == NULL)
  {
    pP->timeV = malloc(ruleP->min * sizeof(long long));
    if (pP->timeV == NULL)
      return false;
  }

  int kept = 0;
  for (int ix = 0; ix < pP->times; ix++)
  {
    if (now - pP->timeV[ix] < ruleP->withinMs)
      pP->timeV[kept++] = pP->timeV[ix];
  }
  pP->times = kept;

  if (pP->times == ruleP->min)                   // cannot happen - a full list is a match - but never write past it
  {
    memmove(&pP->timeV[0], &pP->timeV[1], (ruleP->min - 1) * sizeof(long long));
    --pP->times;
  }
  pP->timeV[pP->times++] = now;

  if (pP->times == ruleP->min)
  {
    pP->times = 0;
    return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// ruleIndex - a rule's index in the generation, by id; -1: not in it
//
static int ruleIndex(CepRules* rulesP, const char* id)
{
  for (int ix = 0; ix < rulesP->rules; ix++)
  {
    if (strcmp(rulesP->ruleV[ix].id, id) == 0)
      return ix;
  }
  return -1;
}



// -----------------------------------------------------------------------------
//
// supersededWithin - the longest 'within' of the rules superseding rule <ix>; 0: none supersedes it
//
static long long supersededWithin(CepRules* rulesP, int ix)
{
  long long within = 0;

  for (int sx = 0; sx < rulesP->rules; sx++)
  {
    CepRule* sP = &rulesP->ruleV[sx];

    for (int jx = 0; jx < sP->supersedes; jx++)
    {
      if ((strcmp(sP->supersedeV[jx], rulesP->ruleV[ix].id) == 0) && (sP->withinMs > within))
        within = sP->withinMs;
    }
  }

  return within;
}



// -----------------------------------------------------------------------------
//
// patternsEvent - an event: a notified entity on watch <watch>. Each rule with an event on the watch is fed;
// the rules done fire. The patterns first, so that a match done can cancel what it supersedes before the
// single-event rules are looked at.
//
void patternsEvent(CepRules* rulesP, int watch, const char* entity, long long now, CepFireFn fire, void* ctx)
{
  bool* doneV = calloc((rulesP->rules > 0) ? rulesP->rules : 1, sizeof(bool));

  if ((doneV == NULL) || (ruleStates != rulesP->rules))
  {
    free(doneV);
    return;
  }

  //
  // 1. The patterns over time
  //
  for (int ix = 0; ix < rulesP->rules; ix++)
  {
    CepRule* ruleP = &rulesP->ruleV[ix];
    State*   sP    = ruleStateV[ix];
    int      ev    = -1;

    if ((ruleP->kind == CepPatternAny) || (sP == NULL))
      continue;

    for (int ex = 0; ex < ruleP->events; ex++)
    {
      if (ruleP->eventV[ex].watch == watch)
        ev = ex;
    }
    if (ev == -1)
      continue;

    Part* pP = partOf(sP, ruleP->partitioned ? entity : "");
    if (pP == NULL)
      continue;

    bool done = false;

    switch (ruleP->kind)
    {
    case CepPatternSeq:     done = feedSeq(sP, ruleP, pP, ev, now); break;
    case CepPatternAll:     done = feedAll(sP, ruleP, pP, ev, now); break;
    case CepPatternRepeat:  done = feedRepeat(ruleP, pP, now);       break;
    case CepPatternAbsent:
      pP->absentAt = now + ruleP->forMs;
      setStr(&pP->absentEntity, entity);
      break;
    default:
      break;
    }

    if (done == false)
      continue;

    doneV[ix] = true;

    //
    // What it supersedes: their waiting fires cancelled
    //
    for (int jx = 0; jx < ruleP->supersedes; jx++)
    {
      int bx = ruleIndex(rulesP, ruleP->supersedeV[jx]);
      if ((bx == -1) || (ruleStateV[bx] == NULL))
        continue;

      Part* bP = partOf(ruleStateV[bx], rulesP->ruleV[bx].partitioned ? entity : "");
      if (bP != NULL)
        bP->pendingAt = 0;
    }

    fire(ruleP, entity, ctx);
  }

  //
  // 2. The single-event rules: fired - or, if superseded, waiting
  //
  for (int ix = 0; ix < rulesP->rules; ix++)
  {
    CepRule* ruleP = &rulesP->ruleV[ix];
    bool     hit   = false;

    if (ruleP->kind != CepPatternAny)
      continue;

    for (int ex = 0; ex < ruleP->events; ex++)
    {
      if (ruleP->eventV[ex].watch == watch)
        hit = true;
    }
    if (hit == false)
      continue;

    long long within = supersededWithin(rulesP, ix);
    if ((within == 0) || (ruleStateV[ix] == NULL))
    {
      fire(ruleP, entity, ctx);
      continue;
    }

    //
    // A superseding rule matched on THIS event: it took it
    //
    bool taken = false;
    for (int sx = 0; sx < rulesP->rules; sx++)
    {
      if (doneV[sx] == false)
        continue;
      for (int jx = 0; jx < rulesP->ruleV[sx].supersedes; jx++)
      {
        if (strcmp(rulesP->ruleV[sx].supersedeV[jx], ruleP->id) == 0)
          taken = true;
      }
    }
    if (taken)
      continue;

    Part* pP = partOf(ruleStateV[ix], ruleP->partitioned ? entity : "");
    if (pP == NULL)
      continue;

    if (pP->pendingAt != 0)                      // one waiting already, its superseding rule not done: it fires now
      fire(ruleP, pP->pendingEntity, ctx);

    pP->pendingAt = now + within;
    setStr(&pP->pendingEntity, entity);
  }

  free(doneV);
}



// -----------------------------------------------------------------------------
//
// patternsTimers - the deadlines passed: absences fire, waiting fires fire; returns the next deadline, 0: none
//
long long patternsTimers(CepRules* rulesP, long long now, CepFireFn fire, void* ctx)
{
  long long next = 0;

  if ((rulesP == NULL) || (ruleStates != rulesP->rules))
    return 0;

  for (int ix = 0; ix < rulesP->rules; ix++)
  {
    CepRule* ruleP = &rulesP->ruleV[ix];
    State*   sP    = ruleStateV[ix];

    if (sP == NULL)
      continue;

    for (Part* pP = sP->parts; pP != NULL; pP = pP->next)
    {
      if ((pP->absentAt != 0) && (pP->absentAt <= now))
      {
        pP->absentAt = 0;                        // once - the next event starts the next absence
        fire(ruleP, pP->absentEntity, ctx);
      }
      if ((pP->pendingAt != 0) && (pP->pendingAt <= now))
      {
        pP->pendingAt = 0;
        fire(ruleP, pP->pendingEntity, ctx);
      }

      if ((pP->absentAt != 0) && ((next == 0) || (pP->absentAt < next)))
        next = pP->absentAt;
      if ((pP->pendingAt != 0) && ((next == 0) || (pP->pendingAt < next)))
        next = pP->pendingAt;
    }
  }

  return next;
}



// -----------------------------------------------------------------------------
//
// patternsDropped - a rule's partial matches dropped since corCep started (the bound per partition)
//
long long patternsDropped(const char* ruleId)
{
  for (State* sP = stateList; sP != NULL; sP = sP->next)
  {
    if (strcmp(sP->id, ruleId) == 0)
      return sP->dropped;
  }
  return 0;
}
