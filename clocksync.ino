//
// clocksync.ino - ESP32 fake radio clock station
//
// Forked from 'nisejjy' by SASAKI Taroh (tarohs).
// Maintained by Tanvach. Wi-Fi setup hotspot and runtime time zone ideas
// from the Rmni2/clocksync-esp32-c3 fork.
//
// Compile and upload: no source changes are needed. On first boot the device
// opens a "clocksync-setup" Wi-Fi hotspot; join it and pick your network,
// station and time zone in the page that opens.
//
// See README.md for usage and HOW_IT_WORKS.md for the signal details.
//

#define VERSION "2.0.0"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_sntp.h"

#include "board.h"
#include "timecode.h"
#include "tzdata.h"
#include "webui.h"

// Optional build-time defaults (Wi-Fi, station, ...): copy secrets_example.h
// to secrets.h. Everything can also be set from the web UI instead.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef DEFAULT_STATION
#define DEFAULT_STATION TC_WWVB
#endif
#ifndef DEFAULT_HOSTNAME
#define DEFAULT_HOSTNAME "clocksync"
#endif
#ifndef DEFAULT_NTP_SERVER
#define DEFAULT_NTP_SERVER "pool.ntp.org"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD ""  // the setup hotspot is open; use 8+ characters to protect it
#endif

#if ESP_ARDUINO_VERSION_MAJOR < 3
#define rgbLedWrite neopixelWrite
#endif

// Any time before this is "clock not set" (the ESP32 boots in 1970).
static const time_t VALID_TIME = 1704067200;  // 2024-01-01
static const IPAddress AP_IP(192, 168, 4, 1);
static const uint32_t AP_AFTER_BOOT_MS = 20000;      // no Wi-Fi this long after boot: open hotspot
static const uint32_t AP_AFTER_DROPOUT_MS = 300000;  // lost Wi-Fi this long: open hotspot
static const uint32_t STA_RETRY_MS = 10000;          // reconnect attempts
static const uint32_t STA_RETRY_AP_MS = 30000;       // ... while the setup hotspot is up
static const uint32_t STA_RETRY_BUSY_MS = 180000;    // ... while someone is connected to it
static const size_t HOST_MAX = 26;                   // "<name>-setup" must fit a 32-byte SSID
static const int SAFE_PIN_LIST[] = SAFE_PINS;

// ------------------------------------------------------------------ settings

struct Settings {
  int station;
  bool tx;
  int pin;
  int drive;  // GPIO drive strength 0..3
  bool led;
  bool ntp;
  int dstMode;  // TC_DST_*
  char ssid[33];
  char pass[64];
  char tzName[48];  // "" = station's own zone, "custom" = tzPosix, else IANA name
  char tzPosix[64];
  char ntpServer[64];
  char host[32];
};

static Settings cfg;
static Preferences prefs;

// ------------------------------------------------------------------- globals

static WebServer server(80);
static DNSServer dns;

// radio
static const ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t LEDC_TIMER = LEDC_TIMER_0;
static const ledc_channel_t LEDC_CHANNEL = LEDC_CHANNEL_0;
// 5-bit resolution keeps the LEDC divider fine enough for a carrier within
// ~30 ppm of nominal (8 bits gives up to ~370 ppm error).
static const int LEDC_BITS = 5;
static const uint32_t DUTY_HIGH = 16;  // 50 %: strongest fundamental
static const uint32_t DUTY_LOW = 2;    // 6.25 %: fundamental at ~20 % (-14 dB)
static SemaphoreHandle_t radioMutex;
static esp_timer_handle_t tickTimer;
static volatile bool radioOn = false;
static int radioStation = -1;
static int radioPin = -1;
static volatile bool forceCarrier = false;  // self-test
static volatile int carrierHigh = 0;
static uint8_t frame[60];
static time_t frameMinute = -1;
static volatile bool frameDirty = true;
static volatile uint32_t frameCount = 0;

// network
static bool apActive = false;
static bool apManual = false;
static uint32_t apStartedMs = 0;
static bool wasConnected = false;
static bool everConnected = false;
static uint32_t connectedSinceMs = 0;
static uint32_t lastConnectedMs = 0;
static uint32_t lastStaAttemptMs = 0;
static uint32_t retryNotBeforeMs = 0;  // a Wi-Fi scan holds off reconnects
static uint32_t apFailedMs = 0;
static volatile int lastDisconnectReason = 0;
static bool mdnsStarted = false;
static char hostActive[32];  // name in use since boot; a new one applies after restart
static bool ntpRunning = false;
// lwIP keeps a pointer to the server name and esp_sntp_stop() finishes
// asynchronously, so alternate buffers instead of overwriting the live one.
static char ntpServerBufs[2][64];
static int ntpServerBuf = 0;
static volatile time_t lastNtpSync = 0;

// misc
static bool rebootPending = false;
static uint32_t rebootAtMs = 0;

// ------------------------------------------------------------------- helpers

static bool timeValid() {
  return time(NULL) >= VALID_TIME;
}

static bool pinAllowed(int p) {
  for (int s : SAFE_PIN_LIST) {
    if (s == p) return true;
  }
  return false;
}

static bool connected() {
  return WiFi.status() == WL_CONNECTED;
}

static const char *effectiveTzName() {
  if (cfg.tzName[0] == 0) return tc_stations[cfg.station].native_tz;
  return cfg.tzName;
}

static const char *effectivePosix() {
  if (cfg.tzName[0] == 0) return tc_stations[cfg.station].native_posix;
  return cfg.tzPosix;
}

// Find the POSIX rule for an IANA zone name in the built-in table.
static bool tzLookup(const char *name, char *out, size_t outLen) {
  size_t len = strlen(name);
  const char *p = TZDATA;
  while (*p) {
    const char *tab = strchr(p, '\t');
    const char *nl = strchr(tab, '\n');
    if ((size_t)(tab - p) == len && strncmp(p, name, len) == 0) {
      size_t n = nl - tab - 1;
      if (n >= outLen) return false;
      memcpy(out, tab + 1, n);
      out[n] = 0;
      return true;
    }
    p = nl + 1;
  }
  return false;
}

// A POSIX TZ rule starts with a zone name (3+ letters or <...>) and an offset,
// e.g. "JST-9" or "<+0330>-3:30". Anything else newlib silently treats as UTC.
static bool validPosix(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n >= sizeof(cfg.tzPosix)) return false;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < 0x21 || s[i] > 0x7e) return false;
  }
  size_t i = 0;
  if (s[0] == '<') {
    const char *close = strchr(s, '>');
    if (!close) return false;
    i = close - s + 1;
  } else {
    while (isalpha((unsigned char)s[i])) i++;
    if (i < 3) return false;
  }
  if (s[i] == '+' || s[i] == '-') i++;
  return isdigit((unsigned char)s[i]);
}

static bool validHostname(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n > HOST_MAX || s[0] == '-' || s[n - 1] == '-') return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

