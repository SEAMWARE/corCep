//
// FILE            cepPlugin.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// cep.so - corCep inside coraine, as a bridge plugin (coraine --bridges cep). doc/design.md § 9, v1.1.
//
// The events: corCep's subscriptions name inproc://cep/notify/<secret>/<n> - "inproc", in process: the broker hands
// the notification to the plugin that claims the scheme (BridgeDriver.notifySchemes, ABI 10), no HTTP, no listener.
// coraine matches the writes against the subscriptions as for any other - q, watchedAttributes, the tenant, the
// owner's rights - and delivers after the response (its post-response hook): never inside the triggering write.
// The envelope's "body" is the notification; engineQueue takes it, and the engine's thread does the rest.
//
// The requests (phase A): as the standalone corCep's - NGSI-LD over HTTP, to coraine on 127.0.0.1 (--cep-broker):
// the token, the author's rights (--cep-pdp), the retries and the order per target unchanged.
//
// The options are coraine's, prefixed: --cep-broker, --cep-tenant, ... (coraine -U lists them).
//
#define _GNU_SOURCE                              // RTLD_DEFAULT
#include <dlfcn.h>                               // dlsym, RTLD_DEFAULT
#include <stdbool.h>                             // bool
#include <stdio.h>                               // snprintf, vsnprintf
#include <string.h>                              // strncmp, strlen
#include <signal.h>                              // SIGTERM
#include <pthread.h>                             // pthread_create, pthread_join
#include <unistd.h>                              // usleep
#include <sys/random.h>                          // getrandom

#include "corAlloc/corAlloc.h"                   // CorAlloc, corAllocStrdup
#include "corAlloc/corAllocBufferInit.h"         // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"        // corAllocBufferReset
#include "corArgs/CorArg.h"                      // CorArg
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corTree/corTreeLookup.h"               // corTreeLookup
#include "corBridge/BridgeDriver.h"              // BridgeDriver
#include "corBridge/BridgeBroker.h"              // BridgeBroker, BRIDGE_*

#include "corCep.h"
#include "cepTransport.h"

#define _vp (void*)



// -----------------------------------------------------------------------------
//
// The options - coraine's, under "Bridge (cep) plugin options:"
//
static char* brokerArg       = NULL;
static char* tenantArg       = NULL;
static char* rulesTenantArg  = NULL;
static char* tenantsArg      = NULL;
static char* tokenEnvArg     = NULL;
static char* tokenCmdArg     = NULL;
static char* stateArg        = NULL;
static char* pdpArg          = NULL;
static char* authorJwksArg   = NULL;
static char* authorIssuerArg = NULL;
static int   retryForArg     = 30;
static int   authorRefreshArg = 300;

