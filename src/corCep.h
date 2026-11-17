//
// FILE            corCep.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corCep - an NGSI-LD complex event processor (doc/design.md)
//
#ifndef CORCEP_CORCEP_H_
#define CORCEP_CORCEP_H_

#include <stdbool.h>                             // bool
#include <time.h>                                // time_t

#include "corAlloc/CorAlloc.h"                   // CorAlloc
#include "corTree/CorNode.h"                     // CorNode
#include "corRest/CorRestVerb.h"                 // CorRestVerb

#define CORCEP_VERSION     "0.1.0"
#define CEP_SUB_PREFIX     "urn:ngsi-ld:Subscription:corcep-"
#define CEP_SUB_RULES      "urn:ngsi-ld:Subscription:corcep-rules"



// -----------------------------------------------------------------------------
//
// CepSession - one broker, one tenant, one identity; set at start
//
typedef struct CepSession
{
  const char*  broker;                           // http://host:port, no trailing slash
  const char*  tenant;                           // NULL: the broker's default tenant
  const char*  notifyBase;                       // the URL the broker notifies: <notifyBase>/notify/<secret>/<n>
  char         secret[33];                       // in every notification path - nothing else can send an event
  unsigned short port;                           // the listener's port
} CepSession;

extern CepSession cep;



// -----------------------------------------------------------------------------
//
// CepResponse - a broker's answer
//
typedef struct CepResponse
{
  int       status;                              // 0: no answer (error says why)
  CorNode*  body;                                // the parsed body, NULL: none or not JSON
  char*     text;                                // the body as received, NULL: none
  char      error[256];
} CepResponse;

extern bool  brokerRequest(CorAlloc* allocP, CorRestVerb verb, const char* path, const char* query, CorNode* bodyP, CepResponse* respP);
extern bool  brokerRequestText(CorAlloc* allocP, CorRestVerb verb, const char* path, const char* query, const char* body, CepResponse* respP);
extern char* jsonText(CorAlloc* allocP, CorNode* nodeP);                                    // a tree as JSON text
extern void  brokerFailed(char* why, int whyLen, const char* what, CepResponse* respP);    // engine.c - why a request failed

//
// The actions' PATCHes (sender.c) - a queue per target, a thread, retried for --retry-for
//
extern bool  senderStart(int retryForS);
extern void  senderPatch(const char* ruleId, int action, const char* entity, const char* body);   // action 0: a step
extern int   senderDrain(long long ms);                                                          // at a stop; still waiting
extern char* urlEncode(CorAlloc* allocP, const char* s);
extern bool  nonexistentTenant(CepResponse* respP);



// -----------------------------------------------------------------------------
//
// The rules (doc/design.md § 3) - what P1 runs: one event, a list of set / toggle actions
//
typedef enum CepActionKind
{
  CepActionSet,
  CepActionToggle,
  CepActionStep
} CepActionKind;

typedef struct CepAction
{
  CepActionKind  kind;
  char*          entity;                         // the target: one entity ...
  char*          type;                           // ... or every entity of a type
  char*          q;                              //     that matches q (NULL: all of them)
  CorNode*       set;                            // Set: { attribute: value, ... }
  char*          attribute;                      // Toggle: the boolean attribute; Step: the number
  double         by;                             // Step: added every 'every'
  double         min, max;                       // Step: the bounds - stepping stops at one
  bool           hasMin, hasMax;
  long long      everyMs;                        // Step
  long long      forMs;                          // Step: the time limit
  int            until;                          // Step: the event that stops it, an index into the rule's untilV; -1: none
} CepAction;

typedef enum CepCondKind
{
  CondLeaf,                                      // holds if the broker has an entity that matches
  CondAll,                                       // every one of kidV holds (a JSON list, or { "all": [...] })
  CondAny                                        // one of kidV holds ({ "any": [...] })
} CepCondKind;

typedef struct CepCondition
{
  CepCondKind           kind;
  char*                 entity;                  // Leaf: one entity ...
  char*                 type;                    // ... or any entity of a type
  char*                 q;                       // ... the NGSI-LD q it must match
  struct CepCondition*  kidV;                    // All, Any: the conditions - leaves or groups
  int                   kids;
} CepCondition;

