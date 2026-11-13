#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include "secrets.h"      // WIFI_SSID, WIFI_PASS, OWM_API_KEY, OTA_PASS, API_TOKEN
#include "LifeClock.h"    // Life Clock logic (LifeClock.cpp is compiled with the sketch)

// ============================================================
//  DISPLAY
// ============================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

// ============================================================
//  TYPES used in function signatures. They MUST sit above the first function,
//  because the Arduino IDE inserts auto-generated prototypes right before it.
// ============================================================
#define TOTAL_HABITS 5
#define TOTAL_CARDS 30
#define DAILY_CARDS  5
struct Note { uint16_t freq; uint16_t ms; };      // freq 0 = rest

#define CFG_MAGIC    0xDB
#define CFG_VERSION  3
#define EEPROM_SIZE  512
#define LIFE_EEPROM_ADDR 64            // Life Clock record lives here, with its own magic/version/CRC

struct __attribute__((packed)) Persist {
  uint8_t  magic, version;
  uint8_t  brightness, theme, workMin, breakMin, timeFormat, sleepMin, clockFace;
  uint8_t  adaptive, buzzer;
  uint8_t  units, cityIdx, pirOn, clapOn;
  uint16_t deskMin;
  uint8_t  alarmHour, alarmMin, alarmOn;
  uint8_t  habitDoneMask;
  uint8_t  todaySessions;
  uint8_t  cardDayIdx;
  uint8_t  habitStreak[TOTAL_HABITS];
  uint16_t totalSessions;
  uint32_t totalFocusMin;
  uint32_t lastEpochDay;
  uint8_t  crc;                      // must stay last
};
static_assert(sizeof(Persist) <= LIFE_EEPROM_ADDR, "Persist too big: it would overlap the Life Clock record");
static_assert(LIFE_EEPROM_ADDR + LIFE_STORE_SIZE <= 208, "Life record would overlap the WiFi record");

// Apps only call present(); loop() pushes ONE frame per pass (after the notification banner).
bool inFrame = false, frameDirty = false;
void present() { if (inFrame) frameDirty = true; else display.display(); }



// ============================================================
//  NTP
// ============================================================
WiFiUDP   ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 12600);

// ============================================================
//  WEATHER STATE  (stored metric, converted when displayed)
// ============================================================
const char* const CITIES[] = {"Shiraz","Tehran","Isfahan","Mashhad","Tabriz","Yazd"};
#define CITY_COUNT 6
uint8_t cityIdx = 0;                 // persisted
uint8_t units   = 0;                 // persisted: 0 = C, km/h   1 = F, mph
String  weatherDesc = "";
float   tempC = 0, feelsC = 0, windMs = 0;
int     humidity = 0;
bool    weatherValid = false;
bool    weatherForce = true;         // fetch as soon as WiFi is up
unsigned long lastWeatherAttempt = 0;

int  dispTemp(float c) { return (int)lroundf(units ? c * 9.0f / 5.0f + 32.0f : c); }
int  dispWind()        { return (int)lroundf(units ? windMs * 2.23694f : windMs * 3.6f); }
const char* tempUnit() { return units ? "F" : "C"; }
const char* windUnit() { return units ? "mph" : "km/h"; }

// ============================================================
//  BUTTONS
// ============================================================
#define BTN_UP     D5
#define BTN_DOWN   D6
#define BTN_SELECT D7

// ============================================================
//  HARDWARE MODULE PINS
// ============================================================
#define BUZZER_PIN   D8
#define PIR_PIN      D0
#define MIC_PIN      A0

// ============================================================
//  updateActivity forward (needed early)
// ============================================================
void updateActivity();

bool buzzerEnabled = true;           // persisted (sound code lives in "Part A" below)

// ============================================================
//  PRESENCE (PIR)  -  logic in presenceTask()
// ============================================================
bool     pirEnabled  = false;        // persisted
bool     seenMotion  = false;
bool     presentNow  = false;
bool     autoPaused  = false;
unsigned long lastMotion  = 0;
unsigned long deskLastTick = 0;
uint16_t deskMin = 0;                // minutes at the desk today (persisted lazily)
#define PIR_WARMUP_MS      30000UL   // PIR needs ~30 s to settle after power-up
#define PRESENCE_HOLD_MS  120000UL   // "here" for 2 min after the last movement
#define AUTOPAUSE_MS      180000UL   // pause a running work session after 3 min without movement

// ============================================================
//  ALARM RINGING / SNOOZE
// ============================================================
bool     alarmRinging   = false;
bool     alarmSnoozed   = false;
uint8_t  alarmRingCount = 0;
uint32_t alarmNextRing  = 0;
#define SNOOZE_MIN 5

// ============================================================
//  BATTERY MONITOR
// ============================================================
bool  batteryEnabled = false;
float batteryVoltage = 0.0;
int   batteryPct     = 100;

void readBattery() {
  if (!batteryEnabled) return;
  int raw = analogRead(MIC_PIN);
  batteryVoltage = (raw / 1023.0) * 3.2 * (320.0 / 100.0);
  batteryPct = constrain((int)((batteryVoltage - 3.0) / 1.2 * 100), 0, 100);
}



// ============================================================
//  SETTINGS
// ============================================================
uint8_t brightness   = 180;
uint8_t theme        = 0;
uint8_t workMinutes  = 25;
uint8_t breakMinutes = 5;
uint8_t timeFormat   = 0;
uint8_t sleepMinutes = 10;
uint8_t clockFace    = 0;

// ============================================================
//  SLEEP / SCREENSAVER
// ============================================================
unsigned long lastActivity = 0;
unsigned long sleepTimeout = 600000UL;
#define SCREENSAVER_TIMEOUT 15000UL
bool screensaverActive = false;



// ============================================================
//  POMODORO STATS
// ============================================================
int totalSessions        = 0;
int totalFocusMin        = 0;
int todaySessions        = 0;
int consecutiveCompleted = 0;

// ============================================================
//  POMODORO
// ============================================================
int  workTime      = 25 * 60;
int  breakTime     = 5  * 60;
int  timerValue    = workTime;
bool isWork        = true;
bool isPaused      = false;
bool pomoActive    = false;   // true once a session was started (runs in the background)
bool adaptiveMode  = true;
unsigned long lastPomodoroTick = 0;
int  pomodoroScreen = 0;

// ============================================================
//  STOPWATCH
// ============================================================
unsigned long swStart    = 0;
unsigned long swElapsed  = 0;
bool          swRunning  = false;
unsigned long swLaps[3]  = {0,0,0};
int           swLapCount = 0;

// ============================================================
//  COUNTDOWN TIMER
// ============================================================
int  ctMinutes  = 5;
int  ctTotal    = 300;
bool ctRunning  = false;
bool ctFinished = false;
bool ctSetting  = true;
unsigned long lastCtTick = 0;

// ============================================================
//  ALARM
// ============================================================
int  alarmHour    = 7;
int  alarmMinute  = 0;
bool alarmEnabled = false;
bool alarmFired   = false;
int  alarmSetPos  = 0;

// ============================================================
//  NAVIGATION
// ============================================================
enum Screen { SCR_HOME, SCR_DRAWER, SCR_APP };
Screen currentScreen = SCR_HOME;

#define APP_CLOCK      0
#define APP_WEATHER    1
#define APP_DASHBOARD  2
#define APP_FACE       3
#define APP_FLASHCARD  4
#define APP_HABITS     5
#define APP_LIFE       6
#define APP_PC         7
#define APP_GAME       8
#define APP_SETTINGS   9
#define TOTAL_APPS     10

int drawerIndex = 0;
int activeApp   = -1;

#define CLKSUB_CLOCK      0
#define CLKSUB_POMODORO   1
#define CLKSUB_STOPWATCH  2
#define CLKSUB_TIMER      3
#define CLKSUB_ALARM      4
#define TOTAL_CLKSUB      5

int  clockSubIndex = 0;
bool inClockSub    = false;

#define SET_DISPLAY   0
#define SET_TIME      1
#define SET_POMODORO  2
#define SET_WEATHER   3
#define SET_SYSTEM    4
#define TOTAL_SETCAT  5

int  settingsCatIndex    = 0;
bool inSettingsCat       = false;
int  settingsItemIndex   = 0;
bool editingSettingsItem = false;

// ============================================================
//  FACE / EMO
// ============================================================
#define EMO_HAPPY      0
#define EMO_SAD        1
#define EMO_ANGRY      2
#define EMO_SURPRISED  3
#define EMO_SLEEPY     4
#define EMO_LOVE       5
#define EMO_GLITCH     6
#define EMO_THINKING   7
#define EMO_COUNT      8

int emoIndex = 0;
const char* emoNames[EMO_COUNT] = {
  "HAPPY","SAD","ANGRY","SURPRISED","SLEEPY","LOVE","GLITCH","THINKING"
};

#define EYE_W  28
#define EYE_H  18
#define EYE_R   4
#define LEX    34
#define REX    94
#define EYE_Y  32
#define PUP_R   5

int  blinkFrame      = 0;
unsigned long lastBlinkTime  = 0;
unsigned long nextBlinkDelay = 3000;
int  pupilOffX = 0, pupilOffY = 0;
int  pupilTgtX = 0, pupilTgtY = 0;
unsigned long lastPupilMove = 0;
unsigned long nextPupilMove = 2000;
unsigned long lastGlitchFrame = 0;
int  glitchSeed = 0;
int  zzzX = 100, zzzY = 14;

// ============================================================
//  DASHBOARD + SYSTEM GRAPHS
// ============================================================
unsigned long lastDashUpdate = 0;
int    rssi       = 0;
String ipAddr     = "";
int    freeHeap   = 0;
float  uptimeHours = 0;
int    dashScreen = 0; // 0=stats 1=graphs

#define HIST_LEN 20
int rssiHistory[HIST_LEN] = {0};
int ramHistory[HIST_LEN]  = {0};
int histIndex = 0;
unsigned long lastHistSample = 0;

// ============================================================
//  MIC VISUALIZER
// ============================================================
#define VIZ_BARS  16
#define VIZ_MAX_H 8
int vizBars[VIZ_BARS] = {0};

// microphone analysis (sampled in micTask(), shares A0 with the battery divider)
bool     clapEnabled = true;         // persisted
float    micBase = 512, micEnv = 0, noiseAvg = 0;
uint32_t lastMicSample = 0, lastVizPush = 0, lastClapAt = 0, clapFirstAt = 0;
uint8_t  clapCount = 0;
uint32_t noiseHighSince = 0, lastNoiseWarn = 0;
#define CLAP_MIN_AMP   140           // raise if it triggers on speech, lower if it misses claps
#define NOISE_WARN      60           // average amplitude that counts as "noisy"

// ============================================================
//  FLASHCARDS
// ============================================================
struct Flashcard { const char* word; const char* definition; };
const Flashcard cards[] = {
  {"Ambiguous",  "Open to more than one interpretation"},
  {"Benevolent", "Well-meaning and kindly"},
  {"Coherent",   "Logical and consistent"},
  {"Deliberate", "Done consciously and intentionally"},
  {"Eloquent",   "Fluent or persuasive in speaking"},
  {"Feasible",   "Possible and practical to do"},
  {"Gregarious", "Fond of company, sociable"},
  {"Hypothesis", "A proposed explanation to test"},
  {"Implicit",   "Implied though not plainly expressed"},
  {"Justify",    "Show or prove to be right or reasonable"},
  {"Keen",       "Having a strong interest or enthusiasm"},
  {"Lucid",      "Expressed clearly, easy to understand"},
  {"Meticulous", "Showing great attention to detail"},
  {"Notion",     "A concept or belief about something"},
  {"Objective",  "Not influenced by personal feelings"},
  {"Perceive",   "Become aware of through the senses"},
  {"Quantify",   "Express as a number or amount"},
  {"Relevant",   "Closely connected or appropriate"},
  {"Scrutinize", "Examine or inspect closely"},
  {"Tentative",  "Not certain or fixed, provisional"},
  {"Ubiquitous", "Present everywhere at the same time"},
  {"Validate",   "Check or prove the validity of"},
  {"Wary",       "Feeling cautious about possible danger"},
  {"Exemplify",  "Be a typical example of something"},
  {"Yield",      "Produce or provide something"},
  {"Abstract",   "Existing in thought, not concrete form"},
  {"Bias",       "Unfair preference for one thing"},
  {"Concise",    "Giving much information briefly"},
  {"Diverge",    "Differ in opinion or direction"},
  {"Emphasize",  "Give special importance to something"},
};
int  cardDayIndex  = 0;
int  cardViewIndex = 0;
bool cardFlipped   = false;

// ============================================================
//  HABIT TRACKER
// ============================================================
const char* habitNames[TOTAL_HABITS] = {"Workout","Read","Code","Water","Sleep 8h"};
bool    habitDoneToday[TOTAL_HABITS] = {false,false,false,false,false};
uint8_t habitStreak[TOTAL_HABITS]    = {0,0,0,0,0};
int     habitIndex = 0;



// ============================================================
//  MATRIX SCREENSAVER
// ============================================================
#define MATRIX_COLS 21
int matrixY[MATRIX_COLS];
int matrixSpeed[MATRIX_COLS];

void initMatrix() {
  for (int c=0;c<MATRIX_COLS;c++) {
    matrixY[c]     = random(-40, 0);
    matrixSpeed[c] = random(1, 3);
  }
}

void drawScreensaver() {
  display.clearDisplay();
  for (int c=0;c<MATRIX_COLS;c++) {
    int x = c*6;
    for (int t=0;t<5;t++) {
      int y = matrixY[c] - t*8;
      if (y>=0 && y<64) {
        char ch = 33 + random(0,90);
        display.setTextSize(1);
        display.setCursor(x,y);
        display.write(ch);
      }
    }
    matrixY[c] += matrixSpeed[c];
    if (matrixY[c] > 100) { matrixY[c]=random(-40,-5); matrixSpeed[c]=random(1,3); }
  }
  present();
}

// ============================================================
//  DINO GAME
// ============================================================
float dinoY        = 0;
float dinoVelY      = 0;
bool  dinoJumping    = false;
bool  dinoGameOver   = false;
int   dinoScore      = 0;
float dinoSpeed      = 3.0;
bool  lastSelState   = false;
#define GROUND_Y 50
#define DINO_X 14
#define DINO_SIZE 8
#define OBS_W 5
#define OBS_H 10

struct Obstacle { float x; bool active; };
#define MAX_OBSTACLES 2
Obstacle obstacles[MAX_OBSTACLES];
unsigned long lastObstacleSpawn = 0;
unsigned long nextSpawnGap      = 1500;   // re-rolled only when an obstacle spawns

void resetDinoGame() {
  dinoY = 0; dinoVelY = 0; dinoJumping = false;
  dinoGameOver = false; dinoScore = 0; dinoSpeed = 3.0;
  for (int i=0;i<MAX_OBSTACLES;i++) obstacles[i].active = false;
  lastObstacleSpawn = millis();
  nextSpawnGap = random(1200, 2200);
}

// ============================================================
//  NOTIFICATION
// ============================================================
String        notifMsg    = "";
unsigned long notifStart  = 0;
bool          notifActive = false;
#define NOTIF_DURATION 3000

// ============================================================
//  WEB SERVER
// ============================================================
ESP8266WebServer server(80);

// ============================================================
//  OTA
// ============================================================
bool otaInProgress = false;
int  otaProgress   = 0;

// ============================================================
//  HOME WIDGETS
// ============================================================
int widgetIndex = 0;
unsigned long lastWidgetSwitch = 0;
#define WIDGET_COUNT 5
#define WIDGET_SWITCH_MS 4000

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
const int categoryItemCounts[]={4,2,3,2,5};   // items per settings category (Display,Time,Pomodoro,Weather,System)
void handleButtons();
void drawStatusBar();
void drawHome();
void drawWidget(int idx, int y);
void drawDrawer();
void runActiveApp();
void showNotif(const String& msg);
void drawNotifBanner();
void micTask();
void drawVisualizer(int y, int h);
void drawClockSuiteMenu();
void clockFaceApp();
void drawCasioFace(int h,int m,int s);
void drawAnalogFace(int h,int m,int s);
void drawFlipFace(int h,int m,int s);
void drawMinimalFace(int h,int m,int s);
void pomodoroApp();
void pomodoroTick();
void drawPomodoroHourglass();
void drawHourglass(int cx,int cy,int w,int h,float progress,bool running);
void drawPomodoroStats();
void stopwatchApp();
void countdownApp();
void alarmApp();
void checkAlarm();
void startAlarm();
void stopAlarm();
void snoozeAlarm();
void weatherApp();
void drawWeatherIconLarge(String desc,int x,int y);
bool fetchWeather();
void weatherTask();
void dashboardApp();
void drawDashStats();
void drawSystemGraphs();
void drawDeskEnv();
void sampleHistory();
void presenceTask();
void faceApp();
void drawHappyEyes(int cx,int cy);
void drawEmoEye(int cx,int cy,int px,int py,int eyelidTop,int eyelidBot,bool halfBlink,bool fullBlink,bool heartPupil);
void drawBrow(int cx,int cy,int angle,bool thick);
void drawGlitchEye(int cx,int cy);
void updatePupilDrift();
void updateBlink();
void drawEmoFace(int emo);
void flashcardApp();
void habitApp();
void dinoGameApp();
void settingsApp();
void adjustSettingsItem(int dir);
bool settingsIsAction(int cat,int item);
void runSettingsAction(int cat,int item);
void saveSettings();
void saveHabits();
void loadSettings();
void applyTheme();
void checkDailyReset();
void goToSleep();
void bootAnimation();
void setupOTA();
void setupWebServer();
String formatTime(int h,int m,int s);
void displayMessage(String msg);
void pomoStart();
void pomoToggle();
void setHabit(int i,bool done);
void flushConfig(bool force = false);
void drawBell(int x,int y);
void drawFlame(int x,int y,uint16_t c);
void drawTinyWeather(int x,int y);
void drawBigIcon(int app,int cx,int cy);
void drawRing(int cx,int cy,int r,float frac,int thick);
void drawSettingGraphic(int cat,int item,int y,uint16_t fg,bool editing);

// ============================================================
//  UPDATE 1 / PART A  —  paste in place of the old "EEPROM" block
//  (the saveSettings/loadSettings pair near line 525) and delete:
//    - #define EEPROM_SIZE 48 and its comment block
//    - loadHabits() / saveHabits()
//    - the old beepTone/beepClick/beepSuccess/beepAlarm/beepPomodoDone
//    - the old flashScreen()
//  It needs the globals declared above it (settings, habits, alarm...).
// ============================================================

#define ARRLEN(a) (sizeof(a) / sizeof((a)[0]))

// ------------------------------------------------------------
//  #6  NON-BLOCKING SOUND  (replaces the busy-wait beeps)
// ------------------------------------------------------------

static const Note SND_CLICK[]   = {{1200, 20}};
static const Note SND_SUCCESS[] = {{880, 80}, {0, 60}, {1320, 120}};
static const Note SND_ALARM[]   = {{1000,200},{0,100},{1400,200},{0,100},
                                   {1000,200},{0,100},{1400,200},{0,100},
                                   {1000,200},{0,100},{1400,200},{0,100}};
static const Note SND_POMO[]    = {{523,150},{0,50},{659,150},{0,50},
                                   {784,150},{0,50},{1047,150},{0,50}};

const Note*   sndSeq  = nullptr;
uint8_t       sndLen  = 0, sndPos = 0;
uint32_t      sndNext = 0;
static Note   sndSingle;

void playSound(const Note* seq, uint8_t len) {
  if (!buzzerEnabled) return;
  sndSeq = seq; sndLen = len; sndPos = 0; sndNext = millis();
}

void updateSound() {                     // call every loop()
  if (!sndSeq) return;
  uint32_t now = millis();
  if ((int32_t)(now - sndNext) < 0) return;
  if (sndPos >= sndLen) {                // finished
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    sndSeq = nullptr;
    return;
  }
  const Note& n = sndSeq[sndPos++];
  if (n.freq) tone(BUZZER_PIN, n.freq); else noTone(BUZZER_PIN);
  sndNext = now + n.ms;
}

void beepTone(int freq, int ms) { sndSingle = { (uint16_t)freq, (uint16_t)ms }; playSound(&sndSingle, 1); }
void beepClick()      { if (sndSeq && sndLen > 1) return;   // never cut a melody
                        playSound(SND_CLICK, ARRLEN(SND_CLICK)); }
void beepSuccess()    { playSound(SND_SUCCESS, ARRLEN(SND_SUCCESS)); }
void beepAlarm()      { playSound(SND_ALARM,   ARRLEN(SND_ALARM)); }
void beepPomodoDone() { playSound(SND_POMO,    ARRLEN(SND_POMO)); }

// ------------------------------------------------------------
//  #6  NON-BLOCKING SCREEN FLASH  (was 720 ms of delay())
// ------------------------------------------------------------
uint8_t  flashStep = 0;
uint32_t flashNext = 0;

void flashScreen() { flashStep = 6; flashNext = 0; }

void updateFlash() {                     // call every loop()
  if (!flashStep) return;
  uint32_t now = millis();
  if ((int32_t)(now - flashNext) < 0) return;
  display.invertDisplay(flashStep % 2 == 0);
  flashNext = now + 120;
  if (--flashStep == 0) applyTheme();    // restore the user's theme
}

// small helper to replace delay(30) frame pacing in faceApp / dino
inline bool frameReady(uint32_t& last, uint16_t ms) {
  uint32_t n = millis();
  if (n - last < ms) return false;
  last = n;
  return true;
}

// ------------------------------------------------------------
//  #6  NON-BLOCKING BUTTONS
//  Events (valid for ONE loop pass):
//    evt.up / evt.dn   press, then auto-repeat while held
//    evt.sel           short press, fires on RELEASE
//    evt.selLong       SELECT held >= LONG_MS (fires once, while held)
//    evt.back          UP+DN together (fires once)
// ------------------------------------------------------------
#define DEBOUNCE_MS    25
#define COMBO_GRACE    70     // wait this long so UP+DN isn't seen as UP alone
#define LONG_MS       800
#define REPEAT_DELAY  400
#define REPEAT_MS     150

struct Button {
  uint8_t  pin;
  bool     down;       // debounced state
  bool     raw;        // last raw reading
  uint32_t rawAt;      // when raw last changed
  uint32_t downAt;     // when the debounced press began
  uint32_t lastRep;    // last repeat time
  bool     fired;      // press already produced an event (or was swallowed)
};
struct InputEvents { bool up, dn, sel, selLong, back; };

Button btns[3] = {
  { BTN_UP,     false, false, 0, 0, 0, false },
  { BTN_DOWN,   false, false, 0, 0, 0, false },
  { BTN_SELECT, false, false, 0, 0, 0, false },
};
InputEvents evt = {};
bool comboLatched = false;

void pollButtons() {
  uint32_t now = millis();
  evt = InputEvents{};
  Button& U = btns[0]; Button& D = btns[1]; Button& S = btns[2];

  for (int i = 0; i < 3; i++) {
    Button& b = btns[i];
    bool r = (digitalRead(b.pin) == LOW);
    if (r != b.raw) { b.raw = r; b.rawAt = now; }
    if (r != b.down && now - b.rawAt >= DEBOUNCE_MS) {
      b.down = r;
      if (r) { b.downAt = now; b.lastRep = now; b.fired = false; }
      else if (i == 2 && !b.fired && !comboLatched) evt.sel = true;   // short press, on release
    }
  }

  // any key wakes the screensaver; that press is swallowed
  if (U.down || D.down || S.down) {
    bool wasSaver = screensaverActive;
    updateActivity();
    if (wasSaver) {
      U.fired = D.fired = S.fired = true;
      evt = InputEvents{};
      return;
    }
  }

  // UP+DN combo = back
  if (U.down && D.down) {
    if (!comboLatched) { comboLatched = true; evt.back = true; }
    U.fired = D.fired = S.fired = true;
  } else if (!U.down && !D.down) {
    comboLatched = false;
  }

  // single UP / DN with auto-repeat
  if (!comboLatched) {
    for (int i = 0; i < 2; i++) {
      Button& b = btns[i];
      if (!b.down) continue;
      bool fire = false;
      if (!b.fired) {
        if (!btns[1 - i].down && now - b.downAt >= COMBO_GRACE) { b.fired = true; fire = true; }
      } else if (now - b.downAt >= REPEAT_DELAY && now - b.lastRep >= REPEAT_MS) {
        fire = true;
      }
      if (fire) { b.lastRep = now; if (i == 0) evt.up = true; else evt.dn = true; }
    }
  }

  // SELECT long press
  if (S.down && !S.fired && !comboLatched && now - S.downAt >= LONG_MS) {
    S.fired = true;
    evt.selLong = true;
  }
}