static CorArg options[] =
{
  { "--cep-broker",         "-cepb",  CorArgString,   _vp &brokerArg,       CorArgOpt, _vp "http://127.0.0.1:1026", NULL, NULL, "corCep's way to this broker, for its reads and actions: http://127.0.0.1:<--port>" },
  { "--cep-rules-tenant",   "-ceprt", CorArgString,   _vp &rulesTenantArg,  CorArgOpt, _vp "cep", NULL,  NULL,        "the tenant the Rule entities are in, reserved: its own database, not in /admin/tenants; no rule runs in it (default: cep)" },
  { "--cep-tenants",        "-cepts", CorArgString,   _vp &tenantsArg,      CorArgOpt, NULL,      NULL,  NULL,        "the tenants the rules run in, comma-separated, an empty name the default tenant (default: every tenant this broker has, new ones too)" },
  { "--cep-tenant",         "-cept",  CorArgString,   _vp &tenantArg,       CorArgOpt, NULL,      NULL,  NULL,        "one tenant for all, as corCep 0.1: the rules, and the entities they run on, all in this tenant - not with --cep-rules-tenant, --cep-tenants" },
  { "--cep-token-env",      "-cepte", CorArgString,   _vp &tokenEnvArg,     CorArgOpt, NULL,      NULL,  NULL,        "the environment variable holding corCep's bearer token" },
  { "--cep-token-command",  "-ceptc", CorArgString,   _vp &tokenCmdArg,     CorArgOpt, NULL,      NULL,  NULL,        "a command printing corCep's bearer token - run again before it expires, and on a 401" },
  { "--cep-stats-interval", "-cepsi", CorArgDuration, _vp &statsInterval,   CorArgOpt, _vp 10,    _vp 0, _vp 86400,   "how often at most a rule's statistics are written to its Rule entity (PT10S; 0: at once)" },
  { "--cep-renew-interval", "-cepri", CorArgDuration, _vp &renewInterval,   CorArgOpt, _vp 3600,  _vp 0, _vp 31536000, "how often corCep's subscriptions are renewed (PT1H; 0: never)" },
  { "--cep-queue-max",      "-cepqm", CorArgInt,      _vp &queueMax,        CorArgOpt, _vp 10000, _vp 1, _vp 1000000, "events waiting for corCep's engine, at most - beyond, the oldest are dropped" },
  { "--cep-retry-for",      "-ceprf", CorArgDuration, _vp &retryForArg,     CorArgOpt, _vp 30,    _vp 0, _vp 3600,    "how long an action is tried again (PT30S; 0: never again)" },
  { "--cep-state",          "-cepst", CorArgString,   _vp &stateArg,        CorArgOpt, NULL,      NULL,  NULL,        "a file corCep's pattern state is kept in over a restart (default: not kept)" },
  { "--cep-pdp",            "-ceppd", CorArgString,   _vp &pdpArg,          CorArgOpt, NULL,      NULL,  NULL,        "the policy decision point asked whether a rule's author may make each request - needs --cep-author-jwks, --cep-author-issuer" },
  { "--cep-author-jwks",    "-cepaj", CorArgString,   _vp &authorJwksArg,   CorArgOpt, NULL,      NULL,  NULL,        "with --cep-pdp: the JWK Set the rule editor signs the rules' 'author' with" },
  { "--cep-author-issuer",  "-cepai", CorArgString,   _vp &authorIssuerArg, CorArgOpt, NULL,      NULL,  NULL,        "with --cep-pdp: the iss a rule's 'author' must have" },
  { "--cep-jwks-refresh",   "-cepjr", CorArgDuration, _vp &authorRefreshArg, CorArgOpt, _vp 300,  _vp 1, _vp 86400,   "with --cep-author-jwks: how often the keys are fetched again" },
  CORARGS_END
};



static const BridgeBroker* hostP = NULL;
static pthread_t           engineThread;
static bool                engineStarted = false;
static char                brokerBase[512];
static char                versionText[64];



// -----------------------------------------------------------------------------
//
// logToHost - corCep's lines in coraine's log
//
static void logToHost(const char* line)
{
  if ((hostP != NULL) && (hostP->logFunction != NULL))
    hostP->logFunction(BRIDGE_LOG_INFO, __FILE__, __LINE__, "corCep", line);
}



// -----------------------------------------------------------------------------
//
// engineMain - the subscriptions, then the engine: the broker may not listen yet (the bridges are started
// before the HTTP port), so the first request is tried again till it answers
//
static void* engineMain(void* arg)
{
  (void) arg;

  int waitMs = 200;
  while (subsInit() == false)
  {
    if (cepStopping)
      return NULL;

    usleep(waitMs * 1000);
    if (waitMs < 5000)
      waitMs *= 2;
  }

  engineRun();                                   // returns once stopped (cepExitOnStop false)
  return NULL;
}



