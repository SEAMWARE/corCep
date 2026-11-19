//
// FILE            transportHttp.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The standalone corCep's transport (cepTransport.h): NGSI-LD over HTTP - the requests broker.c's, the actions
// sender.c's queues, the watches subs.c's subscriptions, whose notifications listener.c hands to engineQueue
//
#include "corRest/CorRestVerb.h"                 // CorVerbGet, CorVerbPost

#include "corCep.h"
#include "cepTransport.h"



static bool httpGet(CorAlloc* allocP, const char* path, const char* query, CepResponse* respP)
{
  return brokerRequest(allocP, CorVerbGet, path, query, NULL, respP);
}

static bool httpStatsPost(CorAlloc* allocP, const char* path, CorNode* bodyP, CepResponse* respP)
{
  return brokerRequest(allocP, CorVerbPost, path, NULL, bodyP, respP);
}



const CepTransport transportHttp =
{
  .name         = "http",
  .get          = httpGet,
  .statsPost    = httpStatsPost,
  .patch        = senderPatch,
  .create       = senderCreate,
  .del          = senderDelete,
  .drain        = senderDrain,
  .watchesApply = subsApply,
  .watchesRenew = subsRenew
};