static bool validServer(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n >= sizeof(cfg.ntpServer)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':')) return false;
  }
  return true;
}

static void copyStr(char *dst, size_t len, const char *src) {
  strncpy(dst, src, len - 1);
  dst[len - 1] = 0;
}

static String fmtLocal(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &lt);
  return String(buf);
}

// Seconds east of UTC at time t (newlib has no timegm or tm_gmtoff).
static long utcOffsetAt(time_t t) {
  struct tm lt, ut;
  localtime_r(&t, &lt);
  gmtime_r(&t, &ut);
  long days = lt.tm_yday - ut.tm_yday;
  if (lt.tm_year != ut.tm_year) days = lt.tm_year > ut.tm_year ? 1 : -1;
  return days * 86400 + (lt.tm_hour - ut.tm_hour) * 3600L + (lt.tm_min - ut.tm_min) * 60L + (lt.tm_sec - ut.tm_sec);
}

static String ageText(time_t then) {
  if (then == 0) return String("never");
  long s = (long)(time(NULL) - then);
  if (s < 0) s = 0;
  if (s < 120) return String(s) + " s ago";
  if (s < 7200) return String(s / 60) + " min ago";
  return String(s / 3600) + " h ago";
}

// --------------------------------------------------------------- persistence

static void saveInt(const char *key, int v) {
  if (!prefs.isKey(key) || prefs.getInt(key, 0) != v) prefs.putInt(key, v);
}

static void saveStr(const char *key, const char *v) {
  if (!prefs.isKey(key) || prefs.getString(key, "") != v) prefs.putString(key, v);
}

static void loadStr(const char *key, char *dst, size_t len, const char *def) {
  // getString() logs an error for missing keys, so check first.
  String s = prefs.isKey(key) ? prefs.getString(key, def) : String(def);
  copyStr(dst, len, s.c_str());
}

static void defaultSettings(Settings &s) {
  s.station = DEFAULT_STATION;
  s.tx = true;
  s.pin = RADIO_PIN;
  s.drive = GPIO_DRIVE_CAP_2;
  s.led = true;
  s.ntp = true;
  s.dstMode = TC_DST_AUTO;
  s.tzName[0] = 0;
  s.tzPosix[0] = 0;
  copyStr(s.ntpServer, sizeof(s.ntpServer), DEFAULT_NTP_SERVER);
  copyStr(s.host, sizeof(s.host), DEFAULT_HOSTNAME);
}

// Small string hash so secrets.h credentials are only applied when they change.
static String hashStr(const char *s) {
  uint32_t h = 5381;
  while (*s) h = h * 33 + (uint8_t)*s++;
  return String(h, HEX);
}

static void loadSettings() {
  prefs.begin("clocksync", false);
  Settings d;
  defaultSettings(d);
  cfg = d;
  int v = prefs.getInt("station", d.station);
  cfg.station = (v >= 0 && v < TC_STATION_COUNT) ? v : d.station;
  cfg.tx = prefs.getInt("tx", 1) != 0;
  v = prefs.getInt("pin", d.pin);
  cfg.pin = pinAllowed(v) ? v : d.pin;
  v = prefs.getInt("drive", d.drive);
  cfg.drive = (v >= GPIO_DRIVE_CAP_0 && v <= GPIO_DRIVE_CAP_3) ? v : d.drive;
  cfg.led = prefs.getInt("led", 1) != 0;
  // v1.x could save "ntp=0" after a single failed boot; v2 uses a new key.
  cfg.ntp = prefs.getInt("ntp2", 1) != 0;
  v = prefs.getInt("dst", d.dstMode);
  cfg.dstMode = (v >= 0 && v <= 2) ? v : d.dstMode;
  loadStr("ssid", cfg.ssid, sizeof(cfg.ssid), "");
  loadStr("pass", cfg.pass, sizeof(cfg.pass), "");
  loadStr("tzname", cfg.tzName, sizeof(cfg.tzName), "");
  loadStr("tz", cfg.tzPosix, sizeof(cfg.tzPosix), "");
  loadStr("ntpsrv", cfg.ntpServer, sizeof(cfg.ntpServer), d.ntpServer);
  loadStr("host", cfg.host, sizeof(cfg.host), d.host);
  if (!validServer(cfg.ntpServer)) copyStr(cfg.ntpServer, sizeof(cfg.ntpServer), d.ntpServer);
  if (!validHostname(cfg.host)) copyStr(cfg.host, sizeof(cfg.host), d.host);

  // Zone names are stored; refresh their rules from this firmware's tzdata.
  if (cfg.tzName[0] && strcmp(cfg.tzName, "custom") != 0) {
    char posix[64];
    if (tzLookup(cfg.tzName, posix, sizeof(posix))) {
      copyStr(cfg.tzPosix, sizeof(cfg.tzPosix), posix);
    } else if (!validPosix(cfg.tzPosix)) {
      cfg.tzName[0] = 0;
    }
  }
  if (strcmp(cfg.tzName, "custom") == 0 && !validPosix(cfg.tzPosix)) cfg.tzName[0] = 0;

  // Credentials from secrets.h win once each time they are changed and reflashed.
  const char *ssid = WIFI_SSID;
  if (ssid[0] && strcmp(ssid, "YourSSID") != 0) {
    String sig = hashStr((String(WIFI_SSID) + "\n" + WIFI_PASS).c_str());
    if (!prefs.isKey("csig") || prefs.getString("csig", "") != sig) {
      copyStr(cfg.ssid, sizeof(cfg.ssid), WIFI_SSID);
      copyStr(cfg.pass, sizeof(cfg.pass), WIFI_PASS);
      saveStr("ssid", cfg.ssid);
      saveStr("pass", cfg.pass);
      prefs.putString("csig", sig);
    }
  }
}

static void saveRadioSettings() {
  saveInt("station", cfg.station);
  saveInt("tx", cfg.tx ? 1 : 0);
  saveInt("pin", cfg.pin);
  saveInt("drive", cfg.drive);
  saveInt("dst", cfg.dstMode);
}

// --------------------------------------------------------------------- radio

// The tick task calls localtime_r(); never let it see a half-written TZ.
static void applyTimeZone() {
  xSemaphoreTake(radioMutex, portMAX_DELAY);
  setenv("TZ", effectivePosix(), 1);
  tzset();
  frameDirty = true;
  xSemaphoreGive(radioMutex);
}

static void pinHighZ(int pin) {
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
}

static void scheduleNextTick(int64_t slot) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  int64_t nowUs = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
  int64_t delay = (slot + 1) * 100000 - nowUs;
  if (delay < 100) delay = 100;
  if (delay > 150000) delay = 100000;  // the clock stepped back mid-tick; resync next time
  esp_timer_stop(tickTimer);
  esp_timer_start_once(tickTimer, (uint64_t)delay);
}