// -----------------------------------------------------------------------------
//
// cepInit -
//
static int cepInit(const char* configFile, const BridgeBroker* brokerP)
{
  (void) configFile;

  hostP         = brokerP;
  cepLogHook    = logToHost;
  cepExitOnStop = false;

  snprintf(brokerBase, sizeof(brokerBase), "%s", brokerArg);
  int len = strlen(brokerBase);
  while ((len > 0) && (brokerBase[len - 1] == '/'))
    brokerBase[--len] = 0;

  if ((strncmp(brokerBase, "http://", 7) != 0) && (strncmp(brokerBase, "https://", 8) != 0))
  {
    logLine("--cep-broker %s - an http:// or https:// URL", brokerBase);
    return BRIDGE_ERR;
  }

  cep.broker     = brokerBase;
  if (tenantArg != NULL)                         // 0.1's one tenant
  {
    if ((tenantsArg != NULL) || ((rulesTenantArg != NULL) && (strcmp(rulesTenantArg, "cep") != 0)))
    {
      logLine("--cep-tenant is one tenant for all - not with --cep-rules-tenant, --cep-tenants");
      return BRIDGE_ERR;
    }
    cep.single      = true;
    cep.rulesTenant = tenantArg;
    cep.tenants     = tenantArg;
  }
  else
  {
    cep.rulesTenant = rulesTenantArg;
    cep.tenants     = tenantsArg;
  }

  if ((cep.rulesTenant == NULL) || ((cep.single == false) && (cep.rulesTenant[0] == 0)))
  {
    logLine("--cep-rules-tenant - a tenant of its own, not the default one");
    return BRIDGE_ERR;
  }

  //
  // coraine's own (0.7.x on): the rules tenant reserved - its database apart, out of /admin/tenants, never a data
  // tenant; the data tenants visited in process. An older coraine: the rules tenant an ordinary one, the tenants
  // read with GET /admin/tenants
  //
  if (cep.single == false)
  {
    bool (*reserve)(const char*) = (bool (*)(const char*)) dlsym(RTLD_DEFAULT, "tenantReserve");

    if (reserve == NULL)
      logLine("this coraine does not reserve tenants - the rules tenant '%s' is an ordinary one", cep.rulesTenant);
    else if (reserve(cep.rulesTenant) == false)
    {
      logLine("the rules tenant '%s' could not be reserved - an ordinary tenant of that name has data (corCep 0.1's --cep-tenant?)."
              " --cep-tenant %s keeps it as it was; --cep-rules-tenant names another", cep.rulesTenant, cep.rulesTenant);
      return BRIDGE_ERR;
    }

    tenantVisitHook = (void (*)(void (*)(const char*, void*), void*)) dlsym(RTLD_DEFAULT, "tenantDataVisit");
  }

  cep.notifyBase = "inproc://cep";
  cep.port       = 0;

  char why[1024];

  if (tokenInit(tokenEnvArg, tokenCmdArg, why, sizeof(why)) == false)
  {
    logLine("%s", why);
    return BRIDGE_ERR;
  }

  bool pdpOn = (pdpArg != NULL) && (pdpArg[0] != 0);
  if (pdpOn != ((authorJwksArg != NULL) && (authorJwksArg[0] != 0)))
  {
    logLine("--cep-pdp and --cep-author-jwks go together (and --cep-author-issuer)");
    return BRIDGE_ERR;
  }

  if (authorInit(pdpArg, authorIssuerArg, why, sizeof(why)) == false)
  {
    logLine("%s", why);
    return BRIDGE_ERR;
  }

  if (keysInit(NULL, authorJwksArg, authorRefreshArg, why, sizeof(why)) == false)
  {
    logLine("%s", why);
    return BRIDGE_ERR;
  }

  if ((stateArg != NULL) && (stateArg[0] != 0))
    stateFile = stateArg;

  unsigned char rnd[16];
  if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd))
  {
    logLine("no random bytes for the notification secret");
    return BRIDGE_ERR;
  }
  for (int ix = 0; ix < 16; ix++)
    snprintf(&cep.secret[ix * 2], 3, "%02x", rnd[ix]);

  // corRestClientInit: coraine's own (the pool is the process's) - not again

  logLine("corCep %s in coraine - requests to %s, rules in tenant %s, run in %s, events in process (inproc://cep)", CORCEP_VERSION, cep.broker,
          TENANT_TEXT(cep.rulesTenant), cep.single ? "it alone" : (cep.tenants != NULL) ? cep.tenants : "every tenant");

  if (senderStart(retryForArg) == false)
  {
    logLine("the actions' sender could not be started");
    return BRIDGE_ERR;
  }

  cepTransport = &transportHttp;

  if (pthread_create(&engineThread, NULL, engineMain, NULL) != 0)
  {
    logLine("the engine's thread could not be started");
    return BRIDGE_ERR;
  }
  engineStarted = true;

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// cepClose - the engine writes the state and the statistics, then stops (a second at most)
//
static void cepClose(void)
{
  if (engineStarted == false)
    return;

  cepStopping = SIGTERM;
  engineWake();                                  // not at its next turn - a second later
  pthread_join(engineThread, NULL);              // the actions drained (2 s at most), the state and the statistics written
  engineStarted = false;

  senderStop();                                  // no thread of ours may outlive the plugin - coraine unloads it next
  keysStop();
  subsFree();
  tenantsFree();
  engineQueueFree();
}



