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
// urlEncode - a path segment or a query value: everything but RFC 3986's unreserved characters escaped
//
char* urlEncode(CorAlloc* allocP, const char* s)
{
  static const char hex[] = "0123456789ABCDEF";
  int   len  = strlen(s);
  char* out  = corAlloc(allocP, len * 3 + 1);
  char* p    = out;

  for (const unsigned char* c = (const unsigned char*) s; *c != 0; c++)
  {
    if (((*c >= 'a') && (*c <= 'z')) || ((*c >= 'A') && (*c <= 'Z')) || ((*c >= '0') && (*c <= '9')) ||
        (*c == '-') || (*c == '.') || (*c == '_') || (*c == '~'))
      *p++ = *c;
    else
    {
      *p++ = '%';
      *p++ = hex[*c >> 4];
      *p++ = hex[*c & 0xF];
    }
  }
  *p = 0;

  return out;
}



// -----------------------------------------------------------------------------
//
// brokerRequest - <broker>/ngsi-ld/v1<path>[?query], bodyP rendered as JSON; the answer parsed
//
bool brokerRequest(CorAlloc* allocP, CorRestVerb verb, const char* path, const char* query, CorNode* bodyP, CepResponse* respP)
{
  CorRestKeyValue  headers[5];
  int              headerCount = 0;
  char*            body        = NULL;
  int              bodyLen     = 0;
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

  if (bodyP != NULL)
  {
    CorJson json;
    corJsonCreate(&json, allocP);
    body = corAlloc(allocP, corJsonRenderSize(&json, bodyP) + 1);
    corJsonRender(&json, bodyP, body);
    bodyLen = strlen(body);

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

  if (cep.token != NULL)
  {
    int alen = strlen(cep.token) + 8;
    auth     = corAlloc(allocP, alen);
    snprintf(auth, alen, "Bearer %s", cep.token);
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

  if (corRestClientMultiAdd(multiP, verb, url, headers, headerCount, body, bodyLen, NULL, NULL) < 0)
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
// nonexistentTenant - the broker's answer is 404 NonexistentTenant: the tenant has nothing in it yet
//
// NGSI-LD makes a tenant when the first entity, subscription or registration is created in it; until then a
// GET in it is this error. For corCep that is "nothing there", not a failure: it starts on a fresh tenant.
//
bool nonexistentTenant(CepResponse* respP)
{
  if ((respP->status != 404) || (respP->body == NULL))
    return false;

  CorNode* typeP = corTreeLookup(respP->body, "type");

  return (typeP != NULL) && (typeP->type == CorString) && (strstr(typeP->value.s, "NonexistentTenant") != NULL);
}
