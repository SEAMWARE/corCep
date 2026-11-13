//
// FILE            schedule.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The schedules (doc/design.md § 3.1): when a 'cron' or an 'at' rule fires next. The host's local time (TZ),
// daylight saving included - as cron itself.
//
//   cron   "<min> <hour> <day of month> <month> <day of week>", or six fields with the second first. Each field: *,
//          n, a-b, with an optional /step, comma-separated lists; months JAN-DEC and days SUN-SAT by name too;
//          day of week 0-7 (0 and 7: Sunday). Day of month and day of week both given (neither *): EITHER fires,
//          as cron's.
//   at     "HH:MM" or "HH:MM:SS" - every day; "sunrise" / "sunset" - at the Rule entity's 'location' (the sunrise
//          equation, the sun's upper edge at the horizon with refraction - within a minute); 'offset' moves it.
//
#include <ctype.h>                               // toupper, isdigit
#include <math.h>                                // sin, cos, acos, asin, fmod, floor
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // strtol
#include <string.h>                              // strchr, strncmp, strlen
#include <time.h>                                // time_t, struct tm, mktime, localtime_r

#include "corCep.h"



// -----------------------------------------------------------------------------
//
// cronField - one field into a bit set; false: not right (why says why)
//
static bool cronField(const char* text, int lo, int hi, const char* const* names, unsigned long long* bitsP, bool* starP, const char* what, char* why, int whyLen)
{
  char buf[128];

  snprintf(buf, sizeof(buf), "%s", text);
  *bitsP = 0;
  *starP = (strcmp(buf, "*") == 0);

  for (char* item = strtok(buf, ","); item != NULL; item = strtok(NULL, ","))
  {
    int   from;
    int   to;
    int   step  = 1;
    char* slash = strchr(item, '/');

    if (slash != NULL)
    {
      char* end;
      *slash = 0;
      step   = (int) strtol(slash + 1, &end, 10);
      if ((*end != 0) || (step < 1) || (slash[1] == 0))
        goto bad;
    }

    if (strcmp(item, "*") == 0)
    {
      from = lo;
      to   = hi;
    }
    else
    {
      char* dash = strchr(item, '-');
      char* parts[2] = { item, NULL };
      int   v[2];

      if (dash != NULL)
      {
        *dash    = 0;
        parts[1] = dash + 1;
      }

      for (int px = 0; px < 2; px++)
      {
        char* p = parts[px];

        if (p == NULL)
        {
          v[px] = v[0];
          continue;
        }

        if (isdigit((unsigned char) p[0]))
        {
          char* end;
          v[px] = (int) strtol(p, &end, 10);
          if (*end != 0)
            goto bad;
        }
        else
        {
          v[px] = -1;
          for (int nx = 0; (names != NULL) && (names[nx] != NULL); nx++)
          {
            if ((strlen(p) == 3) && (toupper((unsigned char) p[0]) == names[nx][0]) && (toupper((unsigned char) p[1]) == names[nx][1]) && (toupper((unsigned char) p[2]) == names[nx][2]))
              v[px] = nx + ((lo == 1) ? 1 : 0);  // months from 1, days from 0
          }
          if (v[px] == -1)
            goto bad;
        }
      }

      from = v[0];
      to   = v[1];
      if ((slash != NULL) && (dash == NULL))     // "5/15": from 5 to the end, every 15
        to = hi;
    }

    if ((from < lo) || (to > hi) || (from > to))
      goto bad;

    for (int ix = from; ix <= to; ix += step)
      *bitsP |= 1ULL << ix;
  }

  if (*bitsP == 0)
    goto bad;
  return true;

 bad:
  snprintf(why, whyLen, "'cron': the %s field '%s' - *, n, a-b, a list, /step; %d to %d", what, text, lo, hi);
  return false;
}



