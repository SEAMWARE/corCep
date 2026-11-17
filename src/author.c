//
// FILE            author.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The author's rights (doc/design.md section 7) - a rule acts with corCep's rights, but only where its author may.
//
// Who the author is: the rule's 'author', a JWS the rule editor signs when the rule is saved - its payload the
// author's identity as the editor knows it (in a data space connector: the author's verifiableCredential) and the
// rule itself ("rule": { "when", "then" }). corCep checks the signature against --author-jwks, the issuer against
// --author-issuer, and that "rule" is the rule's when and then - a rule edited without the editor (its signature
// not made again) is left out. corCep never holds the author's access token.
//
// What the author may: the policy decision point (--pdp, OPA's data API: POST <url> { "input": ... }, answer
// { "result": true }) is asked, for every request corCep makes for a rule, whether the author may make it - the
// request as the broker's PEP would hand it over (method, path, headers, body), the author's signed statement as
// its bearer token. A data space connector's policies (odrl-pap's Rego) decode the token's payload without
// checking its signature - the PEP checked it -, and so does corCep, before it asks.
//
// A decision is kept a minute (the same request, the same author). The PDP not answering: the request waits and
// is tried again (--retry-for); it is never sent unchecked.
//
#include <pthread.h>                             // pthread_mutex_*
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, free
#include <string.h>                              // strcmp, strlen, strdup

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeLookup.h"               // corTreeLookup
#include "corRest/corRestClient.h"               // corRestClientMulti*
#include "corRest/corRestJwt.h"                  // corRestJwtVerify

#include "corCep.h"



#define DECISION_KEEP_MS  60000                  // a decision is kept this long
#define DECISIONS_MAX     1000                   // and this many - the oldest goes
#define PDP_TIMEOUT_MS    5000



static const char*      pdp      = NULL;         // NULL: the author's rights are not checked
static const char*      issuer   = NULL;

typedef struct Decision
{
  struct Decision*  next;
  char*             key;                         // token \n method \n path \n body
  bool              may;
  long long         at;
} Decision;

static pthread_mutex_t  mutex     = PTHREAD_MUTEX_INITIALIZER;
static Decision*        decisions = NULL;
static int              kept      = 0;



// -----------------------------------------------------------------------------
//
// authorInit - --pdp and --author-issuer (both, or neither: the rights not checked)
//
bool authorInit(const char* pdpArg, const char* issuerArg, char* why, int whyLen)
{
  bool p = (pdpArg != NULL) && (pdpArg[0] != 0);
  bool i = (issuerArg != NULL) && (issuerArg[0] != 0);

  if (p != i)
  {
    snprintf(why, whyLen, "--pdp and --author-issuer go together (and --author-jwks)");
    return false;
  }

  pdp    = p ? pdpArg : NULL;
  issuer = i ? issuerArg : NULL;
  return true;
}



bool authorOn(void)
{
  return pdp != NULL;
}



// -----------------------------------------------------------------------------
//
// treeEqual - the same JSON: objects by member name (any order), arrays in order, numbers by value
//
static bool treeEqual(CorNode* aP, CorNode* bP)
{
  bool aNum = (aP->type == CorInt) || (aP->type == CorFloat);
  bool bNum = (bP->type == CorInt) || (bP->type == CorFloat);

  if (aNum && bNum)
  {
    double a = (aP->type == CorInt) ? (double) aP->value.i : aP->value.f;
    double b = (bP->type == CorInt) ? (double) bP->value.i : bP->value.f;
    return a == b;
  }

  if (aP->type != bP->type)
    return false;

  switch (aP->type)
  {
  case CorString:  return strcmp(aP->value.s, bP->value.s) == 0;
  case CorBoolean: return aP->value.b == bP->value.b;
  case CorNull:    return true;
  case CorArray:
  {
    CorNode* xP = aP->value.head;
    CorNode* yP = bP->value.head;
    for (; (xP != NULL) && (yP != NULL); xP = xP->next, yP = yP->next)
    {
      if (treeEqual(xP, yP) == false)
        return false;
    }
    return (xP == NULL) && (yP == NULL);
  }
  case CorObject:
  {
    int a = 0;
    int b = 0;
    for (CorNode* xP = aP->value.head; xP != NULL; xP = xP->next)
    {
      CorNode* yP = corTreeLookup(bP, xP->name);
      if ((yP == NULL) || (treeEqual(xP, yP) == false))
        return false;
      ++a;
    }
    for (CorNode* yP = bP->value.head; yP != NULL; yP = yP->next)
      ++b;
    return a == b;
  }
  default:
    return false;
  }
}



