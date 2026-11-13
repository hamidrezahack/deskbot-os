// ============================================================
//  LifeClock.cpp  -  see LifeClock.h
// ============================================================
#include "LifeClock.h"
#include <string.h>
#include <stdio.h>

// ------------------------------------------------------------
//  Calendar arithmetic
// ------------------------------------------------------------
bool lifeIsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

uint8_t lifeDaysInMonth(int y, int m) {
  static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (m < 1 || m > 12) return 0;
  return (m == 2 && lifeIsLeap(y)) ? 29 : dim[m - 1];
}

bool lifeDateValid(int y, int m, int d) {
  if (y < LIFE_MIN_YEAR || y > LIFE_MAX_YEAR) return false;
  if (m < 1 || m > 12 || d < 1) return false;
  return d <= lifeDaysInMonth(y, m);
}

// Howard Hinnant's days_from_civil / civil_from_days
int32_t lifeDayNumber(int y, int m, int d) {
  y -= (m <= 2);
  const int32_t  era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

void lifeCivil(int32_t z, int* y, int* m, int* d) {
  z += 719468;
  const int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t  yy  = (int32_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp  = (5 * doy + 2) / 153;
  const unsigned dd  = doy - (153 * mp + 2) / 5 + 1;
  const unsigned mm  = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yy + (mm <= 2));
  *m = (int)mm;
  *d = (int)dd;
}

int32_t lifeAddMonths(int y, int m, int d, int months) {
  int total = y * 12 + (m - 1) + months;
  int ny = total / 12, nm = total % 12;
  if (nm < 0) { nm += 12; ny--; }
  nm += 1;
  int dim = lifeDaysInMonth(ny, nm);
  return lifeDayNumber(ny, nm, d < dim ? d : dim);
}

int32_t lifeAddYears(int y, int m, int d, int years) {
  return lifeAddMonths(y, m, d, years * 12);
}

int lifeWeekday(int32_t day) {                 // 1970-01-01 was a Thursday
  int w = (int)((day + 4) % 7);
  return w < 0 ? w + 7 : w;
}

// ------------------------------------------------------------
//  Configuration
// ------------------------------------------------------------
void lifeDefaults(LifeData* L) {
  memset(L, 0, sizeof(*L));
  L->cfg.referenceLifespan = LIFE_DEFAULT_LIFESPAN;
  L->cfg.enabled           = true;
  L->tzMinutes             = LIFE_DEFAULT_TZ_MIN;
}

bool lifeConfigValid(const LifeClockConfig* c) {
  return lifeDateValid(c->birthYear, c->birthMonth, c->birthDay) &&
         c->referenceLifespan >= LIFE_MIN_LIFESPAN &&
         c->referenceLifespan <= LIFE_MAX_LIFESPAN;
}

bool lifeMsValid(const LifeMilestone* m) {
  return m->year != 0 && m->name[0] != 0 && lifeDateValid(m->year, m->month, m->day);
}

void lifeSanitizeName(char* dst, const char* src) {
  char tmp[LIFE_NAME_LEN];
  size_t o = 0;
  bool lastSpace = true;                       // also trims leading spaces
  for (; src && *src && o < LIFE_NAME_LEN - 1; src++) {
    unsigned char c = (unsigned char)*src;
    if (c < 32 || c > 126) continue;           // control chars and non-ASCII: the OLED font is ASCII only
    if (c == '"')  c = '\'';                   // keep the JSON on the web API trivial
    if (c == '\\') c = '/';
    if (c == ' ') { if (lastSpace) continue; lastSpace = true; } else lastSpace = false;
    tmp[o++] = (char)c;
  }
  while (o > 0 && tmp[o - 1] == ' ') o--;
  memset(dst, 0, LIFE_NAME_LEN);
  memcpy(dst, tmp, o);
}

// ------------------------------------------------------------
//  Calculations
// ------------------------------------------------------------
uint32_t lifeYearFirstWeek(const LifeClockConfig* c, int year) {
  if (year <= 0) return 0;
  int32_t birth = lifeDayNumber(c->birthYear, c->birthMonth, c->birthDay);
  int32_t anniv = lifeAddYears(c->birthYear, c->birthMonth, c->birthDay, year);
  return (uint32_t)((anniv - birth + 6) / 7);  // first week that starts on/after the birthday
}

uint8_t lifeYearWeekCount(const LifeClockConfig* c, int year) {
  return (uint8_t)(lifeYearFirstWeek(c, year + 1) - lifeYearFirstWeek(c, year));
}

void lifeCompute(const LifeData* L, int32_t today, LifeStats* s) {
  memset(s, 0, sizeof(*s));
  s->today = today;
  const LifeClockConfig* c = &L->cfg;

  if (!c->enabled)            { s->state = LIFE_DISABLED; return; }
  if (!lifeConfigValid(c))    { s->state = LIFE_NOT_SET;  return; }

  s->birth      = lifeDayNumber(c->birthYear, c->birthMonth, c->birthDay);
  s->end        = lifeAddYears(c->birthYear, c->birthMonth, c->birthDay, c->referenceLifespan);
  s->totalDays  = s->end - s->birth;
  s->weeksTotal = (uint32_t)((s->totalDays + 6) / 7);

  if (today == LIFE_NO_TODAY) { s->state = LIFE_NO_DATE;      return; }
  if (today < s->birth)       { s->state = LIFE_BIRTH_FUTURE; return; }

  s->elapsedDays   = today - s->birth;
  s->remainingDays = (today < s->end) ? s->end - today : 0;
  s->weeksElapsed  = (uint32_t)(s->elapsedDays / 7);
  s->weeksRemaining = (uint32_t)(s->remainingDays / 7);
  int64_t pm = (int64_t)s->elapsedDays * 1000 / s->totalDays;
  s->permille = (uint16_t)(pm > 1000 ? 1000 : pm);
  s->state    = (today >= s->end) ? LIFE_BEYOND : LIFE_OK;

  // age in whole months, then leftover days (anniversary day clamps to month length)
  int ty, tm, td;
  lifeCivil(today, &ty, &tm, &td);
  int months = (ty * 12 + tm) - (c->birthYear * 12 + c->birthMonth);
  int32_t anniv = lifeAddMonths(c->birthYear, c->birthMonth, c->birthDay, months);
  if (today < anniv) { months--; anniv = lifeAddMonths(c->birthYear, c->birthMonth, c->birthDay, months); }
  s->ageYears  = (uint16_t)(months / 12);
  s->ageMonths = (uint8_t)(months % 12);
  s->ageDays   = (uint8_t)(today - anniv);

  // which calendar row holds the current week
  uint8_t row = c->referenceLifespan - 1;
  for (int y = 0; y < c->referenceLifespan; y++) {
    if (s->weeksElapsed < lifeYearFirstWeek(c, y + 1)) { row = (uint8_t)y; break; }
  }
  s->lifeYear = row;
}

uint8_t lifeWeekState(const LifeStats* s, uint32_t week) {
  if (s->state != LIFE_OK && s->state != LIFE_BEYOND) return LIFE_WEEK_FUTURE;
  if (week < s->weeksElapsed)  return LIFE_WEEK_DONE;
  if (week == s->weeksElapsed && s->state == LIFE_OK) return LIFE_WEEK_CURRENT;
  return LIFE_WEEK_FUTURE;
}

// ------------------------------------------------------------
//  Milestones
// ------------------------------------------------------------
static int32_t msDay(const LifeMilestone* m) { return lifeDayNumber(m->year, m->month, m->day); }

// sort key: open milestones first, then completed ones; each group by date
static int64_t msKey(const LifeMilestone* m) {
  return (int64_t)(m->completed ? 1 : 0) * 100000000LL + msDay(m);
}

uint8_t lifeSortMilestones(const LifeData* L, uint8_t order[LIFE_MAX_MILESTONES]) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < LIFE_MAX_MILESTONES; i++)
    if (lifeMsValid(&L->ms[i])) order[n++] = i;
  for (uint8_t a = 1; a < n; a++) {            // insertion sort (stable, at most 5 items)
    uint8_t v = order[a];
    int b = a - 1;
    while (b >= 0 && msKey(&L->ms[order[b]]) > msKey(&L->ms[v])) {
      order[b + 1] = order[b];
      b--;
    }
    order[b + 1] = v;
  }
  return n;
}

