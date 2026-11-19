//
// FILE            util.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Helpers of the engine and the transports alike: a URL-encoded value, a tree as JSON text, NGSI-LD's
// "tenant not there yet"
//
#include <stdarg.h>                              // va_list
#include <stdio.h>                               // fprintf, vsnprintf
#include <string.h>                              // strlen, strstr
#include <time.h>                                // clock_gettime, localtime_r, strftime

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonRender.h"               // corJsonRender
#include "corJson/corJsonRenderSize.h"           // corJsonRenderSize
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corCep.h"



// -----------------------------------------------------------------------------
//
// The session and the stop - set by the host: corCep.c's main, or the plugin
//
CepSession            cep;
volatile sig_atomic_t cepStopping = 0;
void                  (*cepLogHook)(const char* line) = NULL;   // NULL: stderr, a timestamp first (corCep.h)



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
// jsonText - a tree as JSON text, in allocP
//
char* jsonText(CorAlloc* allocP, CorNode* nodeP)
{
  CorJson json;
  corJsonCreate(&json, allocP);

  char* text = corAlloc(allocP, corJsonRenderSize(&json, nodeP) + 1);
  corJsonRender(&json, nodeP, text);
  return text;
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



// -----------------------------------------------------------------------------
//
// logLine - stderr, a timestamp first; or the host's log (cepLogHook - the plugin: coraine's)
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

  if (cepLogHook != NULL)
  {
    char line[2048];

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    cepLogHook(line);
    return;
  }

  va_start(args, fmt);
  fprintf(stderr, "%s.%03ld ", stamp, ts.tv_nsec / 1000000);
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  va_end(args);
}
