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
//   absent  a deadline, moved on by each event; done when it passes - once, until the next event. With 'after':
//           started (again) by the 'after' event, cleared by the absent one - not started at the start
//   window  the values of the last 'for' (numbers - the notified attribute's): sum, avg, count from a ring of
//           them, min and max from a monotonic deque (only the values that can still be the min / max). Done when
//           its bounds START to hold - on an event, or when a value leaves the window; again only after they have
//           stopped holding. An empty window: avg, min, max, sum do not hold, count is 0.
//   cron, at  a wall-clock deadline (schedule.c): fired when it passes, the next one computed from then. corCep not
//           running at the time: that fire is not made up for. The loop sleeps on the monotonic clock - the deadline
//           is converted on every turn, so a clock set (NTP, daylight saving) is followed.
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
#include <stdio.h>                               // snprintf
#include <time.h>                                // clock_gettime

#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeArray, corTreeInteger, ...
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corCep.h"



#define PARTIALS_MAX  32                         // open matches per partition - the oldest dropped beyond, and counted
#define WINDOW_MAX    10000                      // values per window and partition - the oldest dropped beyond, and counted
#define ABSENT_AGAIN  -1                         // an absent deadline that passed while corCep was down: started again by patternsAdopt



// -----------------------------------------------------------------------------
//
// Sample - a value in a window, and when it came
//
typedef struct Sample
{
  long long  at;
  double     v;
} Sample;



// -----------------------------------------------------------------------------
//
// Ring - a window's values, oldest first: a circular buffer, grown to WINDOW_MAX
//
typedef struct Ring
{
  Sample*  v;
  int      cap;
  int      start;
  int      len;
} Ring;



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
  int           timeCap;                         // repeat - timeV's room
  long long     absentAt;                        // absent - the deadline, 0: none
  char*         absentEntity;
  long long     pendingAt;                       // a superseded rule - its fire waiting, 0: none
  char*         pendingEntity;
  Ring          ring;                            // window - the values (min, max: the deque)
  double        sum;                             // window - sum, avg: the values' sum
  bool          holding;                         // window - its bounds held at the last look
  char*         windowEntity;                    // window - the entity of the last value
  time_t        scheduleAt;                      // cron, at - the next fire, wall clock; 0: none
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
  long long      droppedValues;                  // window values dropped, since corCep started
  bool           seen;
} State;



