//
// FILE            corCep.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corCep - an NGSI-LD complex event processor (doc/design.md)
//
//   corCep --broker http://host:port [--port 7602] [--notify-url http://host:7602] [--tenant T]
//          [--token-env VAR | --token-command CMD] [--broker-cert cert.pem] [--stats-interval PT10S] [--renew-interval PT1H] [--retry-for PT30S] [--queue-max N]
//          [--state FILE] [--jwks URL|FILE [--issuer ISS] [--audience AUD] [--roles-claim PATH] [--editor-role ROLE] [--jwks-refresh PT5M]]
//          [--pdp URL --author-jwks URL|FILE --author-issuer ISS]
//
#include <stdarg.h>                              // va_list
#include <stdio.h>                               // fprintf, vfprintf
#include <stdlib.h>                              // getenv
#include <string.h>                              // strlen, strncmp
#include <sys/random.h>                          // getrandom
#include <time.h>                                // clock_gettime, localtime_r

#include <openssl/evp.h>                         // EVP_sha256
#include <openssl/pem.h>                         // PEM_read_X509
#include <openssl/x509.h>                        // X509_digest

#include "corArgs/corArgs.h"                     // corArgsInit, corArgsParse
#include "corArgs/CorArg.h"                      // CorArg
#include "corRest/corRestClient.h"               // corRestClientInit, corRestClientTlsPinSet

#include "corCep.h"



// -----------------------------------------------------------------------------
//
// Globals
//
CepSession cep;

static char* brokerArg    = NULL;
static char* tenantArg    = NULL;
static char* tokenEnvArg  = NULL;
static char* tokenCmdArg  = NULL;
static char* brokerCertArg = NULL;
static char* notifyUrlArg = NULL;
static int   portArg      = 7602;
static char* jwksArg      = NULL;
static char* issuerArg    = NULL;
static char* audienceArg  = NULL;
static char* rolesClaimArg = NULL;
static char* editorRoleArg = NULL;
static int   jwksRefreshArg = 300;
static char* pdpArg        = NULL;
static char* authorJwksArg = NULL;
static char* authorIssuerArg = NULL;
static char* stateArg     = NULL;
static int   retryForArg  = 30;

volatile sig_atomic_t cepStopping = 0;