typedef struct CepEvent
{
  char*  name;                                   // the name the pattern uses ('event'; default: the attribute)
  char*  entity;                                 // one entity ...
  char*  type;                                   // ... or (with entity NULL) every entity of a type
  char*  attribute;                              // the attribute watched
  char*  q;                                      // 'where': an NGSI-LD q the broker applies (the subscription's q), NULL: none
  int    watch;                                  // its index in the watch list - its subscription
} CepEvent;

typedef enum CepPatternKind
{
  CepPatternAny,                                 // one event, or any of several: each fires the rule
  CepPatternSeq,                                 // the steps in order, all within 'within'
  CepPatternAll,                                 // every step, in any order, all within 'within'
  CepPatternRepeat,                              // one event 'min' times within 'within'
  CepPatternAbsent,                              // no event for 'for': a timer each event restarts
  CepPatternWindow,                              // an aggregate of the values of the last 'for' - fires when its bounds start to hold
  CepPatternCron,                                // a cron schedule - no events
  CepPatternAt                                   // every day at a clock time, sunrise or sunset - no events
} CepPatternKind;

typedef struct CepCron                           // a cron schedule: a bit per value of each field
{
  unsigned long long  second;                    // 0-59
  unsigned long long  minute;                    // 0-59
  unsigned long long  hour;                      // 0-23
  unsigned long long  mday;                      // 1-31
  unsigned long long  month;                     // 1-12
  unsigned long long  wday;                      // 0-6, Sunday 0
  bool                mdayStar;                  // day of month '*' - then day of week alone decides (and the other way round)
  bool                wdayStar;
} CepCron;

typedef enum CepAtKind
{
  CepAtClock,
  CepAtSunrise,
  CepAtSunset
} CepAtKind;

typedef enum CepWindowAgg
{
  CepAggAvg,
  CepAggMin,
  CepAggMax,
  CepAggSum,
  CepAggCount
} CepWindowAgg;

typedef enum CepBound                            // a window's bounds: indexes of CepRule::boundV, bits of CepRule::bounds
{
  CepBoundGt,
  CepBoundGe,
  CepBoundLt,
  CepBoundLe
} CepBound;

typedef struct CepRule
{
  char*           id;
  char*           name;
  CepEvent*       eventV;                        // the events the pattern uses, each once
  int             events;
  CepPatternKind  kind;
  int*            stepV;                         // Seq, All: the steps, as indexes into eventV ([press, press]: 0, 0)
  int             steps;
  long long       withinMs;                      // Seq, All, Repeat
  long long       forMs;                         // Absent, Window
  CepWindowAgg    aggregate;                     // Window
  double          boundV[4];                     // Window: by CepBound
  unsigned        bounds;                        // Window: the bounds given, a bit each (1 << CepBound)
  CepCron         cron;                          // Cron
  CepAtKind       atKind;                        // At
  int             atSecond;                      // At, clock: the second of the day
  long long       offsetS;                       // At: 'offset', seconds, signed
  double          lat;                           // At, sun: the Rule entity's 'location'
  double          lon;
  int             min;                           // Repeat
  char**          supersedeV;                    // the rules this one supersedes (their ids)
  int             supersedes;
  bool            partitioned;                   // every event a type selector: a match per entity (design § 3.1)
  char*           whenText;                      // 'when' as JSON text - a rule's state survives a reload if it is the same
  CepEvent*       declV;                         // every event 'when' declares - 'until' names one the pattern does not use
  int             decls;
  CepEvent*       untilV;                        // the events that stop a step - watched, never fire the rule
  int             untils;
  CepCondition*   condV;                         // 'if': every one must hold when the pattern matches - each a leaf or a group
  int             conds;
  CepAction*      actionV;
  int             actions;
  long            firedBase;                     // 'fired' as the Rule entity holds it - where the count goes on from
} CepRule;

typedef struct CepWatch                          // one subscription: (entity or type, attribute, q)
{
  char*  entity;
  char*  type;
  char*  attribute;
  char*  q;
  int    sub;                                    // its subscription's number: corcep-<sub>, /notify/<secret>/<sub>
} CepWatch;

typedef struct CepRules                          // a generation of rules - replaced whole when a Rule changes
{
  CorAlloc   alloc;
  CepRule*   ruleV;
  int        rules;
  CepWatch*  watchV;
  int        watches;
  int        watchMax;
} CepRules;

typedef void (*CepLeftOutFn)(const char* id, const char* why, void* ctx);   // a rule left out, and why (id NULL: none)