// Runs at every 100 ms boundary of the wall clock. The slot is recomputed
// from gettimeofday() each time, so NTP adjustments re-align by themselves
// and HTTP or Wi-Fi work in loop() never delays the modulation.
static void onTick(void *) {
  xSemaphoreTake(radioMutex, portMAX_DELAY);
  if (radioOn) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t nowUs = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    int64_t slot = (nowUs + 50000) / 100000;  // nearest 100 ms boundary
    time_t sec = (time_t)(slot / 10);
    int tenth = (int)(slot % 10);
    time_t minute = sec - sec % 60;
    if (minute != frameMinute || frameDirty) {
      frameDirty = false;
      tc_build_minute(radioStation, minute, cfg.dstMode, frame);
      frameMinute = minute;
      frameCount = frameCount + 1;
    }
    int high = forceCarrier ? 1 : tc_carrier_high(radioStation, frame[sec % 60], tenth);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, high ? DUTY_HIGH : DUTY_LOW);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    carrierHigh = high;
    scheduleNextTick(slot);
  }
  xSemaphoreGive(radioMutex);
}

static void radioStopLocked() {
  if (!radioOn) return;
  radioOn = false;
  esp_timer_stop(tickTimer);
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
  pinHighZ(radioPin);  // no current through the antenna while off
  carrierHigh = 0;
}

static void radioStop() {
  xSemaphoreTake(radioMutex, portMAX_DELAY);
  radioStopLocked();
  xSemaphoreGive(radioMutex);
}

static void radioStart() {
  xSemaphoreTake(radioMutex, portMAX_DELAY);
  radioStopLocked();
  radioStation = cfg.station;
  radioPin = cfg.pin;

  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_MODE;
  t.duty_resolution = (ledc_timer_bit_t)LEDC_BITS;
  t.timer_num = LEDC_TIMER;
  t.freq_hz = tc_stations[radioStation].freq_hz;
  t.clk_cfg = LEDC_AUTO_CLK;
  esp_err_t err = ledc_timer_config(&t);
  ledc_channel_config_t c = {};
  c.gpio_num = radioPin;
  c.speed_mode = LEDC_MODE;
  c.channel = LEDC_CHANNEL;
  c.intr_type = LEDC_INTR_DISABLE;
  c.timer_sel = LEDC_TIMER;
  c.duty = DUTY_HIGH;
  c.hpoint = 0;
  if (err == ESP_OK) err = ledc_channel_config(&c);
  if (err != ESP_OK) {
    Serial.printf("radio: LEDC setup failed (%d)\n", (int)err);
    pinHighZ(radioPin);
    xSemaphoreGive(radioMutex);
    return;
  }
  gpio_set_drive_capability((gpio_num_t)radioPin, (gpio_drive_cap_t)cfg.drive);
  frameDirty = true;
  radioOn = true;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  scheduleNextTick(((int64_t)tv.tv_sec * 1000000 + tv.tv_usec) / 100000);
  xSemaphoreGive(radioMutex);
  Serial.printf("radio: %s on GPIO %d, %lu Hz (LEDC %lu Hz)\n", tc_stations[radioStation].name, radioPin,
                (unsigned long)tc_stations[radioStation].freq_hz,
                (unsigned long)ledc_get_freq(LEDC_MODE, LEDC_TIMER));
}

static void radioSetup() {
  radioMutex = xSemaphoreCreateMutex();
  esp_timer_create_args_t args = {};
  args.callback = &onTick;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "clocksync_tick";
  esp_timer_create(&args, &tickTimer);
  pinHighZ(cfg.pin);
}

// Start or stop the carrier to match the settings and the clock.
static void radioLoop() {
  bool want = cfg.tx && timeValid();
  if (want && (!radioOn || radioStation != cfg.station || radioPin != cfg.pin)) {
    radioStart();
  } else if (!want && radioOn) {
    radioStop();
    Serial.println(cfg.tx ? "radio: waiting for the time to be set" : "radio: off");
  }
}

static String frameString() {
  char buf[61];
  xSemaphoreTake(radioMutex, portMAX_DELAY);
  for (int i = 0; i < 60; i++) {
    buf[i] = frame[i] == TC_SYM_MARK ? 'M' : (char)('0' + frame[i]);
  }
  xSemaphoreGive(radioMutex);
  buf[60] = 0;
  return radioOn ? String(buf) : String();
}

// ------------------------------------------------------------- settings API

static void setStation(int s) {
  cfg.station = s;
  saveRadioSettings();
  applyTimeZone();  // the automatic zone follows the station
}

static void setTx(bool on) {
  cfg.tx = on;
  saveRadioSettings();
}

static void setDstMode(int m) {
  cfg.dstMode = m;
  saveRadioSettings();
  frameDirty = true;
}

static void setDrive(int d) {
  cfg.drive = d;
  saveRadioSettings();
  if (radioOn) gpio_set_drive_capability((gpio_num_t)radioPin, (gpio_drive_cap_t)cfg.drive);
}

static void setPin(int p) {
  if (p == cfg.pin) return;
  if (radioOn) radioStop();
  pinHighZ(cfg.pin);
  cfg.pin = p;
  saveRadioSettings();
}

static void setLed(bool on) {
  cfg.led = on;
  saveInt("led", on ? 1 : 0);
}

// name: "" = station's own zone, "custom" = posix, otherwise an IANA zone.
static bool setTimeZone(const char *name, const char *posix, String &err) {
  if (name[0] == 0) {
    cfg.tzName[0] = 0;
    cfg.tzPosix[0] = 0;
  } else if (strcmp(name, "custom") == 0) {
    if (!validPosix(posix)) {
      err = "invalid POSIX TZ string";
      return false;
    }
    copyStr(cfg.tzName, sizeof(cfg.tzName), "custom");
    copyStr(cfg.tzPosix, sizeof(cfg.tzPosix), posix);
  } else {
    char rule[64];
    if (strlen(name) >= sizeof(cfg.tzName) || !tzLookup(name, rule, sizeof(rule))) {
      err = "unknown time zone";
      return false;
    }
    copyStr(cfg.tzName, sizeof(cfg.tzName), name);
    copyStr(cfg.tzPosix, sizeof(cfg.tzPosix), rule);
  }
  saveStr("tzname", cfg.tzName);
  saveStr("tz", cfg.tzPosix);
  applyTimeZone();
  return true;
}

// ---------------------------------------------------------------------- time

static void onNtpSync(struct timeval *tv) {
  lastNtpSync = tv->tv_sec;
}

static void ntpStop() {
  if (!ntpRunning) return;
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_sntp_stop();
#else
  sntp_stop();
#endif
  ntpRunning = false;
}

