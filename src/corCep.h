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

#define CORCEP_VERSION     "0.2.0"
#define CEP_SUB_PREFIX     "urn:ngsi-ld:Subscription:corcep-"
#define CEP_SUB_RULES      "urn:ngsi-ld:Subscription:corcep-rules"



// -----------------------------------------------------------------------------
//
// CepSession - one broker, one identity; set at start. The rules in one tenant, the rules tenant; the events and
// the actions in the data tenants (tenants.c) - every one of them, unless a rule's 'tenant' names some
//
typedef struct CepSession
{
  const char*  broker;                           // http://host:port, no trailing slash
  const char*  rulesTenant;                      // the Rule entities, corCep's Rule subscription, the statistics (--rules-tenant, default "cep")
  const char*  tenants;                          // --tenants: the data tenants, comma-separated ("" the default tenant); NULL: the broker's
  bool         single;                           // --tenant T: 0.1's one tenant - the rules and the data both in T
  const char*  notifyBase;                       // the URL the broker notifies: <notifyBase>/notify/<secret>/<n>
  char         secret[33];                       // in every notification path - nothing else can send an event
  unsigned short port;                           // the listener's port
} CepSession;

extern CepSession cep;



// -----------------------------------------------------------------------------
//
// The tenant a broker request goes to (broker.c's NGSILD-Tenant) - per thread: the engine sets it for each event,
// each timer, each step; the sender for each request it sends. NULL or "": the broker's default tenant.
// tenantUse sets it, and returns the one it replaces.
//
extern __thread const char* cepTenantNow;
extern const char*          tenantUse(const char* tenant);
#define TENANT_TEXT(t)      (((t) == NULL) || ((t)[0] == 0) ? "(default)" : (t))

//
// The data tenants (tenants.c) - the engine's thread only. Each has its own patterns' state and ${...} values; a
// rule with no 'tenant' runs in each of them, the rules tenant never one.
//
struct CepPatterns;
struct CepVars;
struct CepRules;

typedef struct CepTenant
{
  char*                name;                     // "" the default tenant
  struct CepPatterns*  patterns;
  struct CepVars*      vars;
  bool                 seen;                     // in the broker's list at the last refresh
} CepTenant;

extern CepTenant* tenantV;
extern int        tenants;
extern CepTenant* tenantOf(const char* name);                               // NULL: not a data tenant (yet)
extern int        tenantsRefresh(struct CepRules* rulesP, long long now);   // the list again; the number new
extern void       tenantsFree(void);
extern void      (*tenantVisitHook)(void (*fn)(const char* name, void* ctx), void* ctx);   // cep.so: coraine's, in process; NULL: GET /admin/tenants



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
extern bool  brokerAdminGet(CorAlloc* allocP, const char* path, CepResponse* respP);         // <broker><path> - not under /ngsi-ld/v1, no tenant
extern char* jsonText(CorAlloc* allocP, CorNode* nodeP);                                    // a tree as JSON text
extern void  brokerFailed(char* why, int whyLen, const char* what, CepResponse* respP);    // engine.c - why a request failed

//
// The actions' PATCHes (sender.c) - a queue per target, a thread, retried for --retry-for
//
extern bool  senderStart(int retryForS);
extern void  senderStop(void);                // cep.so's close: the thread ended, the queues freed
// author: the rule's signed 'author' (author.c), NULL: not checked - the PDP asked before each request goes out
extern void  senderPatch(const char* ruleId, int action, const char* entity, const char* body, const char* author);   // action 0: a step
extern void  senderCreate(const char* ruleId, int action, const char* entity, const char* body, const char* author);  // POST /entities
extern void  senderDelete(const char* ruleId, int action, const char* entity, const char* author);                    // DELETE /entities/<id>
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
  CepActionStep,
  CepActionCreate,                               // POST /entities - 'create': an entity, set style
  CepActionDelete                                // DELETE /entities/<id> - the target, one or a group
} CepActionKind;