extern CepRules* rulesLoad(void);                                                            // from the broker
extern CepRules* rulesParse(CorNode* arrayP, time_t now, CepLeftOutFn leftOut, void* ctx);   // from Rule entities (keyValues)
extern bool      durationParse(const char* s, long long* msP);                               // an ISO 8601 duration (PT0.5S, P1D) in ms; false: not one, or zero
extern void      rulesFree(CepRules* rulesP);



// -----------------------------------------------------------------------------
//
// Subscriptions, the listener, the engine
//
extern int  statsInterval;                                    // seconds between two writes of a rule's statistics
extern int  renewInterval;                                    // seconds between two renewals of the subscriptions (0: never)
extern bool subsInit(void);                                   // stale ones removed, the Rule subscription made
extern bool subsApply(CepRules* rulesP);                      // one per watch, the old ones replaced
extern bool subsRenew(void);                                  // all renewed (their owner); true: one was gone - read the rules again
extern bool listenerStart(void);                              // the notification server, its own thread
extern void engineRun(void);                                  // the events, one at a time - never returns
extern void engineQueue(const char* which, const char* body, int bodyLen);   // from the listener thread

//
// The patterns' state (pattern.c) - the engine's thread only
//
typedef void (*CepFireFn)(CepRule* ruleP, const char* entity, void* ctx);

typedef struct CepPatterns CepPatterns;          // the state of one set of rules - the engine's, or a dry run's

extern void         condText(CepCondition* cP, char* buf, int bufLen);        // a condition in words: a leaf its q, a group all(a; b) / any(a | b)
extern long long    nowMs(void);                                               // CLOCK_MONOTONIC, ms
extern CepPatterns* patternsNew(long long wallAtZeroMs);                       // 0: the host's wall clock; else a dry run's, at 'now' 0
extern void         patternsFree(CepPatterns* psP);
extern void         patternsAdopt(CepPatterns* psP, CepRules* rulesP, long long now);   // a new generation: the state of unchanged rules kept
extern void         patternsEvent(CepPatterns* psP, CepRules* rulesP, int watch, const char* entity, const double* valueP, long long now, CepFireFn fire, void* ctx);
extern long long    patternsTimers(CepPatterns* psP, CepRules* rulesP, long long now, CepFireFn fire, void* ctx);   // the expired ones run; the next deadline, 0: none
extern long long    patternsDropped(CepPatterns* psP, const char* ruleId);     // partial matches dropped (the bound), since start
extern long long    patternsDroppedValues(CepPatterns* psP, const char* ruleId);   // window values dropped (the bound), since start
extern CorNode*     patternsSave(CepPatterns* psP, CorAlloc* aP, long long nowMono, long long nowWall);   // the state, for --state
extern int          patternsLoad(CepPatterns* psP, CorNode* topP, long long nowMono, long long nowWall);  // before the first adopt; rules read, -1: not a state

//
// --state (state.c) - the patterns' state over a restart; cepStopping (corCep.c) - a SIGTERM / SIGINT came
//
#include <signal.h>                              // sig_atomic_t
extern const char*            stateFile;                                          // NULL: not kept
extern volatile sig_atomic_t  cepStopping;                                        // the signal, 0: none
extern void                   stateLoad(CepPatterns* psP);
extern CorNode*               steppersSave(CorAlloc* aP, long long nowMono, long long nowWall);   // engine.c - the steps running
extern int                    steppersLoad(CorNode* topP, long long nowMono, long long nowWall);  // engine.c - before the first rules
extern bool                   stateSave(CepPatterns* psP);

//
// /dry-run (dryrun.c) - rules replayed against made-up events; built with DRY_RUN=1 (the default)
//
struct CorHttpConn;
extern bool dryRunInit(const char* jwks, const char* issuer, const char* audience, const char* rolesClaim, const char* editorRole, int refreshS, char* why, int whyLen);
extern void dryRunRequest(struct CorHttpConn* connP);

//
// The schedules (schedule.c)
//
extern bool   cronParse(const char* text, CepCron* cP, char* why, int whyLen);
extern time_t scheduleNext(const CepRule* ruleP, time_t after);              // the next fire after <after>; 0: never
extern char*  scheduleText(time_t t, char* buf, int bufLen);                  // local, with the offset

// token.c - the bearer token: fixed (--token-env) or a command's (--token-command), renewed
extern bool  tokenInit(const char* envVar, const char* command, char* why, int whyLen);
extern char* tokenGet(CorAlloc* allocP);
extern bool  tokenRefused(void);

extern void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif  // CORCEP_CORCEP_H_