// -----------------------------------------------------------------------------
//
// options -
//
static CorArg options[] =
{
  { "--broker",     "-b",  CorArgString, &brokerArg,    CorArgReq, NULL,      NULL,  NULL,        "the broker's base URL: http://host:port" },
  { "--port",       "-p",  CorArgInt,    &portArg,      CorArgOpt, _vp 7602,  _vp 1, _vp 65535,   "the port the broker's notifications come to" },
  { "--notify-url", "-nu", CorArgString, &notifyUrlArg, CorArgOpt, NULL,      NULL,  NULL,        "the URL the broker reaches corCep at (default: http://127.0.0.1:<port>)" },
  { "--tenant",     "-t",  CorArgString, &tenantArg,    CorArgOpt, NULL,      NULL,  NULL,        "the tenant (default: the broker's default tenant)" },
  { "--token-env",  "-te", CorArgString, &tokenEnvArg,  CorArgOpt, NULL,      NULL,  NULL,        "the environment variable holding the bearer token sent to the broker" },
  { "--token-command", "-tc", CorArgString, &tokenCmdArg, CorArgOpt, NULL,     NULL,  NULL,        "a command printing the bearer token - run again before it expires, and on a 401" },
  { "--broker-cert", "-bc", CorArgString, &brokerCertArg, CorArgOpt, NULL,     NULL,  NULL,        "the broker's own certificate (PEM): trusted as it is, instead of the CA store and the host name" },
  { "--stats-interval", "-si", CorArgDuration, &statsInterval, CorArgOpt, _vp 10,  _vp 0, _vp 86400,   "how often at most a rule's statistics are written to its Rule entity - seconds or ISO 8601 (PT10S, PT1M; 0: at once)" },
  { "--renew-interval", "-ri", CorArgDuration, &renewInterval, CorArgOpt, _vp 3600, _vp 0, _vp 31536000, "how often corCep's subscriptions are renewed - seconds or ISO 8601 (PT1H, P1D; up to P365D; 0: never). A broker that keeps a subscription's owner only for a while (coraine --auth oidc: 24 h) stops notifying once it runs out" },
  { "--queue-max",  "-qm", CorArgInt,      &queueMax,     CorArgOpt, _vp 10000, _vp 1, _vp 1000000, "event notifications waiting for the engine, at most (default 10000; their bodies 64 MiB at most) - beyond, the oldest are dropped, and logged" },
  { "--retry-for",  "-rf", CorArgDuration, &retryForArg,  CorArgOpt, _vp 30,    _vp 0, _vp 3600,  "how long an action's PATCH is tried again when the broker does not answer, or answers 408, 429, 5xx - seconds or ISO 8601 (default PT30S, up to PT1H; 0: never again); then dropped and counted" },
  { "--state",      "-st", CorArgString, &stateArg,     CorArgOpt, NULL,      NULL,  NULL,        "a file the patterns' state is kept in over a restart - open matches, deadlines, window values (default: not kept)" },
  { "--jwks",       "-jw", CorArgString, &jwksArg,      CorArgOpt, NULL,      NULL,  NULL,        "the JWK Set (a URL or a file) the rule editors' tokens are checked against - turns POST /dry-run on" },
  { "--issuer",     "-is", CorArgString, &issuerArg,    CorArgOpt, NULL,      NULL,  NULL,        "with --jwks: the iss a token must have (default: not checked)" },
  { "--audience",   "-au", CorArgString, &audienceArg,  CorArgOpt, NULL,      NULL,  NULL,        "with --jwks: an aud a token must have (default: not checked)" },
  { "--roles-claim", "-rc", CorArgString, &rolesClaimArg, CorArgOpt, _vp "realm_access.roles", NULL, NULL, "with --jwks: the claim holding the roles - a path, dot-separated; an array of strings, or a string of them, space-separated" },
  { "--editor-role", "-er", CorArgString, &editorRoleArg, CorArgOpt, _vp "ngsild-write", NULL, NULL, "with --jwks: the role a token needs for /dry-run" },
  { "--jwks-refresh", "-jr", CorArgDuration, &jwksRefreshArg, CorArgOpt, _vp 300, _vp 1, _vp 86400, "with --jwks: how often the keys are fetched again - seconds or ISO 8601 (PT5M); at once too when a token names a key not in the set" },
  { "--pdp",        "-pd", CorArgString, &pdpArg,       CorArgOpt, NULL,      NULL,  NULL,        "the policy decision point asked whether a rule's author may make each request (OPA's data API: http://opa:8181/v1/data/policy/main/allow) - the author's rights checked; needs --author-jwks and --author-issuer" },
  { "--author-jwks", "-aj", CorArgString, &authorJwksArg, CorArgOpt, NULL,     NULL,  NULL,        "with --pdp: the JWK Set (a URL or a file) the rule editor signs the rules' 'author' with" },
  { "--author-issuer", "-ai", CorArgString, &authorIssuerArg, CorArgOpt, NULL, NULL,  NULL,        "with --pdp: the iss a rule's 'author' must have - the rule editor's" },
  CORARGS_END
};



// -----------------------------------------------------------------------------
//
// logLine - stderr, a timestamp first
//
void logLine(const char* fmt, ...)
{
  struct timespec ts;
  struct tm       tm;
  char            stamp[32];
  va_list         args;

  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

  va_start(args, fmt);
  fprintf(stderr, "%s.%03ld ", stamp, ts.tv_nsec / 1000000);
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  va_end(args);
}



// -----------------------------------------------------------------------------
//
// trimSlash - http://host:1026/ -> http://host:1026
//
static void trimSlash(char* url)
{
  int len = strlen(url);
  while ((len > 0) && (url[len - 1] == '/'))
    url[--len] = 0;
}



// -----------------------------------------------------------------------------
//
// brokerCertPin - --broker-cert: the certificate's SHA-256 pinned (corRestClientTlsPinSet) - the broker's own
// certificate trusted as it is: self-signed, its name not the one corCep uses (a home hub's, at localhost)
//
static bool brokerCertPin(const char* path, char* why, int whyLen)
{
  FILE* fP = fopen(path, "r");

  if (fP == NULL)
  {
    snprintf(why, whyLen, "--broker-cert %s - cannot be read", path);
    return false;
  }

  X509* certP = PEM_read_X509(fP, NULL, NULL, NULL);
  fclose(fP);

  if (certP == NULL)
  {
    snprintf(why, whyLen, "--broker-cert %s - no PEM certificate in it", path);
    return false;
  }

  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int  mdLen = 0;
  char          hex[2 * EVP_MAX_MD_SIZE + 1];

  X509_digest(certP, EVP_sha256(), md, &mdLen);
  X509_free(certP);

  for (unsigned int ix = 0; ix < mdLen; ix++)
    snprintf(&hex[ix * 2], 3, "%02x", md[ix]);

  if (corRestClientTlsPinSet(hex) == false)
  {
    snprintf(why, whyLen, "--broker-cert %s - its fingerprint not taken", path);
    return false;
  }

  logLine("the broker's certificate pinned: %.16s... (%s)", hex, path);
  return true;
}



