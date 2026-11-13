// ============================================================
//  LifeClock.h  -  DeskBot Life Clock: pure logic (no Arduino, no display)
//
//  Everything here works on plain integers, so it can be unit-tested on a PC.
//  Rendering (OLED), storage (EEPROM) and the web page live in DeskBot.ino and
//  only call these functions.
//
//  Design rule: the reference lifespan is a visualisation assumption that the
//  user can change. It is NOT a prediction of anyone's lifespan.
// ============================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

#define LIFE_MAX_MILESTONES    5
#define LIFE_NAME_LEN          20          // 19 characters + NUL
#define LIFE_DEFAULT_LIFESPAN  80
#define LIFE_MIN_LIFESPAN      30
#define LIFE_MAX_LIFESPAN      120
#define LIFE_MIN_YEAR          1900
#define LIFE_MAX_YEAR          2100
#define LIFE_DEFAULT_TZ_MIN    210         // UTC+03:30
#define LIFE_MIN_TZ_MIN        (-720)
#define LIFE_MAX_TZ_MIN        840
#define LIFE_OPT_SHOW_DAYS     0x01
#define LIFE_NO_TODAY          INT32_MIN   // "the current date is not known yet"

#define LIFE_STORE_MAGIC       0x4C        // 'L'
#define LIFE_STORE_VERSION     1
#define LIFE_STORE_SIZE        137         // see lifeEncode()

// ---- structures from the specification ----
struct LifeClockConfig {
  uint16_t birthYear;                      // 0 = not set
  uint8_t  birthMonth;
  uint8_t  birthDay;
  uint8_t  referenceLifespan;              // years
  bool     enabled;
};

struct LifeMilestone {
  char     name[LIFE_NAME_LEN];
  uint16_t year;                           // 0 = empty slot
  uint8_t  month;
  uint8_t  day;
  bool     completed;
};

// Everything that is stored in EEPROM
struct LifeData {
  LifeClockConfig cfg;
  int16_t         tzMinutes;               // UTC offset in minutes, multiple of 15
  uint8_t         opts;                    // LIFE_OPT_* bit flags
  LifeMilestone   ms[LIFE_MAX_MILESTONES];
};

// ---- calculation result ----
#define LIFE_OK            0
#define LIFE_DISABLED      1
#define LIFE_NOT_SET       2               // no valid birth date yet
#define LIFE_NO_DATE       3               // today's date is unknown (no NTP, no cache)
#define LIFE_BIRTH_FUTURE  4               // birth date is after today
#define LIFE_BEYOND        5               // today is past the reference end date

struct LifeStats {
  uint8_t  state;                          // LIFE_*
  int32_t  today, birth, end;              // day numbers (days since 1970-01-01)
  uint16_t ageYears;
  uint8_t  ageMonths, ageDays;
  int32_t  elapsedDays, totalDays, remainingDays;
  uint32_t weeksElapsed;                   // completed weeks = elapsed days / 7
  uint32_t weeksRemaining;                 // remaining days / 7 (never negative)
  uint32_t weeksTotal;                     // weeks in the reference lifespan
  uint16_t permille;                       // 0..1000 of the reference period elapsed
  uint8_t  lifeYear;                       // row (0-based) that holds the current week
};

// ---- calendar arithmetic (proleptic Gregorian, real leap years) ----
bool     lifeIsLeap(int y);
uint8_t  lifeDaysInMonth(int y, int m);
bool     lifeDateValid(int y, int m, int d);               // 1900..2100
int32_t  lifeDayNumber(int y, int m, int d);               // days since 1970-01-01
void     lifeCivil(int32_t day, int* y, int* m, int* d);
int32_t  lifeAddMonths(int y, int m, int d, int months);   // day clamps to month length
int32_t  lifeAddYears(int y, int m, int d, int years);     // Feb 29 -> Feb 28 in common years
int      lifeWeekday(int32_t day);                         // 0 = Sunday

// ---- configuration ----
void     lifeDefaults(LifeData* L);
bool     lifeConfigValid(const LifeClockConfig* c);
bool     lifeMsValid(const LifeMilestone* m);
void     lifeSanitizeName(char* dst /*LIFE_NAME_LEN*/, const char* src);

// ---- calculations ----
void     lifeCompute(const LifeData* L, int32_t today, LifeStats* s);

// Weeks start on the birth weekday: week n starts at birth + 7n days.
// Row y of the calendar holds the weeks that START between birthday y and
// birthday y+1, which is 52 or 53 weeks.
uint32_t lifeYearFirstWeek(const LifeClockConfig* c, int year);
uint8_t  lifeYearWeekCount(const LifeClockConfig* c, int year);
#define  LIFE_WEEK_FUTURE   0
#define  LIFE_WEEK_DONE     1
#define  LIFE_WEEK_CURRENT  2
uint8_t  lifeWeekState(const LifeStats* s, uint32_t week);

// ---- milestones ----
uint8_t  lifeSortMilestones(const LifeData* L, uint8_t order[LIFE_MAX_MILESTONES]);   // open ones first, by date
int8_t   lifeNextMilestone(const LifeData* L, int32_t today);                         // -1 if none
void     lifeFormatCountdown(int32_t days, char* out, size_t n);                      // "today", "12d", "3mo", "1y3m", "-5d"

// ---- storage format (independent of struct padding) ----
void     lifeEncode(const LifeData* L, uint8_t out[LIFE_STORE_SIZE]);
bool     lifeDecode(const uint8_t in[LIFE_STORE_SIZE], LifeData* L);   // false = blank/corrupt, L = defaults