int8_t lifeNextMilestone(const LifeData* L, int32_t today) {
  int8_t best = -1;
  int32_t bestDay = 0;
  if (today == LIFE_NO_TODAY) return -1;
  for (uint8_t i = 0; i < LIFE_MAX_MILESTONES; i++) {
    const LifeMilestone* m = &L->ms[i];
    if (!lifeMsValid(m) || m->completed) continue;
    int32_t d = msDay(m);
    if (d < today) continue;
    if (best < 0 || d < bestDay) { best = (int8_t)i; bestDay = d; }
  }
  return best;
}

void lifeFormatCountdown(int32_t d, char* out, size_t n) {
  if (n == 0) return;
  if (d == 0)        snprintf(out, n, "today");
  else if (d < 0) {
    int32_t a = -d;
    if (a < 100)     snprintf(out, n, "-%ldd", (long)a);
    else             snprintf(out, n, "-%ldmo", (long)(a / 30));
  }
  else if (d < 30)   snprintf(out, n, "%ldd", (long)d);
  else if (d < 365)  snprintf(out, n, "%ldmo", (long)(d / 30));
  else {
    long y = (long)(d / 365), mo = (long)((d % 365) / 30);
    if (mo) snprintf(out, n, "%ldy%ldm", y, mo);
    else    snprintf(out, n, "%ldy", y);
  }
}