// -----------------------------------------------------------------------------
//
// stopSignal - SIGTERM, SIGINT: noted; the engine stops at its next turn (a second at most)
//
static void stopSignal(int sig)
{
  cepStopping = sig;
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(int argC, char* argV[])
{
  if (corArgsInit("corCep", options, "CORCEP_") != CorArgsOk)
  {
    fprintf(stderr, "corCep: corArgsInit failed\n");
    return 1;
  }

  if (corArgsParse(argC, argV) != CorArgsOk)
    return 1;

  if ((strncmp(brokerArg, "http://", 7) != 0) && (strncmp(brokerArg, "https://", 8) != 0))
  {
    fprintf(stderr, "corCep: --broker %s - an http:// or https:// URL\n", brokerArg);
    return 1;
  }
  trimSlash(brokerArg);

  static char notifyBase[512];
  if ((notifyUrlArg != NULL) && (notifyUrlArg[0] != 0))
    snprintf(notifyBase, sizeof(notifyBase), "%s", notifyUrlArg);
  else
    snprintf(notifyBase, sizeof(notifyBase), "http://127.0.0.1:%d", portArg);
  trimSlash(notifyBase);

  cep.broker     = brokerArg;
  cep.tenant     = ((tenantArg != NULL) && (tenantArg[0] != 0)) ? tenantArg : NULL;
  cep.notifyBase = notifyBase;
  cep.port       = (unsigned short) portArg;

  char why[1024];

  if (tokenInit(tokenEnvArg, tokenCmdArg, why, sizeof(why)) == false)
  {
    fprintf(stderr, "corCep: %s\n", why);
    return 1;
  }

  if ((brokerCertArg != NULL) && (brokerCertPin(brokerCertArg, why, sizeof(why)) == false))
  {
    fprintf(stderr, "corCep: %s\n", why);
    return 1;
  }

  //
  // The keys: --jwks (/dry-run) and --author-jwks (the rules' signatures) - one set, refreshed together
  //
  bool pdpOn = (pdpArg != NULL) && (pdpArg[0] != 0);
  if (pdpOn != ((authorJwksArg != NULL) && (authorJwksArg[0] != 0)))
  {
    fprintf(stderr, "corCep: --pdp and --author-jwks go together (and --author-issuer)\n");
    return 1;
  }
  if (authorInit(pdpArg, authorIssuerArg, why, sizeof(why)) == false)
  {
    fprintf(stderr, "corCep: %s\n", why);
    return 1;
  }

#if CEP_WITH_DRY_RUN == 0
  if ((jwksArg != NULL) && (jwksArg[0] != 0))
  {
    fprintf(stderr, "corCep: --jwks - this corCep is built without /dry-run (DRY_RUN=0)\n");
    return 1;
  }
#endif

  if (keysInit(jwksArg, authorJwksArg, jwksRefreshArg, why, sizeof(why)) == false)
  {
    fprintf(stderr, "corCep: %s\n", why);
    return 1;
  }

#if CEP_WITH_DRY_RUN
  if ((jwksArg != NULL) && (jwksArg[0] != 0) && (dryRunInit(jwksArg, issuerArg, audienceArg, rolesClaimArg, editorRoleArg, why, sizeof(why)) == false))
  {
    fprintf(stderr, "corCep: %s\n", why);
    return 1;
  }
#endif

  if (pdpOn)
    logLine("the authors' rights checked: %s asked, rules signed by %s (keys: %s)", pdpArg, authorIssuerArg, authorJwksArg);

  if ((stateArg != NULL) && (stateArg[0] != 0))
    stateFile = stateArg;

  //
  // SIGTERM, SIGINT: the engine writes the state and the statistics, then stops
  //
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = stopSignal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT,  &sa, NULL);

  //
  // The secret in every notification path - made here, never written anywhere
  //
  unsigned char rnd[16];
  if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd))
  {
    fprintf(stderr, "corCep: no random bytes for the notification secret\n");
    return 1;
  }
  for (int ix = 0; ix < 16; ix++)
    snprintf(&cep.secret[ix * 2], 3, "%02x", rnd[ix]);

  corRestClientInit(8, 30, "corCep/" CORCEP_VERSION);

  logLine("corCep %s - broker %s, tenant %s, notifications on port %d (%s)", CORCEP_VERSION, cep.broker,
          (cep.tenant != NULL) ? cep.tenant : "(default)", cep.port, cep.notifyBase);

  if (listenerStart() == false)
    return 1;

  if (subsInit() == false)
    return 1;

  if (senderStart(retryForArg) == false)
  {
    logLine("the actions' sender could not be started");
    return 1;
  }

  engineRun();
  return 0;
}
