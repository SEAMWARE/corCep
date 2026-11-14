//
// FILE            token.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The bearer token corCep sends to the broker:
//
//   --token-env VAR       the variable's value, as it is - never renewed
//   --token-command CMD   the first line CMD (sh -c) prints - run at start, again before the token expires (a JWT: its
//                         'exp' less 60 s; a token that is no JWT or has no 'exp': every 5 minutes), and when the
//                         broker answers 401. For a broker that takes short-lived tokens only: a home hub's (each
//                         token signed by a key of corCep's own), a Keycloak client's (client credentials).
//
#include <pthread.h>                             // pthread_mutex_t
#include <stdio.h>                               // popen, fgets, pclose
#include <stdlib.h>                              // getenv, strtoll, free
#include <string.h>                              // strlen, strchr, strstr, strdup
#include <time.h>                                // time

#include "corAlloc/corAlloc.h"                   // corAlloc

#include "corCep.h"



// -----------------------------------------------------------------------------
//
// The token - under the mutex
//
static pthread_mutex_t  mutex     = PTHREAD_MUTEX_INITIALIZER;
static char*            token     = NULL;        // malloc'd; NULL: none
static const char*      command   = NULL;        // --token-command; NULL: the token is fixed
static time_t           renewAt   = 0;           // the command run again at or after this



// -----------------------------------------------------------------------------
//
// jwtExp - a JWT's 'exp' (its payload, base64url - decoded here); 0: no JWT, or no 'exp'
//
static long long jwtExp(const char* jwt)
{
  const char* dot1 = strchr(jwt, '.');
  const char* dot2 = (dot1 != NULL) ? strchr(dot1 + 1, '.') : NULL;

  if (dot2 == NULL)
    return 0;

  int   inLen = dot2 - (dot1 + 1);
  char  payload[4096];
  int   outLen = 0;
  int   bits   = 0;
  int   value  = 0;

  for (int ix = 0; (ix < inLen) && (outLen < (int) sizeof(payload) - 1); ix++)
  {
    char c = dot1[1 + ix];
    int  v = ((c >= 'A') && (c <= 'Z')) ? c - 'A' : ((c >= 'a') && (c <= 'z')) ? c - 'a' + 26 : ((c >= '0') && (c <= '9')) ? c - '0' + 52 :
             ((c == '-') || (c == '+')) ? 62 : ((c == '_') || (c == '/')) ? 63 : -1;

    if (v < 0)
      return 0;
    value = (value << 6) | v;
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      payload[outLen++] = (char) ((value >> bits) & 0xFF);
    }
  }
  payload[outLen] = 0;

  const char* expP = strstr(payload, "\"exp\"");
  const char* colon = (expP != NULL) ? strchr(expP + 5, ':') : NULL;

  return (colon != NULL) ? strtoll(colon + 1, NULL, 10) : 0;
}



// -----------------------------------------------------------------------------
//
// commandRun - the command's first line as the token; false: it failed (the token unchanged). Under the mutex
//
static bool commandRun(char* why, int whyLen)
{
  FILE* fP = popen(command, "r");

  if (fP == NULL)
  {
    snprintf(why, whyLen, "--token-command: '%s' could not be run", command);
    return false;
  }

  char line[16 * 1024];
  bool got    = (fgets(line, sizeof(line), fP) != NULL);
  int  status = pclose(fP);

  if (got == true)
    line[strcspn(line, "\r\n")] = 0;

  if ((got == false) || (line[0] == 0) || (status != 0))
  {
    snprintf(why, whyLen, "--token-command: '%s' %s", command, (status != 0) ? "failed" : "printed no token");
    return false;
  }

  free(token);
  token = strdup(line);

  long long exp = jwtExp(token);
  time_t    now = time(NULL);

  renewAt = ((exp > 0) && (exp - 60 > now)) ? (time_t) (exp - 60) : ((exp > now) ? (time_t) exp : now + 300);
  return true;
}



// -----------------------------------------------------------------------------
//
// tokenInit - from the options; false: neither works (why says why)
//
bool tokenInit(const char* envVar, const char* cmd, char* why, int whyLen)
{
  if ((envVar != NULL) && (cmd != NULL))
  {
    snprintf(why, whyLen, "--token-env and --token-command - one of them");
    return false;
  }

  if (envVar != NULL)
  {
    const char* v = getenv(envVar);

    if ((v == NULL) || (v[0] == 0))
    {
      snprintf(why, whyLen, "--token-env %s - the variable is not set", envVar);
      return false;
    }
    token = strdup(v);
    return true;
  }

  if (cmd == NULL)
    return true;

  command = cmd;

  pthread_mutex_lock(&mutex);
  bool ok = commandRun(why, whyLen);
  pthread_mutex_unlock(&mutex);

  return ok;
}



// -----------------------------------------------------------------------------
//
// tokenGet - the token now, a copy in allocP (renewed first when due); NULL: none
//
char* tokenGet(CorAlloc* allocP)
{
  char* copy = NULL;

  pthread_mutex_lock(&mutex);

  if ((command != NULL) && (time(NULL) >= renewAt))
  {
    char why[512];

    if (commandRun(why, sizeof(why)) == false)
    {
      logLine("%s - the old token kept", why);
      renewAt = time(NULL) + 10;                 // not again at once: a broken command would run on every request
    }
  }

  if (token != NULL)
  {
    copy = corAlloc(allocP, strlen(token) + 1);
    strcpy(copy, token);
  }

  pthread_mutex_unlock(&mutex);
  return copy;
}



// -----------------------------------------------------------------------------
//
// tokenRefused - the broker answered 401: with a command, the next tokenGet runs it again. false: nothing to
// renew (a fixed token, or none) - asking again is pointless
//
bool tokenRefused(void)
{
  if (command == NULL)
    return false;

  pthread_mutex_lock(&mutex);
  renewAt = 0;
  pthread_mutex_unlock(&mutex);
  return true;
}