// ------------------------------------------------------------
//  Storage: explicit little-endian byte layout (137 bytes)
//    0 magic, 1 version, 2-3 birthYear, 4 month, 5 day, 6 lifespan, 7 enabled,
//    8-9 tz minutes, 10 options, 11.. five milestones x 25 bytes
//    (20 name, 2 year, month, day, completed), last byte = CRC-8
// ------------------------------------------------------------
static uint8_t crc8(const uint8_t* p, size_t n) {
  uint8_t c = 0xA5;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (uint8_t b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

void lifeEncode(const LifeData* L, uint8_t out[LIFE_STORE_SIZE]) {
  memset(out, 0, LIFE_STORE_SIZE);
  size_t i = 0;
  out[i++] = LIFE_STORE_MAGIC;
  out[i++] = LIFE_STORE_VERSION;
  out[i++] = (uint8_t)(L->cfg.birthYear & 0xFF);
  out[i++] = (uint8_t)(L->cfg.birthYear >> 8);
  out[i++] = L->cfg.birthMonth;
  out[i++] = L->cfg.birthDay;
  out[i++] = L->cfg.referenceLifespan;
  out[i++] = L->cfg.enabled ? 1 : 0;
  out[i++] = (uint8_t)((uint16_t)L->tzMinutes & 0xFF);
  out[i++] = (uint8_t)((uint16_t)L->tzMinutes >> 8);
  out[i++] = L->opts;
  for (int k = 0; k < LIFE_MAX_MILESTONES; k++) {
    const LifeMilestone* m = &L->ms[k];
    memcpy(out + i, m->name, LIFE_NAME_LEN); i += LIFE_NAME_LEN;
    out[i++] = (uint8_t)(m->year & 0xFF);
    out[i++] = (uint8_t)(m->year >> 8);
    out[i++] = m->month;
    out[i++] = m->day;
    out[i++] = m->completed ? 1 : 0;
  }
  out[LIFE_STORE_SIZE - 1] = crc8(out, LIFE_STORE_SIZE - 1);
}

bool lifeDecode(const uint8_t in[LIFE_STORE_SIZE], LifeData* L) {
  lifeDefaults(L);
  if (in[0] != LIFE_STORE_MAGIC || in[1] != LIFE_STORE_VERSION) return false;
  if (in[LIFE_STORE_SIZE - 1] != crc8(in, LIFE_STORE_SIZE - 1))  return false;

  size_t i = 2;
  L->cfg.birthYear         = (uint16_t)(in[i] | (in[i + 1] << 8)); i += 2;
  L->cfg.birthMonth        = in[i++];
  L->cfg.birthDay          = in[i++];
  L->cfg.referenceLifespan = in[i++];
  L->cfg.enabled           = in[i++] != 0;
  int16_t tz               = (int16_t)(uint16_t)(in[i] | (in[i + 1] << 8)); i += 2;
  L->opts                  = in[i++] & LIFE_OPT_SHOW_DAYS;

  // validate every field: a bad value falls back to its default, never crashes the UI
  if (L->cfg.referenceLifespan < LIFE_MIN_LIFESPAN || L->cfg.referenceLifespan > LIFE_MAX_LIFESPAN)
    L->cfg.referenceLifespan = LIFE_DEFAULT_LIFESPAN;
  if (!lifeDateValid(L->cfg.birthYear, L->cfg.birthMonth, L->cfg.birthDay)) {
    L->cfg.birthYear = 0; L->cfg.birthMonth = 0; L->cfg.birthDay = 0;
  }
  if (tz >= LIFE_MIN_TZ_MIN && tz <= LIFE_MAX_TZ_MIN && tz % 15 == 0) L->tzMinutes = tz;

  for (int k = 0; k < LIFE_MAX_MILESTONES; k++) {
    LifeMilestone* m = &L->ms[k];
    char raw[LIFE_NAME_LEN];
    memcpy(raw, in + i, LIFE_NAME_LEN); i += LIFE_NAME_LEN;
    raw[LIFE_NAME_LEN - 1] = 0;
    lifeSanitizeName(m->name, raw);
    m->year      = (uint16_t)(in[i] | (in[i + 1] << 8)); i += 2;
    m->month     = in[i++];
    m->day       = in[i++];
    m->completed = in[i++] != 0;
    if (!lifeMsValid(m)) memset(m, 0, sizeof(*m));
  }
  return true;
}