static void ntpStart() {
  ntpStop();
  ntpServerBuf ^= 1;
  char *srv = ntpServerBufs[ntpServerBuf];
  copyStr(srv, sizeof(ntpServerBufs[0]), cfg.ntpServer);
  sntp_set_time_sync_notification_cb(onNtpSync);
  sntp_set_sync_interval(3600UL * 1000UL);  // hourly keeps the clock within a few ms
  xSemaphoreTake(radioMutex, portMAX_DELAY);  // configTzTime also sets TZ
  configTzTime(effectivePosix(), srv, "time.nist.gov", "time.google.com");
  xSemaphoreGive(radioMutex);
  ntpRunning = true;
  Serial.printf("ntp: using %s\n", srv);
}

static void setNtp(bool on) {
  cfg.ntp = on;
  saveInt("ntp2", on ? 1 : 0);
  if (on && connected()) ntpStart();
  if (!on) ntpStop();
}

static void setNtpServer(const char *s) {
  copyStr(cfg.ntpServer, sizeof(cfg.ntpServer), s);
  saveStr("ntpsrv", cfg.ntpServer);
  if (cfg.ntp && connected()) ntpStart();
}

static void setClock(time_t sec, long usec) {
  struct timeval tv;
  tv.tv_sec = sec;
  tv.tv_usec = usec;
  settimeofday(&tv, NULL);
  frameDirty = true;
  Serial.printf("time set: %s\n", fmtLocal(sec).c_str());
}

// ---------------------------------------------------------------------- wifi

static void startMdns() {
  if (mdnsStarted) return;
  if (MDNS.begin(hostActive)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.printf("mdns: http://%s.local/\n", hostActive);
  }
}

static String apSsid() {
  return String(hostActive) + "-setup";
}

static void staBegin() {
  lastStaAttemptMs = millis();
  if (cfg.ssid[0] == 0) return;
  Serial.printf("wifi: connecting to \"%s\"\n", cfg.ssid);
  WiFi.begin(cfg.ssid, cfg.pass);
}

static void startAP(bool manual) {
  apManual = apManual || manual;
  apStartedMs = millis();
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  const char *pw = AP_PASSWORD;
  if (!WiFi.softAP(apSsid().c_str(), strlen(pw) >= 8 ? pw : NULL)) {
    Serial.println("wifi: could not start the setup hotspot");
    WiFi.mode(WIFI_STA);
    apFailedMs = millis();
    return;
  }
  apFailedMs = 0;
  dns.start(53, "*", AP_IP);
  apActive = true;
  startMdns();
  Serial.printf("wifi: setup hotspot \"%s\" at http://%s/\n", apSsid().c_str(), AP_IP.toString().c_str());
}

static void stopAP() {
  if (!apActive) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  apActive = false;
  apManual = false;
  Serial.println("wifi: setup hotspot closed");
}

static const char *wifiError() {
  switch (lastDisconnectReason) {
    case 0:
    case 8:  // ASSOC_LEAVE: we disconnected on purpose
      return "";
    case 2:    // AUTH_EXPIRE
    case 14:   // MIC_FAILURE
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
      return "wrong password?";
    case 201:  // NO_AP_FOUND
    case 210:  // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211:  // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case 212:  // NO_AP_FOUND_IN_RSSI_THRESHOLD
      return "network not found";
    default:
      return "could not connect";
  }
}