// -----------------------------------------------------------------------------
//
// cronParse - "<min> <hour> <dom> <month> <dow>" or "<sec> <min> <hour> <dom> <month> <dow>"
//
bool cronParse(const char* text, CepCron* cP, char* why, int whyLen)
{
  static const char* const months[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC", NULL };
  static const char* const days[]   = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT", NULL };
  char  buf[256];
  char* fieldV[7];
  int   fields = 0;
  bool  star;

  snprintf(buf, sizeof(buf), "%s", text);
  for (char* f = strtok(buf, " \t"); f != NULL; f = strtok(NULL, " \t"))
  {
    if (fields == 7)
      break;
    fieldV[fields++] = f;
  }

  if ((fields != 5) && (fields != 6))
  {
    snprintf(why, whyLen, "'cron' - 5 fields (minute hour day month weekday), or 6 with the second first: '%s'", text);
    return false;
  }

  //
  // strtok again inside cronField: the fields are copied out first
  //
  char copyV[6][128];
  for (int ix = 0; ix < fields; ix++)
    snprintf(copyV[ix], sizeof(copyV[ix]), "%s", fieldV[ix]);

  int f = 0;
  if (fields == 6)
  {
    if (cronField(copyV[f++], 0, 59, NULL, &cP->second, &star, "second", why, whyLen) == false)
      return false;
  }
  else
    cP->second = 1ULL;                           // second 0

  unsigned long long dow;

  if (cronField(copyV[f++], 0, 59, NULL,   &cP->minute, &star,         "minute",       why, whyLen) == false) return false;
  if (cronField(copyV[f++], 0, 23, NULL,   &cP->hour,   &star,         "hour",         why, whyLen) == false) return false;
  if (cronField(copyV[f++], 1, 31, NULL,   &cP->mday,   &cP->mdayStar, "day of month", why, whyLen) == false) return false;
  if (cronField(copyV[f++], 1, 12, months, &cP->month,  &star,         "month",        why, whyLen) == false) return false;
  if (cronField(copyV[f++], 0, 7,  days,   &dow,        &cP->wdayStar, "day of week",  why, whyLen) == false) return false;

  if (dow & (1ULL << 7))                         // 7: Sunday, as 0
    dow = (dow | 1ULL) & ~(1ULL << 7);
  cP->wday = dow;

  return true;
}



// -----------------------------------------------------------------------------
//
// cronNext - the first time after <after> the cron matches; 0: none within 5 years ("0 0 30 2 *")
//
time_t cronNext(const CepCron* cP, time_t after)
{
  struct tm tm;
  time_t    t = after + 1;

  localtime_r(&t, &tm);
  int lastYear = tm.tm_year + 5;

  for (int loops = 0; loops < 1000000; loops++)
  {
    if (tm.tm_year > lastYear)
      return 0;

    bool mdayOk = (cP->mday & (1ULL << tm.tm_mday)) != 0;
    bool wdayOk = (cP->wday & (1ULL << tm.tm_wday)) != 0;
    bool dayOk  = (cP->mdayStar || cP->wdayStar) ? (mdayOk && wdayOk) : (mdayOk || wdayOk);

    if ((cP->month & (1ULL << (tm.tm_mon + 1))) == 0)
    {
      tm.tm_mon += 1; tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    }
    else if (dayOk == false)
    {
      tm.tm_mday += 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    }
    else if ((cP->hour & (1ULL << tm.tm_hour)) == 0)
    {
      tm.tm_hour += 1; tm.tm_min = 0; tm.tm_sec = 0;
    }
    else if ((cP->minute & (1ULL << tm.tm_min)) == 0)
    {
      tm.tm_min += 1; tm.tm_sec = 0;
    }
    else if ((cP->second & (1ULL << tm.tm_sec)) == 0)
      tm.tm_sec += 1;
    else
    {
      tm.tm_isdst = -1;
      t = mktime(&tm);
      if (t > after)
        return t;
      tm.tm_sec += 1;                            // a time repeated by daylight saving: the next one
    }

    tm.tm_isdst = -1;
    t = mktime(&tm);                             // normalised: 60 s, day 32, an hour daylight saving skips
    localtime_r(&t, &tm);
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// sunTime - sunrise (rise) or sunset of day <n> (days since 2000-01-01 12:00 UTC) at lat/lon; 0: none that day
// (polar day or night)
//
// The sunrise equation (as https://en.wikipedia.org/wiki/Sunrise_equation): solar noon from the mean anomaly and
// the equation of the centre, the hour angle at which the sun's upper edge is at the horizon (-0.833°).
//
static time_t sunTime(long n, double lat, double lon, bool rise)
{
  const double rad    = M_PI / 180.0;
  double       jStar  = n - lon / 360.0;
  double       m      = fmod(357.5291 + 0.98560028 * jStar, 360.0);
  double       c      = 1.9148 * sin(m * rad) + 0.0200 * sin(2 * m * rad) + 0.0003 * sin(3 * m * rad);
  double       lambda = fmod(m + c + 180.0 + 102.9372, 360.0);
  double       jTrans = 2451545.0 + jStar + 0.0053 * sin(m * rad) - 0.0069 * sin(2 * lambda * rad);
  double       sinD   = sin(lambda * rad) * sin(23.4397 * rad);
  double       cosD   = cos(asin(sinD));
  double       cosW   = (sin(-0.833 * rad) - sin(lat * rad) * sinD) / (cos(lat * rad) * cosD);

  if ((cosW < -1.0) || (cosW > 1.0))
    return 0;

  double w = acos(cosW) / rad;
  double j = rise ? jTrans - w / 360.0 : jTrans + w / 360.0;

  return (time_t) ((j - 2440587.5) * 86400.0 + 0.5);
}



// -----------------------------------------------------------------------------
//
// atNext - the first time after <after> an 'at' rule fires; 0: none within a year (the sun never rises or sets there)
//
time_t atNext(const CepRule* ruleP, time_t after)
{
  if (ruleP->atKind == CepAtClock)
  {
    struct tm tm;

    for (int days = 0; days < 3; days++)
    {
      time_t t = after;

      localtime_r(&t, &tm);
      tm.tm_mday += days;
      tm.tm_hour  = ruleP->atSecond / 3600;
      tm.tm_min   = (ruleP->atSecond / 60) % 60;
      tm.tm_sec   = ruleP->atSecond % 60;
      tm.tm_isdst = -1;

      time_t at = mktime(&tm) + ruleP->offsetS;
      if (at > after)
        return at;
    }
    return 0;
  }

  long n0 = (long) floor(after / 86400.0 + 2440587.5 - 2451545.0) - 1;

  for (long n = n0; n < n0 + 370; n++)
  {
    time_t t = sunTime(n, ruleP->lat, ruleP->lon, ruleP->atKind == CepAtSunrise);

    if ((t != 0) && (t + ruleP->offsetS > after))
      return t + ruleP->offsetS;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// scheduleNext - when a cron or at rule fires next, after <after>; 0: never
//
time_t scheduleNext(const CepRule* ruleP, time_t after)
{
  return (ruleP->kind == CepPatternCron) ? cronNext(&ruleP->cron, after) : atNext(ruleP, after);
}



// -----------------------------------------------------------------------------
//
// scheduleText - a time as the log shows it: local, with its offset ("2026-11-14 07:00:00 +0100")
//
char* scheduleText(time_t t, char* buf, int bufLen)
{
  struct tm tm;

  localtime_r(&t, &tm);
  strftime(buf, bufLen, "%Y-%m-%d %H:%M:%S %z", &tm);
  return buf;
}