// -----------------------------------------------------------------------------
//
// authorVerify - a rule's 'author' (keyValues: the JWS) checked: signed by a key of the set, by --author-issuer,
// for this rule's when and then. True: *tokenP the JWS; false: why
//
bool authorVerify(CorAlloc* allocP, const char* jws, CorNode* whenP, CorNode* thenP, char** tokenP, char* why, int whyLen)
{
  if (jws == NULL)
  {
    snprintf(why, whyLen, "not signed - an 'author' (the rule editor's signature) is needed: --pdp is on");
    return false;
  }

  CorRestJwtExpect expect  = { issuer, NULL, (int64_t) time(NULL), 60 };
  CorNode*         claimsP = NULL;
  CorRestJwtCheck  check   = corRestJwtVerify(jws, &expect, allocP, &claimsP);

  if (check != CorRestJwtOk)
  {
    if (check == CorRestJwtUnknownKey)
      keysRefreshNow();
    snprintf(why, whyLen, "'author': %s", corRestJwtCheckString(check));
    return false;
  }

  CorNode* ruleP = corTreeLookup(claimsP, "rule");
  CorNode* sWhen = (ruleP != NULL) ? corTreeLookup(ruleP, "when") : NULL;
  CorNode* sThen = (ruleP != NULL) ? corTreeLookup(ruleP, "then") : NULL;

  if ((sWhen == NULL) || (sThen == NULL) || (whenP == NULL) || (thenP == NULL) || (treeEqual(sWhen, whenP) == false) || (treeEqual(sThen, thenP) == false))
  {
    snprintf(why, whyLen, "'author' signs another rule - its \"rule\" is not this rule's when and then (edited after it was signed?)");
    return false;
  }

  *tokenP = (char*) jws;
  return true;
}



// -----------------------------------------------------------------------------
//
// decisionGet, decisionPut - the decisions kept
//
static int decisionGet(const char* key, long long now)
{
  int may = -1;

  pthread_mutex_lock(&mutex);
  for (Decision* dP = decisions; dP != NULL; dP = dP->next)
  {
    if ((now - dP->at < DECISION_KEEP_MS) && (strcmp(dP->key, key) == 0))
    {
      may = dP->may ? 1 : 0;
      break;
    }
  }
  pthread_mutex_unlock(&mutex);

  return may;
}

static void decisionPut(const char* key, bool may, long long now)
{
  Decision* dP = calloc(1, sizeof(Decision));
  if ((dP == NULL) || ((dP->key = strdup(key)) == NULL))
  {
    free(dP);
    return;
  }
  dP->may = may;
  dP->at  = now;

  pthread_mutex_lock(&mutex);

  //
  // The old ones go - out of time, or the oldest when there are too many (the list is newest first)
  //
  Decision** prevPP = &decisions;
  int        n      = 0;
  while (*prevPP != NULL)
  {
    Decision* xP = *prevPP;
    if ((now - xP->at >= DECISION_KEEP_MS) || (n >= DECISIONS_MAX - 1))
    {
      *prevPP = xP->next;
      free(xP->key);
      free(xP);
      --kept;
      continue;
    }
    ++n;
    prevPP = &xP->next;
  }

  dP->next  = decisions;
  decisions = dP;
  ++kept;

  pthread_mutex_unlock(&mutex);
}