static void wifiSetup() {
  WiFi.persistent(false);  // credentials live in our own settings
  WiFi.setHostname(hostActive);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  // Reconnects are paced by wifiLoop(): the core's own retries rescan
  // constantly on a wrong password, which knocks phones off the hotspot.
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(
    [](WiFiEvent_t, WiFiEventInfo_t info) {
      // Keep the last real failure; 8 (ASSOC_LEAVE) is our own disconnect() call.
      if (info.wifi_sta_disconnected.reason != 8) lastDisconnectReason = info.wifi_sta_disconnected.reason;
    },
    ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (cfg.ssid[0]) {
    staBegin();
  } else {
    startAP(false);
  }
}

static void wifiLoop() {
  uint32_t now = millis();
  bool up = connected();
  if (up && !wasConnected) {
    everConnected = true;
    connectedSinceMs = now;
    lastDisconnectReason = 0;
    Serial.printf("wifi: connected to \"%s\", http://%s/ (http://%s.local/)\n", cfg.ssid,
                  WiFi.localIP().toString().c_str(), hostActive);
    startMdns();
    if (cfg.ntp) ntpStart();
  } else if (!up && wasConnected) {
    Serial.println("wifi: disconnected");
  }
  wasConnected = up;

  if (up) {
    lastConnectedMs = now;
    if (apActive) {
      // Keep the hotspot open a little so the setup page can show the new address.
      uint32_t since = now - (connectedSinceMs > apStartedMs ? connectedSinceMs : apStartedMs);
      uint32_t grace = apManual ? 600000 : 60000;
      if ((since > grace && WiFi.softAPgetStationNum() == 0) || since > 900000) stopAP();
    }
    return;
  }

  uint32_t lostFor = now - lastConnectedMs;
  if (!apActive && (cfg.ssid[0] == 0 || lostFor > (everConnected ? AP_AFTER_DROPOUT_MS : AP_AFTER_BOOT_MS)) &&
      (apFailedMs == 0 || now - apFailedMs > 60000)) {
    startAP(false);
  }
  // A connection attempt scans channels and briefly disturbs the hotspot, so
  // retry rarely while someone is on it (but do retry: a phone that stays
  // joined must not keep the clock off the home network forever).
  uint32_t retry = !apActive ? STA_RETRY_MS : WiFi.softAPgetStationNum() ? STA_RETRY_BUSY_MS : STA_RETRY_AP_MS;
  if (cfg.ssid[0] && now - lastStaAttemptMs > retry && (int32_t)(now - retryNotBeforeMs) >= 0) {
    staBegin();
  }
}

static void setWifi(const char *ssid, const char *pass) {
  copyStr(cfg.ssid, sizeof(cfg.ssid), ssid);
  copyStr(cfg.pass, sizeof(cfg.pass), pass);
  saveStr("ssid", cfg.ssid);
  saveStr("pass", cfg.pass);
  lastDisconnectReason = 0;
  WiFi.disconnect();
  staBegin();
}

// --------------------------------------------------------------- LED/button

static void ledWrite(int r, int g, int b) {
#if LED_PIN >= 0
#if LED_RGB
  rgbLedWrite(LED_PIN, r, g, b);
#else
  bool on = (r | g | b) != 0;
  digitalWrite(LED_PIN, (on != (LED_ACTIVE_LOW != 0)) ? HIGH : LOW);
#endif
#else
  (void)r;
  (void)g;
  (void)b;
#endif
}

static bool buttonHeld __attribute__((unused)) = false;

static void ledLoop() {
#if LED_PIN >= 0
  static uint32_t last = 0xffffffff;
  uint32_t now = millis();
  uint32_t color = 0;  // 0xRRGGBB, kept dim
  if (buttonHeld) {
    color = 0x000030;
  } else if (apActive) {
    color = ((now / (LED_RGB ? 500 : 125)) & 1) ? 0x000030 : 0;  // setup hotspot: blue blink
  } else if (!cfg.led) {
    color = 0;
  } else if (radioOn) {
    color = carrierHigh ? 0x003000 : 0;  // follows the modulation
  } else if (cfg.tx) {
    color = ((now / 1000) & 1) ? 0x302000 : 0;  // waiting for the time: slow amber blink
  }
  if (color != last) {
    last = color;
    ledWrite((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
  }
#endif
}

static void buttonLoop() {
#if BUTTON_PIN >= 0
  static bool stable = false, lastRaw = false, longDone = false;
  static uint32_t changedAt = 0, pressedAt = 0;
  uint32_t now = millis();
  bool raw = digitalRead(BUTTON_PIN) == LOW;
  if (raw != lastRaw) {
    lastRaw = raw;
    changedAt = now;
  }
  if (raw != stable && now - changedAt >= 50) {
    stable = raw;
    if (stable) {
      pressedAt = now;
      longDone = false;
    } else if (now - pressedAt < 1000) {  // 1-3 s: released while the LED showed blue, ignore
      setTx(!cfg.tx);
      Serial.printf("button: TX %s\n", cfg.tx ? "on" : "off");
    }
  }
  if (stable && !longDone && now - pressedAt >= 3000) {
    longDone = true;
    startAP(true);
  }
  buttonHeld = stable && now - pressedAt >= 1000;
#endif
}

// ----------------------------------------------------------------- self-test

static volatile uint32_t measCount = 0;

static void IRAM_ATTR onMeasEdge() {
  measCount = measCount + 1;
}

// Jumper the radio pin to MEAS_PIN and count carrier edges for 200 ms.
static String selfTest() {
  if (MEAS_PIN < 0 || MEAS_PIN == cfg.pin) return String("no measurement pin available");
  if (!radioOn) return String("carrier is off; turn TX on first");
  pinMode(MEAS_PIN, INPUT_PULLDOWN);
  measCount = 0;
  forceCarrier = true;
  attachInterrupt(digitalPinToInterrupt(MEAS_PIN), onMeasEdge, RISING);
  delay(200);
  detachInterrupt(digitalPinToInterrupt(MEAS_PIN));
  forceCarrier = false;
  pinMode(MEAS_PIN, INPUT);
  uint32_t hz = measCount * 5;
  uint32_t expected = tc_stations[radioStation].freq_hz;
  if (hz == 0) {
    return String("no edges on GPIO ") + MEAS_PIN + " (is it jumpered to GPIO " + radioPin + "?); LEDC reports " +
           ledc_get_freq(LEDC_MODE, LEDC_TIMER) + " Hz, expected " + expected + " Hz";
  }
  return String("measured ~") + hz + " Hz on GPIO " + MEAS_PIN + ", expected " + expected + " Hz";
}

// ------------------------------------------------------------- status/text

static const char *radioState() {
  if (radioOn) return "transmitting";
  if (cfg.tx) return "waiting";
  return "off";
}

static String statusText() {
  time_t now = time(NULL);
  const tc_station &st = tc_stations[cfg.station];
  String s;
  s.reserve(900);
  s += "clocksync v" VERSION " (" BOARD_NAME ")\n\n";
  s += "Station : ";
  s += st.name;
  s += " - ";
  s += st.region;
  s += "\nTX      : ";
  s += cfg.tx ? (radioOn ? "on, transmitting" : "on, waiting for the time to be set") : "off";
  s += "\nCarrier : ";
  s += st.freq_hz;
  s += " Hz on GPIO ";
  s += cfg.pin;
  s += ", drive ";
  s += cfg.drive;
  s += "\nTime    : ";
  s += timeValid() ? fmtLocal(now) : String("not set");
  s += "\nZone    : ";
  s += effectiveTzName();
  s += cfg.tzName[0] ? "" : " (station default)";
  s += " = ";
  s += effectivePosix();
  if (st.encodes_utc) s += "\n          WWVB sends UTC; the zone only sets the DST bits";
  s += "\nDST flag: ";
  s += cfg.dstMode == TC_DST_AUTO ? "auto" : (cfg.dstMode == TC_DST_SUMMER ? "forced summer" : "forced standard");
  s += "\nNTP     : ";
  if (cfg.ntp) {
    s += cfg.ntpServer;
    s += ", last sync ";
    s += ageText(lastNtpSync);
  } else {
    s += "off";
  }
  s += "\nWi-Fi   : ";
  if (connected()) {
    s += "\"";
    s += cfg.ssid;
    s += "\" ";
    s += WiFi.localIP().toString();
    s += " (";
    s += WiFi.RSSI();
    s += " dBm)";
  } else if (cfg.ssid[0]) {
    s += "not connected to \"";
    s += cfg.ssid;
    s += "\" ";
    s += wifiError();
  } else {
    s += "not configured";
  }
  if (apActive) {
    s += "\nHotspot : \"";
    s += apSsid();
    s += "\" http://";
    s += AP_IP.toString();
    s += "/";
  }
  s += "\nWeb UI  : http://";
  s += hostActive;
  s += ".local/";
  String f = frameString();
  if (f.length()) {
    s += "\nFrame   : ";
    s += f;
  }
  s += "\n";
  return s;
}

static const char HELP_TEXT[] =
  "\nCommands (serial or http://<device>/cmd?c=...):\n"
  "  h / status   : help and status\n"
  "  sX           : station: sj JJY40, sk JJY60, sw WWVB, sd DCF77, sm MSF, st BSF, sc BPC\n"
  "  e1 / e0      : transmit on / off\n"
  "  tz<zone>     : time zone, e.g. tzEurope/Berlin, tz<POSIX rule>, or tz alone for the station's own\n"
  "  x0 / x1 / x2 : DST flag standard / summer / auto\n"
  "  y1 / y0      : NTP on / off\n"
  "  ss<name>     : Wi-Fi network name\n"
  "  ps<password> : Wi-Fi password, then connect\n"
  "  setup        : open the Wi-Fi setup hotspot\n"
  "  pNN          : antenna GPIO pin\n"
  "  g0..g3       : drive strength (0 weakest)\n"
  "  l1 / l0      : status LED on / off\n"
  "  dYYMMDD      : set date (local)\n"
  "  tHHMMSS      : set time (local)\n"
  "  f            : frequency self-test (jumper antenna pin to the measurement pin)\n"
  "  r            : reset settings to defaults (keeps Wi-Fi)\n"
  "  reboot       : restart\n";

// ------------------------------------------------------------------ commands

static int twoDigits(const char *p) {
  if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) return -1;
  return (p[0] - '0') * 10 + (p[1] - '0');
}

static bool allDigits(const char *p) {
  if (!*p) return false;
  for (; *p; p++) {
    if (!isdigit((unsigned char)*p)) return false;
  }
  return true;
}

static void resetSettings() {
  static const char *const keys[] = { "station", "tx", "pin", "drive", "led", "ntp2", "dst", "tzname", "tz", "ntpsrv" };
  for (const char *k : keys) prefs.remove(k);
  if (radioOn) radioStop();
  pinHighZ(cfg.pin);
  Settings d;
  defaultSettings(d);
  memcpy(d.ssid, cfg.ssid, sizeof(d.ssid));
  memcpy(d.pass, cfg.pass, sizeof(d.pass));
  memcpy(d.host, cfg.host, sizeof(d.host));
  cfg = d;
  applyTimeZone();
  if (cfg.ntp && connected()) ntpStart();
}

static void scheduleReboot() {
  rebootPending = true;
  rebootAtMs = millis() + 500;
}

// Runs one command; returns false for unknown or invalid commands.
static bool runCommand(const char *c, String &reply) {
  size_t n = strlen(c);
  if (n == 0) return false;
  char c0 = (char)tolower((unsigned char)c[0]);
  char c1 = n > 1 ? (char)tolower((unsigned char)c[1]) : 0;

  if (strcmp(c, "h") == 0 || strcmp(c, "help") == 0) {
    reply = statusText() + HELP_TEXT;
    return true;
  }
  if (strcmp(c, "status") == 0) {
    reply = statusText();
    return true;
  }
  if (strcmp(c, "reboot") == 0) {
    reply = "rebooting\n";
    scheduleReboot();
    return true;
  }
  if (strcmp(c, "setup") == 0) {
    startAP(true);
    reply = String("setup hotspot \"") + apSsid() + "\" at http://" + AP_IP.toString() + "/\n";
    return true;
  }
  if (c0 == 's' && c1 == 's') {  // Wi-Fi SSID
    if (n - 2 >= sizeof(cfg.ssid)) return false;
    copyStr(cfg.ssid, sizeof(cfg.ssid), c + 2);
    saveStr("ssid", cfg.ssid);
    reply = "SSID saved; now send ps<password> (or ps alone for an open network)\n";
    return true;
  }
  if (c0 == 'p' && c1 == 's') {  // Wi-Fi password, then connect
    if (n - 2 >= sizeof(cfg.pass) || cfg.ssid[0] == 0) return false;
    setWifi(cfg.ssid, c + 2);
    reply = String("connecting to \"") + cfg.ssid + "\"\n";
    return true;
  }
  if (c0 == 't' && c1 == 'z') {  // time zone
    const char *z = c + 2;
    String err;
    bool ok;
    char rule[64];
    if (*z == 0 || strcmp(z, "auto") == 0) {
      ok = setTimeZone("", "", err);
    } else if (tzLookup(z, rule, sizeof(rule))) {
      ok = setTimeZone(z, "", err);
    } else {
      ok = setTimeZone("custom", z, err);
    }
    reply = ok ? String("time zone: ") + effectiveTzName() + " = " + effectivePosix() + "\n" : err + "\n";
    return ok;
  }
  switch (c0) {
    case 's': {  // station
      int s = n == 2 ? tc_station_by_cmd(c1) : -1;
      if (s < 0) return false;
      setStation(s);
      reply = String("station: ") + tc_stations[s].name + "\n";
      return true;
    }
    case 'e':  // transmit on/off
      if (n != 2 || (c1 != '0' && c1 != '1')) return false;
      setTx(c1 == '1');
      reply = cfg.tx ? "TX enabled\n" : "TX disabled\n";
      return true;
    case 'p': {  // antenna pin
      if (!allDigits(c + 1) || n > 3) return false;
      int p = atoi(c + 1);
      if (!pinAllowed(p)) {
        reply = "pin not allowed on this board\n";
        return false;
      }
      setPin(p);
      reply = String("radio pin: GPIO ") + p + "\n";
      return true;
    }
    case 'g':  // drive strength
      if (n != 2 || c1 < '0' || c1 > '3') return false;
      setDrive(c1 - '0');
      reply = String("drive strength: ") + cfg.drive + "\n";
      return true;
    case 'y':  // NTP
      if (n != 2 || (c1 != '0' && c1 != '1')) return false;
      setNtp(c1 == '1');
      reply = cfg.ntp ? "NTP on\n" : "NTP off\n";
      return true;
    case 'l':  // LED
      if (n != 2 || (c1 != '0' && c1 != '1')) return false;
      setLed(c1 == '1');
      reply = cfg.led ? "LED on\n" : "LED off\n";
      return true;
    case 'x':  // DST flag mode
      if (n != 2 || c1 < '0' || c1 > '2') return false;
      setDstMode(c1 - '0');
      reply = String("DST flag: ") + (cfg.dstMode == TC_DST_AUTO ? "auto" : cfg.dstMode ? "summer" : "standard") + "\n";
      return true;
    case 'd':
    case 't': {  // dYYMMDD / tHHMMSS in local time
      if (n != 7) return false;
      int a = twoDigits(c + 1), b = twoDigits(c + 3), d = twoDigits(c + 5);
      if (a < 0 || b < 0 || d < 0) return false;
      time_t now = time(NULL);
      struct tm lt;
      localtime_r(&now, &lt);
      if (c0 == 'd') {
        if (b < 1 || b > 12 || d < 1 || d > 31) return false;
        lt.tm_year = a + 100;
        lt.tm_mon = b - 1;
        lt.tm_mday = d;
      } else {
        if (a > 23 || b > 59 || d > 59) return false;
        lt.tm_hour = a;
        lt.tm_min = b;
        lt.tm_sec = d;
      }
      lt.tm_isdst = -1;
      setClock(mktime(&lt), 0);
      reply = String("time: ") + fmtLocal(time(NULL)) + "\n";
      return true;
    }
    case 'f':
      if (n != 1) return false;
      reply = selfTest() + "\n";
      return true;
    case 'r':
      if (n != 1) return false;
      resetSettings();
      reply = "settings reset to defaults (Wi-Fi kept)\n";
      return true;
  }
  return false;
}

static void serialLoop() {
  static char buf[160];
  static size_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      buf[len] = 0;
      if (len) {
        String reply;
        bool ok = runCommand(buf, reply);
        Serial.print(ok ? "OK\n" : "ERR (h for help)\n");
        Serial.print(reply);
      }
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = ch;
    }
  }
}

