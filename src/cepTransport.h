//
// FILE            cepTransport.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The engine's way to the broker. The engine - rules, patterns, schedules, conditions, actions, statistics,
// state - is the library libcorcep.a; how it reaches the broker is a CepTransport, set before engineRun:
//
// - transportHttp.c - the standalone corCep: NGSI-LD over HTTP, the events as notifications of subscriptions
// - (cep.so, coraine's plugin - doc/design.md § 9, v1.1: the events from coraine's write path, the requests
//   in process)
//
// The events come in through engineQueue (corCep.h), whatever the transport: the notification body of a watch,
// "which" its number (CepWatch::sub); "rules" when a Rule entity changed; "action" a failed action's report.
//
// Paths and queries are NGSI-LD's, under /ngsi-ld/v1 (path "/entities", query "type=Rule&options=keyValues").
//
#ifndef CORCEP_CEPTRANSPORT_H_
#define CORCEP_CEPTRANSPORT_H_

#include <stdbool.h>                             // bool

#include "corAlloc/CorAlloc.h"                   // CorAlloc
#include "corTree/CorNode.h"                     // CorNode

struct CepResponse;
struct CepRules;



// -----------------------------------------------------------------------------
//
// CepTransport -
//
typedef struct CepTransport
{
  const char* name;

  //
  // A read, with corCep's identity: the rules, the targets of an action, a condition's entities.
  // false: no answer (respP->error says why); else respP->status, the parsed body in allocP
  //
  bool (*get)(CorAlloc* allocP, const char* path, const char* query, struct CepResponse* respP);

  //
  // The statistics of a rule - POST /entities/<rule>/attrs - with corCep's identity
  //
  bool (*statsPost)(CorAlloc* allocP, const char* path, CorNode* bodyP, struct CepResponse* respP);

  //
  // The actions - queued, sent in order per target, retried; one that fails comes back as an "action" item
  // (engineQueue). author: the rule's signed 'author', NULL: its rights not checked. action: the index in the
  // rule's actions, 0 for a step.
  //
  void (*patch)(const char* ruleId, int action, const char* entity, const char* body, const char* author);    // PATCH /entities/<id>/attrs
  void (*create)(const char* ruleId, int action, const char* entity, const char* body, const char* author);   // POST /entities
  void (*del)(const char* ruleId, int action, const char* entity, const char* author);                        // DELETE /entities/<id>
  int  (*drain)(long long ms);                                                                                // at a stop: the actions still waiting after <ms>

  //
  // The watches of a new generation of rules: what events reach engineQueue from now on (the old ones dropped).
  // renew: kept alive - true if one was gone (the rules are read again)
  //
  bool (*watchesApply)(struct CepRules* rulesP);
  bool (*watchesRenew)(void);
} CepTransport;

extern const CepTransport* cepTransport;         // set before engineRun (corCep.c: &transportHttp)
extern const CepTransport  transportHttp;        // transportHttp.c

#endif  // CORCEP_CEPTRANSPORT_H_