// -----------------------------------------------------------------------------
//
// authorMay - may the author (token: the rule's signed 'author') make this request of the broker? 1: yes, 0: no
// (why), -1: the PDP did not answer (why) - not known
//
int authorMay(CorAlloc* allocP, const char* token, const char* method, const char* path, const char* body, char* why, int whyLen)
{
  if (token[0] == 0)
  {
    snprintf(why, whyLen, "%s %s: the rule is gone - its author not known", method, path);
    return 0;
  }

  long long now    = nowMs();
  int       keyLen = strlen(token) + strlen(method) + strlen(path) + ((body != NULL) ? strlen(body) : 0) + 4;
  char*     key    = corAlloc(allocP, keyLen);

  snprintf(key, keyLen, "%s\n%s\n%s\n%s", token, method, path, (body != NULL) ? body : "");

  int may = decisionGet(key, now);
  if (may == -1)
  {
    //
    // The input, as APISIX's opa plugin hands it to OPA - the request the broker would get from the author
    //
    CorNode* inputP   = corTreeObject(allocP, "input");
    CorNode* requestP = corTreeObject(allocP, "request");
    CorNode* headersP = corTreeObject(allocP, "headers");
    int      authLen  = strlen(token) + 8;
    char*    auth     = corAlloc(allocP, authLen);

    snprintf(auth, authLen, "Bearer %s", token);
    corTreeChildAdd(headersP, corTreeString(allocP, "authorization", auth));
    if (body != NULL)
      corTreeChildAdd(headersP, corTreeString(allocP, "content-type", "application/json"));
    if (cep.tenant != NULL)
      corTreeChildAdd(headersP, corTreeString(allocP, "ngsild-tenant", (char*) cep.tenant));

    corTreeChildAdd(requestP, corTreeString(allocP, "method", (char*) method));
    corTreeChildAdd(requestP, corTreeString(allocP, "path", (char*) path));
    corTreeChildAdd(requestP, headersP);
    corTreeChildAdd(requestP, corTreeObject(allocP, "query"));
    if (body != NULL)
      corTreeChildAdd(requestP, corTreeString(allocP, "body", (char*) body));
    corTreeChildAdd(inputP, corTreeString(allocP, "type", "http"));
    corTreeChildAdd(inputP, requestP);

    CorNode* topP = corTreeObject(allocP, NULL);
    corTreeChildAdd(topP, inputP);

    char*             text    = jsonText(allocP, topP);
    CorRestKeyValue   headers[1] = { { (char*) "Content-Type", (char*) "application/json" } };
    CorRestClientMulti* multiP = corRestClientMultiCreate(1);

    if ((multiP == NULL) || (corRestClientMultiAdd(multiP, CorVerbPost, pdp, headers, 1, text, strlen(text), NULL, NULL) < 0))
    {
      if (multiP != NULL)
        corRestClientMultiDestroy(multiP);
      snprintf(why, whyLen, "the PDP (%s) could not be asked", pdp);
      return -1;
    }

    corRestClientMultiMaxResponse(multiP, 64 * 1024);
    corRestClientMultiPerform(multiP, PDP_TIMEOUT_MS);

    CorRestClientResponse* rP = corRestClientMultiResponse(multiP, 0);
    if ((rP == NULL) || (rP->error != CORR_OK) || (rP->statusCode != 200))
    {
      snprintf(why, whyLen, "the PDP (%s): %s", pdp,
               ((rP == NULL) || (rP->statusCode == 0)) ? (((rP != NULL) && (rP->errorDetail[0] != 0)) ? rP->errorDetail : "no answer") : "not a 200");
      corRestClientMultiDestroy(multiP);
      return -1;
    }

    //
    // { "result": true } - and nothing else - is a yes: false, no result (OPA: undefined), anything else is a no
    //
    char* answer = corAlloc(allocP, rP->bodyLen + 1);
    memcpy(answer, rP->body, rP->bodyLen);
    answer[rP->bodyLen] = 0;
    corRestClientMultiDestroy(multiP);

    CorJson  json;
    corJsonCreate(&json, allocP);
    CorNode* answerP = corJsonParse(&json, answer);
    CorNode* resultP = (answerP != NULL) ? corTreeLookup(answerP, "result") : NULL;

    may = ((resultP != NULL) && (resultP->type == CorBoolean) && resultP->value.b) ? 1 : 0;
    decisionPut(key, may == 1, now);
  }

  if (may == 0)
    snprintf(why, whyLen, "%s %s: the rule's author may not - the PDP says no", method, path);

  return may;
}