// ---------------------------------------------------------------------- HTTP

static void jsonStr(String &o, const char *s) {
  o += '"';
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((uint8_t)c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += c;
    }
  }
  o += '"';
}

static void jsonKey(String &o, const char *k) {
  if (o.length() > 1) o += ',';
  jsonStr(o, k);
  o += ':';
}

static void jsonKV(String &o, const char *k, const char *v) {
  jsonKey(o, k);
  jsonStr(o, v);
}

static void jsonKV(String &o, const char *k, long v) {
  jsonKey(o, k);
  o += v;
}

static void jsonKV(String &o, const char *k, bool v) {
  jsonKey(o, k);
  o += v ? "true" : "false";
}

static String statusJson() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  time_t now = tv.tv_sec;
  struct tm lt;
  localtime_r(&now, &lt);
  char abbr[16];
  strftime(abbr, sizeof(abbr), "%Z", &lt);
  long offset = utcOffsetAt(now);

  String o;
  o.reserve(1800);
  o += '{';
  jsonKV(o, "ok", true);
  jsonKV(o, "version", VERSION);
  jsonKV(o, "board", BOARD_NAME);
  jsonKV(o, "host", hostActive);
  jsonKV(o, "hostSaved", cfg.host);
  jsonKV(o, "uptime", (long)(millis() / 1000));
  jsonKey(o, "ms");
  char ms[24];
  snprintf(ms, sizeof(ms), "%lld", (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
  o += ms;
  jsonKV(o, "timeValid", timeValid());
  jsonKV(o, "station", tc_stations[cfg.station].key);
  jsonKV(o, "tx", cfg.tx);
  jsonKV(o, "state", radioState());
  jsonKV(o, "freq", (long)tc_stations[cfg.station].freq_hz);
  jsonKV(o, "ledcHz", radioOn ? (long)ledc_get_freq(LEDC_MODE, LEDC_TIMER) : 0L);
  jsonKV(o, "tz", cfg.tzName);
  jsonKV(o, "tzEff", effectiveTzName());
  jsonKV(o, "posix", effectivePosix());
  jsonKV(o, "offset", offset);
  jsonKV(o, "isDst", lt.tm_isdst > 0);
  jsonKV(o, "abbr", abbr);
  jsonKV(o, "dstMode", (long)cfg.dstMode);
  jsonKV(o, "ntp", cfg.ntp);
  jsonKV(o, "ntpServer", cfg.ntpServer);
  jsonKV(o, "lastSync", lastNtpSync ? (long)(now - lastNtpSync) : -1L);
  jsonKV(o, "pin", (long)cfg.pin);
  jsonKey(o, "pins");
  o += '[';
  bool first = true;
  for (int p : SAFE_PIN_LIST) {
    if (!first) o += ',';
    o += p;
    first = false;
  }
  o += ']';
  jsonKV(o, "drive", (long)cfg.drive);
  jsonKV(o, "led", cfg.led);
  jsonKV(o, "measPin", (long)MEAS_PIN);
  jsonKV(o, "frame", frameString().c_str());

  bool up = connected();
  jsonKey(o, "wifi");
  String w = "{";
  jsonKV(w, "ssid", cfg.ssid);
  jsonKV(w, "connected", up);
  jsonKV(w, "connecting", !up && cfg.ssid[0] && millis() - lastStaAttemptMs < 15000);
  jsonKV(w, "error", up ? "" : wifiError());
  jsonKV(w, "ip", up ? WiFi.localIP().toString().c_str() : "");
  jsonKV(w, "rssi", up ? (long)WiFi.RSSI() : 0L);
  jsonKV(w, "ap", apActive);
  jsonKV(w, "apSsid", apSsid().c_str());
  jsonKV(w, "apIp", AP_IP.toString().c_str());
  w += '}';
  o += w;

  jsonKey(o, "stations");
  o += '[';
  for (int i = 0; i < TC_STATION_COUNT; i++) {
    const tc_station &st = tc_stations[i];
    String e = "{";
    jsonKV(e, "key", st.key);
    jsonKV(e, "name", st.name);
    jsonKV(e, "region", st.region);
    jsonKV(e, "freq", (long)st.freq_hz);
    jsonKV(e, "tz", st.native_tz);
    jsonKV(e, "utc", st.encodes_utc != 0);
    jsonKey(e, "shapes");  // carrier envelope per symbol, for the UI's signal trace
    e += '[';
    for (int k = 0; k < TC_SYM_COUNT; k++) {
      if (k) e += ',';
      jsonStr(e, tc_shape(i, (uint8_t)k));
    }
    e += ']';
    e += '}';
    if (i) o += ',';
    o += e;
  }
  o += ']';
  o += '}';
  return o;
}

static void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendError(int code, const String &msg) {
  String o = "{";
  jsonKV(o, "ok", false);
  jsonKV(o, "error", msg.c_str());
  o += '}';
  sendJson(code, o);
}

static void sendOk() {
  sendJson(200, "{\"ok\":true}");
}

// State-changing API calls must carry this header. Browsers cannot add it to
// cross-site requests without a CORS preflight, which we never approve.
static bool apiAllowed() {
  if (server.hasHeader("X-Clocksync")) return true;
  sendError(403, "missing X-Clocksync header");
  return false;
}

static bool isOwnHost(const String &host) {
  String h = host;
  int colon = h.indexOf(':');
  if (colon >= 0) h = h.substring(0, colon);
  h.toLowerCase();
  if (h.length() == 0 || h == AP_IP.toString() || h == WiFi.localIP().toString()) return true;
  return h == String(hostActive) + ".local" || h == String(hostActive);
}

// Captive portal: phones that join the hotspot probe a known URL; send those
// requests to the setup page. Requests from the home network are left alone.
static bool captiveRedirect() {
  if (!apActive || !(server.client().localIP() == AP_IP) || isOwnHost(server.hostHeader())) return false;
  server.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
  server.send(302, "text/plain", "");
  return true;
}

static void handleRoot() {
  if (captiveRedirect()) return;
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", WEBUI_HTML);
}

static void handleNotFound() {
  if (captiveRedirect()) return;
  server.send(404, "text/plain", "not found\n");
}

static void handleStatusText() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", statusText());
}