typedef struct CepAction
{
  CepActionKind  kind;
  char*          entity;                         // the target: one entity ...
  char*          type;                           // ... or every entity of a type
  char*          q;                              //     that matches q (NULL: all of them)
  CorNode*       set;                            // Set: { attribute: value, ... }; Create: the entity { id?, type, attribute: value, ... }
  char*          attribute;                      // Toggle: the boolean attribute; Step: the number
  double         by;                             // Step: added every 'every'
  double         min, max;                       // Step: the bounds - stepping stops at one
  bool           hasMin, hasMax;
  long long      everyMs;                        // Step
  long long      forMs;                          // Step: the time limit
  int            until;                          // Step: the event that stops it, an index into the rule's untilV; -1: none
  bool           vars;                           // a ${...} in its target, q or values (vars.c)
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
  char**          tenantV;                       // 'tenant': the data tenants it runs in ("" the default); NULL: every one
  int             tenants;
  CepEvent*       eventV;                        // the events the pattern uses, each once
  int             events;
  CepPatternKind  kind;
  int*            stepV;                         // Seq, All: the steps, as indexes into eventV ([press, press]: 0, 0)
  int             steps;
  long long       withinMs;                      // Seq, All, Repeat
  long long       forMs;                         // Absent, Window
  bool            vars;                          // an action has a ${...}: the events' latest values are kept (vars.c)
  int             afterEv;                       // Absent: the event that starts the deadline (an index into eventV), the absent one clears it; -1: none - each absent event moves it on
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
  char*           author;                        // the rule's signed 'author' (author.c) - NULL: the rights not checked (no --pdp)
  char*           whenText;                      // 'when' as JSON text - a rule's state survives a reload if it is the same
  CepEvent*       declV;                         // every event 'when' declares - 'until' names one the pattern does not use
  int             decls;
  CepEvent*       untilV;                        // the events that stop a step - watched, never fire the rule
  int             untils;
  CepCondition*   condV;                         // 'if': every one must hold when the pattern matches - each a leaf or a group
  int             conds;
  CepAction*      actionV;
  int             actions;                       // then's and else's
  int             thens;                         // the first 'thens' are then's; the rest else's (the 'if' does not hold)
  long            firedElseBase;                 // 'firedElse' as the Rule entity holds it
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
extern bool      ruleApplies(const CepRule* ruleP, const char* tenant);                       // does it run in <tenant> (NULL: a dry run - yes)
extern void      rulesFree(CepRules* rulesP);



// -----------------------------------------------------------------------------
//
// Subscriptions, the listener, the engine
//
extern int  statsInterval;                                    // seconds between two writes of a rule's statistics
extern int  renewInterval;                                    // seconds between two renewals of the subscriptions (0: never)
extern bool subsInit(void);                                   // stale ones removed, the Rule subscription made
extern bool subsApply(CepRules* rulesP);                      // one per watch, the old ones replaced
extern void subsFree(void);                                   // cep.so's close: the table freed
extern bool subsRenew(void);                                  // all renewed (their owner); true: one was gone - read the rules again
extern bool listenerStart(void);                              // the notification server, its own thread
extern void engineRun(void);                                  // the events, one at a time - till a stop (cepStopping): exit(0), or return (cepExitOnStop false)
extern void engineWake(void);                                 // a plugin's close, after cepStopping set: the stop seen at once
extern void engineQueueFree(void);                            // a plugin's close, after senderStop: what still waits freed
extern void engineQueue(const char* which, const char* body, int bodyLen);   // from the listener / sender threads; full: the oldest events dropped
extern int  queueMax;                                                       // --queue-max

//
// The patterns' state (pattern.c) - the engine's thread only
//
typedef void (*CepFireFn)(CepRule* ruleP, const char* entity, void* ctx);

typedef struct CepPatterns CepPatterns;          // the state of one set of rules - the engine's, or a dry run's

extern void         condText(CepCondition* cP, char* buf, int bufLen);        // a condition in words: a leaf its q, a group all(a; b) / any(a | b)
extern long long    nowMs(void);                                               // CLOCK_MONOTONIC, ms
extern CepPatterns* patternsNew(long long wallAtZeroMs, const char* tenant);   // 0: the host's wall clock; else a dry run's, at 'now' 0. tenant: whose (NULL: a dry run's - every rule)
extern void         patternsFree(CepPatterns* psP);
extern void         patternsAdopt(CepPatterns* psP, CepRules* rulesP, long long now);   // a new generation: the state of unchanged rules kept
extern void         patternsEvent(CepPatterns* psP, CepRules* rulesP, int watch, const char* entity, const double* valueP, long long now, CepFireFn fire, void* ctx);
extern long long    patternsTimers(CepPatterns* psP, CepRules* rulesP, long long now, CepFireFn fire, void* ctx);   // the expired ones run; the next deadline, 0: none
extern long long    patternsDropped(CepPatterns* psP, const char* ruleId);     // partial matches dropped (the bound), since start
extern long long    patternsDroppedValues(CepPatterns* psP, const char* ruleId);   // window values dropped (the bound), since start
extern bool         patternsAggregate(CepPatterns* psP, CepRule* ruleP, const char* key, double* aP);   // a window rule's aggregate now; false: no value
extern CorNode*     patternsSave(CepPatterns* psP, CorAlloc* aP, long long nowMono, long long nowWall);   // the state, for --state
extern int          patternsLoad(CepPatterns* psP, CorNode* topP, long long nowMono, long long nowWall);  // before the first adopt; rules read, -1: not a state

//
// ${...} (vars.c) - the events' latest values, and an action resolved for a fire
//
typedef struct CepVars CepVars;
extern CepVars*  varsNew(void);
extern void      varsFree(CepVars* vP);
extern void      varsNote(CepVars* vP, const char* ruleId, const char* key, const char* event, const char* entity, const char* valueJson);
extern void      varsKeep(CepVars* vP, CepRules* rulesP);                                   // the records of rules gone dropped
extern bool      varsCheck(CepRule* ruleP, const char* s, bool* usedP, char* why, int whyLen);
extern bool      varsCheckTree(CepRule* ruleP, CorNode* nodeP, bool* usedP, char* why, int whyLen);
extern bool      varsAction(CorAlloc* allocP, CepVars* vP, CepPatterns* psP, CepRule* ruleP, const char* entity, CepAction* aP, CepAction* outP, char* why, int whyLen);

//
// --state (state.c) - the patterns' state over a restart; cepStopping (corCep.c) - a SIGTERM / SIGINT came
//
#include <signal.h>                              // sig_atomic_t
extern const char*            stateFile;                                          // NULL: not kept
extern volatile sig_atomic_t  cepStopping;                                        // the signal, 0: none
extern bool                   cepExitOnStop;                                      // util.c - true: the stop exits the process; false (a plugin): engineRun returns
extern void                   stateLoad(void);                                     // into each tenant's patterns, as tenants.c makes them
extern CorNode*               steppersSave(CorAlloc* aP, long long nowMono, long long nowWall);   // engine.c - the steps running, each with its tenant
extern int                    steppersLoad(CorNode* topP, long long nowMono, long long nowWall);  // engine.c - before the first rules
extern bool                   stateSave(void);
extern int                    stateLoadTenant(CepPatterns* psP, const char* tenant); // state.c - what --state had for <tenant> into psP (tenants.c, at its first sight); the rules read
extern void                   stateRelease(void);                                   // state.c - what stateLoad read, freed
extern const char*            stateLegacyTenant(void);                              // state.c - the tenant a 0.1 state (one tenant, unnamed) was of

//
// /dry-run (dryrun.c) - rules replayed against made-up events; built with DRY_RUN=1 (the default)
//
struct CorHttpConn;
extern bool dryRunInit(const char* jwks, const char* issuer, const char* audience, const char* rolesClaim, const char* editorRole, char* why, int whyLen);

//
// The keys (keys.c) - --jwks and --author-jwks, one set; the author's rights (author.c) - --pdp
//
extern void  keysStop(void);                  // cep.so's close: the refresh thread ended
extern bool  keysInit(const char* jwks, const char* authorJwks, int refreshS, char* why, int whyLen);
extern void  keysRefreshNow(void);
extern bool  authorInit(const char* pdp, const char* issuer, char* why, int whyLen);
extern bool  authorOn(void);
extern bool  authorVerify(CorAlloc* allocP, const char* jws, CorNode* whenP, CorNode* thenP, CorNode* elseP, char** tokenP, char* why, int whyLen);
extern int   authorMay(CorAlloc* allocP, const char* token, const char* method, const char* path, const char* body, char* why, int whyLen);
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
extern void (*cepLogHook)(const char* line);    // util.c - NULL: logLine writes stderr; else the host's log gets the line

#endif  // CORCEP_CORCEP_H_
