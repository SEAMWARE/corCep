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



// -----------------------------------------------------------------------------
//
// The rules (doc/design.md § 3) - what P1 runs: one event, a list of set / toggle actions
//
typedef enum CepActionKind
{
  CepActionSet,
  CepActionToggle
} CepActionKind;

typedef struct CepAction
{
  CepActionKind  kind;
  char*          entity;                         // the target
  CorNode*       set;                            // CepActionSet: { attribute: value, ... }
  char*          attribute;                      // CepActionToggle: the boolean attribute
} CepAction;

typedef struct CepEvent
{
  char*  name;
  char*  entity;                                 // one entity ...
  char*  type;                                   // ... or (with entity NULL) every entity of a type
  char*  attribute;                              // the attribute watched
  int    watch;                                  // its index in the watch list - its subscription
} CepEvent;

typedef struct CepRule
{
  char*       id;
  char*       name;
  CepEvent    event;
  CepAction*  actionV;
  int         actions;
} CepRule;

typedef struct CepWatch                          // one subscription: (entity or type, attribute)
{
  char*  entity;
  char*  type;
  char*  attribute;
  int    sub;                                    // its subscription's number: corcep-<sub>, /notify/<secret>/<sub>
} CepWatch;

typedef struct CepRules                          // a generation of rules - replaced whole when a Rule changes
{
  CorAlloc   alloc;
  CepRule*   ruleV;
  int        rules;
  CepWatch*  watchV;
  int        watches;
} CepRules;

extern CepRules* rulesLoad(void);
extern void      rulesFree(CepRules* rulesP);



// -----------------------------------------------------------------------------
//
// Subscriptions, the listener, the engine
//
extern bool subsInit(void);                                   // stale ones removed, the Rule subscription made
extern bool subsApply(CepRules* rulesP);                      // one per watch, the old ones replaced
extern bool listenerStart(void);                              // the notification server, its own thread
extern void engineRun(void);                                  // the events, one at a time - never returns
extern void engineQueue(const char* which, const char* body, int bodyLen);   // from the listener thread

extern void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif  // CORCEP_CORCEP_H_