// Commands that change Wi-Fi, reset or restart need the API header. The rest
// stay plain GETs so existing Home Assistant setups keep working.
static bool commandNeedsHeader(const String &c) {
  String l = c;
  l.toLowerCase();
  return l.startsWith("ss") || l.startsWith("ps") || l == "r" || l == "reboot" || l == "setup";
}

static void handleCmd() {
  String c = server.arg("c");
  if (c.length() == 0 || c.length() > 120) {
    server.send(400, "text/plain", "ERR missing or bad c parameter\n");
    return;
  }
  if (commandNeedsHeader(c) && !server.hasHeader("X-Clocksync")) {
    server.send(403, "text/plain", "ERR this command needs the header X-Clocksync: 1\n");
    return;
  }
  String reply;
  bool ok = runCommand(c.c_str(), reply);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", String(ok ? "OK\n" : "ERR\n") + reply);
}

static void handleApiStatus() {
  sendJson(200, statusJson());
}

static void handleApiZones() {
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send_P(200, "text/plain", TZDATA);
}

// Integer form field; false unless it is a plain non-negative number.
static bool argInt(const char *name, int &out) {
  String v = server.arg(name);
  v.trim();
  if (v.length() == 0 || v.length() > 4 || !allDigits(v.c_str())) return false;
  out = v.toInt();
  return true;
}

