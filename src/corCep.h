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
  const char*  token;                            // NULL: no Authorization
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

typedef struct CepCondition                      // holds if the broker has an entity that matches
{
  char*  entity;                                 // one entity ...
  char*  type;                                   // ... or any entity of a type
  char*  q;                                      // the NGSI-LD q it must match
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
  CepPatternAbsent                               // no event for 'for': a timer each event restarts
} CepPatternKind;

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
  long long       forMs;                         // Absent
  int             min;                           // Repeat
  char**          supersedeV;                    // the rules this one supersedes (their ids)
  int             supersedes;
  bool            partitioned;                   // every event a type selector: a match per entity (design § 3.1)
  char*           whenText;                      // 'when' as JSON text - a rule's state survives a reload if it is the same
  CepEvent*       declV;                         // every event 'when' declares - 'until' names one the pattern does not use
  int             decls;
  CepEvent*       untilV;                        // the events that stop a step - watched, never fire the rule
  int             untils;
  CepCondition*   condV;                         // 'if': every one must hold when the pattern matches
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

extern CepRules* rulesLoad(void);
extern void      rulesFree(CepRules* rulesP);



// -----------------------------------------------------------------------------
//
// Subscriptions, the listener, the engine
//
extern int  statsInterval;                                    // seconds between two writes of a rule's statistics
extern bool subsInit(void);                                   // stale ones removed, the Rule subscription made
extern bool subsApply(CepRules* rulesP);                      // one per watch, the old ones replaced
extern bool listenerStart(void);                              // the notification server, its own thread
extern void engineRun(void);                                  // the events, one at a time - never returns
extern void engineQueue(const char* which, const char* body, int bodyLen);   // from the listener thread

//
// The patterns' state (pattern.c) - the engine's thread only
//
typedef void (*CepFireFn)(CepRule* ruleP, const char* entity, void* ctx);

extern long long nowMs(void);                                                  // CLOCK_MONOTONIC, ms
extern void      patternsAdopt(CepRules* rulesP, long long now);              // a new generation: the state of unchanged rules kept
extern void      patternsEvent(CepRules* rulesP, int watch, const char* entity, long long now, CepFireFn fire, void* ctx);
extern long long patternsTimers(CepRules* rulesP, long long now, CepFireFn fire, void* ctx);   // the expired ones run; the next deadline, 0: none
extern long long patternsDropped(const char* ruleId);                         // partial matches dropped (the bound), since start

extern void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif  // CORCEP_CORCEP_H_