// ------------------------------------------------------------
//  #7  PERSISTENT STORAGE  (versioned + checksum + lazy commit)
// ------------------------------------------------------------

uint32_t lastEpochDay = 0;           // replaces the old `int lastDay`
bool     cfgDirty     = false;
uint32_t cfgDirtyAt   = 0;

uint8_t cfgCrc(const Persist& p) {
  const uint8_t* b = (const uint8_t*)&p;
  uint8_t c = 0xA5;
  for (size_t i = 0; i < sizeof(Persist) - 1; i++) { c ^= b[i]; c = (c << 1) | (c >> 7); }
  return c;
}

void applyDerivedSettings() {
  workTime  = workMinutes  * 60;
  breakTime = breakMinutes * 60;
  sleepTimeout = (sleepMinutes == 0) ? 0xFFFFFFFFUL : (unsigned long)sleepMinutes * 60000UL;
}

// Mark data as changed. Written to flash 3 s later (batches rapid edits,
// saves flash wear). Old call sites keep working unchanged.
void saveSettings() { cfgDirty = true; cfgDirtyAt = millis(); }
void saveHabits()   { saveSettings(); }

void flushConfig(bool force) {           // call every loop(); force=true before sleep/OTA
  if (!cfgDirty) return;
  if (!force && millis() - cfgDirtyAt < 3000) return;
  Persist p = {};
  p.magic = CFG_MAGIC; p.version = CFG_VERSION;
  p.brightness = brightness; p.theme = theme;
  p.workMin = workMinutes;   p.breakMin = breakMinutes;
  p.timeFormat = timeFormat; p.sleepMin = sleepMinutes; p.clockFace = clockFace;
  p.adaptive = adaptiveMode; p.buzzer = buzzerEnabled;
  p.units = units; p.cityIdx = cityIdx; p.pirOn = pirEnabled; p.clapOn = clapEnabled;
  p.deskMin = deskMin;            // saved with other changes, never triggers a write by itself
  p.alarmHour = alarmHour;   p.alarmMin = alarmMinute; p.alarmOn = alarmEnabled;
  p.todaySessions = (uint8_t)constrain(todaySessions, 0, 255);
  p.cardDayIdx = (uint8_t)cardDayIndex;
  p.habitDoneMask = 0;
  for (int i = 0; i < TOTAL_HABITS; i++) {
    p.habitStreak[i] = habitStreak[i];
    if (habitDoneToday[i]) p.habitDoneMask |= (1 << i);
  }
  p.totalSessions = (uint16_t)constrain(totalSessions, 0, 65535);
  p.totalFocusMin = (uint32_t)max(totalFocusMin, 0);
  p.lastEpochDay  = lastEpochDay;
  p.crc = cfgCrc(p);
  EEPROM.put(0, p);
  EEPROM.commit();                                // core skips the write if bytes are unchanged
  cfgDirty = false;
}

void loadSettings() {
  Persist p;
  EEPROM.get(0, p);
  if (p.magic != CFG_MAGIC || p.version != CFG_VERSION || p.crc != cfgCrc(p)) {
    applyDerivedSettings();                       // blank/old/corrupt: keep defaults
    saveSettings();                               // and write a clean record
    return;
  }
  brightness   = p.brightness;
  theme        = p.theme > 1 ? 0 : p.theme;
  workMinutes  = (p.workMin  == 0 || p.workMin  > 90) ? 25 : p.workMin;
  breakMinutes = (p.breakMin == 0 || p.breakMin > 30) ? 5  : p.breakMin;
  timeFormat   = p.timeFormat > 1 ? 0 : p.timeFormat;
  sleepMinutes = (p.sleepMin > 30) ? 10 : p.sleepMin;
  clockFace    = p.clockFace > 3 ? 0 : p.clockFace;
  adaptiveMode = p.adaptive;
  buzzerEnabled = p.buzzer;
  units        = p.units > 1 ? 0 : p.units;
  cityIdx      = p.cityIdx < CITY_COUNT ? p.cityIdx : 0;
  pirEnabled   = p.pirOn;
  clapEnabled  = p.clapOn;
  deskMin      = p.deskMin <= 1440 ? p.deskMin : 0;
  alarmHour    = p.alarmHour   < 24 ? p.alarmHour   : 7;
  alarmMinute  = p.alarmMin    < 60 ? p.alarmMin    : 0;
  alarmEnabled = p.alarmOn;
  todaySessions = p.todaySessions;
  cardDayIndex  = p.cardDayIdx % (TOTAL_CARDS / DAILY_CARDS);
  for (int i = 0; i < TOTAL_HABITS; i++) {
    habitStreak[i]    = p.habitStreak[i];
    habitDoneToday[i] = p.habitDoneMask & (1 << i);
  }
  totalSessions = p.totalSessions;
  totalFocusMin = (p.totalFocusMin > 1000000UL) ? 0 : (int)p.totalFocusMin;
  lastEpochDay  = p.lastEpochDay;
  applyDerivedSettings();
}

// Daily rollover based on the real day number, so it survives reboots,
// deep sleep and multi-day gaps.
void checkDailyReset() {
  uint32_t epoch = timeClient.getEpochTime();
  if (epoch < 1700000000UL) return;               // NTP not synced yet
  uint32_t today = epoch / 86400UL;

  if (lastEpochDay == 0 || today < lastEpochDay) { // first run / clock went backwards
    lastEpochDay = today; saveSettings(); return;
  }
  if (today == lastEpochDay) return;

  uint32_t gap = today - lastEpochDay;
  todaySessions = 0;
  deskMin       = 0;
  cardDayIndex  = (cardDayIndex + gap) % (TOTAL_CARDS / DAILY_CARDS);
  cardViewIndex = 0;
  cardFlipped   = false;
  for (int i = 0; i < TOTAL_HABITS; i++) {
    if (gap > 1 || !habitDoneToday[i]) habitStreak[i] = 0;   // missed a day
    habitDoneToday[i] = false;
  }
  lastEpochDay = today;
  saveSettings();
}

void applyTheme() { display.invertDisplay(theme == 1); }

// ============================================================
//  PART C  -  FEATURES  (#8 settings / #10 presence + mic + alarm)
// ============================================================

// ---------- shared actions (buttons AND web use these) ----------
void pomoStart() {
  timerValue = workTime; isWork = true; isPaused = false;
  pomoActive = true; autoPaused = false; lastPomodoroTick = millis();
}
void pomoToggle() {
  autoPaused = false;
  if (!pomoActive) { pomoStart(); showNotif("Pomodoro started"); return; }
  isPaused = !isPaused;
  showNotif(isPaused ? "Pomodoro paused" : "Pomodoro running");
}
void setHabit(int i, bool done) {
  if (i < 0 || i >= TOTAL_HABITS || habitDoneToday[i] == done) return;
  habitDoneToday[i] = done;
  if (done) { if (habitStreak[i] < 255) habitStreak[i]++; }
  else if (habitStreak[i] > 0) habitStreak[i]--;
  saveHabits();
}

// ---------- pomodoro tick: runs from loop(), whatever screen is open ----------
void pomodoroTick() {
  if (!pomoActive || isPaused) return;
  unsigned long now = millis();
  if (now - lastPomodoroTick < 1000) return;
  lastPomodoroTick += 1000;                       // no drift
  if (now - lastPomodoroTick > 2000) lastPomodoroTick = now;   // long stall: resync
  if (--timerValue > 0) return;

  if (isWork) {
    consecutiveCompleted++; todaySessions++; totalSessions++;
    totalFocusMin += workMinutes; saveSettings();
    breakTime = (adaptiveMode && consecutiveCompleted % 3 == 0) ? breakMinutes*60*3 : breakMinutes*60;
    showNotif("Work done! Break time"); beepPomodoDone();
  } else {
    breakTime = breakMinutes * 60;
    workTime  = (adaptiveMode && consecutiveCompleted >= 3) ? (workMinutes+5)*60 : workMinutes*60;
    showNotif("Break over! Focus up"); beepSuccess();
  }
  isWork = !isWork; timerValue = isWork ? workTime : breakTime;
  flashScreen();
}

// ---------- #10 presence: PIR -> desk time, wake screen, auto-pause ----------
void presenceTask() {
  unsigned long now = millis();
  if (!pirEnabled) { presentNow = false; return; }
  if (now < PIR_WARMUP_MS) return;

  bool moving = (digitalRead(PIR_PIN) == HIGH);
  if (moving) { seenMotion = true; lastMotion = now; updateActivity(); }   // also wakes the screensaver
  presentNow = seenMotion && (now - lastMotion < PRESENCE_HOLD_MS);

  if (presentNow) {                                // desk time, one tick per minute
    if (now - deskLastTick >= 60000UL) { deskLastTick = now; if (deskMin < 1440) deskMin++; }
  } else deskLastTick = now;

  if (pomoActive && isWork && !isPaused && seenMotion && now - lastMotion > AUTOPAUSE_MS) {
    isPaused = true; autoPaused = true; showNotif("Away: pomodoro paused");
  }
  if (autoPaused && moving) {
    isPaused = false; autoPaused = false; showNotif("Welcome back!");
  }
}

// ---------- #10 alarm that keeps ringing until dismissed / snoozed ----------
void startAlarm()  { alarmRinging = true; alarmSnoozed = false; alarmRingCount = 0; alarmNextRing = millis(); }
void stopAlarm()   { alarmRinging = false; alarmSnoozed = false; }
void snoozeAlarm() { alarmRinging = false; alarmSnoozed = true; alarmNextRing = millis() + SNOOZE_MIN * 60000UL;
                     showNotif("Snoozed 5 min"); }

void checkAlarm() {
  uint32_t now = millis();
  if (alarmRinging) {
    if ((int32_t)(now - alarmNextRing) >= 0) {
      if (++alarmRingCount > 15) { stopAlarm(); return; }          // give up after ~60 s
      showNotif(clapEnabled ? "ALARM! clap=snooze" : "ALARM!");
      flashScreen(); beepAlarm();
      alarmNextRing = now + 4000;
    }
    return;
  }
  if (alarmSnoozed) { if ((int32_t)(now - alarmNextRing) >= 0) startAlarm(); return; }
  if (!alarmEnabled || timeClient.getEpochTime() < 1700000000UL) return;   // not synced yet

  int h = timeClient.getHours(), m = timeClient.getMinutes();
  bool editing = (currentScreen == SCR_APP && activeApp == APP_CLOCK && inClockSub && clockSubIndex == CLKSUB_ALARM);
  if (h == alarmHour && m == alarmMinute) {
    if (!alarmFired && !editing) { alarmFired = true; startAlarm(); }
  } else alarmFired = false;
}

// ---------- #10 microphone: level, noise warning, double-clap ----------
void onDoubleClap() {
  if (alarmRinging) snoozeAlarm();
}

void micTask() {
  if (batteryEnabled) return;                      // A0 is wired to the battery divider
  uint32_t now = millis();
  if (now - lastMicSample < 8) return;             // ~125 Hz keeps WiFi happy
  lastMicSample = now;

  int   raw = analogRead(MIC_PIN);
  micBase  += (raw - micBase) * 0.01f;             // follows the mic's DC bias
  float amp = fabsf(raw - micBase);
  micEnv    = (amp > micEnv) ? amp : micEnv * 0.95f + amp * 0.05f;   // fast attack, slow decay
  noiseAvg += (amp - noiseAvg) * 0.003f;           // ~2.5 s average

  if (now - lastVizPush >= 60) {
    lastVizPush = now;
    for (int i = 0; i < VIZ_BARS - 1; i++) vizBars[i] = vizBars[i+1];
    vizBars[VIZ_BARS-1] = constrain(map((int)micEnv, 0, 300, 1, VIZ_MAX_H), 1, VIZ_MAX_H);
  }

  if (clapEnabled) {
    float thr = max((float)CLAP_MIN_AMP, noiseAvg * 4.0f);
    if (amp > thr && now - lastClapAt > 120) {     // 120 ms dead-time ignores the echo
      lastClapAt = now;
      if (clapCount == 1 && now - clapFirstAt <= 700) { clapCount = 0; onDoubleClap(); }
      else { clapCount = 1; clapFirstAt = now; }
    }
    if (clapCount == 1 && now - clapFirstAt > 700) clapCount = 0;
  }

  if (noiseAvg > NOISE_WARN) {
    if (!noiseHighSince) noiseHighSince = now;
    else if (now - noiseHighSince > 8000 && now - lastNoiseWarn > 300000UL) {
      lastNoiseWarn = now; showNotif("Noisy room!");
    }
  } else noiseHighSince = 0;
}

int noisePercent() { return constrain((int)(noiseAvg * 100.0f / 150.0f), 0, 100); }

// ---------- #8 weather: background fetch, units, city ----------
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client; HTTPClient http;
  String url = String("http://api.openweathermap.org/data/2.5/weather?q=") + CITIES[cityIdx]
             + "&appid=" + OWM_API_KEY + "&units=metric";
  http.setTimeout(4000);
  if (!http.begin(client, url)) return false;
  bool ok = false;
  if (http.GET() == 200) {
    StaticJsonDocument<192> filter;
    filter["main"]["temp"] = true; filter["main"]["feels_like"] = true; filter["main"]["humidity"] = true;
    filter["wind"]["speed"] = true; filter["weather"][0]["main"] = true;
    DynamicJsonDocument doc(768);
    if (!deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter)) && doc["main"]["temp"].is<float>()) {
      tempC    = doc["main"]["temp"];
      feelsC   = doc["main"]["feels_like"];
      humidity = doc["main"]["humidity"];
      windMs   = doc["wind"]["speed"];
      weatherDesc = doc["weather"][0]["main"].as<String>();
      weatherValid = ok = true;
    }
  }
  http.end();
  return ok;
}

void weatherTask() {                               // from loop(): the status bar/home/web always have data
  if (currentScreen == SCR_APP && activeApp == APP_GAME) return;
  uint32_t now = millis();
  uint32_t interval = weatherValid ? 600000UL : 60000UL;   // retry cooldown when it fails
  if (!weatherForce && now - lastWeatherAttempt < interval) return;
  weatherForce = false;
  lastWeatherAttempt = now ? now : 1;
  fetchWeather();
}

// ---------- #8 settings actions ----------
bool settingsIsAction(int cat, int item) { return cat == SET_SYSTEM && (item == 0 || item == 3 || item == 4); }

void runSettingsAction(int cat, int item) {
  if (cat != SET_SYSTEM) return;
  if (item == 0) {
    totalSessions = 0; totalFocusMin = 0; todaySessions = 0; consecutiveCompleted = 0;
    saveSettings(); showNotif("Stats reset!"); beepSuccess();
  } else if (item == 3) {
    showNotif("DeskBot OS v4.0");
  } else if (item == 4) {
    wifiStartSetup(true);                          // opens the hotspot and shows how to join it
  }
}

void adjustSettingsItem(int dir) {                 // dir: +1 = UP (increase / next), -1 = DOWN
  switch (settingsCatIndex) {
    case SET_DISPLAY:
      if (settingsItemIndex == 0) { brightness = constrain((int)brightness + dir*20, 20, 255); display.setContrast(brightness); }
      if (settingsItemIndex == 1) { theme = !theme; applyTheme(); }
      if (settingsItemIndex == 2) {
        const uint8_t opts[] = {0, 5, 10, 30}; int cur = 0;
        for (int i = 0; i < 4; i++) if (opts[i] == sleepMinutes) cur = i;
        cur = (cur + dir + 4) % 4; sleepMinutes = opts[cur];
        sleepTimeout = (sleepMinutes == 0) ? 0xFFFFFFFFUL : (unsigned long)sleepMinutes * 60000UL;
      }
      if (settingsItemIndex == 3) buzzerEnabled = !buzzerEnabled;
      break;
    case SET_TIME:
      if (settingsItemIndex == 0) timeFormat = !timeFormat;
      if (settingsItemIndex == 1) clockFace = (clockFace + dir + 4) % 4;
      break;
    case SET_POMODORO:
      if (settingsItemIndex == 0) { workMinutes  = constrain((int)workMinutes + dir*5, 5, 90); workTime = workMinutes*60; if (!pomoActive) timerValue = workTime; }
      if (settingsItemIndex == 1) { breakMinutes = constrain((int)breakMinutes + dir, 1, 30);  breakTime = breakMinutes*60; }
      if (settingsItemIndex == 2) adaptiveMode = !adaptiveMode;
      break;
    case SET_WEATHER:
      if (settingsItemIndex == 0) units = !units;
      if (settingsItemIndex == 1) { cityIdx = (cityIdx + dir + CITY_COUNT) % CITY_COUNT; weatherValid = false; weatherForce = true; }
      break;
    case SET_SYSTEM:
      if (settingsItemIndex == 1) { pirEnabled = !pirEnabled; seenMotion = false; presentNow = false; autoPaused = false; }
      if (settingsItemIndex == 2) clapEnabled = !clapEnabled;
      break;
  }
}

// ============================================================
//  HELPERS
// ============================================================
void updateActivity() { lastActivity = millis(); screensaverActive = false; }



String formatTime(int h,int m,int s) {
  char buf[14];
  if (timeFormat == 1) {
    const char* ampm = h>=12 ? "PM":"AM";
    int h12=h%12; if(h12==0) h12=12;
    if (s<0) snprintf(buf,sizeof(buf),"%d:%02d %s",h12,m,ampm);
    else      snprintf(buf,sizeof(buf),"%d:%02d:%02d%s",h12,m,s,ampm);
  } else {
    if (s<0) snprintf(buf,sizeof(buf),"%02d:%02d",h,m);
    else      snprintf(buf,sizeof(buf),"%02d:%02d:%02d",h,m,s);
  }
  return String(buf);
}

void showNotif(const String& msg) { notifMsg=msg; notifStart=millis(); notifActive=true; }

void goToSleep() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(20,16); display.println("No activity...");
  display.setCursor(20,28); display.println("Going to sleep.");
  display.drawRoundRect(44,38,40,16,4,SH110X_WHITE);
  display.drawLine(50,44,56,44,SH110X_WHITE);
  display.drawLine(72,44,78,44,SH110X_WHITE);
  display.drawLine(54,49,74,49,SH110X_WHITE);
  present();
  delay(1500);
  flushConfig(true); lifeFlush(true);
  ESP.deepSleep(0);
}

void displayMessage(String msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,20);
  display.println(msg);
  present();
}

// ============================================================
//  BOOT ANIMATION
// ============================================================
void bootAnimation() {
  display.setTextSize(2);
  String logo="DeskBot";
  for (int i=0;i<=(int)logo.length();i++) {
    display.clearDisplay();
    display.setCursor(14,6);
    display.println(logo.substring(0,i));
    if (i<(int)logo.length()) display.fillRect(14+i*12,6,2,16,SH110X_WHITE);
    present();
    delay(85);
  }
  delay(150);

  display.setTextSize(1);
  String sub="Operating System v4.0";
  for (int i=0;i<=(int)sub.length();i++) {
    display.fillRect(0,26,128,10,SH110X_BLACK);
    display.setCursor(10,27);
    display.println(sub.substring(0,i));
    if (i<(int)sub.length()) display.fillRect(10+i*6,27,2,8,SH110X_WHITE);
    present();
    delay(38);
  }
  delay(350);

  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(14,4); display.println("DeskBot");
  display.setTextSize(1);
  display.setCursor(10,26); display.println("OS  v4.0");
  display.drawLine(0,35,128,35,SH110X_WHITE);
  display.setCursor(20,40); display.println("Initializing...");
  display.drawRect(10,52,108,9,SH110X_WHITE);
  present();

  for (int p=0;p<=100;p+=2) {
    display.fillRect(11,53,(p*106)/100,7,SH110X_WHITE);
    display.setTextColor(p>50?SH110X_BLACK:SH110X_WHITE);
    display.setCursor(52,54); display.printf("%3d%%",p);
    display.setTextColor(SH110X_WHITE);
    present();
    delay(16);
  }

  wifiLoad();
  wifiConnectAll();                              // saved networks, then the built-in one from secrets.h

  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10,2); display.println("DeskBot");
  display.setTextSize(1);
  display.setCursor(10,22); display.println("OS  v4.0");
  display.drawLine(0,32,128,32,SH110X_WHITE);
  if (WiFi.status()==WL_CONNECTED) {
    display.setCursor(10,37); display.println("WiFi  Connected!");
    display.setCursor(10,48); display.println(WiFi.localIP().toString());
  } else {
    display.setCursor(10,37); display.println("WiFi  Failed.");
    display.setCursor(10,48); display.println("Setup hotspot opens");
  }
  present();
  delay(1800);
}

// ============================================================
//  OTA
// ============================================================
void setupOTA() {
  ArduinoOTA.setHostname("DeskBotOS");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    otaInProgress=true; otaProgress=0; flushConfig(true); lifeFlush(true);
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(20,10); display.println("OTA Update");
    display.setCursor(10,22); display.println("Receiving code...");
    display.drawRect(10,40,108,12,SH110X_WHITE);
    present();
  });
  ArduinoOTA.onProgress([](unsigned int progress,unsigned int total) {
    otaProgress=(progress*100)/total;
    display.fillRect(11,41,(otaProgress*106)/100,10,SH110X_WHITE);
    display.fillRect(44,54,40,10,SH110X_BLACK);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(46,55); display.printf("%d%%",otaProgress);
    present();
  });
  ArduinoOTA.onEnd([]() {
    display.clearDisplay(); display.setTextSize(1);
    display.setCursor(20,20); display.println("Update done!");
    present(); delay(800);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress=false;
    display.clearDisplay(); display.setTextSize(1);
    display.setCursor(20,20); display.println("OTA FAILED!");
    present(); delay(2000);
  });
  ArduinoOTA.begin();
}

