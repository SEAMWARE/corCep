//
// FILE            corCep.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corCep - an NGSI-LD complex event processor (doc/design.md)
//
//   corCep --broker http://host:port [--port 7602] [--notify-url http://host:7602] [--tenant T] [--token-env VAR]
//
#include <stdarg.h>                              // va_list
#include <stdio.h>                               // fprintf, vfprintf
#include <stdlib.h>                              // getenv
#include <string.h>                              // strlen, strncmp
#include <sys/random.h>                          // getrandom
#include <time.h>                                // clock_gettime, localtime_r

#include "corArgs/corArgs.h"                     // corArgsInit, corArgsParse
#include "corArgs/CorArg.h"                      // CorArg
#include "corRest/corRestClient.h"               // corRestClientInit

#include "corCep.h"



// -----------------------------------------------------------------------------
//
// Globals
//
CepSession cep;

static char* brokerArg    = NULL;
static char* tenantArg    = NULL;
static char* tokenEnvArg  = NULL;
static char* notifyUrlArg = NULL;
static int   portArg      = 7602;



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
  cep.token      = NULL;
  cep.notifyBase = notifyBase;
  cep.port       = (unsigned short) portArg;

  if (tokenEnvArg != NULL)
  {
    cep.token = getenv(tokenEnvArg);
    if ((cep.token == NULL) || (cep.token[0] == 0))
    {
      fprintf(stderr, "corCep: --token-env %s - the variable is not set\n", tokenEnvArg);
      return 1;
    }
  }

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

  engineRun();
  return 0;
}