// -----------------------------------------------------------------------------
//
// CepPatterns - the state of one set of rules: the engine's, or a dry run's (its own clock)
//
struct CepPatterns
{
  State*     stateList;
  State**    ruleStateV;                         // the generation's rules' states, by rule index
  int        ruleStates;
  long long  wallAtZeroMs;                       // 0: the wall clock is the host's; else the wall clock is this + 'now'
};



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
// wallMs - the wall clock, ms since the epoch: the host's, or a dry run's ('now' from its start)
//
static long long wallMs(CepPatterns* psP, long long now)
{
  if (psP->wallAtZeroMs != 0)
    return psP->wallAtZeroMs + now;

  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
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
    free(pP->ring.v);
    free(pP->windowEntity);
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
// scheduleSet - a cron / at rule's next fire after <after>, logged (not in a dry run)
//
static void scheduleSet(CepPatterns* psP, CepRule* ruleP, Part* pP, time_t after)
{
  char buf[64];

  pP->scheduleAt = scheduleNext(ruleP, after);
  if (psP->wallAtZeroMs != 0)                    // a dry run: not logged
    return;
  if (pP->scheduleAt != 0)
    logLine("rule %s: next %s", ruleP->id, scheduleText(pP->scheduleAt, buf, sizeof(buf)));
  else
    logLine("rule %s: the schedule fires no more", ruleP->id);
}



// -----------------------------------------------------------------------------
//
// patternsAdopt - a new generation of rules: the state of each rule found by id - kept if its 'when' is the
// same, started afresh if not; the state of rules gone dropped. An 'absent' rule on an entity id starts its
// timer now: no event at all for 'for' is an absence too.
//
void patternsAdopt(CepPatterns* psP, CepRules* rulesP, long long now)
{
  for (State* sP = psP->stateList; sP != NULL; sP = sP->next)
    sP->seen = false;

  free(psP->ruleStateV);
  psP->ruleStates = 0;
  psP->ruleStateV = calloc((rulesP->rules > 0) ? rulesP->rules : 1, sizeof(State*));
  if (psP->ruleStateV == NULL)
    return;
  psP->ruleStates = rulesP->rules;

  for (int ix = 0; ix < rulesP->rules; ix++)
  {
    CepRule* ruleP = &rulesP->ruleV[ix];
    State*   sP;

    for (sP = psP->stateList; sP != NULL; sP = sP->next)
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
      sP->next     = psP->stateList;
      psP->stateList    = sP;
      fresh        = true;
    }
    else if (strcmp(sP->whenText, ruleP->whenText) != 0)
    {
      partsFree(sP);
      setStr(&sP->whenText, ruleP->whenText);
      fresh = true;
    }

    sP->seen        = true;
    psP->ruleStateV[ix]  = sP;

    if ((ruleP->kind == CepPatternCron) || (ruleP->kind == CepPatternAt))
    {
      Part* pP = partOf(sP, "");
      if ((pP != NULL) && (pP->scheduleAt == 0))
        scheduleSet(psP, ruleP, pP, (time_t) (wallMs(psP, now) / 1000));
    }

    if (ruleP->kind == CepPatternAbsent)          // deadlines passed while corCep was down (patternsLoad): again from now
    {
      for (Part* pP = sP->parts; pP != NULL; pP = pP->next)
      {
        if (pP->absentAt == ABSENT_AGAIN)
          pP->absentAt = now + ruleP->forMs;
      }
    }

    if (fresh && (ruleP->kind == CepPatternAbsent) && (ruleP->partitioned == false) && (ruleP->afterEv < 0))   // 'after': its event starts it
    {
      Part* pP = partOf(sP, "");
      if (pP != NULL)
      {
        pP->absentAt = now + ruleP->forMs;
        setStr(&pP->absentEntity, ruleP->eventV[0].entity);
      }
    }
  }

  State** prevPP = &psP->stateList;
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
// patternsNew - an empty state; <wallAtZeroMs> 0: the host's wall clock, else the wall clock at 'now' 0 (a dry run)
//
CepPatterns* patternsNew(long long wallAtZeroMs)
{
  CepPatterns* psP = calloc(1, sizeof(CepPatterns));

  if (psP != NULL)
    psP->wallAtZeroMs = wallAtZeroMs;

  return psP;
}



// -----------------------------------------------------------------------------
//
// patternsFree -
//
void patternsFree(CepPatterns* psP)
{
  if (psP == NULL)
    return;

  while (psP->stateList != NULL)
  {
    State* sP      = psP->stateList;
    psP->stateList = sP->next;
    partsFree(sP);
    free(sP->id);
    free(sP->whenText);
    free(sP);
  }
  free(psP->ruleStateV);
  free(psP);
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
  if (pP->timeCap < ruleP->min)                  // first time - or a state read back (patternsLoad) with less room
  {
    long long* tV = realloc(pP->timeV, ruleP->min * sizeof(long long));
    if (tV == NULL)
      return false;
    pP->timeV   = tV;
    pP->timeCap = ruleP->min;
  }
  if (pP->times > ruleP->min)                    // a state read back of a rule whose 'min' was larger - the newest kept
  {
    memmove(&pP->timeV[0], &pP->timeV[pP->times - ruleP->min], ruleP->min * sizeof(long long));
    pP->times = ruleP->min;
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
// ringAt - the ix:th value, 0: the oldest
//
static Sample* ringAt(Ring* rP, int ix)
{
  return &rP->v[(rP->start + ix) % rP->cap];
}



// -----------------------------------------------------------------------------
//
// ringPush - a value at the end; false: no room (WINDOW_MAX, or no memory) - the caller makes room
//
static bool ringPush(Ring* rP, long long at, double v)
{
  if (rP->len == rP->cap)
  {
    int cap = (rP->cap == 0) ? 16 : rP->cap * 2;

    if (cap > WINDOW_MAX)
      cap = WINDOW_MAX;
    if (cap == rP->cap)
      return false;

    Sample* nV = malloc(cap * sizeof(Sample));
    if (nV == NULL)
      return false;

    for (int ix = 0; ix < rP->len; ix++)
      nV[ix] = *ringAt(rP, ix);

    free(rP->v);
    rP->v     = nV;
    rP->cap   = cap;
    rP->start = 0;
  }

  Sample* sP = &rP->v[(rP->start + rP->len) % rP->cap];
  sP->at = at;
  sP->v  = v;
  ++rP->len;

  return true;
}



// -----------------------------------------------------------------------------
//
// ringPopFront, ringPopBack -
//
static void ringPopFront(Ring* rP)
{
  rP->start = (rP->start + 1) % rP->cap;
  --rP->len;
}

static void ringPopBack(Ring* rP)
{
  --rP->len;
}



// -----------------------------------------------------------------------------
//
// windowExpire - the values of a window older than 'for' leave it
//
static void windowExpire(CepRule* ruleP, Part* pP, long long now)
{
  Ring* rP = &pP->ring;

  while ((rP->len > 0) && (ringAt(rP, 0)->at <= now - ruleP->forMs))
  {
    pP->sum -= ringAt(rP, 0)->v;
    ringPopFront(rP);
  }

  if (rP->len == 0)
    pP->sum = 0;                                 // no drift carried over from values long gone
}



// -----------------------------------------------------------------------------
//
// windowFeed - a value into a window
//
// min / max: the values that can no longer be the min (max) - an older one not below (above) the new one - are
// not kept; the deque's front is the min (max).
//
static void windowFeed(State* sP, CepRule* ruleP, Part* pP, double v, long long now)
{
  Ring* rP = &pP->ring;

  windowExpire(ruleP, pP, now);

  if (ruleP->aggregate == CepAggMin)
  {
    while ((rP->len > 0) && (ringAt(rP, rP->len - 1)->v >= v))
      ringPopBack(rP);
  }
  else if (ruleP->aggregate == CepAggMax)
  {
    while ((rP->len > 0) && (ringAt(rP, rP->len - 1)->v <= v))
      ringPopBack(rP);
  }

  if (ringPush(rP, now, v) == false)
  {
    if (rP->len == 0)
      return;                                    // no memory at all

    pP->sum -= ringAt(rP, 0)->v;
    ringPopFront(rP);
    ++sP->droppedValues;
    ringPush(rP, now, v);
  }
  pP->sum += v;
}



// -----------------------------------------------------------------------------
//
// windowHolds - the aggregate of a window, within all of its bounds
//
static bool windowHolds(CepRule* ruleP, Part* pP)
{
  Ring*  rP = &pP->ring;
  double a;

  if (ruleP->aggregate == CepAggCount)
    a = rP->len;
  else if (rP->len == 0)
    return false;
  else if (ruleP->aggregate == CepAggSum)
    a = pP->sum;
  else if (ruleP->aggregate == CepAggAvg)
    a = pP->sum / rP->len;
  else
    a = ringAt(rP, 0)->v;                        // min, max: the deque's front

  if ((ruleP->bounds & (1u << CepBoundGt)) && !(a >  ruleP->boundV[CepBoundGt])) return false;
  if ((ruleP->bounds & (1u << CepBoundGe)) && !(a >= ruleP->boundV[CepBoundGe])) return false;
  if ((ruleP->bounds & (1u << CepBoundLt)) && !(a <  ruleP->boundV[CepBoundLt])) return false;
  if ((ruleP->bounds & (1u << CepBoundLe)) && !(a <= ruleP->boundV[CepBoundLe])) return false;

  return true;
}



// -----------------------------------------------------------------------------
//
// windowCheck - true: the bounds START to hold now (they did not at the last look)
//
static bool windowCheck(CepRule* ruleP, Part* pP)
{
  bool holds = windowHolds(ruleP, pP);
  bool start = holds && (pP->holding == false);

  pP->holding = holds;
  return start;
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
void patternsEvent(CepPatterns* psP, CepRules* rulesP, int watch, const char* entity, const double* valueP, long long now, CepFireFn fire, void* ctx)
{
  bool* doneV = calloc((rulesP->rules > 0) ? rulesP->rules : 1, sizeof(bool));

  if ((doneV == NULL) || (psP->ruleStates != rulesP->rules))
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
    State*   sP    = psP->ruleStateV[ix];
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
      if ((ruleP->afterEv >= 0) && (ev != ruleP->afterEv))
        pP->absentAt = 0;                        // the absent event came: no absence - cleared, not moved on
      else
      {
        pP->absentAt = now + ruleP->forMs;
        setStr(&pP->absentEntity, entity);
      }
      break;
    case CepPatternWindow:
      if (valueP == NULL)
      {
        if (psP->wallAtZeroMs == 0)                // a dry run: not logged
          logLine("rule %s: %s's '%s' is not a number - not in the window", ruleP->id, entity, ruleP->eventV[ev].attribute);
        break;
      }
      windowFeed(sP, ruleP, pP, *valueP, now);
      setStr(&pP->windowEntity, entity);
      done = windowCheck(ruleP, pP);
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
      if ((bx == -1) || (psP->ruleStateV[bx] == NULL))
        continue;

      Part* bP = partOf(psP->ruleStateV[bx], rulesP->ruleV[bx].partitioned ? entity : "");
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
    if ((within == 0) || (psP->ruleStateV[ix] == NULL))
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

    Part* pP = partOf(psP->ruleStateV[ix], ruleP->partitioned ? entity : "");
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
long long patternsTimers(CepPatterns* psP, CepRules* rulesP, long long now, CepFireFn fire, void* ctx)
{
  long long next = 0;

  if ((rulesP == NULL) || (psP->ruleStates != rulesP->rules))
    return 0;

  for (int ix = 0; ix < rulesP->rules; ix++)
  {
    CepRule* ruleP = &rulesP->ruleV[ix];
    State*   sP    = psP->ruleStateV[ix];

    if (sP == NULL)
      continue;

    for (Part* pP = sP->parts; pP != NULL; pP = pP->next)
    {
      if ((ruleP->kind == CepPatternCron) || (ruleP->kind == CepPatternAt))
      {
        long long wall    = wallMs(psP, now);
        time_t    wallSec = (time_t) (wall / 1000);

        if ((pP->scheduleAt != 0) && ((long long) pP->scheduleAt * 1000 <= wall))
        {
          time_t at = pP->scheduleAt;
          fire(ruleP, NULL, ctx);
          scheduleSet(psP, ruleP, pP, (at > wallSec) ? at : wallSec);
        }

        if (pP->scheduleAt != 0)
        {
          long long at = now + ((long long) pP->scheduleAt * 1000 - wall);
          if ((next == 0) || (at < next))
            next = at;
        }
        continue;
      }

      if (ruleP->kind == CepPatternWindow)        // a value leaving the window may change what holds
      {
        windowExpire(ruleP, pP, now);
        if (windowCheck(ruleP, pP))
          fire(ruleP, pP->windowEntity, ctx);

        long long leaves = (pP->ring.len > 0) ? ringAt(&pP->ring, 0)->at + ruleP->forMs : 0;
        if ((leaves != 0) && ((next == 0) || (leaves < next)))
          next = leaves;
        continue;
      }

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
long long patternsDropped(CepPatterns* psP, const char* ruleId)
{
  for (State* sP = psP->stateList; sP != NULL; sP = sP->next)
  {
    if (strcmp(sP->id, ruleId) == 0)
      return sP->dropped;
  }
  return 0;
}



// -----------------------------------------------------------------------------
//
// patternsDroppedValues - a rule's window values dropped since corCep started (WINDOW_MAX per partition)
//
long long patternsDroppedValues(CepPatterns* psP, const char* ruleId)
{
  for (State* sP = psP->stateList; sP != NULL; sP = sP->next)
  {
    if (strcmp(sP->id, ruleId) == 0)
      return sP->droppedValues;
  }
  return 0;
}



// -----------------------------------------------------------------------------
//
// The state over a restart (--state, doc/design.md § 6) - JSON:
//
//   { "version": 1, "savedAt": <wall ms>,
//     "rules": [ { "id", "when", "dropped", "droppedValues",
//                  "parts": [ { "key", "partials": [ [start, step, "seen hex"], ... ], "times": [ ... ],
//                               "absentAt", "absentEntity", "pendingAt", "pendingEntity",
//                               "ring": [ [at, value], ... ], "sum", "holding", "windowEntity" } ] } ] }
//
// Every time is the WALL clock, ms: the monotonic clock starts again with the host. Not kept: the schedules' next
// fire (made again from the rule), a rule's state whose 'when' changed meanwhile (patternsAdopt starts it afresh).
//
static CorNode* timeNode(CorAlloc* aP, const char* name, long long mono, long long monoToWall)
{
  return corTreeInteger(aP, name, mono + monoToWall);
}

CorNode* patternsSave(CepPatterns* psP, CorAlloc* aP, long long nowMono, long long nowWall)
{
  long long  toWall  = nowWall - nowMono;
  CorNode*   topP    = corTreeObject(aP, NULL);
  CorNode*   rulesP  = corTreeArray(aP, "rules");

  corTreeChildAdd(topP, corTreeInteger(aP, "version", 1));
  corTreeChildAdd(topP, corTreeInteger(aP, "savedAt", nowWall));

  for (State* sP = psP->stateList; sP != NULL; sP = sP->next)
  {
    CorNode* rP     = corTreeObject(aP, NULL);
    CorNode* partsP = corTreeArray(aP, "parts");

    corTreeChildAdd(rP, corTreeString(aP, "id", sP->id));
    corTreeChildAdd(rP, corTreeString(aP, "when", sP->whenText));
    corTreeChildAdd(rP, corTreeInteger(aP, "dropped", sP->dropped));
    corTreeChildAdd(rP, corTreeInteger(aP, "droppedValues", sP->droppedValues));

    for (Part* pP = sP->parts; pP != NULL; pP = pP->next)
    {
      CorNode* oP = corTreeObject(aP, NULL);
      corTreeChildAdd(oP, corTreeString(aP, "key", pP->key));

      if (pP->partials > 0)
      {
        CorNode* aV = corTreeArray(aP, "partials");
        for (int ix = 0; ix < pP->partials; ix++)
        {
          CorNode* mP = corTreeArray(aP, NULL);
          char     hex[20];

          snprintf(hex, sizeof(hex), "%llx", pP->partialV[ix].seen);
          corTreeChildAdd(mP, timeNode(aP, NULL, pP->partialV[ix].start, toWall));
          corTreeChildAdd(mP, corTreeInteger(aP, NULL, pP->partialV[ix].step));
          corTreeChildAdd(mP, corTreeString(aP, NULL, corAllocStrdup(aP, hex)));
          corTreeChildAdd(aV, mP);
        }
        corTreeChildAdd(oP, aV);
      }

      if (pP->times > 0)
      {
        CorNode* tV = corTreeArray(aP, "times");
        for (int ix = 0; ix < pP->times; ix++)
          corTreeChildAdd(tV, timeNode(aP, NULL, pP->timeV[ix], toWall));
        corTreeChildAdd(oP, tV);
      }

      if (pP->absentAt > 0)
        corTreeChildAdd(oP, timeNode(aP, "absentAt", pP->absentAt, toWall));
      if (pP->absentEntity != NULL)
        corTreeChildAdd(oP, corTreeString(aP, "absentEntity", pP->absentEntity));
      if (pP->pendingAt > 0)
        corTreeChildAdd(oP, timeNode(aP, "pendingAt", pP->pendingAt, toWall));
      if (pP->pendingEntity != NULL)
        corTreeChildAdd(oP, corTreeString(aP, "pendingEntity", pP->pendingEntity));

      if (pP->ring.len > 0)
      {
        CorNode* vV = corTreeArray(aP, "ring");
        for (int ix = 0; ix < pP->ring.len; ix++)
        {
          CorNode* sampleP = corTreeArray(aP, NULL);
          corTreeChildAdd(sampleP, timeNode(aP, NULL, ringAt(&pP->ring, ix)->at, toWall));
          corTreeChildAdd(sampleP, corTreeFloat(aP, NULL, ringAt(&pP->ring, ix)->v));
          corTreeChildAdd(vV, sampleP);
        }
        corTreeChildAdd(oP, vV);
        corTreeChildAdd(oP, corTreeFloat(aP, "sum", pP->sum));
      }
      if (pP->holding)
        corTreeChildAdd(oP, corTreeBoolean(aP, "holding", true));
      if (pP->windowEntity != NULL)
        corTreeChildAdd(oP, corTreeString(aP, "windowEntity", pP->windowEntity));

      corTreeChildAdd(partsP, oP);
    }

    corTreeChildAdd(rP, partsP);
    corTreeChildAdd(rulesP, rP);
  }

  corTreeChildAdd(topP, rulesP);
  return topP;
}



// -----------------------------------------------------------------------------
//
// numberOf, longOf, strOf - a saved member; the default when it is absent or of another type
//
static double numberOf(CorNode* nP)
{
  if (nP == NULL)
    return 0;
  if (nP->type == CorInt)
    return (double) nP->value.i;
  if (nP->type == CorFloat)
    return nP->value.f;
  return 0;
}

static long long longOf(CorNode* objP, const char* name)
{
  CorNode* nP = corTreeLookup(objP, name);
  return ((nP != NULL) && (nP->type == CorInt)) ? nP->value.i : 0;
}

static const char* strOf(CorNode* objP, const char* name)
{
  CorNode* nP = corTreeLookup(objP, name);
  return ((nP != NULL) && (nP->type == CorString)) ? nP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// patternsLoad - a saved state, before the first patternsAdopt: each rule's state found again by its id (and kept
// if its 'when' is the same). A deadline passed while corCep was down: an absent one starts again from now
// (patternsAdopt), a superseded fire waiting is dropped - fired minutes late it would be worse than not fired.
// The number of rules' states read; -1: not a state file of this version.
//
int patternsLoad(CepPatterns* psP, CorNode* topP, long long nowMono, long long nowWall)
{
  long long toMono = nowMono - nowWall;
  CorNode*  rulesP = corTreeLookup(topP, "rules");
  int       n      = 0;

  if ((topP == NULL) || (topP->type != CorObject) || (longOf(topP, "version") != 1) || (rulesP == NULL) || (rulesP->type != CorArray))
    return -1;

  for (CorNode* rP = rulesP->value.head; rP != NULL; rP = rP->next)
  {
    const char* id   = strOf(rP, "id");
    const char* when = strOf(rP, "when");
    CorNode*    pV   = corTreeLookup(rP, "parts");

    if ((id == NULL) || (when == NULL) || (pV == NULL) || (pV->type != CorArray))
      continue;

    State* sP = calloc(1, sizeof(State));
    if (sP == NULL)
      break;
    sP->id            = strdup(id);
    sP->whenText      = strdup(when);
    sP->dropped       = longOf(rP, "dropped");
    sP->droppedValues = longOf(rP, "droppedValues");

    for (CorNode* oP = pV->value.head; oP != NULL; oP = oP->next)
    {
      const char* key = strOf(oP, "key");
      Part*       pP  = (key != NULL) ? partOf(sP, key) : NULL;

      if (pP == NULL)
        continue;

      CorNode* aV = corTreeLookup(oP, "partials");
      for (CorNode* mP = (aV != NULL) && (aV->type == CorArray) ? aV->value.head : NULL; (mP != NULL) && (pP->partials < PARTIALS_MAX); mP = mP->next)
      {
        CorNode* startP = (mP->type == CorArray) ? mP->value.head : NULL;
        CorNode* stepP  = (startP != NULL) ? startP->next : NULL;
        CorNode* seenP  = (stepP  != NULL) ? stepP->next  : NULL;

        if ((seenP == NULL) || (startP->type != CorInt) || (stepP->type != CorInt) || (seenP->type != CorString))
          continue;
        pP->partialV[pP->partials].start = startP->value.i + toMono;
        pP->partialV[pP->partials].step  = (int) stepP->value.i;
        pP->partialV[pP->partials].seen  = strtoull(seenP->value.s, NULL, 16);
        ++pP->partials;
      }

      CorNode* tV = corTreeLookup(oP, "times");
      if ((tV != NULL) && (tV->type == CorArray) && (tV->value.head != NULL))
      {
        int times = 0;
        for (CorNode* tP = tV->value.head; tP != NULL; tP = tP->next)
          ++times;
        pP->timeV = malloc(times * sizeof(long long));   // feedRepeat makes room for 'min' when it needs it
        if (pP->timeV != NULL)
          pP->timeCap = times;
        for (CorNode* tP = tV->value.head; (tP != NULL) && (pP->timeV != NULL); tP = tP->next)
          pP->timeV[pP->times++] = (long long) numberOf(tP) + toMono;
      }

      long long absentAt  = longOf(oP, "absentAt");
      long long pendingAt = longOf(oP, "pendingAt");

      if (absentAt != 0)
        pP->absentAt = (absentAt > nowWall) ? absentAt + toMono : ABSENT_AGAIN;
      if ((pendingAt != 0) && (pendingAt > nowWall))
        pP->pendingAt = pendingAt + toMono;

      if (strOf(oP, "absentEntity") != NULL)
        setStr(&pP->absentEntity, strOf(oP, "absentEntity"));
      if ((pP->pendingAt != 0) && (strOf(oP, "pendingEntity") != NULL))
        setStr(&pP->pendingEntity, strOf(oP, "pendingEntity"));
      if (strOf(oP, "windowEntity") != NULL)
        setStr(&pP->windowEntity, strOf(oP, "windowEntity"));

      CorNode* vV = corTreeLookup(oP, "ring");
      for (CorNode* sampleP = (vV != NULL) && (vV->type == CorArray) ? vV->value.head : NULL; sampleP != NULL; sampleP = sampleP->next)
      {
        CorNode* atP = (sampleP->type == CorArray) ? sampleP->value.head : NULL;
        CorNode* vP  = (atP != NULL) ? atP->next : NULL;

        if ((vP == NULL) || (atP->type != CorInt) || (ringPush(&pP->ring, atP->value.i + toMono, numberOf(vP)) == false))
          break;
      }
      pP->sum     = numberOf(corTreeLookup(oP, "sum"));
      CorNode* hP = corTreeLookup(oP, "holding");
      pP->holding = (hP != NULL) && (hP->type == CorBoolean) && hP->value.b;
    }

    sP->next        = psP->stateList;
    psP->stateList  = sP;
    ++n;
  }

  return n;
}