// ============================================================
//  WEB DASHBOARD + JSON API  (#9)
//  "/" serves a static page (no data). Everything under /api needs the token
//  from secrets.h, sent as header "X-Token" or as ?token=... (for iOS Shortcuts).
// ============================================================
const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DeskBot OS</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0a0a;color:#e0e0e0;font-family:'Courier New',monospace;padding:16px;max-width:720px;margin:auto}
h1{font-size:26px;color:#fff;letter-spacing:4px;text-align:center}
.sub{font-size:11px;color:#555;text-align:center;margin:4px 0 16px;letter-spacing:1px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
@media(max-width:480px){.grid{grid-template-columns:1fr}}
.card{background:#111;border:1px solid #222;border-radius:8px;padding:14px;margin-bottom:12px}
.grid .card{margin:0}.wide{grid-column:1/-1}
.lbl{font-size:10px;color:#555;letter-spacing:2px;text-transform:uppercase;margin-bottom:6px}
.val{font-size:26px;color:#fff;font-weight:bold}
.small{font-size:12px;color:#888;margin-top:4px}
.row{display:flex;gap:8px;margin-top:8px;flex-wrap:wrap}
button,input,select{font-family:inherit;font-size:12px;background:#1a1a1a;color:#e0e0e0;border:1px solid #333;border-radius:6px;padding:10px 8px}
button{cursor:pointer;flex:1}button:hover{border-color:#777}
input,select{flex:2;min-width:0}input[type=number]{flex:1}input[type=checkbox]{flex:0}
.habit{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:8px 0;border-bottom:1px solid #1c1c1c}
.habit:last-child{border:0}.on{color:#00ff88}
code{background:#1a1a1a;padding:2px 6px;border-radius:4px;color:#00ff88;font-size:11px;word-break:break-all}
</style></head><body>
<h1>DESKBOT OS</h1><div class="sub" id="sub">connecting...</div><div class="sub"><a href="/life" style="color:#00ff88">LIFE CLOCK</a> &nbsp; <a href="/wifi" style="color:#00ff88">WIFI</a></div>
<div id="login" class="card" hidden><div class="lbl">Access token</div>
<div class="row"><input id="tk" type="password" placeholder="token"><button onclick="saveT()">UNLOCK</button></div></div>
<div id="app" hidden>
<div class="grid">
<div class="card"><div class="lbl">Time</div><div class="val" id="t">--</div></div>
<div class="card"><div class="lbl">Weather</div><div class="val" id="w">--</div><div class="small" id="wd"></div>
<div class="row"><select id="ci"></select><button onclick="setC()">SET</button></div></div>
<div class="card"><div class="lbl">Pomodoro</div><div class="val" id="p">--</div><div class="small" id="ps"></div>
<div class="row"><button onclick="cmd('pomo_pause')">START/PAUSE</button><button onclick="cmd('pomo_reset')">RESET</button></div></div>
<div class="card"><div class="lbl">Desk</div><div class="val" id="dk">--</div><div class="small" id="dm"></div></div>
<div class="card wide"><div class="lbl">Habits</div><div id="hl"></div></div>
<div class="card wide"><div class="lbl">Alarm</div>
<div class="row"><input type="number" id="ah" min="0" max="23"><input type="number" id="am" min="0" max="59">
<label class="small" style="align-self:center"><input type="checkbox" id="ao"> on</label><button onclick="setA()">SAVE</button><button onclick="cmd('alarm_stop')">STOP</button></div></div>
<div class="card wide"><div class="lbl">Remote</div>
<div class="row"><button onclick="cmd('goto_home')">HOME</button><button onclick="cmd('face_prev')">PREV FACE</button><button onclick="cmd('face_next')">NEXT FACE</button><button onclick="cmd('clock_next')">CLOCK FACE</button></div>
<div class="row"><input id="ni" placeholder="notification..." maxlength="20"><button onclick="sendN()">SEND</button></div>
<div class="small" style="margin-top:10px">iOS Shortcut: Get Contents of URL <code>http://THIS-IP/api/notif?token=YOUR_TOKEN&amp;msg=Hello</code></div></div>
</div></div>
<script>
var T='';try{T=localStorage.getItem('t')||''}catch(e){}
var $=function(i){return document.getElementById(i)};
function saveT(){T=$('tk').value;try{localStorage.setItem('t',T)}catch(e){}poll()}
function lock(b){$('login').hidden=!b;$('app').hidden=b}
function api(p){return fetch(p,{headers:{'X-Token':T}}).then(function(r){
 if(r.status==401){lock(true);$('sub').textContent='enter token';throw 1}
 if(r.status==429){$('sub').textContent='too many tries, wait 30 s';throw 2}return r})}
function fmt(s){var m=Math.floor(s/60),x=s%60;return(m<10?'0':'')+m+':'+(x<10?'0':'')+x}
function poll(){api('/api/status').then(function(r){return r.json()}).then(draw).catch(function(){})}
function draw(d){lock(false);
 $('sub').textContent=d.city+' | '+d.rssi+' dBm | '+d.heap+' B free | up '+d.up+' min';
 $('t').textContent=d.time;$('w').textContent=d.temp+'°'+d.unit;$('wd').textContent=d.desc+' | '+d.hum+'%';
 $('p').textContent=fmt(d.pomo.left);$('ps').textContent=d.pomo.state+' | today '+d.pomo.today+' | total '+d.pomo.total;
 $('dk').textContent=d.pir?(d.here?'HERE':'AWAY'):'OFF';
 $('dm').textContent=Math.floor(d.desk/60)+'h '+d.desk%60+'m at desk | noise '+d.noise+'%';
 var c=$('ci');if(!c.options.length){d.cities.forEach(function(n,i){c.add(new Option(n,i))})}if(document.activeElement!==c)c.value=d.ci;
 var h='';d.habits.forEach(function(x,i){h+='<div class="habit"><span class="'+(x.d?'on':'')+'">'+(x.d?'[x] ':'[ ] ')+x.n+' | '+x.s+'d</span><button style="flex:0 0 72px" onclick="hb('+i+','+(x.d?0:1)+')">'+(x.d?'UNDO':'DONE')+'</button></div>'});
 $('hl').innerHTML=h;
 var a=document.activeElement;if(a!==$('ah')&&a!==$('am')&&a!==$('ao')){$('ah').value=d.alarm.h;$('am').value=d.alarm.m;$('ao').checked=d.alarm.on}}
function cmd(c){api('/api/cmd?c='+c).then(poll)}
function hb(i,v){api('/api/habit?i='+i+'&done='+v).then(poll)}
function setA(){api('/api/alarm?h='+$('ah').value+'&m='+$('am').value+'&on='+($('ao').checked?1:0)).then(poll)}
function setC(){api('/api/city?i='+$('ci').value).then(poll)}
function sendN(){var m=$('ni').value.trim();if(m)api('/api/notif?msg='+encodeURIComponent(m)).then(function(){$('ni').value=''})}
poll();setInterval(poll,3000);
</script></body></html>
)rawhtml";

uint8_t  authFails     = 0;
uint32_t authLockUntil = 0;

bool authOK() {
  uint32_t now = millis();
  if (authLockUntil && (int32_t)(now - authLockUntil) < 0) { server.send(429, "text/plain", "Locked"); return false; }
  String t = server.header("X-Token");
  if (!t.length()) t = server.arg("token");
  if (t == API_TOKEN) { authFails = 0; return true; }
  if (++authFails >= 5) { authFails = 0; authLockUntil = now + 30000UL; if (!authLockUntil) authLockUntil = 1; }
  server.send(401, "text/plain", "Unauthorized");
  return false;
}
#define REQUIRE_AUTH  if (!authOK()) return

// ============================================================
//  LIFE CLOCK  -  storage, OLED screens, input
//  (all date maths lives in LifeClock.h / LifeClock.cpp)
// ============================================================
LifeData  lifeData;
LifeStats lifeStats;
bool      lifeStale    = true;       // stats must be recomputed
bool      lifeDirty    = false;      // EEPROM record must be written
uint32_t  lifeDirtyAt  = 0;
int32_t   lifeStatsDay = LIFE_NO_TODAY;
uint8_t   lifeSrc      = 0;          // 0 = no date, 1 = NTP, 2 = cached day + uptime

#define LIFE_RIGHT 120               // content stays left of the page dots

// ---- today's day number: NTP, else the day saved in EEPROM + uptime, else unknown ----
int32_t lifeToday() {
  uint32_t e = timeClient.getEpochTime();            // already includes the timezone offset
  if (e >= 1700000000UL) { lifeSrc = 1; return (int32_t)(e / 86400UL); }
  if (lastEpochDay)      { lifeSrc = 2; return (int32_t)lastEpochDay + (int32_t)(millis() / 86400000UL); }
  lifeSrc = 0;
  return LIFE_NO_TODAY;
}

void lifeApplyTz() { timeClient.setTimeOffset((long)lifeData.tzMinutes * 60L); }

void lifeChanged(bool tzToo) {                       // call after every edit
  lifeStale = true; lifeDirty = true; lifeDirtyAt = millis();
  if (tzToo) lifeApplyTz();
}

void lifeLoad() {
  uint8_t buf[LIFE_STORE_SIZE];
  for (int i = 0; i < LIFE_STORE_SIZE; i++) buf[i] = EEPROM.read(LIFE_EEPROM_ADDR + i);
  lifeDecode(buf, &lifeData);                        // blank / corrupt -> defaults (not written back)
  lifeApplyTz();
  lifeStale = true;
}

void lifeFlush(bool force) {
  if (!lifeDirty) return;
  if (!force && millis() - lifeDirtyAt < 3000UL) return;     // debounce: one write after editing stops
  uint8_t buf[LIFE_STORE_SIZE];
  lifeEncode(&lifeData, buf);
  for (int i = 0; i < LIFE_STORE_SIZE; i++) EEPROM.write(LIFE_EEPROM_ADDR + i, buf[i]);
  EEPROM.commit();
  lifeDirty = false;
}

void lifeTask() {                                    // every loop(): recompute only when something changed
  int32_t t = lifeToday();
  if (lifeStale || t != lifeStatsDay) {
    lifeCompute(&lifeData, t, &lifeStats);
    lifeStatsDay = t; lifeStale = false;
  }
}

// ---------------- UI state ----------------
#define LS_OVER 0
#define LS_CAL  1
#define LS_MILE 2
#define LS_SET  3
#define LS_COUNT 4
#define LSET_BIRTH 0
#define LSET_SPAN  1
#define LSET_TZ    2
#define LSET_DAYS  3
#define LSET_ON    4
#define LSET_COUNT 5

uint8_t  lifeScreen  = LS_OVER;
bool     lifeSub     = false;          // navigating inside a screen (calendar years / milestone list / settings list)
bool     lifeSetEdit = false;          // editing one settings value
uint8_t  lifeSetCur  = 0, lifeSetField = 0, lifeMsCur = 0;
int      lifeCalRow  = 0;
uint32_t lifeSubAt   = 0;

static const char* const LIFE_MON[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

void lifeEnter() { lifeScreen = LS_OVER; lifeSub = false; lifeSetEdit = false; lifeSetCur = 0; lifeMsCur = 0; }
void lifeTouch() { lifeSubAt = millis(); }

bool lifeBack() {                      // UP+DN: leave edit, then sub-mode, then the app
  if (lifeSetEdit) { lifeSetEdit = false; return true; }
  if (lifeSub)     { lifeSub = false;     return true; }
  return false;
}
void lifeGoOverview() { lifeScreen = LS_OVER; lifeSub = false; lifeSetEdit = false; }

// ---------------- small drawing helpers ----------------
static int lifeTextW(const char* s) { return (int)strlen(s) * 6; }
static void lifePrintRight(int xRight, int y, const char* s) { display.setCursor(xRight - lifeTextW(s), y); display.print(s); }
static void lifePrintCenter(int y, const char* s) { display.setCursor((128 - lifeTextW(s)) / 2, y); display.print(s); }

static void lifeDots() {               // which of the four screens we are on
  for (int i = 0; i < LS_COUNT; i++) {
    if (i == lifeScreen) display.fillRect(124, 22 + i * 8, 3, 3, SH110X_WHITE);
    else                 display.drawPixel(125, 23 + i * 8, SH110X_WHITE);
  }
}

static void lifeFmtDate(char* out, size_t n, int32_t day) {
  int y, m, d; lifeCivil(day, &y, &m, &d);
  (void)y; snprintf(out, n, "%d %s", d, LIFE_MON[m - 1]);
}

static void lifeFmtTz(char* out, size_t n, int tz) {
  int a = tz < 0 ? -tz : tz;
  snprintf(out, n, "UTC%c%d:%02d", tz < 0 ? '-' : '+', a / 60, a % 60);
}

// ---------------- screens ----------------
static void lifeStateScreen() {        // everything that is not LIFE_OK / LIFE_BEYOND
  display.setTextSize(1);
  switch (lifeStats.state) {
    case LIFE_DISABLED:
      lifePrintCenter(22, "Life Clock is off");
      lifePrintCenter(38, "SELECT = turn on");
      break;
    case LIFE_NOT_SET:
      lifePrintCenter(20, "Set your birth date");
      lifePrintCenter(32, "SELECT = open");
      lifePrintCenter(42, "settings");
      break;
    case LIFE_NO_DATE:
      lifePrintCenter(22, "Waiting for the");
      lifePrintCenter(34, "time (WiFi / NTP)");
      break;
    case LIFE_BIRTH_FUTURE:
      lifePrintCenter(22, "Birth date is in");
      lifePrintCenter(34, "the future");
      lifePrintCenter(46, "check settings");
      break;
  }
}

static void lifeDrawOverview() {
  const LifeStats& s = lifeStats;
  char b[40];
  display.setTextSize(2);
  snprintf(b, sizeof(b), "%uy %um", (unsigned)s.ageYears, (unsigned)s.ageMonths);
  display.setCursor(0, 12); display.print(b);
  const int ageW = lifeTextW(b) * 2;
  display.setTextSize(1);
  if (ageW <= 72) {                                           // right column only when there is room
    lifeFmtDate(b, sizeof(b), s.today);
    lifePrintRight(LIFE_RIGHT, 13, b);
    if (lifeData.opts & LIFE_OPT_SHOW_DAYS) { snprintf(b, sizeof(b), "+%ud", (unsigned)s.ageDays); lifePrintRight(LIFE_RIGHT, 22, b); }
  }
  snprintf(b, sizeof(b), "Lived %lu wk", (unsigned long)s.weeksElapsed);
  display.setCursor(0, 31); display.print(b);
  snprintf(b, sizeof(b), "%u.%u%%", s.permille / 10, s.permille % 10);
  lifePrintRight(LIFE_RIGHT, 31, b);
  snprintf(b, sizeof(b), "Left %lu wk", (unsigned long)s.weeksRemaining);
  display.setCursor(0, 40); display.print(b);
  snprintf(b, sizeof(b), "of %uy", (unsigned)lifeData.cfg.referenceLifespan);
  lifePrintRight(LIFE_RIGHT, 40, b);

  display.drawRect(0, 50, LIFE_RIGHT, 6, SH110X_WHITE);       // progress bar
  int fill = (int)((uint32_t)s.permille * (LIFE_RIGHT - 2) / 1000UL);
  if (fill > 0) display.fillRect(1, 51, fill, 4, SH110X_WHITE);

  int8_t nm = lifeNextMilestone(&lifeData, s.today);          // bottom line
  if (nm >= 0) {
    const LifeMilestone& m = lifeData.ms[nm];
    char cd[10]; lifeFormatCountdown(lifeDayNumber(m.year, m.month, m.day) - s.today, cd, sizeof(cd));
    int room = 20 - 3 - (int)strlen(cd);
    char nb[LIFE_NAME_LEN]; strncpy(nb, m.name, room); nb[room] = 0;
    snprintf(b, sizeof(b), "> %s %s", nb, cd);
  } else {
    snprintf(b, sizeof(b), "Week %lu of %lu", (unsigned long)s.weeksElapsed + 1, (unsigned long)s.weeksTotal);
  }
  display.setCursor(0, 57); display.print(b);
}

static void lifeDrawCalendar() {
  const LifeStats& s = lifeStats;
  const LifeClockConfig* c = &lifeData.cfg;
  int rows = c->referenceLifespan;
  if (lifeCalRow >= rows) lifeCalRow = rows - 1;
  if (lifeCalRow < 0) lifeCalRow = 0;
  int row = lifeSub ? lifeCalRow : (s.lifeYear < rows ? s.lifeYear : rows - 1);
  if (!lifeSub) lifeCalRow = row;

  uint32_t first = lifeYearFirstWeek(c, row);
  int n = lifeYearWeekCount(c, row);                          // 52 or 53
  char b[28];
  display.setTextSize(1);
  snprintf(b, sizeof(b), "Age %d", row);
  display.setCursor(0, 11); display.print(b);
  if (n == 53) { display.setCursor(42, 11); display.print("53wk"); }
  snprintf(b, sizeof(b), "%lu-%lu", (unsigned long)first + 1, (unsigned long)first + n);
  lifePrintRight(LIFE_RIGHT, 11, b);

  const bool blink = (millis() / 500) & 1;
  for (int k = 0; k < n; k++) {
    int r = k < 52 ? k / 13 : 3, col = k < 52 ? k % 13 : 13;   // the 53rd week continues the last row
    int x = 9 + col * 8, y = 22 + r * 8;
    uint8_t st = lifeWeekState(&s, first + k);
    if (st == LIFE_WEEK_DONE) display.fillRect(x, y, 6, 6, SH110X_WHITE);
    else {
      display.drawRect(x, y, 6, 6, SH110X_WHITE);
      if (st == LIFE_WEEK_CURRENT) {
        display.drawRect(x - 1, y - 1, 8, 8, SH110X_WHITE);
        if (blink) display.fillRect(x + 2, y + 2, 2, 2, SH110X_WHITE);
      }
    }
  }
  if (lifeSub) {                                              // year navigation arrows
    if (row > 0)        display.fillTriangle(2, 37, 6, 34, 6, 40, SH110X_WHITE);
    if (row < rows - 1) display.fillTriangle(117, 37, 113, 34, 113, 40, SH110X_WHITE);
  }
  display.drawRect(9, 57, 110, 5, SH110X_WHITE);              // where this row sits in the whole life
  display.fillRect(9 + (int)((uint32_t)row * 108 / rows), 58, 3, 3, SH110X_WHITE);
}

static void lifeDrawMilestones() {
  uint8_t order[LIFE_MAX_MILESTONES];
  uint8_t cnt = lifeSortMilestones(&lifeData, order);
  char b[28];
  display.setTextSize(1);
  snprintf(b, sizeof(b), "Milestones %u/%d", cnt, LIFE_MAX_MILESTONES);
  display.setCursor(0, 11); display.print(b);
  if (!cnt) {
    lifePrintCenter(27, "Nothing yet");
    lifePrintCenter(40, "Add them on the web:");
    snprintf(b, sizeof(b), "%s/life", WiFi.localIP().toString().c_str());
    lifePrintCenter(50, b);
    return;
  }
  if (lifeMsCur >= cnt) lifeMsCur = cnt - 1;
  for (uint8_t i = 0; i < cnt; i++) {
    const LifeMilestone& m = lifeData.ms[order[i]];
    int y = 21 + i * 8;
    bool cur = lifeSub && i == lifeMsCur;
    uint16_t fg = cur ? SH110X_BLACK : SH110X_WHITE;
    if (cur) display.fillRect(0, y - 1, LIFE_RIGHT, 9, SH110X_WHITE);
    display.drawRect(1, y, 7, 7, fg);
    if (m.completed) display.fillRect(3, y + 2, 3, 3, fg);
    char nb[14]; strncpy(nb, m.name, 13); nb[13] = 0;
    char cd[10];
    if (m.completed) strcpy(cd, "done");
    else lifeFormatCountdown(lifeDayNumber(m.year, m.month, m.day) - lifeStats.today, cd, sizeof(cd));
    display.setTextColor(fg);
    display.setCursor(11, y); display.print(nb);
    lifePrintRight(LIFE_RIGHT - 1, y, cd);
    display.setTextColor(SH110X_WHITE);
  }
}

static void lifeSettingValue(uint8_t item, char* out, size_t n) {
  const LifeClockConfig* c = &lifeData.cfg;
  switch (item) {
    case LSET_BIRTH:
      if (!c->birthYear) snprintf(out, n, "not set");
      else snprintf(out, n, "%04u-%02u-%02u", c->birthYear, c->birthMonth, c->birthDay);
      break;
    case LSET_SPAN: snprintf(out, n, "%u y", c->referenceLifespan); break;
    case LSET_TZ:   lifeFmtTz(out, n, lifeData.tzMinutes); break;
    case LSET_DAYS: snprintf(out, n, "%s", (lifeData.opts & LIFE_OPT_SHOW_DAYS) ? "ON" : "OFF"); break;
    default:        snprintf(out, n, "%s", c->enabled ? "ON" : "OFF"); break;
  }
}

static void lifeDrawSettings() {
  static const char* const LBL[LSET_COUNT] = {"Birth", "Lifespan", "Timezone", "Show days", "Enabled"};
  display.setTextSize(1);
  display.setCursor(0, 11); display.print("LIFE SETTINGS");
  int top = lifeSetCur > 3 ? lifeSetCur - 3 : 0;               // 4 visible rows
  for (int r = 0; r < 4; r++) {
    uint8_t it = top + r;
    if (it >= LSET_COUNT) break;
    int y = 22 + r * 9;
    bool cur = lifeSub && it == lifeSetCur;
    char v[16]; lifeSettingValue(it, v, sizeof(v));
    if (cur && !lifeSetEdit) display.fillRect(0, y - 1, LIFE_RIGHT, 9, SH110X_WHITE);
    display.setTextColor((cur && !lifeSetEdit) ? SH110X_BLACK : SH110X_WHITE);
    display.setCursor(2, y); display.print(LBL[it]);
    int vx = LIFE_RIGHT - 2 - lifeTextW(v);
    display.setCursor(vx, y); display.print(v);
    if (cur && lifeSetEdit) {                                  // highlight the value (or the birth-date field) being edited
      int fx = vx, fw = lifeTextW(v);
      if (it == LSET_BIRTH && lifeData.cfg.birthYear) { static const uint8_t FO[3] = {0, 5, 8}, FW[3] = {4, 2, 2}; fx = vx + FO[lifeSetField] * 6; fw = FW[lifeSetField] * 6; }
      display.fillRect(fx - 1, y - 1, fw + 1, 9, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
      display.setCursor(fx, y);
      for (int k = 0; k < fw / 6; k++) display.print(v[(fx - vx) / 6 + k]);
    }
    display.setTextColor(SH110X_WHITE);
  }
  if (top > 0)                  display.fillTriangle(123, 20, 121, 17, 125, 17, SH110X_WHITE);
  if (top + 4 < LSET_COUNT)     display.fillTriangle(123, 60, 121, 57, 125, 57, SH110X_WHITE);
}

// ---------------- app entry (called from runActiveApp) ----------------
void lifeApp() {
  static uint32_t last = 0;
  if (!frameReady(last, 100)) return;
  if (lifeSub && millis() - lifeSubAt > 15000UL) { lifeSub = false; lifeSetEdit = false; }   // idle: back to browsing

  display.clearDisplay();
  drawStatusBar();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);

  const bool ok = lifeStats.state == LIFE_OK || lifeStats.state == LIFE_BEYOND;
  if (lifeScreen == LS_SET)       lifeDrawSettings();          // settings must stay reachable in every state
  else if (lifeScreen == LS_MILE) lifeDrawMilestones();        // milestones too (only the countdown needs a date)
  else if (!ok)                   lifeStateScreen();
  else if (lifeScreen == LS_CAL)  lifeDrawCalendar();
  else                            lifeDrawOverview();
  lifeDots();
  present();
}

// ---------------- input (called from handleButtons) ----------------
void lifeFixBirthDay() {
  LifeClockConfig* c = &lifeData.cfg;
  int dim = lifeDaysInMonth(c->birthYear, c->birthMonth);
  if (c->birthDay > dim) c->birthDay = dim;
  if (c->birthDay < 1)   c->birthDay = 1;
}

void lifeAdjust(int d) {                                       // UP = +1, DOWN = -1 while a value is being edited
  LifeClockConfig* c = &lifeData.cfg;
  switch (lifeSetCur) {
    case LSET_BIRTH:
      if (lifeSetField == 0)      c->birthYear  = constrain((int)c->birthYear + d, LIFE_MIN_YEAR, LIFE_MAX_YEAR);
      else if (lifeSetField == 1) c->birthMonth = (c->birthMonth - 1 + 12 + d) % 12 + 1;
      else { int dim = lifeDaysInMonth(c->birthYear, c->birthMonth); c->birthDay = (c->birthDay - 1 + dim + d) % dim + 1; }
      lifeFixBirthDay();
      lifeChanged(false);
      break;
    case LSET_SPAN:
      c->referenceLifespan = constrain((int)c->referenceLifespan + d, LIFE_MIN_LIFESPAN, LIFE_MAX_LIFESPAN);
      lifeChanged(false);
      break;
    case LSET_TZ:
      lifeData.tzMinutes = constrain((int)lifeData.tzMinutes + d * 15, LIFE_MIN_TZ_MIN, LIFE_MAX_TZ_MIN);
      lifeChanged(true);
      break;
  }
}

void lifeSelectSetting() {                                      // SELECT on a settings row
  switch (lifeSetCur) {
    case LSET_BIRTH:
      if (!lifeData.cfg.birthYear) { lifeData.cfg.birthYear = 2000; lifeData.cfg.birthMonth = 1; lifeData.cfg.birthDay = 1; lifeChanged(false); }
      lifeSetField = 0; lifeSetEdit = true; showNotif("UP/DN change, SEL next");
      break;
    case LSET_SPAN: case LSET_TZ:
      lifeSetEdit = true; showNotif("UP/DN change, SEL ok");
      break;
    case LSET_DAYS: lifeData.opts ^= LIFE_OPT_SHOW_DAYS; lifeChanged(false); break;
    case LSET_ON:   lifeData.cfg.enabled = !lifeData.cfg.enabled; lifeChanged(false); break;
  }
}

void lifeHandleInput(bool up, bool dn, bool sel) {
  lifeTouch();
  if (!lifeSub) {                                               // browsing: UP/DN switch screen
    if (up) { lifeScreen = (lifeScreen + 1) % LS_COUNT; beepClick(); }
    if (dn) { lifeScreen = (lifeScreen + LS_COUNT - 1) % LS_COUNT; beepClick(); }
    if (!sel) return;
    switch (lifeScreen) {
      case LS_OVER:
        if (lifeStats.state == LIFE_DISABLED) { lifeData.cfg.enabled = true; lifeChanged(false); beepSuccess(); }
        else if (lifeStats.state == LIFE_NOT_SET) { lifeScreen = LS_SET; beepClick(); }
        break;
      case LS_CAL:
        if (lifeStats.state == LIFE_OK || lifeStats.state == LIFE_BEYOND) {
          lifeSub = true; lifeCalRow = lifeStats.lifeYear; showNotif("UP/DN = year"); beepClick();
        }
        break;
      case LS_MILE: {
        uint8_t order[LIFE_MAX_MILESTONES];
        if (lifeSortMilestones(&lifeData, order)) { lifeSub = true; lifeMsCur = 0; showNotif("SEL = tick / untick"); beepClick(); }
        break; }
      case LS_SET:
        lifeSub = true; lifeSetEdit = false; lifeSetCur = 0; beepClick();
        break;
    }
    return;
  }

  switch (lifeScreen) {                                         // sub-mode
    case LS_CAL:
      if (up && lifeCalRow < (int)lifeData.cfg.referenceLifespan - 1) { lifeCalRow++; beepClick(); }
      if (dn && lifeCalRow > 0)                                       { lifeCalRow--; beepClick(); }
      if (sel) { lifeSub = false; beepClick(); }
      break;
    case LS_MILE: {
      uint8_t order[LIFE_MAX_MILESTONES];
      uint8_t cnt = lifeSortMilestones(&lifeData, order);
      if (!cnt) { lifeSub = false; break; }
      if (up) lifeMsCur = (lifeMsCur + cnt - 1) % cnt;
      if (dn) lifeMsCur = (lifeMsCur + 1) % cnt;
      if (sel) {
        LifeMilestone& m = lifeData.ms[order[lifeMsCur % cnt]];
        m.completed = !m.completed; lifeChanged(false);
        if (m.completed) beepSuccess(); else beepClick();
      }
      break; }
    case LS_SET:
      if (lifeSetEdit) {
        if (up) lifeAdjust(1);
        if (dn) lifeAdjust(-1);
        if (sel) {
          if (lifeSetCur == LSET_BIRTH && lifeSetField < 2) lifeSetField++;
          else { lifeSetEdit = false; beepSuccess(); }
        }
      } else {
        if (up) lifeSetCur = (lifeSetCur + LSET_COUNT - 1) % LSET_COUNT;
        if (dn) lifeSetCur = (lifeSetCur + 1) % LSET_COUNT;
        if (sel) lifeSelectSetting();
      }
      break;
  }
}

// ============================================================
//  LIFE CLOCK  -  web page (/life) and JSON API
// ============================================================
const char LIFE_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Life Clock</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0a0a;color:#e0e0e0;font-family:'Courier New',monospace;padding:16px;max-width:720px;margin:auto}
h1{font-size:24px;color:#fff;letter-spacing:4px;text-align:center}
.sub{font-size:11px;color:#555;text-align:center;margin:4px 0 16px;letter-spacing:1px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
.card{background:#111;border:1px solid #222;border-radius:8px;padding:14px;margin-bottom:12px}
.grid .card{margin:0}.wide{grid-column:1/-1}
.lbl{font-size:10px;color:#555;letter-spacing:2px;text-transform:uppercase;margin-bottom:6px}
.val{font-size:24px;color:#fff;font-weight:bold}
.small{font-size:12px;color:#888;margin-top:4px}
.row{display:flex;gap:8px;margin-top:8px;flex-wrap:wrap;align-items:center}
button,input,select{font-family:inherit;font-size:12px;background:#1a1a1a;color:#e0e0e0;border:1px solid #333;border-radius:6px;padding:10px 8px}
button{cursor:pointer;flex:1}button:hover{border-color:#777}
input,select{flex:2;min-width:0}input[type=number]{flex:1}input[type=checkbox]{flex:0}
.bar{height:8px;background:#1a1a1a;border-radius:4px;overflow:hidden;margin-top:10px}.bar i{display:block;height:100%;background:#00ff88}
canvas{width:100%;display:block;border-radius:4px;background:#0d0d0d}
.ms{display:flex;gap:8px;align-items:center;padding:8px 0;border-bottom:1px solid #1c1c1c;font-size:12px}
.ms:last-child{border:0}.ms .n{flex:1;word-break:break-word}.done{color:#555;text-decoration:line-through}
.note{font-size:11px;color:#666;margin-top:8px;line-height:1.5}
a{color:#00ff88}
</style></head><body>
<h1>LIFE CLOCK</h1><div class="sub" id="sub">connecting...</div>
<div id="login" class="card" hidden><div class="lbl">Access token</div>
<div class="row"><input id="tk" type="password" placeholder="token"><button onclick="saveT()">UNLOCK</button></div></div>
<div id="app" hidden>
<div class="grid">
<div class="card wide"><div class="lbl">Age</div><div class="val" id="age">--</div><div class="small" id="agd"></div>
<div class="bar"><i id="pb" style="width:0"></i></div><div class="small" id="pc"></div></div>
<div class="card"><div class="lbl">Weeks lived</div><div class="val" id="wl">--</div></div>
<div class="card"><div class="lbl">Weeks left</div><div class="val" id="wr">--</div><div class="small" id="wt"></div></div>
</div>
<div class="card"><div class="lbl">Your life in weeks</div><canvas id="cv"></canvas>
<div class="note">Each row is one year of life (birthday to birthday), each square one week. Filled = lived, green = this week.
The reference lifespan is a visual assumption you can change, not a prediction.</div></div>
<div class="card"><div class="lbl">Milestones</div><div id="ml"></div>
<div class="row"><input id="mn" placeholder="name" maxlength="19"><input id="md" type="date"><button onclick="addM()">ADD</button></div></div>
<div class="card"><div class="lbl">Settings</div>
<div class="row"><span class="small" style="width:90px">Birth date</span><input id="bd" type="date"></div>
<div class="row"><span class="small" style="width:90px">Lifespan</span><input id="ls" type="number" min="30" max="120"><span class="small">years</span></div>
<div class="row"><span class="small" style="width:90px">Timezone</span><select id="tz"></select></div>
<div class="row"><label class="small"><input type="checkbox" id="od"> show extra days on the screen</label></div>
<div class="row"><label class="small"><input type="checkbox" id="en"> Life Clock enabled</label></div>
<div class="row"><button onclick="saveC()">SAVE</button></div>
<div class="note">The timezone is a fixed UTC offset. In regions with daylight saving, change it twice a year.</div></div>
<div class="small" style="text-align:center"><a href="/">back to dashboard</a></div>
</div>
<script>
var T='';try{T=localStorage.getItem('t')||''}catch(e){}
var $=function(i){return document.getElementById(i)},D=null,G=null;
function saveT(){T=$('tk').value;try{localStorage.setItem('t',T)}catch(e){}load()}
function lock(b){$('login').hidden=!b;$('app').hidden=b}
function api(p){return fetch(p,{headers:{'X-Token':T}}).then(function(r){
 if(r.status==401){lock(true);$('sub').textContent='enter token';throw 1}
 if(r.status==429){$('sub').textContent='too many tries, wait 30 s';throw 2}
 if(r.status==400){alert('Rejected: invalid value');throw 3}return r})}
var tzs=$('tz');for(var m=-720;m<=840;m+=15){var a=Math.abs(m),o=document.createElement('option');o.value=m;
 o.textContent='UTC'+(m<0?'-':'+')+Math.floor(a/60)+':'+('0'+a%60).slice(-2);tzs.appendChild(o)}
function p2(n){return('0'+n).slice(-2)}
function ds(y,m,d){return y+'-'+p2(m)+'-'+p2(d)}
function load(){api('/api/life').then(function(r){return r.json()}).then(function(d){D=d;
 if(!G||G.length!=d.span){return api('/api/life/grid').then(function(r){return r.json()}).then(function(g){G=g.rows;draw()})}draw()}).catch(function(){})}
function draw(){var d=D;lock(false);
 var st=['','Life Clock is switched off','Set your birth date below','Waiting for the time (NTP)','Birth date is in the future',''];
 $('sub').textContent=d.state>0&&d.state<5?st[d.state]:'today '+d.today+(d.src==2?' (cached)':'');
 var ok=d.state==0||d.state==5;
 $('age').textContent=ok?d.ay+'y '+d.am+'m '+d.ad+'d':'--';
 $('agd').textContent=ok?d.days+' days lived of '+d.total+' ('+d.left+' left, until '+d.end+')':'';
 $('pb').style.width=ok?d.pm/10+'%':'0';$('pc').textContent=ok?(d.pm/10).toFixed(1)+'% of the reference lifespan':'';
 $('wl').textContent=ok?d.wl:'--';$('wr').textContent=ok?d.wr:'--';$('wt').textContent=ok?'of '+d.wt+' weeks':'';
 var h='';
 d.ms.forEach(function(x){h+='<div class="ms"><button style="flex:0 0 36px" data-i="'+x.i+'" data-a="t">'+(x.c?'[x]':'[ ]')+'</button><span class="n'+(x.c?' done':'')+'" id="mn'+x.i+'"></span><span class="small">'+ds(x.y,x.m,x.d)+'</span><button style="flex:0 0 36px" data-i="'+x.i+'" data-a="d">X</button></div>'});
 $('ml').innerHTML=h||'<div class="small">No milestones yet.</div>';
 d.ms.forEach(function(x){$('mn'+x.i).textContent=x.n});
 var a=document.activeElement;
 if(a.tagName!='INPUT'&&a.tagName!='SELECT'){
  $('bd').value=d.by?ds(d.by,d.bm,d.bd):'';$('ls').value=d.ls;$('tz').value=d.tz;$('od').checked=!!d.od;$('en').checked=!!d.en}
 cvDraw()}
function cvDraw(){var c=$('cv'),d=D;if(!G||!d)return;var W=c.clientWidth,cell=Math.max(3,Math.floor((W-2)/53)),rows=G.length;
 var dpr=window.devicePixelRatio||1;c.width=W*dpr;c.height=(rows*cell+2)*dpr;c.style.height=(rows*cell+2)+'px';
 var x=c.getContext('2d');x.scale(dpr,dpr);var ok=d.state==0||d.state==5,first=0,g=Math.max(1,cell-1);
 for(var r=0;r<rows;r++){for(var k=0;k<G[r];k++){var w=first+k,px=1+k*cell,py=1+r*cell;
  if(ok&&w<d.wl){x.fillStyle='#d8d8d8';x.fillRect(px,py,g,g)}
  else if(ok&&w==d.wl&&d.state==0){x.fillStyle='#00ff88';x.fillRect(px,py,g,g)}
  else{x.strokeStyle='#2c2c2c';x.strokeRect(px+.5,py+.5,g-1,g-1)}}first+=G[r]}}
window.addEventListener('resize',cvDraw);
document.addEventListener('click',function(e){var b=e.target;if(!b.dataset||!b.dataset.a)return;
 if(b.dataset.a=='t'){var x=D.ms.filter(function(m){return m.i==b.dataset.i})[0];
  api('/api/life/ms?i='+x.i+'&c='+(x.c?0:1)).then(load)}
 else if(confirm('Delete this milestone?'))api('/api/life/ms/del?i='+b.dataset.i).then(load)});
function addM(){var n=$('mn').value.trim(),v=$('md').value;if(!n||!v){alert('Enter a name and a date');return}
 var p=v.split('-');api('/api/life/ms?name='+encodeURIComponent(n)+'&y='+ +p[0]+'&m='+ +p[1]+'&d='+ +p[2]).then(function(){$('mn').value='';$('md').value='';load()})}
function saveC(){var v=$('bd').value,q='';if(v){var p=v.split('-');q+='&by='+ +p[0]+'&bm='+ +p[1]+'&bd='+ +p[2]}
 q+='&ls='+$('ls').value+'&tz='+$('tz').value+'&od='+($('od').checked?1:0)+'&en='+($('en').checked?1:0);
 api('/api/life/config?'+q.slice(1)).then(function(){G=null;load()})}
load();setInterval(load,15000);
</script></body></html>
)rawhtml";

static void lifeJsonOK() { server.send(200, "application/json", "{\"ok\":1}"); }

void handleLifeGet() {
  REQUIRE_AUTH;
  lifeTask();
  const LifeStats& s = lifeStats;
  const LifeClockConfig& c = lifeData.cfg;
  char b[1500]; int n = 0;
  int by = 0, bm = 0, bd = 0, ey = 0, em = 0, ed = 0, ty = 0, tm = 0, td = 0;
  if (c.birthYear) { by = c.birthYear; bm = c.birthMonth; bd = c.birthDay; }
  char end[12] = "", today[12] = "";
  if (s.state == LIFE_OK || s.state == LIFE_BEYOND) { lifeCivil(s.end, &ey, &em, &ed); snprintf(end, sizeof(end), "%04d-%02d-%02d", ey, em, ed); }
  if (s.today != LIFE_NO_TODAY) { lifeCivil(s.today, &ty, &tm, &td); snprintf(today, sizeof(today), "%04d-%02d-%02d", ty, tm, td); }
  n += snprintf(b + n, sizeof(b) - n,
    "{\"state\":%u,\"src\":%u,\"today\":\"%s\",\"by\":%d,\"bm\":%d,\"bd\":%d,\"ls\":%u,\"tz\":%d,\"en\":%u,\"od\":%u,"
    "\"ay\":%u,\"am\":%u,\"ad\":%u,\"days\":%ld,\"total\":%ld,\"left\":%ld,\"end\":\"%s\","
    "\"wl\":%lu,\"wr\":%lu,\"wt\":%lu,\"pm\":%u,\"span\":%u,\"ms\":[",
    s.state, lifeSrc, today, by, bm, bd, c.referenceLifespan, lifeData.tzMinutes, c.enabled ? 1 : 0, (lifeData.opts & LIFE_OPT_SHOW_DAYS) ? 1 : 0,
    s.ageYears, s.ageMonths, s.ageDays, (long)s.elapsedDays, (long)s.totalDays, (long)s.remainingDays, end,
    (unsigned long)s.weeksElapsed, (unsigned long)s.weeksRemaining, (unsigned long)s.weeksTotal, s.permille, c.referenceLifespan);
  uint8_t order[LIFE_MAX_MILESTONES];
  uint8_t cnt = lifeSortMilestones(&lifeData, order);
  for (uint8_t k = 0; k < cnt && n < (int)sizeof(b) - 120; k++) {
    const LifeMilestone& m = lifeData.ms[order[k]];
    n += snprintf(b + n, sizeof(b) - n, "%s{\"i\":%u,\"n\":\"%s\",\"y\":%u,\"m\":%u,\"d\":%u,\"c\":%u}",
                  k ? "," : "", order[k], m.name, m.year, m.month, m.day, m.completed ? 1 : 0);   // names are sanitised: no quotes or backslashes
  }
  snprintf(b + n, sizeof(b) - n, "]}");
  server.send(200, "application/json", b);
}

void handleLifeGrid() {
  REQUIRE_AUTH;
  const LifeClockConfig& c = lifeData.cfg;
  char b[LIFE_MAX_LIFESPAN * 3 + 24]; int n = snprintf(b, sizeof(b), "{\"rows\":[");
  for (int y = 0; y < c.referenceLifespan; y++)
    n += snprintf(b + n, sizeof(b) - n, "%s%u", y ? "," : "", c.birthYear ? lifeYearWeekCount(&c, y) : 52);
  snprintf(b + n, sizeof(b) - n, "]}");
  server.send(200, "application/json", b);
}

void handleLifeConfig() {                       // /api/life/config?by&bm&bd&ls&tz&en&od   (every parameter optional)
  REQUIRE_AUTH;
  LifeData t = lifeData;
  if (server.hasArg("by") || server.hasArg("bm") || server.hasArg("bd")) {
    if (!(server.hasArg("by") && server.hasArg("bm") && server.hasArg("bd"))) { server.send(400, "text/plain", "by, bm, bd together"); return; }
    long y = server.arg("by").toInt(), m = server.arg("bm").toInt(), d = server.arg("bd").toInt();
    if (!lifeDateValid(y, m, d)) { server.send(400, "text/plain", "bad date"); return; }
    t.cfg.birthYear = y; t.cfg.birthMonth = m; t.cfg.birthDay = d;
  }
  if (server.hasArg("ls")) {
    long v = server.arg("ls").toInt();
    if (v < LIFE_MIN_LIFESPAN || v > LIFE_MAX_LIFESPAN) { server.send(400, "text/plain", "bad lifespan"); return; }
    t.cfg.referenceLifespan = v;
  }
  if (server.hasArg("tz")) {
    long v = server.arg("tz").toInt();
    if (v < LIFE_MIN_TZ_MIN || v > LIFE_MAX_TZ_MIN || v % 15) { server.send(400, "text/plain", "bad timezone"); return; }
    t.tzMinutes = v;
  }
  if (server.hasArg("en")) t.cfg.enabled = server.arg("en").toInt() != 0;
  if (server.hasArg("od")) { if (server.arg("od").toInt()) t.opts |= LIFE_OPT_SHOW_DAYS; else t.opts &= ~LIFE_OPT_SHOW_DAYS; }
  if (t.cfg.birthYear && !lifeConfigValid(&t.cfg)) { server.send(400, "text/plain", "invalid"); return; }
  bool tzChanged = t.tzMinutes != lifeData.tzMinutes;
  lifeData = t;
  lifeChanged(tzChanged);
  lifeJsonOK();
}

void handleLifeMs() {                           // /api/life/ms?i&name&y&m&d&c   (no i = first free slot)
  REQUIRE_AUTH;
  int i = -1;
  if (server.hasArg("i")) {
    i = server.arg("i").toInt();
    if (i < 0 || i >= LIFE_MAX_MILESTONES || !lifeData.ms[i].year) { server.send(400, "text/plain", "bad index"); return; }
  } else {
    for (int k = 0; k < LIFE_MAX_MILESTONES; k++) if (!lifeData.ms[k].year) { i = k; break; }
    if (i < 0) { server.send(400, "text/plain", "full"); return; }
    if (!(server.hasArg("name") && server.hasArg("y") && server.hasArg("m") && server.hasArg("d"))) { server.send(400, "text/plain", "name,y,m,d needed"); return; }
  }
  LifeMilestone m = lifeData.ms[i];
  if (server.hasArg("name")) lifeSanitizeName(m.name, server.arg("name").c_str());
  if (server.hasArg("y") || server.hasArg("m") || server.hasArg("d")) {
    long y = server.hasArg("y") ? server.arg("y").toInt() : m.year;
    long mo = server.hasArg("m") ? server.arg("m").toInt() : m.month;
    long d = server.hasArg("d") ? server.arg("d").toInt() : m.day;
    if (!lifeDateValid(y, mo, d)) { server.send(400, "text/plain", "bad date"); return; }
    m.year = y; m.month = mo; m.day = d;
  }
  if (server.hasArg("c")) m.completed = server.arg("c").toInt() != 0;
  if (!lifeMsValid(&m) || !m.name[0]) { server.send(400, "text/plain", "invalid"); return; }
  lifeData.ms[i] = m;
  lifeChanged(false);
  lifeJsonOK();
}

void handleLifeMsDel() {
  REQUIRE_AUTH;
  int i = server.arg("i").toInt();
  if (!server.hasArg("i") || i < 0 || i >= LIFE_MAX_MILESTONES) { server.send(400, "text/plain", "bad index"); return; }
  memset(&lifeData.ms[i], 0, sizeof(LifeMilestone));
  lifeChanged(false);
  lifeJsonOK();
}


// ============================================================
//  PC REMOTE  -  DeskBot sends fixed commands to deskbot_pc.py on the laptop
//  Optional overrides in secrets.h:  PC_HOST "192.168.1.20"   PC_PORT 8765   PC_TOKEN "..."
//  (PC_TOKEN defaults to API_TOKEN, so the helper script can use the same token)
// ============================================================
#ifndef PC_HOST
#define PC_HOST "192.168.1.100"
#endif
#ifndef PC_PORT
#define PC_PORT 8765
#endif
#ifndef PC_TOKEN
#define PC_TOKEN API_TOKEN
#endif

struct PcAction { const char* label; const char* cmd; };
static const PcAction PC_ACTIONS[] = {
  {"Play / Pause", "playpause"}, {"Next track", "next"},  {"Previous track", "prev"},
  {"Volume +",     "vol_up"},    {"Volume -",   "vol_down"}, {"Mute",        "mute"},
  {"Lock screen",  "lock"},      {"Sleep",      "sleep"}
};
#define PC_COUNT 8

uint8_t  pcCur = 0;
char     pcMsg[14] = "";
uint32_t pcMsgAt = 0;

void pcSay(const char* m) { strncpy(pcMsg, m, sizeof(pcMsg) - 1); pcMsg[sizeof(pcMsg) - 1] = 0; pcMsgAt = millis(); }

// One short request; returns true when the laptop answered 200.
bool pcSend(const char* cmd) {
  if (WiFi.status() != WL_CONNECTED) { pcSay("No WiFi"); return false; }
  WiFiClient cl;
  cl.setTimeout(800);
  if (!cl.connect(PC_HOST, PC_PORT)) { pcSay("PC offline"); return false; }   // connect uses the 800 ms timeout set above
  cl.printf("GET /cmd?do=%s HTTP/1.0\r\nHost: %s\r\nX-Token: %s\r\nConnection: close\r\n\r\n", cmd, PC_HOST, PC_TOKEN);
  String line = cl.readStringUntil('\n');                    // "HTTP/1.0 200 OK"
  cl.stop();
  int code = line.length() >= 12 ? line.substring(9, 12).toInt() : 0;
  if (code == 200)              { pcSay("Sent");    return true; }
  if (code == 401 || code == 403) pcSay("Denied");
  else if (code == 429)           pcSay("Locked");
  else if (code == 400 || code == 404) pcSay("Bad cmd");
  else                            pcSay("No reply");
  return false;
}

void pcEnter() { pcMsg[0] = 0; }

void pcHandleInput(bool up, bool dn, bool sel) {
  if (up) { pcCur = (pcCur + PC_COUNT - 1) % PC_COUNT; beepClick(); }
  if (dn) { pcCur = (pcCur + 1) % PC_COUNT;            beepClick(); }
  if (sel) { if (pcSend(PC_ACTIONS[pcCur].cmd)) beepSuccess(); else beepClick(); }
}

void pcApp() {
  static uint32_t last = 0;
  if (!frameReady(last, 100)) return;
  display.clearDisplay();
  drawStatusBar();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 12); display.print("PC REMOTE");
  if (pcMsg[0] && millis() - pcMsgAt < 2500UL) {
    display.setCursor(120 - (int)strlen(pcMsg) * 6, 12); display.print(pcMsg);
  }
  int top = pcCur > 3 ? pcCur - 3 : 0;                      // 4 visible rows
  for (int r = 0; r < 4 && top + r < PC_COUNT; r++) {
    int it = top + r, y = 24 + r * 10;
    bool cur = it == pcCur;
    if (cur) display.fillRect(0, y - 1, 120, 10, SH110X_WHITE);
    display.setTextColor(cur ? SH110X_BLACK : SH110X_WHITE);
    display.setCursor(4, y); display.print(PC_ACTIONS[it].label);
    display.setTextColor(SH110X_WHITE);
  }
  int bh = 36 * 4 / PC_COUNT, by = 24 + (36 - bh) * top / (PC_COUNT - 4);   // scroll thumb
  display.drawFastVLine(125, 24, 36, SH110X_WHITE);
  display.fillRect(124, by, 3, bh, SH110X_WHITE);
  present();
}

// ============================================================
//  WIFI MANAGER  -  saved networks, setup hotspot, /wifi page
//  * up to 3 networks are stored in EEPROM (offset 208) and tried in order
//  * secrets.h WIFI_SSID / WIFI_PASS stay as the last-resort built-in network
//  * nothing can be connected -> hotspot "DeskBot-Setup" opens (password shown on the OLED)
//  * from a phone or laptop: scan, tap a network, type the password, done - no code change
// ============================================================
#define WIFI_EEPROM_ADDR 208
#define WIFI_SLOTS       3
#define WIFI_SLOT_SIZE   98                       // 33 ssid + 65 password
#define WIFI_STORE_SIZE  (2 + WIFI_SLOTS * WIFI_SLOT_SIZE + 1)
#define WIFI_STORE_MAGIC 0x57                     // 'W'

char     wifiSsid[WIFI_SLOTS][33];
char     wifiPass[WIFI_SLOTS][65];
bool     wifiSetupMode   = false;                 // hotspot is on
bool     wifiInfoShown   = false;                 // OLED shows the setup instructions
uint32_t wifiSetupAt     = 0;
uint32_t wifiRestartAt   = 0;                     // 0 = no restart pending
char     wifiApPass[9]   = "";
DNSServer wifiDns;

static uint8_t wifiCrc(const uint8_t* p, size_t n) {
  uint8_t c = 0x5A;
  while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1); }
  return c;
}

int wifiCount() { int n = 0; for (int i = 0; i < WIFI_SLOTS; i++) if (wifiSsid[i][0]) n++; return n; }

void wifiSave() {
  uint8_t b[WIFI_STORE_SIZE]; memset(b, 0, sizeof(b));
  b[0] = WIFI_STORE_MAGIC; b[1] = 1;
  for (int i = 0; i < WIFI_SLOTS; i++) {
    memcpy(b + 2 + i * WIFI_SLOT_SIZE, wifiSsid[i], 33);
    memcpy(b + 2 + i * WIFI_SLOT_SIZE + 33, wifiPass[i], 65);
  }
  b[WIFI_STORE_SIZE - 1] = wifiCrc(b, WIFI_STORE_SIZE - 1);
  for (int i = 0; i < WIFI_STORE_SIZE; i++) EEPROM.write(WIFI_EEPROM_ADDR + i, b[i]);
  EEPROM.commit();
}

void wifiLoad() {
  uint8_t b[WIFI_STORE_SIZE];
  for (int i = 0; i < WIFI_STORE_SIZE; i++) b[i] = EEPROM.read(WIFI_EEPROM_ADDR + i);
  memset(wifiSsid, 0, sizeof(wifiSsid)); memset(wifiPass, 0, sizeof(wifiPass));
  if (b[0] != WIFI_STORE_MAGIC || b[1] != 1 || b[WIFI_STORE_SIZE - 1] != wifiCrc(b, WIFI_STORE_SIZE - 1)) return;   // blank -> no saved networks
  for (int i = 0; i < WIFI_SLOTS; i++) {
    memcpy(wifiSsid[i], b + 2 + i * WIFI_SLOT_SIZE, 33);      wifiSsid[i][32] = 0;
    memcpy(wifiPass[i], b + 2 + i * WIFI_SLOT_SIZE + 33, 65); wifiPass[i][64] = 0;
  }
}

static void wifiMoveToFront(int i) {
  if (i <= 0 || i >= WIFI_SLOTS) return;
  char s[33], p[65];
  memcpy(s, wifiSsid[i], 33); memcpy(p, wifiPass[i], 65);
  for (int k = i; k > 0; k--) { memcpy(wifiSsid[k], wifiSsid[k - 1], 33); memcpy(wifiPass[k], wifiPass[k - 1], 65); }
  memcpy(wifiSsid[0], s, 33); memcpy(wifiPass[0], p, 65);
}

// 0 = ok, 1 = bad name, 2 = bad password
int wifiAdd(const char* ssid, const char* pass) {
  size_t sl = strlen(ssid), pl = strlen(pass);
  if (sl < 1 || sl > 32) return 1;
  if (pl != 0 && (pl < 8 || pl > 63)) return 2;               // WPA2 passwords are 8..63 characters
  int at = -1;
  for (int i = 0; i < WIFI_SLOTS; i++) if (!strcmp(wifiSsid[i], ssid)) at = i;
  if (at < 0) {                                               // new network: take the last slot (oldest drops out)
    at = WIFI_SLOTS - 1;
    for (int i = 0; i < WIFI_SLOTS; i++) if (!wifiSsid[i][0]) { at = i; break; }
  }
  memset(wifiSsid[at], 0, 33); memset(wifiPass[at], 0, 65);
  memcpy(wifiSsid[at], ssid, sl); memcpy(wifiPass[at], pass, pl);
  wifiMoveToFront(at);
  wifiSave();
  return 0;
}

void wifiRemove(int i) {
  if (i < 0 || i >= WIFI_SLOTS) return;
  for (int k = i; k < WIFI_SLOTS - 1; k++) { memcpy(wifiSsid[k], wifiSsid[k + 1], 33); memcpy(wifiPass[k], wifiPass[k + 1], 65); }
  memset(wifiSsid[WIFI_SLOTS - 1], 0, 33); memset(wifiPass[WIFI_SLOTS - 1], 0, 65);
  wifiSave();
}

static void wifiShort(char* out, size_t n, const char* ssid) {   // OLED-safe copy (the font is ASCII only)
  size_t k = 0;
  for (; ssid[k] && k + 1 < n; k++) { unsigned char c = ssid[k]; out[k] = (c >= 32 && c < 127) ? c : '?'; }
  out[k] = 0;
}

// ---------------- connecting ----------------
static bool wifiTry(const char* ssid, const char* pass, uint32_t ms) {
  char nm[22]; wifiShort(nm, sizeof(nm), ssid);
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(14, 6);  display.println("Connecting to WiFi");
  display.setCursor(14, 16); display.print("SSID: "); display.println(nm);
  present();
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis(); int a = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) {
    a++;
    display.fillRect(10, 30, 108, 22, SH110X_BLACK);
    for (int d = 0; d < 3; d++) display.fillCircle(34 + d * 24, ((a + d) % 3 == 0) ? 32 : 40, 5, SH110X_WHITE);
    present();
    delay(320);
  }
  return WiFi.status() == WL_CONNECTED;
}

static bool wifiInScan(int n, const char* ssid) {
  for (int i = 0; i < n; i++) if (WiFi.SSID(i) == ssid) return true;
  return false;
}

// Tries visible saved networks first, then the built-in one, then networks that were not seen (hidden ones).
bool wifiConnectAll() {
  WiFi.persistent(false);                                     // do not wear out the flash with every begin()
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(14, 24); display.println("Looking for WiFi...");
  present();
  int n = WiFi.scanNetworks();
  if (n <= 0) { delay(300); n = WiFi.scanNetworks(); }
  bool scanOK = n > 0;

  const char* ss[WIFI_SLOTS + 1]; const char* pp[WIFI_SLOTS + 1]; int cnt = 0;
  for (int i = 0; i < WIFI_SLOTS; i++) if (wifiSsid[i][0]) { ss[cnt] = wifiSsid[i]; pp[cnt] = wifiPass[i]; cnt++; }
  bool dup = false;
  for (int i = 0; i < cnt; i++) if (!strcmp(ss[i], WIFI_SSID)) dup = true;
  if (!dup && WIFI_SSID[0]) { ss[cnt] = WIFI_SSID; pp[cnt] = WIFI_PASS; cnt++; }

  bool tried[WIFI_SLOTS + 1] = {false};
  for (int pass = 0; pass < 2; pass++) {                      // pass 0: seen in the scan, pass 1: everything else
    for (int i = 0; i < cnt; i++) {
      if (tried[i]) continue;
      bool seen = scanOK && wifiInScan(n, ss[i]);
      if ((pass == 0) != (seen || !scanOK)) continue;
      tried[i] = true;
      if (wifiTry(ss[i], pp[i], pass == 0 ? 9000UL : 5000UL)) {
        WiFi.scanDelete();
        if (i < WIFI_SLOTS && wifiSsid[i][0] && ss[i] == wifiSsid[i] && i > 0) { wifiMoveToFront(i); wifiSave(); }   // remember what worked
        return true;
      }
      WiFi.disconnect();
    }
  }
  WiFi.scanDelete();
  return false;
}

// ---------------- setup hotspot ----------------
void wifiStartSetup(bool showInfo) {
  if (!wifiSetupMode) {
    for (int i = 0; i < 8; i++) wifiApPass[i] = '0' + (RANDOM_REG32 % 10);   // new random password every time
    wifiApPass[8] = 0;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("DeskBot-Setup", wifiApPass);
    wifiDns.start(53, "*", WiFi.softAPIP());                  // phones open the setup page by themselves
    wifiSetupMode = true; wifiSetupAt = millis();
  }
  wifiInfoShown = showInfo;
}

void wifiStopSetup() {
  wifiDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  wifiSetupMode = false; wifiInfoShown = false;
}

void wifiTask() {                                             // called from loop()
  if (wifiSetupMode) {
    wifiDns.processNextRequest();
    if (WiFi.status() == WL_CONNECTED && millis() - wifiSetupAt > 600000UL) wifiStopSetup();   // 10 min, only when online
  }
  if (wifiRestartAt && (int32_t)(millis() - wifiRestartAt) >= 0) {
    flushConfig(true); lifeFlush(true);
    ESP.restart();
  }
}

void drawWifiInfo() {
  display.clearDisplay();
  display.fillRect(0, 0, 128, 10, SH110X_WHITE);
  display.setTextSize(1);
  display.setTextColor(SH110X_BLACK); display.setCursor(4, 1); display.print("WIFI SETUP");
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 13); display.print("Join: DeskBot-Setup");
  display.setCursor(0, 23); display.print("Pass: "); display.print(wifiApPass);
  display.setCursor(0, 33); display.print("Open: 192.168.4.1");
  display.setCursor(0, 44);
  if (WiFi.status() == WL_CONNECTED) { display.print("Or: "); display.print(WiFi.localIP().toString()); display.print("/wifi"); }
  else display.print("Not on any WiFi");
  display.setCursor(0, 56); display.print("any key = close");
  present();
}

// ---------------- web: /wifi and /api/wifi/* ----------------
static bool wifiOnAp() { return wifiSetupMode && server.client().localIP() == WiFi.softAPIP(); }
static bool wifiAuth() { return wifiOnAp() ? true : authOK(); }     // joined the hotspot with its password = trusted
#define REQUIRE_WIFI_AUTH  if (!wifiAuth()) return

static void wifiJsonStr(String& o, const char* s) {
  o += '"';
  for (; *s; s++) {
    unsigned char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += (char)c;                                        // UTF-8 (Persian etc.) passes through unchanged
  }
  o += '"';
}

void handleWifiGet() {
  REQUIRE_WIFI_AUTH;
  String o; o.reserve(500);
  o += "{\"cur\":";
  if (WiFi.status() == WL_CONNECTED) wifiJsonStr(o, WiFi.SSID().c_str()); else o += "\"\"";
  o += ",\"ip\":\""; o += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
  o += "\",\"rssi\":"; o += (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  o += ",\"ap\":"; o += wifiSetupMode ? 1 : 0;
  o += ",\"max\":"; o += WIFI_SLOTS;
  o += ",\"saved\":[";
  for (int i = 0; i < WIFI_SLOTS; i++) if (wifiSsid[i][0]) { if (o[o.length() - 1] != '[') o += ','; wifiJsonStr(o, wifiSsid[i]); }
  o += "]}";
  server.send(200, "application/json", o);
}

void handleWifiScan() {                                       // first call starts a scan, later calls report progress
  REQUIRE_WIFI_AUTH;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { server.send(200, "application/json", "{\"state\":\"busy\"}"); return; }
  if (n < 0) { WiFi.scanNetworks(true); server.send(200, "application/json", "{\"state\":\"busy\"}"); return; }
  if (n > 30) n = 30;
  int idx[30]; for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 1; i < n; i++) { int k = idx[i], j = i - 1; while (j >= 0 && WiFi.RSSI(idx[j]) < WiFi.RSSI(k)) { idx[j + 1] = idx[j]; j--; } idx[j + 1] = k; }   // strongest first
  String o; o.reserve(1800);
  o += "{\"state\":\"done\",\"nets\":[";
  int emitted = 0;
  for (int a = 0; a < n && emitted < 20; a++) {
    String s = WiFi.SSID(idx[a]);
    if (!s.length()) continue;                                // hidden network
    bool dupe = false;
    for (int b = 0; b < a; b++) if (WiFi.SSID(idx[b]) == s) dupe = true;
    if (dupe) continue;
    if (emitted++) o += ',';
    o += "{\"s\":"; wifiJsonStr(o, s.c_str());
    o += ",\"r\":"; o += WiFi.RSSI(idx[a]);
    o += ",\"e\":"; o += (WiFi.encryptionType(idx[a]) != ENC_TYPE_NONE) ? 1 : 0;
    o += '}';
  }
  o += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", o);
}

void handleWifiAdd() {                                        // POST ssid, pass  -> saves as the preferred network, then restarts
  REQUIRE_WIFI_AUTH;
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "ssid needed"); return; }
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  int r = wifiAdd(ssid.c_str(), pass.c_str());
  if (r == 1) { server.send(400, "text/plain", "bad network name"); return; }
  if (r == 2) { server.send(400, "text/plain", "password must be 8 to 63 characters"); return; }
  wifiRestartAt = millis() + 1500;
  server.send(200, "application/json", "{\"ok\":1,\"restart\":1}");
}

void handleWifiDel() {
  REQUIRE_WIFI_AUTH;
  int i = server.arg("i").toInt();
  if (!server.hasArg("i") || i < 0 || i >= WIFI_SLOTS || !wifiSsid[i][0]) { server.send(400, "text/plain", "bad index"); return; }
  wifiRemove(i);
  server.send(200, "application/json", "{\"ok\":1}");
}

void handleWifiUse() {                                        // make a saved network the preferred one and restart
  REQUIRE_WIFI_AUTH;
  int i = server.arg("i").toInt();
  if (!server.hasArg("i") || i < 0 || i >= WIFI_SLOTS || !wifiSsid[i][0]) { server.send(400, "text/plain", "bad index"); return; }
  wifiMoveToFront(i); wifiSave();
  wifiRestartAt = millis() + 1500;
  server.send(200, "application/json", "{\"ok\":1,\"restart\":1}");
}

static_assert(WIFI_EEPROM_ADDR + WIFI_STORE_SIZE <= EEPROM_SIZE, "raise EEPROM_SIZE");

const char WIFI_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DeskBot WiFi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0a0a;color:#e0e0e0;font-family:'Courier New',monospace;padding:16px;max-width:560px;margin:auto}
h1{font-size:24px;color:#fff;letter-spacing:4px;text-align:center}
.sub{font-size:11px;color:#555;text-align:center;margin:4px 0 16px;letter-spacing:1px}
.card{background:#111;border:1px solid #222;border-radius:8px;padding:14px;margin-bottom:12px}
.lbl{font-size:10px;color:#555;letter-spacing:2px;text-transform:uppercase;margin-bottom:8px}
.val{font-size:18px;color:#fff;font-weight:bold;word-break:break-all}.small{font-size:12px;color:#888;margin-top:4px}
.item{display:flex;gap:8px;align-items:center;padding:10px 0;border-bottom:1px solid #1c1c1c;font-size:13px}
.item:last-child{border:0}.item .n{flex:1;word-break:break-all;cursor:pointer}.item .m{color:#666;font-size:11px;white-space:nowrap}
.row{display:flex;gap:8px;margin-top:8px;align-items:center}
button,input{font-family:inherit;font-size:13px;background:#1a1a1a;color:#e0e0e0;border:1px solid #333;border-radius:6px;padding:10px}
button{cursor:pointer}button:hover{border-color:#777}button.go{background:#00ff88;color:#000;border:0;font-weight:bold;flex:1}
input[type=text],input[type=password]{flex:1;min-width:0}input[type=checkbox]{padding:0}
.msg{margin-top:10px;font-size:12px;color:#00ff88;min-height:16px}.err{color:#ff5566}
a{color:#00ff88}
</style></head><body>
<h1>WIFI</h1><div class="sub" id="sub">connecting...</div>
<div id="login" class="card" hidden><div class="lbl">Access token</div>
<div class="row"><input id="tk" type="password" placeholder="token"><button onclick="saveT()">UNLOCK</button></div></div>
<div id="app" hidden>
<div class="card"><div class="lbl">Now</div><div class="val" id="cur">--</div><div class="small" id="curd"></div></div>
<div class="card"><div class="lbl">Saved networks</div><div id="sv"></div>
<div class="small">The first one is tried first when DeskBot starts.</div></div>
<div class="card"><div class="lbl">Nearby networks</div><div id="nets"><div class="small">Press SCAN.</div></div>
<div class="row"><button class="go" onclick="scan()" id="sb">SCAN</button></div></div>
<div class="card"><div class="lbl">Connect to a network</div>
<div class="row"><input id="ss" type="text" placeholder="network name" maxlength="32" autocomplete="off" autocapitalize="off"></div>
<div class="row"><input id="pw" type="password" placeholder="password (empty = open network)" maxlength="63" autocomplete="off"></div>
<div class="row"><label class="small"><input type="checkbox" onclick="$('pw').type=this.checked?'text':'password'"> show password</label></div>
<div class="row"><button class="go" onclick="add()">SAVE &amp; CONNECT</button></div>
<div class="msg" id="msg"></div>
<div class="small">DeskBot restarts to join the new network. Then reconnect your phone or laptop to your normal WiFi and open the address shown on the DeskBot screen.</div></div>
<div class="small" style="text-align:center"><a href="/">dashboard</a></div>
</div>
<script>
var T='';try{T=localStorage.getItem('t')||''}catch(e){}
var $=function(i){return document.getElementById(i)};
function say(t,e){$('msg').textContent=t;$('msg').className='msg'+(e?' err':'')}
function saveT(){T=$('tk').value;try{localStorage.setItem('t',T)}catch(e){}load()}
function lock(b){$('login').hidden=!b;$('app').hidden=b}
function api(p,o){o=o||{};o.headers={'X-Token':T};return fetch(p,o).then(function(r){
 if(r.status==401){lock(true);$('sub').textContent='enter token';throw 1}
 if(r.status==429){$('sub').textContent='too many tries, wait 30 s';throw 2}
 if(r.status==400)return r.text().then(function(t){say(t,1);throw 3});
 return r})}
function bars(r){return r>-55?'▂▄▆█':r>-67?'▂▄▆':r>-80?'▂▄':'▂'}
function mk(t,c,f){var b=document.createElement('button');b.textContent=t;b.onclick=f;return b}
function load(){api('/api/wifi').then(function(r){return r.json()}).then(function(d){lock(false);
 $('sub').textContent=d.ap?'setup hotspot is on':'connected';
 $('cur').textContent=d.cur||'not connected';$('curd').textContent=d.cur?d.ip+'  ·  '+d.rssi+' dBm':'';
 var h=$('sv');h.textContent='';
 if(!d.saved.length){h.innerHTML='<div class="small">Nothing saved yet.</div>'}
 d.saved.forEach(function(s,i){var r=document.createElement('div');r.className='item';
  var n=document.createElement('span');n.className='n';n.textContent=(i==0?'★ ':'')+s;r.appendChild(n);
  if(i>0)r.appendChild(mk('USE',0,function(){if(confirm('Switch to '+s+'? DeskBot will restart.'))api('/api/wifi/use?i='+i).then(done)}));
  r.appendChild(mk('X',0,function(){if(confirm('Forget '+s+'?'))api('/api/wifi/del?i='+i).then(load)}));h.appendChild(r)})}).catch(function(){})}
function done(){say('Restarting DeskBot... reconnect to your normal WiFi in about 20 seconds.');}
var tries=0;
function scan(){$('sb').disabled=true;$('sb').textContent='SCANNING...';tries=0;poll()}
function poll(){api('/api/wifi/scan').then(function(r){return r.json()}).then(function(d){
 if(d.state=='busy'&&tries++<14){setTimeout(poll,1500);return}
 $('sb').disabled=false;$('sb').textContent='SCAN AGAIN';
 var h=$('nets');h.textContent='';
 if(d.state!='done'||!d.nets.length){h.innerHTML='<div class="small">No networks found. Try again.</div>';return}
 d.nets.forEach(function(x){var r=document.createElement('div');r.className='item';
  var n=document.createElement('span');n.className='n';n.textContent=x.s;
  n.onclick=function(){$('ss').value=x.s;$('pw').focus()};r.appendChild(n);
  var m=document.createElement('span');m.className='m';m.textContent=bars(x.r)+(x.e?' 🔒':' open');r.appendChild(m);h.appendChild(r)})
 }).catch(function(){$('sb').disabled=false;$('sb').textContent='SCAN'})}
function add(){var s=$('ss').value;if(!s){say('Type or tap a network name',1);return}
 var b=new URLSearchParams();b.append('ssid',s);b.append('pass',$('pw').value);
 api('/api/wifi/add',{method:'POST',body:b}).then(function(){say('Saved. DeskBot is restarting and joining "'+s+'"...')})}
load();
</script></body></html>
)rawhtml";

void handleStatus() {
  REQUIRE_AUTH;
  DynamicJsonDocument doc(1536);
  char tb[8]; snprintf(tb, sizeof(tb), "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
  doc["time"] = tb;
  doc["temp"] = weatherValid ? dispTemp(tempC) : 0;
  doc["unit"] = tempUnit();
  doc["desc"] = weatherValid ? weatherDesc : String("--");
  doc["hum"]  = humidity;
  doc["city"] = CITIES[cityIdx];
  doc["ci"]   = cityIdx;
  JsonArray cs = doc.createNestedArray("cities");
  for (int i = 0; i < CITY_COUNT; i++) cs.add(CITIES[i]);

  JsonObject pm = doc.createNestedObject("pomo");
  pm["state"] = !pomoActive ? "idle" : (isPaused ? "paused" : (isWork ? "work" : "break"));
  pm["left"]  = timerValue;
  pm["today"] = todaySessions;
  pm["total"] = totalSessions;

  JsonArray hs = doc.createNestedArray("habits");
  for (int i = 0; i < TOTAL_HABITS; i++) {
    JsonObject o = hs.createNestedObject();
    o["n"] = habitNames[i]; o["d"] = habitDoneToday[i]; o["s"] = habitStreak[i];
  }
  JsonObject al = doc.createNestedObject("alarm");
  al["h"] = alarmHour; al["m"] = alarmMinute; al["on"] = alarmEnabled;

  doc["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();
  doc["up"]   = millis() / 60000UL;
  doc["pir"]  = pirEnabled;
  doc["here"] = presentNow;
  doc["desk"] = deskMin;
  doc["noise"] = batteryEnabled ? 0 : noisePercent();
  doc["clap"] = clapEnabled;

  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleCmd() {
  REQUIRE_AUTH;
  String c = server.arg("c");
  if      (c == "pomo_pause")  pomoToggle();
  else if (c == "pomo_reset")  { pomoStart(); showNotif("Pomodoro reset"); }
  else if (c == "goto_home")   { currentScreen = SCR_HOME; activeApp = -1; inClockSub = false; inSettingsCat = false; }
  else if (c == "face_next")   emoIndex = (emoIndex + 1) % EMO_COUNT;
  else if (c == "face_prev")   emoIndex = (emoIndex + EMO_COUNT - 1) % EMO_COUNT;
  else if (c == "clock_next")  { clockFace = (clockFace + 1) % 4; saveSettings(); }
  else if (c == "alarm_stop")  stopAlarm();
  else { server.send(400, "text/plain", "Unknown command"); return; }
  server.send(200, "text/plain", "OK");
}

void handleNotif() {
  REQUIRE_AUTH;
  String msg = server.arg("msg");
  if (!msg.length()) { server.send(400, "text/plain", "msg required"); return; }
  showNotif(msg.substring(0, 20)); beepTone(1000, 100);
  server.send(200, "text/plain", "OK");
}

void handleHabit() {
  REQUIRE_AUTH;
  if (!server.hasArg("i") || !server.hasArg("done")) { server.send(400, "text/plain", "i and done required"); return; }
  int i = server.arg("i").toInt();
  if (i < 0 || i >= TOTAL_HABITS) { server.send(400, "text/plain", "bad habit"); return; }
  setHabit(i, server.arg("done").toInt() != 0);
  server.send(200, "text/plain", "OK");
}

void handleAlarm() {
  REQUIRE_AUTH;
  int h = server.arg("h").toInt(), m = server.arg("m").toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) { server.send(400, "text/plain", "bad time"); return; }
  alarmHour = h; alarmMinute = m; alarmEnabled = server.arg("on").toInt() != 0; alarmFired = false;
  saveSettings();
  server.send(200, "text/plain", "OK");
}

void handleCity() {
  REQUIRE_AUTH;
  int i = server.arg("i").toInt();
  if (!server.hasArg("i") || i < 0 || i >= CITY_COUNT) { server.send(400, "text/plain", "bad city"); return; }
  cityIdx = i; weatherValid = false; weatherForce = true; saveSettings();
  server.send(200, "text/plain", "OK");
}

void setupWebServer() {
  server.collectHeaders("X-Token");              // core 3.x takes the header names as arguments
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", DASHBOARD_HTML); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/cmd",   handleCmd);   server.on("/cmd",   handleCmd);     // "/cmd" and "/notif" kept for old shortcuts
  server.on("/api/notif", handleNotif); server.on("/notif", handleNotif);
  server.on("/api/habit", handleHabit);
  server.on("/api/alarm", handleAlarm);
  server.on("/api/city",  handleCity);
  server.on("/wifi", HTTP_GET, []() { server.send_P(200, "text/html", WIFI_HTML); });
  server.on("/api/wifi", HTTP_GET, handleWifiGet);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/add", HTTP_POST, handleWifiAdd);
  server.on("/api/wifi/del", HTTP_GET, handleWifiDel);
  server.on("/api/wifi/use", HTTP_GET, handleWifiUse);
  server.on("/life", HTTP_GET, []() { server.send_P(200, "text/html", LIFE_HTML); });
  server.on("/api/life", HTTP_GET, handleLifeGet);
  server.on("/api/life/grid", HTTP_GET, handleLifeGrid);
  server.on("/api/life/config", handleLifeConfig);
  server.on("/api/life/ms", handleLifeMs);
  server.on("/api/life/ms/del", handleLifeMsDel);
  server.onNotFound([]() {
    if (wifiOnAp()) { server.sendHeader("Location", "http://192.168.4.1/wifi"); server.send(302, "text/plain", ""); return; }   // phone "sign in to network" popup
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP,     INPUT_PULLUP);
  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(PIR_PIN, INPUT);                       // was never configured before (D0/GPIO16 has no pull-up)

  Wire.begin(D2, D1);
  Wire.setClock(400000);                         // 4x faster screen refresh than the 100 kHz default
  display.begin(0x3C, true);
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  EEPROM.begin(EEPROM_SIZE);
  loadSettings();
  lifeLoad();
  applyTheme();
  display.setContrast(brightness);               // brightness setting now actually does something

  bootAnimation();
  if (WiFi.status() != WL_CONNECTED) wifiStartSetup(true);   // nothing worked: open the setup hotspot
  setupOTA();
  setupWebServer();

  timeClient.begin();
  timeClient.update();
  lastActivity = millis();

  timerValue = workTime;
  resetDinoGame();
  initMatrix();

  beepSuccess();

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(10,8);  display.println("Web Dashboard ready");
  display.setCursor(10,20); display.print("IP: "); display.println(WiFi.localIP().toString());
  present();
  uint32_t t0 = millis();                        // 2 s splash that keeps sound/OTA alive
  while (millis() - t0 < 2000) { updateSound(); ArduinoOTA.handle(); yield(); }
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  timeClient.update();

  presenceTask();
  micTask();
  checkDailyReset();
  checkAlarm();
  pomodoroTick();
  weatherTask();
  sampleHistory();
  handleButtons();

  updateSound();
  updateFlash();
  flushConfig();
  lifeTask();
  lifeFlush(false);
  wifiTask();

  inFrame = true;                                // apps only draw; the frame is pushed once below
  if (wifiInfoShown) {
    drawWifiInfo();
  } else if (currentScreen == SCR_HOME) {
    if (!screensaverActive && millis()-lastActivity > SCREENSAVER_TIMEOUT) {
      screensaverActive = true;
      initMatrix();
    }
    if (screensaverActive) drawScreensaver();
    else                   drawHome();
  } else if (currentScreen == SCR_DRAWER) {
    drawDrawer();
  } else {
    runActiveApp();
  }
  inFrame = false;

  if (frameDirty) {
    drawNotifBanner();                           // banner is drawn before the single display() call (no flicker)
    display.display();
    frameDirty = false;
  }

  if (millis()-lastActivity>sleepTimeout && currentScreen==SCR_HOME && !wifiInfoShown)
    goToSleep();
}

// ============================================================
//  SYSTEM HISTORY SAMPLING
// ============================================================
void sampleHistory() {
  if (millis() - lastHistSample < 2000) return;
  lastHistSample = millis();
  rssiHistory[histIndex] = WiFi.RSSI();
  ramHistory[histIndex]  = constrain((ESP.getFreeHeap()*100)/81920, 0, 100);
  histIndex = (histIndex + 1) % HIST_LEN;
}

// ============================================================
//  STATUS BAR
// ============================================================
void drawStatusBar() {
  display.fillRect(0,0,128,10,SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setTextSize(1);
  display.setCursor(2,1);
  display.print(formatTime(timeClient.getHours(),timeClient.getMinutes(),-1));
  if (weatherValid) { display.setCursor(60,1); display.print(dispTemp(tempC)); display.print(tempUnit()); }
  if (batteryEnabled)  { display.setCursor(84,1); display.print(batteryPct); display.print("%"); }
  int r=WiFi.RSSI(),bars=0;
  if(r>-50)bars=4;else if(r>-65)bars=3;else if(r>-75)bars=2;else if(r>-85)bars=1;
  for (int b=0;b<4;b++) {
    int bx=106+b*5,bh=2+b*2;
    if(b<bars) display.fillRect(bx,9-bh,3,bh,SH110X_BLACK);
    else       display.drawRect(bx,9-bh,3,bh,SH110X_BLACK);
  }
  display.setTextColor(SH110X_WHITE);
}

// ============================================================
//  NOTIFICATION BANNER
// ============================================================
void drawNotifBanner() {                   // draws into the buffer only; loop() pushes the frame once
  if (!notifActive) return;
  unsigned long age = millis()-notifStart;
  if (age>NOTIF_DURATION) { notifActive=false; return; }
  int bannerY=(age<300)?map((long)age,0,300,-12,0):0;
  display.fillRect(0,bannerY,128,12,SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setTextSize(1);
  display.setCursor(4,bannerY+2);
  display.print(notifMsg.substring(0,20));
  display.setTextColor(SH110X_WHITE);
}

// ============================================================
//  MIC VISUALIZER
// ============================================================
void drawVisualizer(int y, int h) {        // sampling now happens in micTask()
  int barW=128/VIZ_BARS;
  for (int i=0;i<VIZ_BARS;i++) {
    int barH = map(vizBars[i], 0, VIZ_MAX_H, 0, h);
    display.fillRect(i*barW+1, y-barH, barW-2, barH, SH110X_WHITE);
  }
}

// ============================================================
//  HOME SCREEN + WIDGETS
// ============================================================
// ---------- tiny 9x9 icons for the home widgets ----------
void drawBell(int x,int y) {
  display.fillTriangle(x+4,y, x,y+6, x+8,y+6, SH110X_WHITE);
  display.fillRect(x,y+6,9,1,SH110X_WHITE);
  display.drawPixel(x+4,y+8,SH110X_WHITE);
}
void drawFlame(int x,int y,uint16_t c) {       // 5x7 flame for streaks
  display.fillTriangle(x+2,y, x,y+4, x+4,y+4, c);
  display.fillCircle(x+2,y+4,2,c);
}
void drawTinyWeather(int x,int y) {
  if (weatherDesc == "Clear") {
    display.fillCircle(x+4,y+4,2,SH110X_WHITE);
    display.drawPixel(x+4,y,SH110X_WHITE);   display.drawPixel(x+4,y+8,SH110X_WHITE);
    display.drawPixel(x,y+4,SH110X_WHITE);   display.drawPixel(x+8,y+4,SH110X_WHITE);
    display.drawPixel(x+1,y+1,SH110X_WHITE); display.drawPixel(x+7,y+1,SH110X_WHITE);
    display.drawPixel(x+1,y+7,SH110X_WHITE); display.drawPixel(x+7,y+7,SH110X_WHITE);
    return;
  }
  display.fillCircle(x+3,y+4,2,SH110X_WHITE);
  display.fillCircle(x+6,y+3,3,SH110X_WHITE);
  display.fillRect(x+1,y+4,7,3,SH110X_WHITE);
  if (weatherDesc == "Rain" || weatherDesc == "Drizzle" || weatherDesc == "Thunderstorm") {
    display.drawPixel(x+2,y+8,SH110X_WHITE); display.drawPixel(x+5,y+8,SH110X_WHITE);
    display.drawPixel(x+7,y+7,SH110X_WHITE);
  }
}

void drawWidget(int idx, int y) {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  switch (idx) {
    case 0:                                          // weather
      drawTinyWeather(3, y);
      display.setCursor(16,y);
      if (weatherValid) { display.print(dispTemp(tempC)); display.print(tempUnit()); display.print(" "); display.print(weatherDesc); }
      else display.print("--");
      break;
    case 1:                                          // alarm
      drawBell(3, y);
      display.setCursor(16,y);
      if (alarmEnabled) { char b[6]; snprintf(b,sizeof(b),"%02d:%02d",alarmHour,alarmMinute); display.print(b); if (alarmSnoozed) display.print(" snz"); }
      else display.print("Alarm off");
      break;
    case 2:                                          // pomodoro: tomato + progress bar
      display.drawCircle(7,y+5,4,SH110X_WHITE);
      display.drawPixel(7,y,SH110X_WHITE); display.drawPixel(5,y+1,SH110X_WHITE); display.drawPixel(9,y+1,SH110X_WHITE);
      display.setCursor(16,y);
      if (!pomoActive) display.print("Pomo idle");
      else {
        display.print(isPaused ? "paus " : (isWork ? "work " : "rest "));
        display.print(timerValue/60); display.print("m");
        int total = isWork ? workTime : breakTime;
        float frac = total ? 1.0f - (float)timerValue/total : 0;
        display.drawRect(76,y,48,7,SH110X_WHITE);
        display.fillRect(77,y+1,(int)(frac*46),5,SH110X_WHITE);
      }
      break;
    case 3: {                                        // habits: one dot per habit
      int done=0, best=0;
      for (int i=0;i<TOTAL_HABITS;i++) {
        if (habitDoneToday[i]) { done++; display.fillCircle(6+i*9,y+4,3,SH110X_WHITE); }
        else                    display.drawCircle(6+i*9,y+4,3,SH110X_WHITE);
        if (habitStreak[i] > best) best = habitStreak[i];
      }
      display.setCursor(54,y); display.print(done); display.print("/"); display.print(TOTAL_HABITS);
      drawFlame(86,y,SH110X_WHITE);
      display.setCursor(94,y); display.print(best); display.print("d");
      break; }
    case 4:                                          // desk time
      display.fillCircle(7,y+2,2,SH110X_WHITE);
      display.fillRoundRect(3,y+5,9,4,1,SH110X_WHITE);
      display.setCursor(16,y);
      if (pirEnabled) { display.print(deskMin/60); display.print("h "); display.print(deskMin%60); display.print("m"); if (presentNow) display.print(" *"); }
      else display.print("Desk: sensor off");
      break;
  }
}

void drawHome() {
  display.clearDisplay();
  drawStatusBar();

  display.setTextSize(2);
  display.setCursor(8,13);
  display.println(formatTime(timeClient.getHours(),timeClient.getMinutes(),timeClient.getSeconds()));

  const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  display.setTextSize(1);
  display.setCursor(8,32);
  display.print(days[timeClient.getDay()]);

  display.drawLine(0,40,128,40,SH110X_WHITE);

  // rotating widget
  if (millis()-lastWidgetSwitch>WIDGET_SWITCH_MS) {
    widgetIndex=(widgetIndex+1)%WIDGET_COUNT;
    lastWidgetSwitch=millis();
  }
  drawWidget(widgetIndex, 43);

  display.drawLine(0,53,128,53,SH110X_WHITE);

  // mini visualizer
  drawVisualizer(63, 8);

  present();
}

// ============================================================
//  APP DRAWER
// ============================================================
const char* appNames[]={
  " Clock Suite"," Weather"," Dashboard",
  " Face"," Flashcards"," Habits"," Life Clock"," PC Remote"," Dino Game"," Settings"
};

void drawAppIcon(int app,int x,int y) {
  switch(app) {
    case APP_CLOCK:
      display.drawCircle(x+3,y+3,4,SH110X_WHITE);
      display.drawLine(x+3,y+3,x+3,y+1,SH110X_WHITE);
      display.drawLine(x+3,y+3,x+5,y+4,SH110X_WHITE);
      break;
    case APP_WEATHER:
      display.fillCircle(x+2,y+4,2,SH110X_WHITE);
      display.fillCircle(x+5,y+4,2,SH110X_WHITE);
      display.fillCircle(x+3,y+2,2,SH110X_WHITE);
      display.fillRect(x+1,y+4,6,3,SH110X_WHITE);
      break;
    case APP_DASHBOARD:
      display.drawRect(x,y+1,8,6,SH110X_WHITE);
      display.drawLine(x+2,y+4,x+2,y+6,SH110X_WHITE);
      display.drawLine(x+4,y+3,x+4,y+6,SH110X_WHITE);
      display.drawLine(x+6,y+2,x+6,y+6,SH110X_WHITE);
      break;
    case APP_FACE:
      display.drawCircle(x+3,y+3,4,SH110X_WHITE);
      display.drawPixel(x+2,y+2,SH110X_WHITE);
      display.drawPixel(x+4,y+2,SH110X_WHITE);
      break;
    case APP_FLASHCARD:
      display.drawRect(x,y,8,7,SH110X_WHITE);
      display.drawLine(x+1,y+2,x+6,y+2,SH110X_WHITE);
      break;
    case APP_HABITS:
      display.drawRect(x,y,7,7,SH110X_WHITE);
      display.drawLine(x+1,y+4,x+3,y+6,SH110X_WHITE);
      display.drawLine(x+3,y+6,x+6,y+1,SH110X_WHITE);
      break;
    case APP_LIFE:
      for (int r=0;r<3;r++) for (int c=0;c<3;c++) {
        if (r*3+c<5) display.fillRect(x+c*3,y+r*3,2,2,SH110X_WHITE);
        else         display.drawRect(x+c*3,y+r*3,2,2,SH110X_WHITE);
      }
      break;
    case APP_PC:
      display.drawRect(x,y,8,5,SH110X_WHITE);
      display.drawLine(x-1,y+7,x+8,y+7,SH110X_WHITE);
      break;
    case APP_GAME:
      display.drawRect(x,y+2,6,5,SH110X_WHITE);
      display.drawLine(x+6,y+2,x+8,y+2,SH110X_WHITE);
      break;
    case APP_SETTINGS:
      display.drawCircle(x+3,y+3,3,SH110X_WHITE);
      display.drawPixel(x+3,y,   SH110X_WHITE);
      display.drawPixel(x+3,y+6, SH110X_WHITE);
      display.drawPixel(x,  y+3, SH110X_WHITE);
      display.drawPixel(x+6,y+3, SH110X_WHITE);
      break;
  }
}

// ---------- 26x24 app icons for the carousel ----------
void drawBigIcon(int app,int cx,int cy) {
  const uint16_t W = SH110X_WHITE, K = SH110X_BLACK;
  switch (app) {
    case APP_CLOCK:
      display.drawCircle(cx,cy,11,W); display.drawCircle(cx,cy,10,W);
      for (int t=0;t<4;t++) { float a=t*PI/2; display.drawPixel(cx+(int)lroundf(8*sinf(a)),cy-(int)lroundf(8*cosf(a)),W); }
      display.drawLine(cx,cy,cx,cy-6,W); display.drawLine(cx,cy,cx+4,cy+3,W);
      display.fillCircle(cx,cy,1,W);
      break;
    case APP_WEATHER:
      display.drawCircle(cx+6,cy-6,3,W);
      display.drawPixel(cx+6,cy-11,W); display.drawPixel(cx+11,cy-6,W); display.drawPixel(cx+10,cy-10,W);
      display.fillCircle(cx-6,cy+3,5,W); display.fillCircle(cx+1,cy,6,W); display.fillCircle(cx+7,cy+4,4,W);
      display.fillRect(cx-6,cy+3,14,6,W);
      break;
    case APP_DASHBOARD:
      display.drawRect(cx-12,cy-10,24,20,W);
      display.fillRect(cx-8,cy+2,4,7,W); display.fillRect(cx-2,cy-2,4,11,W); display.fillRect(cx+4,cy-6,4,15,W);
      break;
    case APP_FACE:
      display.drawCircle(cx,cy,11,W);
      display.fillRect(cx-6,cy-4,3,5,W); display.fillRect(cx+4,cy-4,3,5,W);
      display.drawLine(cx-5,cy+4,cx-3,cy+7,W); display.drawLine(cx-3,cy+7,cx+3,cy+7,W); display.drawLine(cx+3,cy+7,cx+5,cy+4,W);
      break;
    case APP_FLASHCARD:
      display.drawRoundRect(cx-6,cy-10,18,20,2,W);
      display.fillRoundRect(cx-11,cy-6,18,20,2,K); display.drawRoundRect(cx-11,cy-6,18,20,2,W);
      display.drawLine(cx-8,cy-1,cx+3,cy-1,W); display.drawLine(cx-8,cy+3,cx+3,cy+3,W); display.drawLine(cx-8,cy+7,cx-1,cy+7,W);
      break;
    case APP_HABITS:
      display.drawRoundRect(cx-10,cy-10,20,20,3,W);
      display.drawLine(cx-5,cy,cx-2,cy+4,W); display.drawLine(cx-5,cy+1,cx-2,cy+5,W);
      display.drawLine(cx-2,cy+4,cx+5,cy-5,W); display.drawLine(cx-2,cy+5,cx+5,cy-4,W);
      break;
    case APP_LIFE:
      for (int r=0;r<4;r++) for (int c=0;c<5;c++) {
        int px=cx-12+c*5, py=cy-10+r*5, k=r*5+c;
        if (k<11) display.fillRect(px,py,4,4,W);
        else if (k==11) { display.drawRect(px,py,4,4,W); display.drawPixel(px+1,py+1,W); }
        else display.drawRect(px,py,4,4,W);
      }
      break;
    case APP_PC:
      display.drawRect(cx-10,cy-9,21,14,W); display.drawRect(cx-8,cy-7,17,10,W);
      display.drawLine(cx-13,cy+8,cx+14,cy+8,W); display.drawLine(cx-13,cy+9,cx+14,cy+9,W);
      display.fillRect(cx-3,cy+5,7,2,W);
      display.drawPixel(cx,cy-2,W); display.drawPixel(cx-2,cy-1,W); display.drawPixel(cx+2,cy-1,W);
      break;
    case APP_GAME:
      display.drawRoundRect(cx-12,cy-6,24,14,5,W);
      display.fillRect(cx-8,cy-1,6,2,W); display.fillRect(cx-6,cy-3,2,6,W);
      display.fillCircle(cx+4,cy+2,2,W); display.fillCircle(cx+8,cy-1,2,W);
      break;
    case APP_SETTINGS:
      display.drawCircle(cx,cy,7,W); display.drawCircle(cx,cy,3,W);
      for (int i=0;i<8;i++) { float a=i*PI/4; display.fillCircle(cx+(int)lroundf(9.5f*cosf(a)),cy+(int)lroundf(9.5f*sinf(a)),2,W); }
      break;
  }
}

void drawDrawer() {
  display.clearDisplay();
  drawStatusBar();
  display.setTextColor(SH110X_WHITE);
  const int cx = 64, cy = 29;

  int prev = (drawerIndex + TOTAL_APPS - 1) % TOTAL_APPS;
  int next = (drawerIndex + 1) % TOTAL_APPS;
  drawAppIcon(prev, 14, cy-4);                                  // neighbours peek in from the sides
  drawAppIcon(next, 106, cy-4);
  display.fillTriangle(3,cy, 7,cy-3, 7,cy+3, SH110X_WHITE);     // arrows
  display.fillTriangle(124,cy, 120,cy-3, 120,cy+3, SH110X_WHITE);

  display.drawRoundRect(cx-20, cy-17, 40, 34, 6, SH110X_WHITE); // selected tile
  drawBigIcon(drawerIndex, cx, cy);

  const char* nm = appNames[drawerIndex] + 1;                   // names start with a space
  int w = strlen(nm) * 6;
  display.setTextSize(1);
  display.setCursor((128 - w) / 2, 50);
  display.print(nm);

  int x0 = 64 - (TOTAL_APPS * 6) / 2;                           // page dots
  for (int i = 0; i < TOTAL_APPS; i++) {
    if (i == drawerIndex) display.fillRect(x0 + i*6, 60, 4, 3, SH110X_WHITE);
    else                  display.drawPixel(x0 + i*6 + 1, 61, SH110X_WHITE);
  }
  present();
}

// ============================================================
//  BUTTON HANDLER
// ============================================================
// ============================================================
//  UPDATE 1 / PART B  —  replace the whole old handleButtons()
//  (every delay(150)/delay(200)/delay(300) is gone; input comes
//   from pollButtons() in Part A)
// ============================================================
void handleButtons() {
  pollButtons();
  const bool up = evt.up, dn = evt.dn, sel = evt.sel;
  if (!(up || dn || sel || evt.selLong || evt.back)) return;

  if (wifiInfoShown) { wifiInfoShown = false; beepClick(); return; }      // any key closes the WiFi setup screen

  // a ringing alarm: any button stops it (a double clap snoozes it)
  if (alarmRinging && !evt.back) { stopAlarm(); beepClick(); return; }

  // ---- UP+DN : back one level ----
  if (evt.back) {
    if (currentScreen == SCR_APP) {
      if (activeApp == APP_CLOCK && inClockSub) {
        inClockSub = false; swRunning = false; ctRunning = false; pomodoroScreen = 0;   // pomodoro keeps running
        saveSettings();                                   // alarm / clock face edits
      } else if (activeApp == APP_LIFE && lifeBack()) {
        // consumed: left an edit / sub-mode only
      } else if (activeApp == APP_SETTINGS && inSettingsCat) {
        inSettingsCat = false; editingSettingsItem = false; saveSettings();
      } else {
        currentScreen = SCR_DRAWER; activeApp = -1; inClockSub = false; inSettingsCat = false;
      }
    } else if (currentScreen == SCR_DRAWER) {
      currentScreen = SCR_HOME;
    }
    beepClick();
    return;
  }

  // ---- SELECT long press : quick pause / resume the Pomodoro from anywhere ----
  if (evt.selLong) {
    if (activeApp == APP_LIFE) { lifeGoOverview(); beepClick(); return; }
    if (activeApp != APP_GAME) {
      pomoToggle();
      beepClick();
    }
    return;
  }

  if (currentScreen == SCR_HOME) {
    if (sel) { currentScreen = SCR_DRAWER; beepClick(); }
    return;
  }

  if (currentScreen == SCR_DRAWER) {
    if (up) { drawerIndex = (drawerIndex + TOTAL_APPS - 1) % TOTAL_APPS; beepClick(); }
    if (dn) { drawerIndex = (drawerIndex + 1) % TOTAL_APPS;              beepClick(); }
    if (sel) {
      activeApp = drawerIndex; currentScreen = SCR_APP;
      if (activeApp == APP_CLOCK)    { clockSubIndex = 0; inClockSub = false; }
      if (activeApp == APP_SETTINGS) { settingsCatIndex = 0; inSettingsCat = false; }
      if (activeApp == APP_LIFE)     { lifeEnter(); }
      if (activeApp == APP_PC)       { pcEnter(); }
      if (activeApp == APP_GAME)     { resetDinoGame(); }
      beepClick();
    }
    return;
  }

  if (currentScreen != SCR_APP) return;

  switch (activeApp) {

    case APP_LIFE:
      lifeHandleInput(up, dn, sel);
      return;

    case APP_PC:
      pcHandleInput(up, dn, sel);
      return;

    case APP_GAME:                       // dinoGameApp() reads SELECT itself
      return;

    case APP_CLOCK:
      if (!inClockSub) {
        if (up) { clockSubIndex = (clockSubIndex + TOTAL_CLKSUB - 1) % TOTAL_CLKSUB; beepClick(); }
        if (dn) { clockSubIndex = (clockSubIndex + 1) % TOTAL_CLKSUB;                beepClick(); }
        if (sel) {
          inClockSub = true; beepClick();
          if (clockSubIndex == CLKSUB_POMODORO && !pomoActive) pomoStart();   // joins a running session instead of restarting it
          if (clockSubIndex == CLKSUB_STOPWATCH) { swElapsed = 0; swRunning = false; swLapCount = 0; }
          if (clockSubIndex == CLKSUB_TIMER)     { ctSetting = true; ctMinutes = 5; }
        }
        return;
      }
      switch (clockSubIndex) {
        case CLKSUB_CLOCK:
          if (up) { clockFace = (clockFace + 1) % 4; saveSettings(); }
          if (dn) { clockFace = (clockFace + 3) % 4; saveSettings(); }
          break;
        case CLKSUB_POMODORO:
          if (up) pomodoroScreen = !pomodoroScreen;
          if (dn) pomoToggle();
          break;
        case CLKSUB_STOPWATCH:
          if (up && swRunning) { swLaps[swLapCount % 3] = swElapsed + (millis() - swStart); swLapCount++; }
          if (dn) {
            if (swRunning) { swElapsed += millis() - swStart; swRunning = false; }
            else           { swStart = millis(); swRunning = true; }
          }
          if (sel) { swElapsed = 0; swRunning = false; swLapCount = 0; }
          break;
        case CLKSUB_TIMER:
          if (ctSetting) {
            if (up) ctMinutes = constrain(ctMinutes + 1, 0, 99);
            if (dn) ctMinutes = constrain(ctMinutes - 1, 0, 99);
            if (sel) { ctTotal = ctMinutes * 60; ctSetting = false; ctRunning = true; ctFinished = false; lastCtTick = millis(); }
          } else {
            if (up)  ctRunning = !ctRunning;
            if (sel) { ctSetting = true; ctRunning = false; ctFinished = false; }
          }
          break;
        case CLKSUB_ALARM:
          if (up || dn) {
            int d = up ? 1 : -1;
            if (alarmSetPos == 0) alarmHour   = (alarmHour + 24 + d) % 24;
            if (alarmSetPos == 1) alarmMinute = (alarmMinute + 60 + d) % 60;
            if (alarmSetPos == 2) alarmEnabled = !alarmEnabled;
            alarmFired = false;
            saveSettings();
          }
          if (sel) alarmSetPos = (alarmSetPos + 1) % 3;
          break;
      }
      return;

    case APP_FLASHCARD:
      if (up) { cardViewIndex = (cardViewIndex + DAILY_CARDS - 1) % DAILY_CARDS; cardFlipped = false; }
      if (dn) { cardViewIndex = (cardViewIndex + 1) % DAILY_CARDS;               cardFlipped = false; }
      if (sel) { cardFlipped = !cardFlipped; beepClick(); }
      return;

    case APP_FACE:
      if (up) emoIndex = (emoIndex + EMO_COUNT - 1) % EMO_COUNT;
      if (dn) emoIndex = (emoIndex + 1) % EMO_COUNT;
      return;

    case APP_HABITS:
      if (up) habitIndex = (habitIndex + TOTAL_HABITS - 1) % TOTAL_HABITS;
      if (dn) habitIndex = (habitIndex + 1) % TOTAL_HABITS;
      if (sel) {
        bool nowDone = !habitDoneToday[habitIndex];
        setHabit(habitIndex, nowDone);                    // un-check on the same day is allowed
        if (nowDone) beepSuccess(); else beepClick();
      }
      return;

    case APP_DASHBOARD:
      if (up) dashScreen = (dashScreen + 1) % 3;
      return;

    case APP_SETTINGS:
      if (!inSettingsCat) {
        if (up) settingsCatIndex = (settingsCatIndex + TOTAL_SETCAT - 1) % TOTAL_SETCAT;
        if (dn) settingsCatIndex = (settingsCatIndex + 1) % TOTAL_SETCAT;
        if (sel) { inSettingsCat = true; settingsItemIndex = 0; editingSettingsItem = false; }
      } else if (!editingSettingsItem) {
        int maxItem = categoryItemCounts[settingsCatIndex] - 1;
        if (up) settingsItemIndex = max(settingsItemIndex - 1, 0);
        if (dn) settingsItemIndex = min(settingsItemIndex + 1, maxItem);   // was unbounded before
        if (sel) {
          if (settingsIsAction(settingsCatIndex, settingsItemIndex)) runSettingsAction(settingsCatIndex, settingsItemIndex);
          else editingSettingsItem = true;
        }
      } else {
        if (up) adjustSettingsItem( 1);                   // UP = increase (it was inverted before)
        if (dn) adjustSettingsItem(-1);
        if (sel) { editingSettingsItem = false; saveSettings(); showNotif("Settings saved!"); beepSuccess(); }
      }
      return;
  }
}

// ============================================================
//  APP ROUTER
// ============================================================
void runActiveApp() {
  switch(activeApp) {
    case APP_CLOCK:
      if (!inClockSub) drawClockSuiteMenu();
      else {
        switch(clockSubIndex) {
          case CLKSUB_CLOCK:     clockFaceApp();  break;
          case CLKSUB_POMODORO:  pomodoroApp();   break;
          case CLKSUB_STOPWATCH: stopwatchApp();  break;
          case CLKSUB_TIMER:     countdownApp();  break;
          case CLKSUB_ALARM:     alarmApp();      break;
        }
      }
      break;
    case APP_WEATHER:   weatherApp();   break;
    case APP_DASHBOARD: dashboardApp(); break;
    case APP_FACE:      faceApp();      break;
    case APP_FLASHCARD: flashcardApp(); break;
    case APP_HABITS:    habitApp();     break;
    case APP_LIFE:      lifeApp();      break;
    case APP_PC:        pcApp();        break;
    case APP_GAME:       dinoGameApp(); break;
    case APP_SETTINGS:  settingsApp();  break;
  }
}

// ============================================================
//  CLOCK SUITE MENU
// ============================================================
const char* clockSubNames[]={" World Clock"," Pomodoro"," Stopwatch"," Timer"," Alarm"};

void drawClockSuiteMenu() {
  display.clearDisplay();
  drawStatusBar();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(28,12); display.println("CLOCK SUITE");
  display.drawLine(0,20,128,20,SH110X_WHITE);
  int startItem=0;
  if (clockSubIndex>=4) startItem=clockSubIndex-3;
  for (int i=startItem;i<TOTAL_CLKSUB&&i<startItem+4;i++) {
    int y=22+(i-startItem)*10;
    if (i==clockSubIndex) { display.fillRect(0,y-1,120,10,SH110X_WHITE); display.setTextColor(SH110X_BLACK); }
    else { display.setTextColor(SH110X_WHITE); }
    display.setCursor(4,y);
    display.println(clockSubNames[i]);
  }
  display.setTextColor(SH110X_WHITE);
  present();
}

// ============================================================
//  CLOCK FACE APP
// ============================================================
void clockFaceApp() {
  int h=timeClient.getHours(), m=timeClient.getMinutes(), s=timeClient.getSeconds();
  display.clearDisplay();
  drawStatusBar();
  switch(clockFace) {
    case 0: drawCasioFace(h,m,s);   break;
    case 1: drawAnalogFace(h,m,s);  break;
    case 2: drawFlipFace(h,m,s);    break;
    case 3: drawMinimalFace(h,m,s); break;
  }
  present();
}

void drawCasioFace(int h,int m,int s) {
  display.drawRect(0,10,128,54,SH110X_WHITE);
  bool isPM=(h>=12);
  display.setTextSize(1);
  display.setCursor(4,13); display.print(isPM?"PM":"AM");
  const char* days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
  display.setCursor(96,13); display.print(days[timeClient.getDay()]);
  int h12=h%12; if(h12==0)h12=12;
  char tb[6]; snprintf(tb,sizeof(tb),"%2d:%02d",h12,m);
  display.setTextSize(3);
  display.setCursor(8,22); display.print(tb);
  static bool colonOn=true; static unsigned long lc=0;
  if (millis()-lc>500){colonOn=!colonOn;lc=millis();}
  if (!colonOn) display.fillRect(44,22,6,24,SH110X_BLACK);
  display.setTextSize(2);
  display.setCursor(98,30); display.printf("%02d",s);
  display.drawLine(2,47,125,47,SH110X_WHITE);
  if (alarmEnabled) {
    display.setTextSize(1); display.setCursor(4,50); display.print("AL");
    char ab[6]; snprintf(ab,sizeof(ab),"%02d:%02d",alarmHour,alarmMinute);
    display.setCursor(18,50); display.print(ab);
  }
  static bool dotOn=true; static unsigned long ld=0;
  if (millis()-ld>1000){dotOn=!dotOn;ld=millis();}
  if (dotOn) display.fillRect(120,50,4,4,SH110X_WHITE);
}

void drawAnalogFace(int h,int m,int s) {
  int cx=50,cy=36,r=24;
  display.drawCircle(cx,cy,r,SH110X_WHITE);
  for (int t=0;t<4;t++) {
    float a=t*PI/2-PI/2;
    display.drawLine(cx+(int)((r-3)*cos(a)),cy+(int)((r-3)*sin(a)),
                      cx+(int)((r-7)*cos(a)),cy+(int)((r-7)*sin(a)),SH110X_WHITE);
  }
  display.fillCircle(cx,cy,2,SH110X_WHITE);
  float hAngle=((h%12)*30+m*0.5)*PI/180-PI/2;
  display.drawLine(cx,cy,cx+(int)((r-10)*cos(hAngle)),cy+(int)((r-10)*sin(hAngle)),SH110X_WHITE);
  float mAngle=(m*6)*PI/180-PI/2;
  display.drawLine(cx,cy,cx+(int)((r-5)*cos(mAngle)),cy+(int)((r-5)*sin(mAngle)),SH110X_WHITE);
  float sAngle=(s*6)*PI/180-PI/2;
  for (int step=0;step<r-3;step+=3)
    display.drawPixel(cx+(int)(step*cos(sAngle)),cy+(int)(step*sin(sAngle)),SH110X_WHITE);

  display.setTextSize(1);
  display.setCursor(84,13); display.print(formatTime(h,m,-1));
  const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  display.setCursor(84,26); display.print(days[timeClient.getDay()]);
  display.setCursor(84,38); display.printf(":%02d",s);
  if (alarmEnabled) { display.setCursor(84,50); display.print("AL "); display.printf("%02d:%02d",alarmHour,alarmMinute); }
}

void drawFlipFace(int h,int m,int s) {
  int h12=h%12; if(h12==0)h12=12;
  int panels[4][2]={{4,13},{36,13},{72,13},{104,13}};
  int digits[4]={h12/10,h12%10,m/10,m%10};
  for (int p=0;p<4;p++) {
    int px=panels[p][0],py=panels[p][1],pw=26,ph=36;
    display.drawRoundRect(px,py,pw,ph,3,SH110X_WHITE);
    display.drawLine(px+1,py+ph/2,px+pw-2,py+ph/2,SH110X_WHITE);
    display.setTextSize(3);
    display.setCursor(px+(pw-18)/2,py+(ph-24)/2);
    display.print(digits[p]);
  }
  static bool colonOn=true; static unsigned long lc=0;
  if (millis()-lc>500){colonOn=!colonOn;lc=millis();}
  if (colonOn) { display.fillCircle(64,24,3,SH110X_WHITE); display.fillCircle(64,38,3,SH110X_WHITE); }
  display.setTextSize(1);
  bool isPM=(h>=12);
  display.setCursor(4,54); display.print(isPM?"PM":"AM");
  display.setCursor(50,54); display.printf(":%02d",s);
  const char* days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
  display.setCursor(90,54); display.print(days[timeClient.getDay()]);
}

void drawMinimalFace(int h,int m,int s) {
  display.setTextSize(3);
  char hb[3]; snprintf(hb,sizeof(hb),"%02d",h);
  display.setCursor(24,10); display.print(hb);
  display.drawLine(14,36,114,36,SH110X_WHITE);
  char mb[3]; snprintf(mb,sizeof(mb),"%02d",m);
  display.setCursor(24,40); display.print(mb);
  display.drawRect(0,60,128,3,SH110X_WHITE);
  display.fillRect(0,60,(s*128)/60,3,SH110X_WHITE);
  const char* days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
  display.setTextSize(1);
  display.setCursor(96,12); display.print(days[timeClient.getDay()]);
  display.setCursor(2,12); display.printf(":%02d",s);
}

// ============================================================
//  POMODORO — HOURGLASS REDESIGN
// ============================================================


void drawHourglass(int cx,int cy,int w,int h,float progress,bool running) {
  int halfH=h/2;
  int halfW=w/2;

  // outer frame
  display.drawTriangle(cx-halfW,cy-halfH, cx+halfW,cy-halfH, cx,cy, SH110X_WHITE);
  display.drawTriangle(cx-halfW,cy+halfH, cx+halfW,cy+halfH, cx,cy, SH110X_WHITE);
  // top/bottom caps
  display.drawLine(cx-halfW,cy-halfH,cx+halfW,cy-halfH,SH110X_WHITE);
  display.drawLine(cx-halfW,cy+halfH,cx+halfW,cy+halfH,SH110X_WHITE);

  // remaining sand in TOP chamber (shrinks toward neck as progress increases)
  float topFrac = 1.0 - progress;
  if (topFrac > 0.02) {
    int tw = (int)(halfW * topFrac);
    int ty = cy - (int)(halfH * topFrac);
    display.fillTriangle(cx-tw, ty, cx+tw, ty, cx, cy, SH110X_WHITE);
  }

  // accumulated sand in BOTTOM chamber (grows from neck outward)
  float botFrac = progress;
  if (botFrac > 0.02) {
    int bw = (int)(halfW * botFrac);
    int by = cy + (int)(halfH * botFrac);
    display.fillTriangle(cx-bw, by, cx+bw, by, cx, cy, SH110X_WHITE);
  }

  // falling sand animation at the neck
  if (running && progress < 0.98) {
    unsigned long t = millis() % 400;
    int dropY = cy - 6 + (int)(t / 33);
    display.drawPixel(cx, dropY, SH110X_WHITE);
    display.drawPixel(cx, dropY+3, SH110X_WHITE);
  }
}

void pomodoroApp() {                       // the countdown itself runs in pomodoroTick() from loop()
  if (pomodoroScreen==0) drawPomodoroHourglass();
  else                   drawPomodoroStats();
}

void drawPomodoroHourglass() {
  display.clearDisplay();
  drawStatusBar();

  int total=isWork?workTime:breakTime;
  float progress = 1.0 - (float)timerValue/total;

  const char* label = isPaused?"PAUSED":(isWork?"WORK":"BREAK");
  display.setTextSize(1);
  int lw=strlen(label)*6;
  display.setCursor((128-lw)/2, 13);
  display.print(label);

  drawHourglass(64, 34, 24, 30, progress, !isPaused);

  int mn=timerValue/60, sc=timerValue%60;
  display.setTextSize(1);
  char tb[6]; snprintf(tb,sizeof(tb),"%02d:%02d",mn,sc);
  int tw=strlen(tb)*6;
  display.setCursor((128-tw)/2, 54);
  display.print(tb);

  display.setCursor(0,56);
  display.print("DN:pause");
  display.setCursor(80,56);
  display.print("UP:stats");

  present();
}

void drawPomodoroStats() {
  display.clearDisplay();
  drawStatusBar();
  display.setTextSize(1);
  display.setCursor(22,12); display.println("POMODORO STATS");
  display.drawLine(0,21,128,21,SH110X_WHITE);
  display.setCursor(0,24); display.print("Today:");
  for (int i=0;i<min(todaySessions,10);i++) display.fillRect(42+i*7,24,5,5,SH110X_WHITE);
  for (int i=todaySessions;i<10;i++) display.drawRect(42+i*7,24,5,5,SH110X_WHITE);
  display.setCursor(0,34); display.print("Total:  "); display.print(totalSessions); display.println(" sessions");
  display.setCursor(0,44); display.print("Focus:  ");
  if (totalFocusMin>=60) { display.print(totalFocusMin/60); display.print("h "); display.print(totalFocusMin%60); display.println("m"); }
  else { display.print(totalFocusMin); display.println("m"); }
  display.setCursor(0,54); display.print("Streak: "); display.print(consecutiveCompleted);
  present();
}

// ============================================================
//  STOPWATCH
// ============================================================
void stopwatchApp() {
  unsigned long elapsed=swElapsed+(swRunning?millis()-swStart:0);
  int cs=(elapsed/10)%100, sc=(elapsed/1000)%60, mn=(elapsed/60000)%60;
  display.clearDisplay(); drawStatusBar();
  display.setTextSize(1); display.setCursor(36,12); display.println("STOPWATCH");
  display.drawLine(0,21,128,21,SH110X_WHITE);
  display.setTextSize(2); display.setCursor(10,24); display.printf("%02d:%02d.%02d",mn,sc,cs);
  display.setTextSize(1); display.setCursor(0,44);
  display.println(swRunning?"Running...":(swElapsed>0?"Paused":"Ready"));
  int lapShow=min(swLapCount,3);
  for (int i=0;i<lapShow;i++) {
    int li=(swLapCount-lapShow+i)%3;
    unsigned long lt=swLaps[li];
    display.setCursor(68,44+i*8);
    display.printf("L%d %02d:%02d.%02d",swLapCount-lapShow+i+1,(int)((lt/60000)%60),(int)((lt/1000)%60),(int)((lt/10)%100));
  }
  display.drawLine(0,56,128,56,SH110X_WHITE);
  display.setCursor(0,57); display.print("DN:strt UP:lap SEL:rst");
  present();
}

// ============================================================
//  COUNTDOWN TIMER
// ============================================================
// circular progress ring: starts at 12 o'clock, clockwise
void drawRing(int cx,int cy,int r,float frac,int thick) {
  frac = constrain(frac, 0.0f, 1.0f);
  display.drawCircle(cx, cy, r + 1, SH110X_WHITE);              // thin track
  int steps = (int)(frac * 180);
  for (int s = 0; s <= steps; s++) {
    float a = s * (2.0f * PI / 180.0f);
    float sx = sinf(a), sy = -cosf(a);
    for (int t = 0; t < thick; t++)
      display.drawPixel(cx + (int)lroundf(sx * (r - t)), cy + (int)lroundf(sy * (r - t)), SH110X_WHITE);
  }
}

void countdownApp() {
  if (ctRunning&&!ctFinished) {
    unsigned long now=millis();
    if (now-lastCtTick>=1000) {
      lastCtTick=now; ctTotal--;
      if (ctTotal<=0) { ctTotal=0; ctRunning=false; ctFinished=true; flashScreen(); showNotif("Timer finished!"); beepAlarm(); }
    }
  }
  display.clearDisplay(); drawStatusBar();
  if (ctSetting) {
    display.setTextSize(1); display.setCursor(40,12); display.println("TIMER");
    display.drawLine(0,21,128,21,SH110X_WHITE);
    display.setCursor(20,26); display.println("Set duration:");
    display.setTextSize(3); display.setCursor(28,36); display.printf("%02d:00",ctMinutes);
    display.setTextSize(1); display.setCursor(10,57); display.print("UP/DN:min  SEL:start");
  } else if (ctFinished) {
    display.setTextSize(2); display.setCursor(14,22); display.println("FINISHED!");
    display.setTextSize(1); display.setCursor(20,48); display.println("SEL to reset");
  } else {
    int total = max(1, ctMinutes*60);
    float remain = (float)ctTotal / total;
    drawRing(32, 38, 22, remain, 3);                            // ring shrinks as time runs out
    int pct = (int)(remain * 100);
    int digits = pct >= 100 ? 3 : (pct >= 10 ? 2 : 1);
    display.setTextSize(1);
    display.setCursor(32 - ((digits + 1) * 6) / 2, 34); display.print(pct); display.print("%");
    display.setTextSize(2); display.setCursor(62,24);
    display.printf("%02d:%02d", ctTotal/60, ctTotal%60);
    display.setTextSize(1);
    display.setCursor(62,46); display.print(ctRunning ? "UP:pause" : "UP:resume");
    display.setCursor(62,55); display.print("SEL:reset");
  }
  present();
}

// ============================================================
//  ALARM
// ============================================================


void alarmApp() {
  display.clearDisplay(); drawStatusBar();
  display.setTextSize(1); display.setCursor(46,12); display.println("ALARM");
  display.drawLine(0,21,128,21,SH110X_WHITE);
  if (alarmSetPos==0){display.fillRect(16,26,28,18,SH110X_WHITE);display.setTextColor(SH110X_BLACK);}
  display.setTextSize(2); display.setCursor(18,28); display.printf("%02d",alarmHour);
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(2); display.setCursor(46,28); display.print(":");
  if (alarmSetPos==1){display.fillRect(54,26,28,18,SH110X_WHITE);display.setTextColor(SH110X_BLACK);}
  display.setTextSize(2); display.setCursor(56,28); display.printf("%02d",alarmMinute);
  display.setTextColor(SH110X_WHITE);
  if (alarmSetPos==2){display.fillRect(86,26,38,18,SH110X_WHITE);display.setTextColor(SH110X_BLACK);}
  display.setTextSize(1); display.setCursor(88,32); display.println(alarmEnabled?" ON ":" OFF");
  display.setTextColor(SH110X_WHITE);
  display.setCursor(10,50); display.print("Alarm: "); display.println(alarmEnabled?"Armed":"Disabled");
  display.drawLine(0,56,128,56,SH110X_WHITE);
  display.setCursor(0,57); display.print("UP/DN:set  SEL:next");
  present();
}

// ============================================================
//  WEATHER
// ============================================================
void drawWeatherIconLarge(String desc,int x,int y) {
  if (desc=="Clear") {
    display.drawCircle(x,y,9,SH110X_WHITE);
    for (int ray=0;ray<8;ray++) {
      float a=ray*PI/4;
      display.drawLine(x+(int)(11*cos(a)),y+(int)(11*sin(a)),x+(int)(14*cos(a)),y+(int)(14*sin(a)),SH110X_WHITE);
    }
  } else if (desc=="Clouds") {
    display.fillCircle(x-6,y+2,7,SH110X_WHITE); display.fillCircle(x+4,y+2,6,SH110X_WHITE);
    display.fillCircle(x-1,y-3,6,SH110X_WHITE); display.fillRect(x-13,y+2,24,8,SH110X_WHITE);
  } else if (desc=="Rain"||desc=="Drizzle") {
    display.fillCircle(x-5,y-2,6,SH110X_WHITE); display.fillCircle(x+4,y-2,5,SH110X_WHITE);
    display.fillRect(x-11,y-2,20,7,SH110X_WHITE);
    for (int d=0;d<4;d++) display.drawLine(x-9+d*6,y+7,x-9+d*6-3,y+14,SH110X_WHITE);
  } else if (desc=="Thunderstorm") {
    display.fillCircle(x-5,y-3,6,SH110X_WHITE); display.fillCircle(x+4,y-3,5,SH110X_WHITE);
    display.fillRect(x-11,y-3,20,7,SH110X_WHITE);
    display.drawLine(x+2,y+5,x-4,y+12,SH110X_WHITE); display.drawLine(x-4,y+12,x+2,y+12,SH110X_WHITE);
  } else if (desc=="Snow") {
    display.drawLine(x-10,y,x+10,y,SH110X_WHITE); display.drawLine(x,y-10,x,y+10,SH110X_WHITE);
    display.drawLine(x-7,y-7,x+7,y+7,SH110X_WHITE); display.drawLine(x+7,y-7,x-7,y+7,SH110X_WHITE);
  } else {
    for (int l=0;l<4;l++) display.drawLine(x-10,y-6+l*4,x+10,y-6+l*4,SH110X_WHITE);
  }
}

void weatherApp() {
  display.clearDisplay(); drawStatusBar();
  if (!weatherValid) {
    display.setTextSize(1); display.setCursor(22,28);
    display.print(WiFi.status()==WL_CONNECTED ? "Updating weather..." : "Offline");
    display.setCursor(22,40); display.print(CITIES[cityIdx]);
    present();
    return;
  }
  drawWeatherIconLarge(weatherDesc,22,30);
  int t = dispTemp(tempC);
  int digits = (t < 0 ? 1 : 0) + (abs(t) >= 100 ? 3 : (abs(t) >= 10 ? 2 : 1));
  display.setTextSize(3); display.setCursor(46,16); display.print(t);
  display.setTextSize(2); display.setCursor(46 + digits*18 + 2,16); display.print(tempUnit());
  display.setTextSize(1); display.setCursor(52,40); display.print("feels "); display.print(dispTemp(feelsC)); display.println(tempUnit());
  display.setCursor(52,50); display.println(CITIES[cityIdx]);
  display.drawLine(0,56,128,56,SH110X_WHITE);
  display.fillTriangle(4,57, 2,60, 6,60, SH110X_WHITE);           // droplet
  display.fillCircle(4,60,2,SH110X_WHITE);
  display.setCursor(11,57); display.print(humidity); display.print("%");
  display.drawLine(40,59,46,59,SH110X_WHITE);                      // wind
  display.drawLine(40,62,44,62,SH110X_WHITE);
  display.setCursor(50,57); display.print(dispWind()); display.print(windUnit());
  display.setCursor(88,57);
  String sd=weatherDesc; if (sd.length()>6) sd=sd.substring(0,6);
  display.print(sd);
  present();
}

// ============================================================
//  DASHBOARD + SYSTEM GRAPHS
// ============================================================
void dashboardApp() {
  unsigned long now=millis();
  if (now-lastDashUpdate>3000||lastDashUpdate==0) {
    lastDashUpdate=now;
    rssi=WiFi.RSSI(); ipAddr=WiFi.localIP().toString();
    freeHeap=ESP.getFreeHeap(); uptimeHours=millis()/3600000.0;
    if (batteryEnabled) readBattery();
  }
  if      (dashScreen==0) drawDashStats();
  else if (dashScreen==1) drawSystemGraphs();
  else                    drawDeskEnv();
}

void drawDeskEnv() {
  display.clearDisplay(); drawStatusBar();
  display.setTextSize(1); display.setCursor(24,12); display.println("DESK & ROOM");
  display.drawLine(0,21,128,21,SH110X_WHITE);

  display.setCursor(0,24); display.print("Presence: ");
  display.print(!pirEnabled ? "sensor off" : (millis() < PIR_WARMUP_MS ? "warming up" : (presentNow ? "HERE" : "away")));

  display.setCursor(0,34); display.print("Desk today: ");
  display.print(deskMin/60); display.print("h "); display.print(deskMin%60); display.print("m");

  display.setCursor(0,44);
  if (batteryEnabled) { display.print("Noise: n/a (A0 busy)"); }
  else {
    int np = noisePercent();
    display.print("Noise:");
    display.drawRect(40,44,66,7,SH110X_WHITE); display.fillRect(41,45,(np*64)/100,5,SH110X_WHITE);
    display.setCursor(110,44); display.print(np);
  }
  display.setCursor(0,54); display.print("Clap snooze: "); display.print(clapEnabled?"ON":"OFF");
  present();
}

void drawDashStats() {
  display.clearDisplay(); drawStatusBar();
  display.setTextSize(1); display.setCursor(34,12); display.println("DASHBOARD");
  display.drawLine(0,21,128,21,SH110X_WHITE);
  display.setCursor(0,24); display.print("WiFi:");
  int bars=0; if(rssi>-50)bars=4;else if(rssi>-65)bars=3;else if(rssi>-75)bars=2;else if(rssi>-85)bars=1;
  for (int b=0;b<4;b++){int bx=36+b*7,bh=3+b*2;
    if(b<bars)display.fillRect(bx,30-bh,5,bh,SH110X_WHITE);else display.drawRect(bx,30-bh,5,bh,SH110X_WHITE);}
  display.setCursor(68,24); display.print(rssi); display.println("dBm");
  display.setCursor(0,34); display.print("IP: "); display.println(ipAddr);
  display.setCursor(0,44); display.print("RAM:");
  int hp=constrain((freeHeap*100)/81920,0,100);
  display.drawRect(28,44,80,7,SH110X_WHITE); display.fillRect(29,45,(hp*78)/100,5,SH110X_WHITE);
  display.setCursor(111,44); display.print(hp); display.print("%");
  display.setCursor(0,55); display.print("Up:");
  if(uptimeHours<1){display.print((int)(uptimeHours*60));display.print("m");}
  else{display.print(uptimeHours,1);display.print("h");}
  display.setCursor(70,55); display.print("UP:graphs");
  present();
}

void drawSystemGraphs() {
  display.clearDisplay(); drawStatusBar();
  display.setTextSize(1);
  display.setCursor(30,12); display.println("SYSTEM GRAPHS");
  display.drawLine(0,20,128,20,SH110X_WHITE);

  // RSSI graph
  display.setCursor(0,22); display.print("RSSI");
  int gx=0, gy=32, gw=126, gh=14;
  display.drawRect(gx,gy,gw,gh,SH110X_WHITE);
  for (int i=0;i<HIST_LEN-1;i++) {
    int idx1=(histIndex+i)%HIST_LEN, idx2=(histIndex+i+1)%HIST_LEN;
    int v1=constrain(map(rssiHistory[idx1],-100,-30,0,gh),0,gh);
    int v2=constrain(map(rssiHistory[idx2],-100,-30,0,gh),0,gh);
    int x1=gx+ (i*gw)/HIST_LEN, x2=gx+((i+1)*gw)/HIST_LEN;
    display.drawLine(x1,gy+gh-v1,x2,gy+gh-v2,SH110X_WHITE);
  }

  // RAM graph
  display.setCursor(0,48); display.print("RAM%");
  int gy2=50;
  display.drawRect(gx,gy2,gw,gh,SH110X_WHITE);
  for (int i=0;i<HIST_LEN-1;i++) {
    int idx1=(histIndex+i)%HIST_LEN, idx2=(histIndex+i+1)%HIST_LEN;
    int v1=constrain(map(ramHistory[idx1],0,100,0,gh),0,gh);
    int v2=constrain(map(ramHistory[idx2],0,100,0,gh),0,gh);
    int x1=gx+(i*gw)/HIST_LEN, x2=gx+((i+1)*gw)/HIST_LEN;
    display.drawLine(x1,gy2+gh-v1,x2,gy2+gh-v2,SH110X_WHITE);
  }
  present();
}

// ============================================================
//  FACE APP  — no status bar, no name, no hints, just the face
// ============================================================
void drawEmoEye(int cx,int cy,int px,int py,int eyelidTop,int eyelidBot,
                bool halfBlink,bool fullBlink,bool heartPupil=false) {
  int x0=cx-EYE_W/2, y0=cy-EYE_H/2;
  if (fullBlink) { display.fillRoundRect(x0,cy-1,EYE_W,3,1,SH110X_WHITE); return; }
  if (halfBlink) { display.fillRoundRect(x0,cy-EYE_H/4,EYE_W,EYE_H/2+2,EYE_R/2,SH110X_WHITE); return; }
  display.fillRoundRect(x0,y0,EYE_W,EYE_H,EYE_R,SH110X_WHITE);
  if (eyelidTop>0) display.fillRect(x0,y0,EYE_W,eyelidTop,SH110X_BLACK);
  if (eyelidBot>0) display.fillRect(x0,y0+EYE_H-eyelidBot,EYE_W,eyelidBot,SH110X_BLACK);
  int pupX=constrain(cx+px,x0+PUP_R+1,x0+EYE_W-PUP_R-1);
  int pupY=constrain(cy+py,y0+PUP_R+1,y0+EYE_H-PUP_R-1);
  if (heartPupil) {
    display.fillCircle(pupX-2,pupY-1,2,SH110X_BLACK); display.fillCircle(pupX+2,pupY-1,2,SH110X_BLACK);
    for (int i=0;i<4;i++) display.drawLine(pupX-3+i,pupY+1,pupX,pupY+4-i,SH110X_BLACK);
  } else {
    display.fillCircle(pupX,pupY,PUP_R,SH110X_BLACK);
    display.drawPixel(pupX+2,pupY-2,SH110X_WHITE);
  }
}

void drawBrow(int cx,int cy,int angle,bool thick=false) {
  int y=cy-EYE_H/2-5, x0=cx-10, x1=cx+10, dy=angle*3;
  display.drawLine(x0,y+dy,x1,y-dy,SH110X_WHITE);
  if (thick) display.drawLine(x0,y+dy+1,x1,y-dy+1,SH110X_WHITE);
}

void drawGlitchEye(int cx,int cy) {
  int x0=cx-EYE_W/2,y0=cy-EYE_H/2;
  display.fillRoundRect(x0,y0,EYE_W,EYE_H,EYE_R,SH110X_WHITE);
  uint32_t gs = (uint32_t)glitchSeed * 2654435761UL + 1;           // local PRNG: no more reseeding the global random()
  auto rnd = [&gs](int n) { gs = gs * 1664525UL + 1013904223UL; return (int)((gs >> 16) % n); };
  for (int i=0;i<4;i++) display.fillRect(x0+rnd(EYE_W/2),y0+rnd(EYE_H),4+rnd(EYE_W/2-4),1+rnd(3),SH110X_BLACK);
  display.fillRect(cx-3,cy-3,6,6,SH110X_BLACK);
}

// FIXED HAPPY EYES — upward "^ ^" arc shape, actually reads as happy
void drawHappyEyes(int cx,int cy) {
  for (int i=-11;i<=11;i++) {
    int dy = -( (11-abs(i)) / 2 );  // peak at center, curves down at edges = "^" shape
    display.drawPixel(cx+i, cy+dy,   SH110X_WHITE);
    display.drawPixel(cx+i, cy+dy-1, SH110X_WHITE); // thickness
    display.drawPixel(cx+i, cy+dy-2, SH110X_WHITE);
  }
}

void updatePupilDrift() {
  unsigned long now=millis();
  if (now-lastPupilMove>nextPupilMove) {
    lastPupilMove=now; nextPupilMove=random(1500,4000);
    int pos[9][2]={{0,0},{-4,0},{4,0},{0,-3},{0,3},{-4,-3},{4,-3},{-4,3},{4,3}};
    int p=random(9); pupilTgtX=pos[p][0]; pupilTgtY=pos[p][1];
  }
  if(pupilOffX<pupilTgtX)pupilOffX++;else if(pupilOffX>pupilTgtX)pupilOffX--;
  if(pupilOffY<pupilTgtY)pupilOffY++;else if(pupilOffY>pupilTgtY)pupilOffY--;
}

void updateBlink() {
  unsigned long now=millis();
  if (blinkFrame==0&&now-lastBlinkTime>nextBlinkDelay) { blinkFrame=1; lastBlinkTime=now; nextBlinkDelay=random(2500,6000); }
  static unsigned long lba=0;
  if (blinkFrame>0&&now-lba>60) { lba=now; blinkFrame++; if(blinkFrame>4)blinkFrame=0; }
}

void drawEmoFace(int emo) {
  bool isHalf=(blinkFrame==1||blinkFrame==3);
  bool isFull=(blinkFrame==2);

  // HAPPY gets its own special render — no eyelid trick, just arc eyes
  if (emo==EMO_HAPPY && !isFull) {
    drawHappyEyes(LEX, EYE_Y);
    drawHappyEyes(REX, EYE_Y);
    return;
  }
  if (emo==EMO_HAPPY && isFull) {
    display.fillRoundRect(LEX-EYE_W/2,EYE_Y-1,EYE_W,3,1,SH110X_WHITE);
    display.fillRoundRect(REX-EYE_W/2,EYE_Y-1,EYE_W,3,1,SH110X_WHITE);
    return;
  }

  int lEyelidTop=0,lEyelidBot=0,rEyelidTop=0,rEyelidBot=0;
  int lBrowAngle=0,rBrowAngle=0;
  bool browThick=false,drawBrows=false,heartPupil=false;
  int extraPX=0,extraPY=0;

  switch(emo) {
    case EMO_SAD:      lEyelidTop=4;rEyelidTop=4;extraPY=3;drawBrows=true;lBrowAngle=1;rBrowAngle=-1; break;
    case EMO_ANGRY:    lEyelidTop=6;rEyelidTop=6;extraPY=1;drawBrows=true;lBrowAngle=-1;rBrowAngle=1;browThick=true; break;
    case EMO_SURPRISED:drawBrows=true; break;
    case EMO_SLEEPY:   lEyelidTop=9;rEyelidTop=9;extraPY=2; break;
    case EMO_LOVE:     heartPupil=true;lEyelidBot=4;rEyelidBot=4;extraPY=-1;drawBrows=true; break;
    case EMO_THINKING: lEyelidTop=2;extraPX=-3;extraPY=-2;drawBrows=true;rBrowAngle=1; break;
    default: break;
  }

  if (emo==EMO_GLITCH) {
    if (millis()-lastGlitchFrame>120){glitchSeed=random(1000);lastGlitchFrame=millis();}
    drawEmoEye(LEX,EYE_Y,pupilOffX+extraPX,pupilOffY+extraPY,0,0,isHalf,isFull);
    drawGlitchEye(REX,EYE_Y);
    int scanY=(millis()/30)%64;
    for(int sx=0;sx<128;sx+=2)display.drawPixel(sx,scanY,SH110X_WHITE);
    return;
  }

  if (drawBrows&&!isHalf&&!isFull) { drawBrow(LEX,EYE_Y,lBrowAngle,browThick); drawBrow(REX,EYE_Y,rBrowAngle,browThick); }
  drawEmoEye(LEX,EYE_Y, pupilOffX+extraPX, pupilOffY+extraPY,lEyelidTop,lEyelidBot,isHalf,isFull,heartPupil);
  drawEmoEye(REX,EYE_Y,-(pupilOffX+extraPX),pupilOffY+extraPY,rEyelidTop,rEyelidBot,isHalf,isFull,heartPupil);

  switch(emo) {
    case EMO_SAD:
      if (!isFull) { display.drawLine(LEX+6,EYE_Y+EYE_H/2+1,LEX+5,EYE_Y+EYE_H/2+7,SH110X_WHITE); display.fillCircle(LEX+5,EYE_Y+EYE_H/2+8,2,SH110X_WHITE); }
      break;
    case EMO_SLEEPY: {
      unsigned long zt=millis()/600;
      display.setTextSize(1);
      display.setCursor(zzzX-(zt%3)*4,zzzY-(zt%4)*2); display.print("z");
      display.setCursor(zzzX+6-(zt%3)*2,zzzY-6-(zt%4)); display.print("Z");
      break;
    }
    case EMO_LOVE: {
      unsigned long ht=millis()/400;
      int hpos[3]={24,64,104};
      for(int h=0;h<3;h++){int hy=56-(ht+h*2)%50,hx=hpos[h];
        display.fillCircle(hx-1,hy,2,SH110X_WHITE); display.fillCircle(hx+2,hy,2,SH110X_WHITE);
        display.fillTriangle(hx-3,hy+1,hx+4,hy+1,hx,hy+5,SH110X_WHITE);}
      break;
    }
    case EMO_THINKING: {
      unsigned long tt=millis()/300; int dotX[3]={102,110,118};
      for(int d=0;d<3;d++){ if((tt+d)%3==0)display.fillCircle(dotX[d],10,3,SH110X_WHITE); else display.drawCircle(dotX[d],10,3,SH110X_WHITE); }
      break;
    }
  }
}

void faceApp() {
  static uint32_t lastFrame = 0;
  if (!frameReady(lastFrame, 30)) return;
  updateBlink();
  updatePupilDrift();
  display.clearDisplay();
  drawEmoFace(emoIndex);
  present();
}

// ============================================================
//  FLASHCARDS
// ============================================================
void flashcardApp() {
  display.clearDisplay(); drawStatusBar();
  int base=cardDayIndex*DAILY_CARDS;
  int cardIdx=(base+cardViewIndex)%TOTAL_CARDS;
  display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(22,12); display.println("IELTS FLASHCARDS");
  display.drawLine(0,21,128,21,SH110X_WHITE);
  display.setCursor(96,12); display.print(cardViewIndex+1); display.print("/"); display.print(DAILY_CARDS);
  if (!cardFlipped) {
    display.setCursor(4,26); display.println("WORD:");
    display.setTextSize(2);
    String w=String(cards[cardIdx].word);
    int wx=max(0,(128-(int)w.length()*12)/2);
    display.setCursor(wx,36); display.println(w);
    display.setTextSize(1); display.setCursor(20,56); display.println("SEL: flip card");
  } else {
    display.setCursor(4,26); display.println("MEANING:");
    String def=String(cards[cardIdx].definition);
    int lineY=36,lineWidth=21;
    while(def.length()>0&&lineY<56){
      String line;
      if((int)def.length()<=lineWidth){line=def;def="";}
      else{int brk=lineWidth; while(brk>0&&def[brk]!=' ')brk--; if(brk==0)brk=lineWidth;
        line=def.substring(0,brk); def=def.substring(brk+1);}
      display.setCursor(4,lineY); display.println(line); lineY+=10;
    }
    display.setCursor(20,56); display.println("SEL: flip back");
  }
  present();
}

// ============================================================
//  HABIT TRACKER
// ============================================================
void habitApp() {
  display.clearDisplay(); drawStatusBar();
  display.setTextSize(1);
  display.setCursor(28,12); display.println("HABIT TRACKER");
  int done = 0;
  for (int i=0;i<TOTAL_HABITS;i++) if (habitDoneToday[i]) done++;
  display.setCursor(110,12); display.print(done); display.print("/"); display.print(TOTAL_HABITS);
  display.drawLine(0,20,128,20,SH110X_WHITE);
  display.fillRect(0,19,(done*128)/TOTAL_HABITS,2,SH110X_WHITE);   // today's progress bar

  for (int i=0;i<TOTAL_HABITS;i++) {
    int y = 22 + i*8;
    uint16_t fg = (i==habitIndex) ? SH110X_BLACK : SH110X_WHITE;
    if (i==habitIndex) display.fillRect(0,y-1,128,8,SH110X_WHITE);
    display.setTextColor(fg);

    display.drawRect(2,y,7,7,fg);                                  // checkbox with a real check mark
    if (habitDoneToday[i]) {
      display.drawLine(3,y+3,4,y+5,fg); display.drawLine(4,y+5,7,y+1,fg);
    }
    display.setCursor(13,y); display.print(habitNames[i]);

    if (habitStreak[i] > 0) {                                      // flame + streak
      drawFlame(84,y,fg);
      display.setCursor(94,y); display.print(habitStreak[i]); display.print("d");
    }
  }
  display.setTextColor(SH110X_WHITE);
  present();
}

// ============================================================
//  DINO GAME
// ============================================================
void dinoGameApp() {
  bool selNow = digitalRead(BTN_SELECT)==LOW;
  if (digitalRead(BTN_UP)==LOW && digitalRead(BTN_DOWN)==LOW) { lastSelState = selNow; return; }  // back combo

  bool pressed = selNow && !lastSelState;          // edge, read every loop so no tap is lost
  lastSelState = selNow;
  static uint32_t lastFrame = 0;

  if (dinoGameOver) {
    if (pressed) { resetDinoGame(); updateActivity(); return; }
    if (!frameReady(lastFrame, 100)) return;
    display.clearDisplay();
    display.setTextSize(2); display.setCursor(10,20); display.println("GAME OVER");
    display.setTextSize(1); display.setCursor(30,40); display.print("Score: "); display.print(dinoScore);
    display.setCursor(20,54); display.print("SEL to restart");
    present();
    return;
  }

  if (pressed && !dinoJumping) { dinoJumping = true; dinoVelY = -3.2; beepClick(); }
  if (!frameReady(lastFrame, 30)) return;          // fixed 30 ms physics step, no delay()

  if (dinoJumping) {
    dinoY += dinoVelY;
    dinoVelY += 0.25;
    if (dinoY >= 0) { dinoY = 0; dinoJumping = false; dinoVelY = 0; }
  }

  if (millis() - lastObstacleSpawn > nextSpawnGap) {
    for (int i=0;i<MAX_OBSTACLES;i++)
      if (!obstacles[i].active) { obstacles[i].active=true; obstacles[i].x=128; break; }
    lastObstacleSpawn = millis();
    nextSpawnGap = random(1200, 2200);
  }

  // hitbox uses the SAME pixel coordinates as the drawing
  int dinoTop = GROUND_Y - DINO_SIZE + (int)(dinoY*2);
  for (int i=0;i<MAX_OBSTACLES;i++) {
    if (!obstacles[i].active) continue;
    obstacles[i].x -= dinoSpeed;
    if (obstacles[i].x < -OBS_W) { obstacles[i].active=false; dinoScore++; continue; }
    int ox = (int)obstacles[i].x;
    bool overlapX = ox < DINO_X + DINO_SIZE - 1 && ox + OBS_W > DINO_X + 1;
    bool overlapY = dinoTop + DINO_SIZE > GROUND_Y - OBS_H + 1;
    if (overlapX && overlapY) { dinoGameOver = true; beepAlarm(); }
  }

  dinoSpeed = min(7.0f, 3.0f + dinoScore * 0.05f);

  display.clearDisplay();
  display.drawLine(0, GROUND_Y, 128, GROUND_Y, SH110X_WHITE);
  display.fillRect(DINO_X, dinoTop, DINO_SIZE, DINO_SIZE, SH110X_WHITE);
  for (int i=0;i<MAX_OBSTACLES;i++)
    if (obstacles[i].active) display.fillRect((int)obstacles[i].x, GROUND_Y-OBS_H, OBS_W, OBS_H, SH110X_WHITE);
  display.setTextSize(1); display.setCursor(90,2);
  display.print("Sc:"); display.print(dinoScore);
  present();
}

// ============================================================
//  SETTINGS
// ============================================================
const char* settingsCatNames[]={"Display","Time","Pomodoro","Weather","System"};
const char* displayItems[]={"Brightness","Theme","Sleep timeout","Sound"};
const char* timeItems[]={"Time format","Clock face"};
const char* pomodoroItems[]={"Work min","Break min","Adaptive mode"};
const char* weatherItems[]={"Units","City"};
const char* systemItems[]={"Reset Pomo stats","Presence PIR","Clap snooze","About","WiFi setup"};

// graphic controls for slider / on-off items; returns nothing, draws at the right edge of the row
void drawSettingGraphic(int cat,int item,int y,uint16_t fg,bool editing) {
  uint16_t bg = (fg == SH110X_WHITE) ? SH110X_BLACK : SH110X_WHITE;
  if (cat == SET_DISPLAY && item == 0) {                          // brightness slider
    int x = 74, w = 48;
    display.drawRect(x,y,w,7,fg);
    display.fillRect(x+1,y+1,((w-2)*brightness)/255,5,fg);
    if (editing) display.drawRect(x-2,y-1,w+4,9,fg);
    return;
  }
  bool on = false;
  if (cat == SET_DISPLAY  && item == 3) on = buzzerEnabled;
  if (cat == SET_POMODORO && item == 2) on = adaptiveMode;
  if (cat == SET_SYSTEM   && item == 1) on = pirEnabled;
  if (cat == SET_SYSTEM   && item == 2) on = clapEnabled;
  int x = 100;                                                    // toggle switch
  display.drawRoundRect(x,y,24,8,4,fg);
  if (on) { display.fillRoundRect(x,y,24,8,4,fg); display.fillCircle(x+19,y+4,2,bg); }
  else    { display.fillCircle(x+4,y+4,2,fg); }
  if (editing) display.drawRect(x-2,y-1,28,9,fg);
}

void settingsApp() {
  display.clearDisplay(); drawStatusBar();
  if (!inSettingsCat) {
    display.setTextSize(1); display.setCursor(34,12); display.println("SETTINGS");
    display.drawLine(0,21,128,21,SH110X_WHITE);
    int startItem=0; if(settingsCatIndex>=4) startItem=settingsCatIndex-3;
    for(int i=startItem;i<TOTAL_SETCAT&&i<startItem+4;i++){
      int y=23+(i-startItem)*10;
      if(i==settingsCatIndex){display.fillRect(0,y-1,120,10,SH110X_WHITE);display.setTextColor(SH110X_BLACK);}
      else{display.setTextColor(SH110X_WHITE);}
      display.setCursor(4,y); display.print(settingsCatNames[i]); display.print("  >");
    }
    display.setTextColor(SH110X_WHITE);
    display.drawLine(0,56,128,56,SH110X_WHITE);
    display.setCursor(10,57); display.print("SEL:enter  UP+DN:back");
  } else {
    const char** items; int count=categoryItemCounts[settingsCatIndex];
    switch(settingsCatIndex){
      case SET_DISPLAY: items=displayItems; break;
      case SET_TIME: items=timeItems; break;
      case SET_POMODORO: items=pomodoroItems; break;
      case SET_WEATHER: items=weatherItems; break;
      default: items=systemItems; break;
    }
    settingsItemIndex=constrain(settingsItemIndex,0,count-1);
    display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0,12); display.print(settingsCatNames[settingsCatIndex]);
    display.drawLine(0,21,128,21,SH110X_WHITE);

    int first = max(0, settingsItemIndex - 2);                 // 3 visible rows, scrolls
    for(int i=first;i<count&&i<first+3;i++){
      int y=23+(i-first)*10;
      if(i==settingsItemIndex){display.fillRect(0,y-1,128,10,SH110X_WHITE);display.setTextColor(SH110X_BLACK);}
      else{display.setTextColor(SH110X_WHITE);}
      uint16_t fg = (i==settingsItemIndex) ? SH110X_BLACK : SH110X_WHITE;
      bool gfx = (settingsCatIndex==SET_DISPLAY && (i==0 || i==3)) || (settingsCatIndex==SET_POMODORO && i==2)
              || (settingsCatIndex==SET_SYSTEM && (i==1 || i==2));
      display.setCursor(4,y); display.print(items[i]); if (!gfx) display.print(": ");
      if (gfx) drawSettingGraphic(settingsCatIndex, i, y, fg, i==settingsItemIndex && editingSettingsItem);
      else switch(settingsCatIndex){
        case SET_DISPLAY:
          if(i==0)display.print(brightness);
          if(i==1)display.print(theme==0?"Normal":"Invert");
          if(i==2){if(sleepMinutes==0)display.print("Never");else{display.print(sleepMinutes);display.print("m");}}
          if(i==3)display.print(buzzerEnabled?"ON":"OFF");
          break;
        case SET_TIME: {
          const char* faceNames[]={"Casio","Analog","Flip","Minimal"};
          if(i==0)display.print(timeFormat==0?"24h":"12h");
          if(i==1)display.print(faceNames[clockFace]);
          break; }
        case SET_POMODORO:
          if(i==0){display.print(workMinutes);display.print("m");}
          if(i==1){display.print(breakMinutes);display.print("m");}
          if(i==2)display.print(adaptiveMode?"ON":"OFF");
          break;
        case SET_WEATHER:
          if(i==0)display.print(units?"F, mph":"C, km/h");
          if(i==1)display.print(CITIES[cityIdx]);
          break;
        case SET_SYSTEM:
          if(i==0)display.print("press SEL");
          if(i==1)display.print(pirEnabled?"ON":"OFF");
          if(i==2)display.print(clapEnabled?"ON":"OFF");
          if(i==3)display.print("v4.0");
          if(i==4)display.print("press SEL");
          break;
      }
      if(!gfx && i==settingsItemIndex&&editingSettingsItem) display.print(" <>");
    }
    display.setTextColor(SH110X_WHITE);
    display.drawLine(0,56,128,56,SH110X_WHITE);
    display.setCursor(0,57);
    display.print(editingSettingsItem?"UP/DN:chg SEL:save":"SEL:edit UP+DN:back");
  }
  present();
}