static bool argBool(const char *name) {
  String v = server.arg(name);
  return v == "1" || v == "true" || v == "on";
}

static void handleApiSet() {
  if (!apiAllowed()) return;
  String err;
  if (server.hasArg("station")) {
    int s = tc_station_by_key(server.arg("station").c_str());
    if (s < 0) err = "unknown station";
    else setStation(s);
  }
  if (err.length() == 0 && server.hasArg("tz")) {
    setTimeZone(server.arg("tz").c_str(), server.arg("posix").c_str(), err);
  }
  if (err.length() == 0 && server.hasArg("dst")) {
    int d;
    if (!argInt("dst", d) || d > 2) err = "bad dst mode";
    else setDstMode(d);
  }
  if (err.length() == 0 && server.hasArg("pin")) {
    int p;
    if (!argInt("pin", p) || !pinAllowed(p)) err = "pin not allowed on this board";
    else setPin(p);
  }
  if (err.length() == 0 && server.hasArg("drive")) {
    int d;
    if (!argInt("drive", d) || d > 3) err = "bad drive strength";
    else setDrive(d);
  }
  if (err.length() == 0 && server.hasArg("ntpServer")) {
    String s = server.arg("ntpServer");
    s.trim();
    if (!validServer(s.c_str())) err = "bad NTP server name";
    else setNtpServer(s.c_str());
  }
  if (err.length() == 0 && server.hasArg("ntp")) setNtp(argBool("ntp"));
  if (err.length() == 0 && server.hasArg("led")) setLed(argBool("led"));
  if (err.length() == 0 && server.hasArg("tx")) setTx(argBool("tx"));
  if (err.length() == 0 && server.hasArg("host")) {
    String h = server.arg("host");
    h.trim();
    h.toLowerCase();
    if (!validHostname(h.c_str())) {
      err = "name: up to 26 of a-z, 0-9 and -";
    } else {
      copyStr(cfg.host, sizeof(cfg.host), h.c_str());
      saveStr("host", cfg.host);  // used after the next restart
    }
  }
  if (err.length()) {
    sendError(400, err);
    return;
  }
  radioLoop();  // apply immediately so the response shows the new state
  sendJson(200, statusJson());
}

static void handleApiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    sendJson(200, "{\"ok\":true,\"scanning\":true}");
    return;
  }
  if (n < 0 || server.hasArg("start")) {
    WiFi.scanDelete();
    if (!connected()) WiFi.disconnect();  // a pending connection attempt blocks scanning
    retryNotBeforeMs = millis() + 15000;  // and hold off retries while it runs
    WiFi.scanNetworks(true);
    sendJson(200, "{\"ok\":true,\"scanning\":true}");
    return;
  }
  String o = "{\"ok\":true,\"networks\":[";
  int count = 0;
  for (int i = 0; i < n && count < 30; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool dup = false;  // one entry per name, strongest first
    for (int j = 0; j < i && !dup; j++) dup = WiFi.SSID(j) == ssid && WiFi.RSSI(j) >= WiFi.RSSI(i);
    if (dup) continue;
    String e = "{";
    jsonKV(e, "ssid", ssid.c_str());
    jsonKV(e, "rssi", (long)WiFi.RSSI(i));
    jsonKV(e, "open", WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    e += '}';
    if (count++) o += ',';
    o += e;
  }
  o += "]}";
  sendJson(200, o);
}

static void handleApiWifi() {
  if (!apiAllowed()) return;
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0 || ssid.length() >= sizeof(cfg.ssid)) {
    sendError(400, "network name must be 1-32 characters");
    return;
  }
  if (pass.length() >= sizeof(cfg.pass) || (pass.length() > 0 && pass.length() < 8)) {
    sendError(400, "password must be 8-63 characters (or empty for an open network)");
    return;
  }
  setWifi(ssid.c_str(), pass.c_str());
  sendOk();
}

static void handleApiTime() {
  if (!apiAllowed()) return;
  long long ms = atoll(server.arg("ms").c_str());
  if (ms < (long long)VALID_TIME * 1000) {
    sendError(400, "bad time");
    return;
  }
  setClock((time_t)(ms / 1000), (long)(ms % 1000) * 1000);
  sendJson(200, statusJson());
}

static void handleApiSetup() {
  if (!apiAllowed()) return;
  startAP(true);
  sendOk();
}

static void handleApiReboot() {
  if (!apiAllowed()) return;
  sendOk();
  scheduleReboot();
}

static void handleApiForget() {
  if (!apiAllowed()) return;
  cfg.ssid[0] = 0;
  cfg.pass[0] = 0;
  saveStr("ssid", "");
  saveStr("pass", "");
  sendOk();
  scheduleReboot();
}

static void handleApiReset() {
  if (!apiAllowed()) return;
  resetSettings();
  sendJson(200, statusJson());
}

static void webSetup() {
  static const char *headers[] = { "X-Clocksync" };
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status.txt", HTTP_GET, handleStatusText);
  server.on("/cmd", HTTP_GET, handleCmd);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/zones", HTTP_GET, handleApiZones);
  server.on("/api/scan", HTTP_GET, handleApiScan);
  server.on("/api/set", HTTP_POST, handleApiSet);
  server.on("/api/wifi", HTTP_POST, handleApiWifi);
  server.on("/api/time", HTTP_POST, handleApiTime);
  server.on("/api/setup", HTTP_POST, handleApiSetup);
  server.on("/api/reboot", HTTP_POST, handleApiReboot);
  server.on("/api/forget", HTTP_POST, handleApiForget);
  server.on("/api/reset", HTTP_POST, handleApiReset);
  server.onNotFound(handleNotFound);
  server.begin();
}

// --------------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nclocksync v" VERSION " (" BOARD_NAME ")");

#if LED_PIN >= 0 && !LED_RGB
  pinMode(LED_PIN, OUTPUT);
#endif
  ledWrite(0, 0, 0);
#if BUTTON_PIN >= 0
  pinMode(BUTTON_PIN, BUTTON_PULLUP ? INPUT_PULLUP : INPUT);
#endif

  loadSettings();
  copyStr(hostActive, sizeof(hostActive), cfg.host);
  radioSetup();
  applyTimeZone();
  wifiSetup();
  webSetup();
  Serial.println("Type h + Enter for help.");
}

void loop() {
  serialLoop();
  buttonLoop();
  wifiLoop();
  if (apActive) dns.processNextRequest();
  server.handleClient();
  radioLoop();
  ledLoop();

  static uint32_t loggedFrame = 0;
  if (radioOn && frameCount != loggedFrame) {
    loggedFrame = frameCount;
    Serial.printf("%s  %s\n", fmtLocal(time(NULL)).c_str(), frameString().c_str());
  }
  if (rebootPending && (int32_t)(millis() - rebootAtMs) >= 0) {
    Serial.println("rebooting");
    Serial.flush();
    ESP.restart();
  }
  delay(2);
}