// -----------------------------------------------------------------------------
//
// cepNotify - a notification of one of corCep's subscriptions: inproc://cep/notify/<secret>/<which>, the payload
// the TS 104 243 envelope { "metadata": {...}, "body": <the Notification> }
//
static int cepNotify(const char* url, const char* payload, const char* info, bool tlsInsecure)
{
  (void) info;
  (void) tlsInsecure;

  char prefix[96];
  int  prefixLen = snprintf(prefix, sizeof(prefix), "inproc://cep/notify/%s/", cep.secret);

  if ((strncmp(url, prefix, prefixLen) != 0) || (url[prefixLen] == 0) || (strlen(&url[prefixLen]) >= 32))
    return BRIDGE_BAD_INPUT;                     // not corCep's - another inproc:// consumer, or an old secret

  const char* which = &url[prefixLen];

  CorAlloc alloc;
  corAllocBufferInit(&alloc, NULL, 0, 16 * 1024, NULL, "corCep notify");

  char*    copy   = corAllocStrdup(&alloc, payload);
  CorJson  json;
  corJsonCreate(&json, &alloc);
  CorNode* treeP  = (copy != NULL) ? corJsonParse(&json, copy) : NULL;
  CorNode* bodyP  = (treeP != NULL) ? corTreeLookup(treeP, "body") : NULL;
  char*    text   = (bodyP != NULL) ? jsonText(&alloc, bodyP) : NULL;

  if (text == NULL)
  {
    logLine("a notification for %s without a body - dropped", which);
    corAllocBufferReset(&alloc, false);
    return BRIDGE_BAD_INPUT;
  }

  engineQueue(which, text, strlen(text));
  corAllocBufferReset(&alloc, false);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// cepVersionInfo -
//
static const char* cepVersionInfo(void)
{
  snprintf(versionText, sizeof(versionText), "corCep %s", CORCEP_VERSION);
  return versionText;
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol this .so exports
//
__attribute__((visibility("default"))) void bridgeRegister(BridgeDriver* driverP)
{
  const int hostAbi = (driverP->abiVersion > 0) ? driverP->abiVersion : 1;

  driverP->alias       = "cep";
  driverP->version     = CORCEP_VERSION;
  driverP->args        = options;
  driverP->init        = cepInit;
  driverP->close       = cepClose;
  driverP->versionInfo = cepVersionInfo;

  if (hostAbi < 10)
  {
    driverP->init = NULL;                        // a coraine without notifySchemes: no way for the events in
    driverP->abiVersion = BRIDGE_ABI_VERSION;
    return;
  }

  {
    driverP->notifySchemes = "inproc";
    driverP->notify        = cepNotify;
  }

  driverP->abiVersion = BRIDGE_ABI_VERSION;
}
