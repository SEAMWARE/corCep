//
// FILE            broker.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The requests to the broker - one at a time, from the engine's thread
//
#include <stdio.h>                               // snprintf
#include <string.h>                              // strlen, memcpy, strstr

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corJson/corJsonRender.h"               // corJsonRender
#include "corJson/corJsonRenderSize.h"           // corJsonRenderSize
#include "corRest/corRestClient.h"               // corRestClientMulti*
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corCep.h"



#define BROKER_ANSWER_MAX  (16 * 1024 * 1024)
#define BROKER_TIMEOUT_MS  10000



// -----------------------------------------------------------------------------
//
// requestOnce - <broker>/ngsi-ld/v1<path>[?query], bodyP rendered as JSON; the answer parsed
//
static bool requestOnce(CorAlloc* allocP, CorRestVerb verb, const char* path, const char* query, const char* body, CepResponse* respP)
{
  CorRestKeyValue  headers[5];
  int              headerCount = 0;
  int              bodyLen     = (body != NULL) ? strlen(body) : 0;
  char*            auth        = NULL;

  respP->status   = 0;
  respP->body     = NULL;
  respP->text     = NULL;
  respP->error[0] = 0;

  int   urlLen = strlen(cep.broker) + strlen(path) + ((query != NULL) ? strlen(query) : 0) + 32;
  char* url    = corAlloc(allocP, urlLen);
  snprintf(url, urlLen, "%s/ngsi-ld/v1%s%s%s", cep.broker, path, ((query != NULL) && (query[0] != 0)) ? "?" : "", (query != NULL) ? query : "");

  headers[headerCount].key   = (char*) "Accept";
  headers[headerCount].value = (char*) "application/json";
  ++headerCount;

  if (body != NULL)
  {
    headers[headerCount].key   = (char*) "Content-Type";
    headers[headerCount].value = (char*) "application/json";
    ++headerCount;
  }

  if (cep.tenant != NULL)
  {
    headers[headerCount].key   = (char*) "NGSILD-Tenant";
    headers[headerCount].value = (char*) cep.tenant;
    ++headerCount;
  }

  char* token = tokenGet(allocP);

  if (token != NULL)
  {
    int alen = strlen(token) + 8;
    auth     = corAlloc(allocP, alen);
    snprintf(auth, alen, "Bearer %s", token);
    headers[headerCount].key   = (char*) "Authorization";
    headers[headerCount].value = auth;
    ++headerCount;
  }

  CorRestClientMulti* multiP = corRestClientMultiCreate(1);
  if (multiP == NULL)
  {
    snprintf(respP->error, sizeof(respP->error), "out of memory");
    return false;
  }

  corRestClientMultiMaxResponse(multiP, BROKER_ANSWER_MAX);

  if (corRestClientMultiAdd(multiP, verb, url, headers, headerCount, (char*) body, bodyLen, NULL, NULL) < 0)
  {
    snprintf(respP->error, sizeof(respP->error), "the URL %s could not be used", url);
    corRestClientMultiDestroy(multiP);
    return false;
  }

  corRestClientMultiPerform(multiP, BROKER_TIMEOUT_MS);

  CorRestClientResponse* rP = corRestClientMultiResponse(multiP, 0);

  if ((rP == NULL) || (rP->error != CORR_OK) || (rP->statusCode == 0))
  {
    snprintf(respP->error, sizeof(respP->error), "%s", ((rP != NULL) && (rP->errorDetail[0] != 0)) ? rP->errorDetail : "no answer");
    corRestClientMultiDestroy(multiP);
    return false;
  }

  respP->status = rP->statusCode;

  if ((rP->body != NULL) && (rP->bodyLen > 0))
  {
    respP->text = corAlloc(allocP, rP->bodyLen + 1);
    memcpy(respP->text, rP->body, rP->bodyLen);
    respP->text[rP->bodyLen] = 0;

    char*   copy = corAlloc(allocP, rP->bodyLen + 1);   // the parse works in place: the text stays readable
    CorJson json;

    memcpy(copy, respP->text, rP->bodyLen + 1);
    corJsonCreate(&json, allocP);
    respP->body = corJsonParse(&json, copy);
  }

  corRestClientMultiDestroy(multiP);
  return true;
}



// -----------------------------------------------------------------------------
//
// brokerRequestText - <broker>/ngsi-ld/v1<path>[?query] with a body already JSON text (NULL: none); the answer
// parsed. A 401 with a token that can be renewed (--token-command): renewed, the request once more. Thread-safe:
// the engine and the action sender (sender.c) both call it
//
bool brokerRequestText(CorAlloc* allocP, CorRestVerb verb, const char* path, const char* query, const char* body, CepResponse* respP)
{
  bool ok = requestOnce(allocP, verb, path, query, body, respP);

  if ((ok == true) && (respP->status == 401) && (tokenRefused() == true))
  {
    logLine("the broker refused the token (401) - a new one, the request again");
    return requestOnce(allocP, verb, path, query, body, respP);
  }

  return ok;
}



// -----------------------------------------------------------------------------
//
// brokerRequest - as brokerRequestText, bodyP rendered as JSON
//
bool brokerRequest(CorAlloc* allocP, CorRestVerb verb, const char* path, const char* query, CorNode* bodyP, CepResponse* respP)
{
  return brokerRequestText(allocP, verb, path, query, (bodyP != NULL) ? jsonText(allocP, bodyP) : NULL, respP);
}
