//
// nisejjy - fake JJY (standarad time radio broadcast) station
//     using software radio on ESP32.
//
// (c) 2021 by taroh (sasaki.taroh@gmail.com)
//
// Forked as 'clocksync' and maintained by Tanvach (2025).
// This is a fork with improvements and enhancements; see README for details.
//
// 2021. 10. 28-29: ver. 0.1: worked on JJY 40KHz
// 2021. 10. 30: ver 0.2: added Bluetooth command
// 2021. 10. 30-11. 2: ver 1.0: added codes for WWVB/DCF77/HBG/MSF/BPC
//   => removed HBG, added BSF, checked WWVB/DCF77/MSF by world radio clock.
//   * DCF77, MSF forwards 1 minute.
// 2022.  1. 21: ver 1.1: added NTP feature
//
// JJY (Japan): https://ja.wikipedia.org/wiki/JJY
// WWVB (US): https://en.wikipedia.org/wiki/WWVB
//            *set UT for WWVB.
// DCF77 (Germany) : https://www.eecis.udel.edu/~mills/ntp/dcf77.html
// (HBG (Switzerland) discon: https://msys.ch/decoding-time-signal-stations )
// BSF (Taiwan): https://en.wikipedia.org/wiki/BSF_(time_service)
// MSF (UK): https://en.wikipedia.org/wiki/Time_from_NPL_(MSF)
// BPC (China): https://harmonyos.51cto.com/posts/1731
//
// note: BSF, BPC codes are not certified.

//...................................................................
// hardware config
#define PIN_RADIO (25)  //(23)
#define PIN_BUZZ (-1)   // no onboard buzzer on M5 Atom Lite
#define PIN_LED (-1)    // use M5 Atom RGB LED instead
// Optional measurement input (jumper to radio pin for self-test)
#define PIN_MEAS (33)
// note: {pin23 -> 330ohm -> 30cm loop antenna -> GND} works
//     (33mW, but detuned length (super shorten), only very weak radiowave emitted).

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
// M5 Atom Lite support (RGB LED, button, etc.)
#include <M5Atom.h>
// Drive strength configuration for safe antenna drive
#include "driver/gpio.h"
// High-resolution timer for precise tick scheduling
#include "esp_timer.h"
// ESP-IDF LEDC driver
#include "driver/ledc.h"
#include <Preferences.h>
#define DEVICENAME "clocksync"

// WiFi credentials are sourced from secrets.h (gitignored). An example
// file is provided as secrets_example.h. If secrets.h is missing, the
// example is used so the sketch still builds.
#if __has_include("secrets.h")
#include "secrets.h"
#elif __has_include("secrets_example.h")
#include "secrets_example.h"
#else
#error "Missing secrets.h or secrets_example.h"
#endif

char ssid[] = WIFI_SSID;
char passwd[] = WIFI_PASS;
// POSIX TZ string (see README for station-specific guidance)
#define TZ "UTC0"  // e.g., "UTC0", "JST-9", "CET-1CEST,M3.5.0/2,M10.5.0/3"

// ------------------------------------------------------------------
// Logging helpers (USB serial only; web UI shows status separately)
#define LOG_PRINT(x) do { Serial.print(x); } while (0)
#define LOG_PRINTLN(x) do { Serial.println(x); } while (0)
#define LOG_PRINTF(fmt, ...) do { Serial.printf((fmt), ##__VA_ARGS__); } while (0)

// Minimal HTTP server for control/status
WebServer server(80);

//...................................................................
// station specs
//
#define SN_JJY_E (0)  // JJY Fukushima Japan
#define SN_JJY_W (1)  // JJY Fukuoka Japan
#define SN_WWVB (2)   // WWVB US
#define SN_DCF77 (3)  // DCF77 Germany
#define SN_BSF (4)    // BSF Taiwan
#define SN_MSF (5)    // MSF UK
#define SN_BPC (6)    // BPC China

#define SN_DEFAULT (SN_WWVB)

int st_cycle2[] = {
  // interrupt cycle, KHz: double of station freq
  80,   // 40KHz JJY-E
  120,  // 60KHz JJY-W
  120,  // 60KHz WWVB
  155,  // 77.5KHz DCF77
  155,  // 77.5KHz BSF
  120,  // 60KHz MSF
  137   // 68.5KHz BPC
};

// interrupt cycle to makeup radio wave, buzzer (500Hz = 1KHz cycle):
// peripheral freq == 80MHz
//    ex. radio freq 40KHz: intr 80KHz: 80KHz / 80MHz => 1/1000 (1/tm0cycle)
//    buzz cycle: 1KHz / 80KHz 1/80 (1/radiodiv)
int tm0cycle;
#define TM0RES (1)
int radiodiv;

// TM0RES (interrupt counter), AMPDIV (buzz cycle(1000) / subsec(10)), SSECDIV (subsec / sec)
// don't depend on station specs.
#define AMPDIV (100)  // 1KHz / 100 => 10Hz, amplitude may change every 0.1 seconds
#define SSECDIV (10)  // 10Hz / 10 => 1Hz, clock ticks

// enum symbols
#define SP_0 (0)
#define SP_1 (1)
#define SP_M (2)
#define SP_P0 (SP_1)  // for MSF
#define SP_P1 (3)     // for MSF
/*#define SP_M0 (3) // for HBG
#define SP_M00 (4) // for HBG
#define SP_M000 (5) // for HBG
 */
#define SP_2 (2)   // for BSF/BPC
#define SP_3 (3)   // for BSF/BPC
#define SP_M4 (4)  // for BSF/BPC
#define SP_MAX (SP_M4)
//
// bits_STATION[] => *bits60: 60 second symbol buffers, initialized with patterns
// sp_STATION[] => *secpattern: 0.1sec term pattern in one second, for each symbol
// * note: when sp_STATION[n * 10], secpattern[] is like 2-dim array secpattern[n][10].
//
// JJY & WWVB  *note: comment is the format of JJY.
int8_t bits_jjy[] = {
  // 60bit transmitted frame, of {SP_0, SP_1, SP_M}
  SP_M, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M,  // (M), MIN10[3], 0, MIN1[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M,  // 0, 0, HOUR10[2], 0, HOUR1[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M,  // 0, 0, DOY100[2], DOY10[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M,  // DOY1[4], 0, 0, PA1, PA2, 0, (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M,  // 0, YEAR[8], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M   // DOW[3], LS1, LS2, 0, 0, 0, 0, (M)
};
// *note: if summer time, set bit 57/58 (WWVB) (bit 38/40 (JJY, in future))
int8_t sp_jjy[] = {
  // in (0, 1), [SP_x][amplitude_for_0.1sec_term_in_second]
  1, 1, 1, 1, 1, 1, 1, 1, 0, 0,  // SP_0
  1, 1, 1, 1, 1, 0, 0, 0, 0, 0,  // SP_1
  1, 1, 0, 0, 0, 0, 0, 0, 0, 0   // SP_M
};
int8_t sp_wwvb[] = {
  // in (0, 1), [SP_x][amplitude_for_0.1sec_term_in_second]
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,  // SP_0
  0, 0, 0, 0, 0, 1, 1, 1, 1, 1,  // SP_1
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1   // SP_M
};

// DCF77 encoding is LSB->MSB. //HBG *note: [0] is changed depending on DCF/HBG (also min/hour).
int8_t bits_dcf[] = {
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // 0, reserved[9]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_1, SP_0,  // reserved[5], 0, 0, (0, 1)(MEZ), 0
  SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // 1, MIN1[4], MIN10[3], P1, (1->)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // HOUR1[4], HOUR10[2], P2, D1[4]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // D10[2], DOW[3], M1[4], M10[1]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M   // Y1[4], Y10[4], P3, (M)
};
int8_t sp_dcf[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // SP_0
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,  // SP_1
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1   // SP_M
};
/*
int8_t sp_hbg[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_1
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_M
  0, 1, 0, 1, 1, 1, 1, 1, 1, 1,   // SP_M0    // 00sec
  0, 1, 0, 1, 0, 1, 1, 1, 1, 1,   // SP_M00   // 00sec at 00min
  0, 1, 0, 1, 0, 1, 0, 1, 1, 1    // SP_M000  // 00sec at 00/12 hour 00min
};
*/

// BSF: quad encoding.
int8_t bits_bsf[] = {
  SP_M4, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M4,
  SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // 1, min[3], hour[2.5], P1[.5],
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M4  // DOM[2.5], DOW[2.5], mon[2],
                                                               // year[3.5], P2[.5], 0, 0, M
};
int8_t sp_bsf[] = {
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,  // SP_0
  0, 0, 0, 0, 1, 1, 1, 1, 1, 1,  // SP_1
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1,  // SP_2
  0, 0, 0, 0, 0, 0, 1, 1, 1, 1,  // SP_3
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1   // SP_M4
};

// MSF has 4 patterns.
int8_t bits_msf[] = {
  SP_M, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_P0, SP_P0, SP_P0, SP_P0, SP_P0, SP_P0, SP_0
};
int8_t sp_msf[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // SP_0
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,  // SP_1/SP_P0 (parity 0)
  0, 0, 0, 0, 0, 1, 1, 1, 1, 1,  // SP_M
  0, 0, 0, 1, 1, 1, 1, 1, 1, 1   // SP_P1 (parity 1)
};

// BPC: quadary and has 5 patterns.
int8_t bits_bpc[] = {
  SP_M4, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // (B), P1, P2, h[2], m[3], DOW[2]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,   // P3, D[3], M[2], Y[3], P4
  SP_M4, SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // P1: 0, 1, 2 for 00-19, -39, -59s
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,   // P2: 0, P3: AM/PM(0/2)+par<hmDOW>
  SP_M4, SP_2, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // P4: par<DMY> (0/1)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0
};
int8_t sp_bpc[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // SP_0
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,  // SP_1
  0, 0, 0, 1, 1, 1, 1, 1, 1, 1,  // SP_2
  0, 0, 0, 0, 1, 1, 1, 1, 1, 1,  // SP_3
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0   // SP_M4
};

// func for makeup patterns
void mb_jjy(void);   // JJY-E, JJY-W
void mb_wwvb(void);  // WWVB
void mb_dcf(void);   // DCF77
void mb_bsf(void);   // BSF
void mb_msf(void);   // MSF
void mb_bpc(void);   // BPC

int8_t *st_bits[] = { bits_jjy, bits_jjy, bits_jjy, bits_dcf, bits_bsf, bits_msf, bits_bpc };
int8_t *bits60;
int8_t *st_sp[] = { sp_jjy, sp_jjy, sp_wwvb, sp_dcf, sp_bsf, sp_msf, sp_bpc };
int8_t *secpattern;
void (*st_makebits[])(void) = { mb_jjy, mb_jjy, mb_wwvb, mb_dcf, mb_bsf, mb_msf, mb_bpc };
void (*makebitpattern)(void);
const char *stationNames[] = {
  "JJY_E (40 kHz)",
  "JJY_W (60 kHz)",
  "WWVB (60 kHz)",
  "DCF77 (77.5 kHz)",
  "BSF (77.5 kHz)",
  "MSF (60 kHz)",
  "BPC (68.5 kHz)"
};
const char stationCmds[] = { 'j', 'k', 'w', 'd', 't', 'm', 'c' };

//...................................................................
// globals
hw_timer_t *tm0 = NULL;
volatile SemaphoreHandle_t timerSemaphore;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t buzzup = 0;  // inc if buzz cycle (/2) passed
int istimerstarted = 0;

int radioc = 0;  // 0..(RADIODIV - 1)
int ampc = 0;    // 0..(AMPDIV - 1)
int tssec = 0;   // 0..(SSECDIV - 1)

// Radio output GPIO (runtime configurable)
volatile int pinRadio = PIN_RADIO;

int currentStation = SN_DEFAULT;

int ntpsync = 1;
time_t now;
struct tm nowtm;
//int tsec, tmin, thour,    // initial values for date/time
//    tday, tmon, tyear; // tyear: lower 2 digits of 20xx
//int tdoy, tdow; // day of year (1-365/366), day of week (0-6)
int radioout = 0,  // pin output values
  buzzout = 0;
int ampmod;                        // 1 if radio out is active (vibrating), 0 if reducted,
                                   // at cuttent subsecond-second frame for current date-time
int buzzsw = 1;                    // sound on/off
volatile int overrideCarrier = 0;  // 1: force continuous carrier for self-test
volatile uint32_t measCount = 0;   // edge counter for self-test
int wwvbEncodeNextMinute = 0;      // 1: encode next minute (WWVB compatibility)
int dstOverride = 2;               // 0=standard,1=DST,2=auto(nowtm.tm_isdst)
int wwvbPendingOverride = 2;       // 0=off,1=on,2=auto(US DST 2h window)
Preferences prefs;
// Last command response to surface in web UI (optional detail)
String lastCmdResp;
int ledEnabled = 1;                // 0=off, 1=on (M5 LED or PIN_LED)
static int lastLedState = -1;      // -1 unknown, 0 off, 1 on, -2 disabled

// LEDC carrier generation (single output pin)
static const ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t LEDC_TIMERIDX = LEDC_TIMER_0;
static const ledc_channel_t LEDC_CHANNEL_ID = LEDC_CHANNEL_0;
static const int LEDC_RES_BITS = 8;  // 8-bit duty resolution
static uint32_t carrierHz = 60000;   // updated per station
static int dutyHigh = 128;           // ~50% duty (max fundamental)
static int dutyLow = 14;             // ~5.5% duty (~17% amplitude)

// esp_timer for 100ms aligned ticks
static esp_timer_handle_t tick_timer = NULL;
static esp_timer_handle_t align_timer = NULL;
static volatile int secBoundaryFlag = 0;  // set at each second boundary
static volatile int tickFlag = 0;         // set each 100ms tick
static volatile int currentSecond = 0;    // updated at second boundary

// WWVB minute bits prepared like txtempus (bit 59 is first second)
static uint64_t wwvb_bits = 0;
static time_t wwvb_minute_prepared = (time_t)-1;

// extern
void IRAM_ATTR onTimer(void);
void setup(void);
void loop(void);
void starttimer(void);
void stoptimer(void);
void ampchange(void);
void setstation(int station);
void binarize(int v, int pos, int len);
void bcdize(int v, int pos, int len);
void rbinarize(int v, int pos, int len);
void rbcdize(int v, int pos, int len);
void quadize(int v, int pos, int len);
int parity(int pos, int len);
int qparity(int pos, int len);
void setlocaltime(void);
void getlocaltime(void);
//void setdoydow(void);
//int julian(int y, int m, int d);
//void incday(void); // for DCF77
int docmd(char *buf);
int a2toi(char *chp);
void printbits60(void);
void ntpstart(void);
void ntpstop(void);
int isSafeAtomPin(int p);
void printhelp(void);
void IRAM_ATTR meas_isr(void);
int isUSDSTPending2h(void);
int getDSTFlag(void);
int getWWVBPendFlag(void);

// LEDC/timer helpers
void setupCarrierLEDC(void);
void setCarrierPowerLevel(int highPower);
void scheduleAlignedTicks(void);
void IRAM_ATTR onAlignedStart(void *arg);
void IRAM_ATTR onTick100ms(void *arg);
void prepareWWVBMinuteBits(time_t minuteStartUTC);
  // Web server helpers
  void setupWebServer(void);
  String generateStatusText(void);
  void handleRoot(void);
  void handleStatus(void);
  void handleCmd(void);
  void saveSettings(void);
  void loadSettings(void);
  void applyDefaultSettings(void);

//...................................................................
// intr handler:
//   this routine is called once every 1/2f sec (where f is radio freq).
//   - reverse radio output pin if modulation flag "ampmod" == 1,
//   - count up "radioc", if exceeds "radiodev" then
//     turn on buzzer flag "buzzup"; the "buzzup" is set once in 1/1000sec
//     on every frequency of the station (so "radiodev" should be set
//     propery depending to the intr cycle "tm0cycle" of the station).
void IRAM_ATTR onTimer(void) {
  // no-op: LEDC-based carrier; ISR not used
  return;
}

//...................................................................
void setup(void) {

  Serial.begin(115200);
  delay(100);
  LOG_PRINT("started...\n");

  // Initialize M5 Atom Lite (Serial, no I2C, Display/LED enabled)
  M5.begin(true, false, true);
  // Ensure onboard RGB LED is off initially
  M5.dis.drawpix(0, 0x000000);

  LOG_PRINTLN("\nUSB serial control ready (115200 bps). Type 'h' + Enter for help.");

  // Bluetooth removed; HTTP server provides control/status.

  // Load persisted settings before initializing subsystems
  loadSettings();
  if (ntpsync) {
    ntpstart();
  }
  // Start HTTP control/status server
  setupWebServer();
  // Prepare radio pin
  pinMode(PIN_MEAS, INPUT_PULLDOWN);
  pinMode(pinRadio, OUTPUT);
  digitalWrite(pinRadio, LOW);
  // Max drive for stronger field with short wire antenna
  gpio_set_drive_capability((gpio_num_t)pinRadio, GPIO_DRIVE_CAP_3);
  if (PIN_BUZZ >= 0) {
    pinMode(PIN_BUZZ, OUTPUT);
    digitalWrite(PIN_BUZZ, buzzout);
  }
  if (PIN_LED >= 0) {
    if (ledEnabled) {
      pinMode(PIN_LED, OUTPUT);
      digitalWrite(PIN_LED, LOW);
      lastLedState = 0;
    } else {
      pinMode(PIN_LED, INPUT);  // tri-state to avoid any drive
      lastLedState = -2;
    }
  } else {
    if (ledEnabled) {
      // leave off initially; will blink based on ampmod in loop()
      M5.dis.drawpix(0, 0x000000);
      lastLedState = 0;
    } else {
      // ensure fully off and no further updates in loop when disabled
      M5.dis.drawpix(0, 0x000000);
      lastLedState = -2;
    }
  }
  //  setdoydow();
  setstation(currentStation);  // start LEDC carrier + aligned ticks
  LOG_PRINT("radio started.\n");
}


void loop() {
  int buzzup2 = 0;  // legacy
  static char buf[128];
  static int bufp = 0;
  static char usb[128];
  static int usbp = 0;

  // Update M5 state (button, etc.)
  M5.update();

  // Aligned second boundary
  if (secBoundaryFlag) {
    secBoundaryFlag = 0;
    int lastmin = nowtm.tm_min;
    getlocaltime();
    currentSecond = nowtm.tm_sec;
    if (lastmin != nowtm.tm_min) {
      if (currentStation != SN_WWVB) {
        makebitpattern();
        printbits60();
      }
    }
    if (currentStation == SN_WWVB) {
      time_t tnow;
      time(&tnow);
      time_t minuteStartUTC = tnow - (tnow % 60);
      if (wwvb_minute_prepared != minuteStartUTC) {
        prepareWWVBMinuteBits(minuteStartUTC);
        wwvb_minute_prepared = minuteStartUTC;
      }
    }
    LOG_PRINTF("%d-%d-%d, %d(%d) %02d:%02d:%02d\n",
               nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday,
               nowtm.tm_yday, nowtm.tm_wday,
               nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec);
  }

  // 100ms tick: drive amplitude
  if (tickFlag) {
    tickFlag = 0;
    int highPower = 1;
    if (overrideCarrier) {
      highPower = 1;
    } else if (currentStation == SN_WWVB) {
      int sec = currentSecond % 60;
      int lowTicks;
      if (sec == 0 || (sec % 10) == 9) {
        lowTicks = 8;  // marker
      } else {
        int bit = (int)((wwvb_bits >> (59 - sec)) & 1ULL);
        lowTicks = bit ? 5 : 2;
      }
      highPower = (tssec >= lowTicks) ? 1 : 0;
    } else {
      highPower = secpattern[bits60[nowtm.tm_sec] * 10 + tssec] ? 1 : 0;
    }
    ampmod = highPower;
    setCarrierPowerLevel(ampmod);
    if (ledEnabled) {
      int desired = ampmod ? 1 : 0;
      if (PIN_LED >= 0) {
        if (lastLedState != desired) {
          // ensure output mode when enabled
          pinMode(PIN_LED, OUTPUT);
          digitalWrite(PIN_LED, desired ? HIGH : LOW);
          lastLedState = desired;
        }
      } else {
        if (lastLedState != desired) {
          M5.dis.drawpix(0, desired ? 0x10ff10 : 0x000000);
          lastLedState = desired;
        }
      }
    } else {
      // Disable LED hardware completely; do this once
      if (lastLedState != -2) {
        if (PIN_LED >= 0) {
          pinMode(PIN_LED, INPUT);
        } else {
          M5.dis.drawpix(0, 0x000000);
        }
        lastLedState = -2;
      }
    }
    Serial.print(ampmod ? "~" : ".");
  }

  // Bluetooth removed; commands accepted via USB serial and HTTP
  while (Serial.available()) {
    usb[usbp] = Serial.read();
    if (usb[usbp] == '\n' || usb[usbp] == '\r' || usbp == sizeof(usb) - 1) {
      usb[usbp] = '\0';
      docmd(usb);
      usbp = 0;
    } else {
      usbp++;
    }
  }
  // Handle HTTP requests quickly; non-blocking
  server.handleClient();
  delay(1);  // feed watchdog
}


//...................................................................
void starttimer(void) {
  if (istimerstarted) {
    stoptimer();
  }
  setupCarrierLEDC();
  scheduleAlignedTicks();
  LOG_PRINT("(re)started LEDC carrier + tick scheduler...\n");
  istimerstarted = 1;
  return;
}

void stoptimer(void) {
  if (align_timer) {
    esp_timer_stop(align_timer);
  }
  if (tick_timer) {
    esp_timer_stop(tick_timer);
  }
  // Turn off carrier
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_ID, 0);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_ID);
  istimerstarted = 0;
  return;
}

// setup amplitude value ampmod depends on bit pattern & 0.1 second frame
void ampchange(void) {
  // handled in loop() by LEDC amplitude update
  return;
}


void setstation(int station) {
  stoptimer();
  LOG_PRINTF("station #%d:\n", station);
  currentStation = station;
  tm0cycle = 80000 / st_cycle2[station];
  radiodiv = st_cycle2[station];
  LOG_PRINTF("  freq %fMHz, timer intr: 80M / (%d x %d), buzz/radio: /%d\n",
             (float)radiodiv / 2., tm0cycle, TM0RES, radiodiv);
  bits60 = st_bits[station];
  LOG_PRINTF("  bits60 pattern: ");
  for (int i = 0; i < 60; i++) {
    LOG_PRINTF("%d", (int)bits60[i]);
  }
  secpattern = st_sp[station];
  LOG_PRINTF("\n  second pattern: ");
  for (int i = 0; i < 10; i++) {
    LOG_PRINTF("%d", (int)secpattern[i]);
  }
  LOG_PRINTF("...\n");
  makebitpattern = st_makebits[station];
  makebitpattern();
  printbits60();
  starttimer();
  return;
}

//...................................................................
// makeup bit pattern for current date, hour:min

void mbc_wwvbjjy(void)  //--- [0..33] are common in WWVB/JJY
{
  binarize(nowtm.tm_min / 10, 1, 3);
  binarize(nowtm.tm_min % 10, 5, 4);
  binarize(nowtm.tm_hour / 10, 12, 2);
  binarize(nowtm.tm_hour % 10, 15, 4);
  int y100 = nowtm.tm_yday / 100;
  int y1 = (nowtm.tm_yday - y100 * 100);
  int y10 = y1 / 10;
  y1 = y1 % 10;
  binarize(y100, 22, 2);
  binarize(y10, 25, 4);
  binarize(y1, 30, 4);
  //  Serial.printf("min%d-%d hour%d-%d doy%d-%d-%d ", tmin / 10, tmin % 10, thour / 10, thour % 10,
  //    y100, y10, y1);
  return;
}

void mb_jjy(void)  //---- JJY_E & JJY_W
{
  LOG_PRINT("encode JJY format - ");
  mbc_wwvbjjy();
  bits60[36] = parity(12, 7);
  bits60[37] = parity(1, 8);
  //  Serial.printf("pa2%d ", s % 2);
  binarize((nowtm.tm_year - 100) / 10, 41, 4);
  binarize(nowtm.tm_year % 10, 45, 4);
  binarize(nowtm.tm_wday, 50, 3);
  //  Serial.printf("year%d-%d dow%d\n", tyear / 10, tyear % 10, tdow);
  return;
}

void mb_wwvb(void) {
  LOG_PRINT("encode WWVB format - ");
  // For display/debug only; actual modulation uses wwvb_bits
  time_t nowtime;
  time(&nowtime);
  struct tm *utm = gmtime(&nowtime);
  struct tm ut = *utm;
  // minutes
  binarize(ut.tm_min / 10, 1, 3);
  binarize(ut.tm_min % 10, 5, 4);
  // hours
  binarize(ut.tm_hour / 10, 12, 2);
  binarize(ut.tm_hour % 10, 15, 4);
  // day of year (1..366)
  int yday1 = ut.tm_yday + 1;
  binarize((yday1 / 100) % 10, 22, 2);
  binarize((yday1 / 10) % 10, 25, 4);
  binarize(yday1 % 10, 30, 4);
  // year (00..99)
  binarize((ut.tm_year) / 10, 45, 4);
  binarize((ut.tm_year) % 10, 50, 4);
  // local DST state (approximate; not used for modulation)
  bits60[57] = nowtm.tm_isdst ? SP_1 : SP_0;
  bits60[58] = isUSDSTPending2h();
  return;
}

void mb_dcf(void)  //---- DCF77
{
  LOG_PRINT("encode DCF77 format - ");
  //  bits60[0] = SP_0; // (obsolate) this routine is used also by mb_hbg() which changes bits60[0]
  // Seasonal flags (Z1/Z2)
  if (getDSTFlag()) {
    bits60[17] = SP_1;  // Z1 = 1
    bits60[18] = SP_0;  // Z2 = 0 (DST)
  } else {
    bits60[17] = SP_0;  // Z1 = 0
    bits60[18] = SP_1;  // Z2 = 1 (standard)
  }
  rbcdize(nowtm.tm_min, 21, 7);
  bits60[28] = parity(21, 7);
  rbcdize(nowtm.tm_hour, 29, 6);
  bits60[35] = parity(29, 6);
  rbcdize(nowtm.tm_mday, 36, 6);
  rbinarize(nowtm.tm_wday, 42, 3);
  rbcdize(nowtm.tm_mon + 1, 45, 5);
  rbcdize(nowtm.tm_year - 100, 50, 8);
  bits60[58] = parity(36, 22);
  return;
}

/*
void
mb_hbg(void)   //---- HBG
{
    mb_dcf();
    if (tmin != 0) {
      bits60[0] = SP_M0;
    } else {
      if (thour % 12 != 0) {
        bits60[0] = SP_M00;
      } else {
        bits60[0] = SP_M000;
      }
    }
    return;
}
*/

void mb_bsf(void)  //---- BSF
{
  LOG_PRINT("encode BSF format - ");
  quadize(nowtm.tm_min, 41, 3);
  quadize(nowtm.tm_hour * 2, 44, 3);
  bits60[46] |= qparity(41, 6);
  quadize(nowtm.tm_mday * 2, 47, 3);
  bits60[49] |= nowtm.tm_wday / 4;
  quadize(nowtm.tm_wday % 4, 50, 1);
  quadize(nowtm.tm_mon + 1, 51, 2);
  quadize((nowtm.tm_year - 100) * 2, 53, 4);
  bits60[56] |= qparity(47, 10);
  return;
}

void mb_msf(void) {
  LOG_PRINT("encode MSF format - ");
  bcdize(nowtm.tm_year - 100, 17, 8);
  bcdize(nowtm.tm_mon + 1, 25, 5);
  bcdize(nowtm.tm_mday, 30, 6);
  binarize(nowtm.tm_wday, 36, 3);
  bcdize(nowtm.tm_hour, 39, 6);
  bcdize(nowtm.tm_min, 45, 7);
  bits60[54] = parity(17, 8) * 2 + 1;   // in MSF parity bits, values are {1, 3} (SP_1, SP_P1)
  bits60[55] = parity(25, 11) * 2 + 1;  // for parity {0, 1}.
  bits60[56] = parity(36, 3) * 2 + 1;
  bits60[57] = parity(39, 13) * 2 + 1;
  // Set summertime indicator using DST override
  bits60[58] = getDSTFlag() ? SP_P1 : SP_1;

  return;
}

void mb_bpc(void) {
  LOG_PRINT("encode BPC format - ");
  quadize(nowtm.tm_hour % 12, 3, 2);
  quadize(nowtm.tm_min, 5, 3);
  quadize(nowtm.tm_wday, 8, 2);
  bits60[10] = (nowtm.tm_hour / 12) * 2 + qparity(3, 7);
  quadize(nowtm.tm_mday, 11, 3);
  quadize(nowtm.tm_mon + 1, 14, 2);
  quadize(nowtm.tm_year - 100, 16, 3);
  bits60[19] = qparity(11, 8);
  for (int i = 2; i < 20; i++) {
    bits60[20 + i] = bits60[i];
    bits60[40 + i] = bits60[i];
  }
  return;
}


// write binary value into bit pattern (little endian)
void binarize(int v, int pos, int len) {
  for (pos = pos + len - 1; 0 < len; pos--, len--) {
    bits60[pos] = (uint8_t)(v & 1);
    v >>= 1;
  }
  return;
}

void IRAM_ATTR meas_isr(void) {
  measCount++;
}

// -- LEDC carrier helpers -------------------------------------------------

void setupCarrierLEDC(void) {
  // Configure LEDC timer and channel for the selected frequency (ESP-IDF)
  ledc_timer_config_t tcfg = {};
  tcfg.speed_mode = LEDC_MODE;
  tcfg.duty_resolution = (ledc_timer_bit_t)LEDC_RES_BITS;
  tcfg.timer_num = LEDC_TIMERIDX;
  tcfg.freq_hz = carrierHz;
  tcfg.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&tcfg);

  ledc_channel_config_t c = {};
  c.gpio_num = pinRadio;
  c.speed_mode = LEDC_MODE;
  c.channel = LEDC_CHANNEL_ID;
  c.intr_type = LEDC_INTR_DISABLE;
  c.timer_sel = LEDC_TIMERIDX;
  c.duty = dutyHigh;
  c.hpoint = 0;
  ledc_channel_config(&c);
}

void setCarrierPowerLevel(int highPower) {
  uint32_t d = highPower ? (uint32_t)dutyHigh : (uint32_t)dutyLow;
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_ID, d);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_ID);
}

// -- esp_timer aligned tick scheduler ------------------------------------

void IRAM_ATTR onTick100ms(void *arg) {
  int t = tssec + 1;
  if (t >= 10) {
    t = 0;
    secBoundaryFlag = 1;
  }
  tssec = t;
  tickFlag = 1;
}

void IRAM_ATTR onAlignedStart(void *arg) {
  // Start periodic 100ms timer exactly at second boundary
  if (!tick_timer) {
    esp_timer_create_args_t targs = {};
    targs.callback = &onTick100ms;
    targs.arg = NULL;
    targs.dispatch_method = ESP_TIMER_TASK;
    targs.name = "tick100ms";
    esp_timer_create(&targs, &tick_timer);
  }
  tssec = 0;
  secBoundaryFlag = 1;
  tickFlag = 1;
  esp_timer_start_periodic(tick_timer, 100000);  // 100 ms
}

void scheduleAlignedTicks(void) {
  // Create or restart a one-shot timer to fire at the next wall-clock second
  if (!align_timer) {
    esp_timer_create_args_t aargs = {};
    aargs.callback = &onAlignedStart;
    aargs.arg = NULL;
    aargs.dispatch_method = ESP_TIMER_TASK;
    aargs.name = "alignStart";
    esp_timer_create(&aargs, &align_timer);
  }
  struct timeval tv;
  gettimeofday(&tv, NULL);
  int64_t us_to_next = 1000000 - tv.tv_usec;
  if (us_to_next <= 0) us_to_next = 1;
  esp_timer_start_once(align_timer, (uint64_t)us_to_next);
}

// -- WWVB minute bits exactly like txtempus ------------------------------

static uint64_t to_padded5_bcd(int n) {
  return (uint64_t)(((n / 100) % 10) << 10) | (uint64_t)(((n / 10) % 10) << 5) | (uint64_t)(n % 10);
}

static int is_leap_year_full(int year) {
  return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

void prepareWWVBMinuteBits(time_t minuteStartUTC) {
  struct tm ut;
  gmtime_r(&minuteStartUTC, &ut);

  // Build 60-bit frame, bit 59 is first second of the minute
  uint64_t bits = 0ULL;
  bits |= to_padded5_bcd(ut.tm_min) << (59 - 8);
  bits |= to_padded5_bcd(ut.tm_hour) << (59 - 18);
  bits |= to_padded5_bcd(ut.tm_yday + 1) << (59 - 33);
  bits |= to_padded5_bcd(ut.tm_year % 100) << (59 - 53);
  bits |= (uint64_t)(is_leap_year_full(ut.tm_year + 1900) ? 1 : 0) << (59 - 55);

  // Local DST flags: tomorrow and now, like txtempus
  struct tm lt_now, lt_tom;
  time_t t_now_local = minuteStartUTC;
  localtime_r(&t_now_local, &lt_now);
  time_t t_tom_local = minuteStartUTC + 86400;
  localtime_r(&t_tom_local, &lt_tom);
  bits |= (uint64_t)(lt_tom.tm_isdst ? 1 : 0) << (59 - 57);
  bits |= (uint64_t)(lt_now.tm_isdst ? 1 : 0) << (59 - 58);

  wwvb_bits = bits;
}

// Return 1 if a US DST switch occurs within the next 2 hours (local time)
int isUSDSTPending2h(void) {
  // current local time_t from nowtm
  struct tm nowLocal = nowtm;
  time_t nowt = mktime(&nowLocal);
  int year = nowLocal.tm_year;  // years since 1900

  auto firstSunday = [](int year1900, int mon) -> int {
    struct tm t = { 0 };
    t.tm_year = year1900;
    t.tm_mon = mon;
    t.tm_mday = 1;
    t.tm_hour = 0;
    t.tm_isdst = -1;
    mktime(&t);
    int dow = t.tm_wday;  // 0..6, 0=Sunday
    return 1 + ((7 - dow) % 7);
  };

  auto secondSunday = [&](int year1900, int mon) -> int {
    int d1 = firstSunday(year1900, mon);
    return d1 + 7;
  };

  // DST start: second Sunday in March, 02:00 local
  struct tm spring = { 0 };
  spring.tm_year = year;
  spring.tm_mon = 2;  // March
  spring.tm_mday = secondSunday(year, 2);
  spring.tm_hour = 2;
  spring.tm_isdst = -1;
  time_t tSpring = mktime(&spring);

  // DST end: first Sunday in November, 02:00 local
  struct tm fall = { 0 };
  fall.tm_year = year;
  fall.tm_mon = 10;  // November
  fall.tm_mday = firstSunday(year, 10);
  fall.tm_hour = 2;
  fall.tm_isdst = -1;
  time_t tFall = mktime(&fall);

  time_t tx;
  if (nowLocal.tm_isdst > 0) {  // currently DST → next is fall
    if (nowt < tFall) {
      tx = tFall;
    } else {
      // compute next year's fall
      fall.tm_year = year + 1;
      fall.tm_mday = firstSunday(year + 1, 10);
      tx = mktime(&fall);
    }
  } else {  // currently standard → next is spring
    if (nowt < tSpring) {
      tx = tSpring;
    } else {
      // next year's spring
      spring.tm_year = year + 1;
      spring.tm_mday = secondSunday(year + 1, 2);
      tx = mktime(&spring);
    }
  }

  double dt = difftime(tx, nowt);
  return (dt >= 0 && dt <= 7200) ? 1 : 0;
}

int getDSTFlag(void) {
  if (dstOverride == 2) {
    return (nowtm.tm_isdst > 0) ? 1 : 0;
  }
  return dstOverride ? 1 : 0;
}

int getWWVBPendFlag(void) {
  if (wwvbPendingOverride == 2) {
    return isUSDSTPending2h();
  }
  return wwvbPendingOverride ? 1 : 0;
}

// continuous (over 4 bit) BCD to write
void bcdize(int v, int pos, int len) {
  int l;

  pos = pos + len - 1;
  while (0 < len) {
    if (4 <= len) {
      l = 4;
    } else {
      l = len;
    }
    binarize(v % 10, pos - l + 1, l);
    v = v / 10;
    pos = pos - l;
    len = len - l;
  }
  return;
}

// LSB->MSB (big endian) binarize
void rbinarize(int v, int pos, int len) {
  for (; 0 < len; pos++, len--) {
    bits60[pos] = (uint8_t)(v & 1);
    v >>= 1;
  }
  return;
}

// LSB->MSB BCDize
void rbcdize(int v, int pos, int len) {
  int l;

  LOG_PRINTF("\nrbcd %d[pos %d, len%d]=", v, pos, len);
  while (0 < len) {
    if (4 <= len) {
      l = 4;
    } else {
      l = len;
    }
    rbinarize(v % 10, pos, l);
    v = v / 10;
    pos = pos + l;
    len = len - l;
  }
  return;
}


// 4-ary encoding (little endian)
void quadize(int v, int pos, int len) {
  for (pos = pos + len - 1; 0 < len; pos--, len--) {
    bits60[pos] = (uint8_t)(v & 3);
    v >>= 2;
  }
  return;
}

// calculate even parity
int parity(int pos, int len) {
  int s = 0;

  for (pos; 0 < len; pos++, len--) {
    s += bits60[pos];
  }
  return (s % 2);
}

// binary parity for 4-ary data (for BSF/BPC): is it OK?
int qparity(int pos, int len) {
  int s = 0;

  for (pos; 0 < len; pos++, len--) {
    s += (bits60[pos] & 1) + ((bits60[pos] & 2) >> 1);
  }
  return (s % 2);
}


//// calculate doy (day of year)/dow (day of week) from YY/MM/DD
//void
//setdoydow(void)
//{
//  int j0 = julian(tyear, 1, 1);      // new year day of this year
//  int j1 = julian(tyear, tmon, tday);
//  tdoy = j1 - j0 + 1; // 1..365/366
//  tdow = j1 % 7;      // 0..6
//  return;
//}
//
//// return julian date (? relative date from a day)
//// sunday is multiple of 7
//int
//julian(int y, int m, int d)
//{
//  if (m <= 2) {
//    m = m + 12;
//    y--;
//  }
//  return y * 1461 / 4 + (m + 1) * 153 / 5 + d + 6;
////  1461 / 4 == 365.25, 153 / 5 == 30.6
//}
//
//// increment tday-tmon-tyear, tdoy, tdow
//void
//incday(void)
//{
//  int year1 = tyear;   // year of next month
//  int mon1 = tmon + 1; // next month
//  if (12 < mon1) {
//    mon1 = 1;
//    year1++;
//  }
//  int day1 = tday + 1; // date# of tomorrow
//  if (julian(year1, mon1, 1) - julian(tyear, tmon, 1) < day1) {
//    tday = 1;  // date# exceeds # of date in this month
//    tmon = mon1;
//    tyear = year1;
//  } else {
//    tday = day1;
//  }
//  setdoydow(); // tdoy, tdow is updated from tyear-tmonth-tday
//  return;
//}

//...................................................................
// Bluetooth command
//
// y[01]: NTP sync off/on
//   to force set the current date/time (d/t), first turn off NTP sync.
// dYYMMDD: set date to YY/MM/DD
// tHHmmSS: set time to HH:mm:SS
// z[01]: buzzer off/on
// s[jkwdhmb]: set station to JJY_E, JJY_W, WWVB, DCF77, HBG, MSF, BPC

int docmd(char *buf) {
  int arg1, arg2;
  LOG_PRINTF("cmd: >>%s<<\n", buf);
  if (buf[0] == 'd' || buf[0] == 'D') {  // set date
    if (strlen(buf) != 7) {
      return 0;
    }
    int y = a2toi(buf + 1);
    int m = a2toi(buf + 3);
    int d = a2toi(buf + 5);
    LOG_PRINTF("%d %d %d\n", y, m, d);
    if (y < 0 || m < 0 || 12 < m || d < 0 || 31 < d) {  // can set Feb 31 :-)
      return 0;
    }
    nowtm.tm_year = y + 100;
    nowtm.tm_mon = m - 1;
    nowtm.tm_mday = d;
    setlocaltime();
    LOG_PRINTF("set date: >>%s<<\n", buf + 1);
    return 1;
  } else if (buf[0] == 't' || buf[0] == 'T') {  // set time & start tick
    if (strlen(buf) != 7) {
      return 0;
    }
    int h = a2toi(buf + 1);
    int m = a2toi(buf + 3);
    int s = a2toi(buf + 5);
    if (h < 0 || 24 < h || m < 0 || 60 < m || s < 0 || 60 < s) {
      return 0;
    }
    nowtm.tm_hour = h;
    nowtm.tm_min = m;
    nowtm.tm_sec = s;
    tssec = 0;
    ampc = 0;
    radioc = 0;  // no semaphore lock: don't care if override by intr routine :-)
    setlocaltime();
    LOG_PRINTF("set time...restart tick: >>%s<<\n", buf + 1);
    return 1;
  } else if (buf[0] == 'z' || buf[0] == 'Z') {  // buzzer on(1)/off(0)
    if (buf[1] == '0') {
      buzzsw = 0;
    } else if (buf[1] == '1') {
      buzzsw = 1;
    } else {
      return 0;
    }
    LOG_PRINTF("buzzer: >>%c<<\n", buf + 1);
    saveSettings();
    return 1;
  } else if (buf[0] == 'l' || buf[0] == 'L') {  // LED on(1)/off(0)
    if (buf[1] == '0') {
      ledEnabled = 0;
    } else if (buf[1] == '1') {
      ledEnabled = 1;
    } else {
      return 0;
    }
    // Apply immediately with proper hardware state
    if (PIN_LED >= 0) {
      if (ledEnabled) {
        pinMode(PIN_LED, OUTPUT);
        digitalWrite(PIN_LED, LOW);
        lastLedState = 0;
      } else {
        pinMode(PIN_LED, INPUT);  // tri-state
        lastLedState = -2;
      }
    } else {
      if (ledEnabled) {
        M5.dis.drawpix(0, 0x000000);
        lastLedState = 0;
      } else {
        M5.dis.drawpix(0, 0x000000);
        lastLedState = -2;
      }
    }
    saveSettings();
    return 1;
  } else if (buf[0] == 's' || buf[0] == 'S') {  // set station
    char s[] =                                  //"jJkKwWdDhHmMbB"
      { 'j', 'J', 'k', 'K', 'w', 'W', 'd', 'D', 't', 'T', 'm', 'M', 'c', 'C', '\0' },
         *chp;
    if ((chp = strchr(s, buf[1])) != NULL) {
      setstation((int)(chp - s) / 2);
      saveSettings();
      return 1;
    } else {
      return 0;
    }
  } else if (buf[0] == 'p' || buf[0] == 'P') {  // set radio output pin number
    // accepts decimal gpio number, e.g., p26
    int plen = strlen(buf);
    if (plen < 2 || plen > 3) {
      return 0;
    }
    int newPin = atoi(buf + 1);
    if (!isSafeAtomPin(newPin)) {
      LOG_PRINTF("pin not allowed: %d\n", newPin);
      return 0;
    }
    int oldPin = pinRadio;
    if (oldPin == newPin) {
      return 1;
    }
    stoptimer();
    pinMode(oldPin, OUTPUT);
    digitalWrite(oldPin, LOW);
    pinRadio = newPin;
    pinMode(pinRadio, OUTPUT);
    digitalWrite(pinRadio, LOW);
    // Max drive on the new pin as well
    gpio_set_drive_capability((gpio_num_t)pinRadio, GPIO_DRIVE_CAP_3);
    starttimer();
    LOG_PRINTF("radio pin set to %d\n", pinRadio);
    saveSettings();
    return 1;
  } else if (buf[0] == 'y' || buf[0] == 'Y') {  // NTP sync
    if (buf[1] == '0') {
      ntpsync = 0;
      ntpstop();
    } else if (buf[1] == '1') {
      ntpsync = 1;
      ntpstart();
    } else {
      return 0;
    }
    saveSettings();
    return 1;
  } else if (buf[0] == 'f' || buf[0] == 'F') {  // frequency self-test (jumper radio pin to PIN_MEAS)
    pinMode(PIN_MEAS, INPUT_PULLDOWN);
    measCount = 0;
    overrideCarrier = 1;
    attachInterrupt(digitalPinToInterrupt(PIN_MEAS), meas_isr, RISING);
    delay(200);
    detachInterrupt(digitalPinToInterrupt(PIN_MEAS));
    overrideCarrier = 0;
    uint32_t hz = (uint32_t)(measCount * 5);
    if (hz == 0) {
      // Fallback: read LEDC timer freq if no edges captured
      uint32_t ledcHz = ledc_get_freq(LEDC_MODE, LEDC_TIMERIDX);
      LOG_PRINTF("no edges on GPIO%d; LEDC timer reports ~%u Hz (expected ~%d Hz)\n",
                 PIN_MEAS, ledcHz, radiodiv * 500);
      lastCmdResp = String("no edges on GPIO") + String(PIN_MEAS) + String("; LEDC ~") + String(ledcHz) + String(" Hz (expected ~") + String(radiodiv * 500) + String(" Hz)\n");
    } else {
      LOG_PRINTF("measured ~%u Hz on GPIO%d (expected ~%d Hz)\n",
                 hz, PIN_MEAS, radiodiv * 500);
      lastCmdResp = String("measured ~") + String(hz) + String(" Hz on GPIO") + String(PIN_MEAS) + String(" (expected ~") + String(radiodiv * 500) + String(" Hz)\n");
    }
    return 1;
  } else if (buf[0] == 'n' || buf[0] == 'N') {  // WWVB next-minute encoding off/on
    if (buf[1] == '0') {
      wwvbEncodeNextMinute = 0;
    } else if (buf[1] == '1') {
      wwvbEncodeNextMinute = 1;
    } else {
      return 0;
    }
    LOG_PRINTF("WWVB next-minute encoding: %s (ignored; txtempus defaults)\n", wwvbEncodeNextMinute ? "on" : "off");
    return 1;
  } else if (buf[0] == 'x' || buf[0] == 'X') {  // DST override
    if (buf[1] == '0') dstOverride = 0;         // STD
    else if (buf[1] == '1') dstOverride = 1;    // DST
    else if (buf[1] == '2') dstOverride = 2;    // AUTO
    else return 0;
    LOG_PRINTF("DST override: %s (DCF/MSF only; WWVB ignores)\n", dstOverride == 2 ? "auto" : (dstOverride ? "DST" : "STD"));
    saveSettings();
    return 1;
  } else if (buf[0] == 'q' || buf[0] == 'Q') {  // WWVB pending override
    if (buf[1] == '0') wwvbPendingOverride = 0;
    else if (buf[1] == '1') wwvbPendingOverride = 1;
    else if (buf[1] == '2') wwvbPendingOverride = 2;
    else return 0;
    LOG_PRINTF("WWVB pending override: %s (ignored; txtempus defaults)\n", wwvbPendingOverride == 2 ? "auto" : (wwvbPendingOverride ? "on" : "off"));
    saveSettings();
    return 1;
  } else if (buf[0] == 'r' || buf[0] == 'R') {  // reset to defaults and clear persisted settings
    applyDefaultSettings();
    // Clear saved prefs so next boot uses defaults
    prefs.begin("clocksync", false);
    prefs.clear();
    prefs.end();
    LOG_PRINTLN("settings reset to defaults");
    lastCmdResp = String("settings reset to defaults\n");
    return 1;
  } else if (buf[0] == 'h' || buf[0] == 'H') {  // help
    printhelp();
    return 1;
  }
  return 0;
}

int a2toi(char *chp) {
  int v = 0;
  for (int i = 0; i < 2; chp++, i++) {
    if (*chp < '0' || '9' < *chp) {
      return -1;
    }
    v = v * 10 + (*chp - '0');
  }
  return v;
}


void printbits60(void) {
  LOG_PRINT("\n");
  for (int i = 0; i < 60; i++) {
    LOG_PRINT(bits60[i]);
  }
  LOG_PRINT("\n");
  return;
}

void printhelp(void) {
  LOG_PRINTLN("Status:");
  LOG_PRINTF("  Station : %s\n", stationNames[currentStation]);
  int carrierHz = radiodiv * 500;  // radiodiv is double-freq in kHz
  LOG_PRINTF("  Carrier : %d Hz (%.1f kHz)\n", carrierHz, (float)carrierHz / 1000.0);
  LOG_PRINTF("  Pin     : GPIO %d\n", pinRadio);
  LOG_PRINTF("  NTP     : %s\n", ntpsync ? "on" : "off");
  LOG_PRINTF("  TZ      : %s\n", TZ);
  LOG_PRINTF("  Buzzer  : %s\n", buzzsw ? "on" : "off");
  LOG_PRINTF("  DST ov  : %s (applies to DCF/MSF; WWVB ignores)\n", dstOverride == 2 ? "auto" : (dstOverride ? "DST" : "STD"));
  LOG_PRINTF("  WWVB    : txtempus framing (UTC; DST bits now/tomorrow). Overrides ignored.\n");

  LOG_PRINTLN("\nCommands:");
  LOG_PRINTLN("  h           : show this help");
  LOG_PRINTLN("  y0|y1       : NTP sync off/on");
  LOG_PRINTLN("  dYYMMDD     : set date (interpreted in local TZ)");
  LOG_PRINTLN("  tHHmmSS     : set time and restart tick (local TZ)");
  LOG_PRINTLN("  z0|z1       : buzzer off/on");
  LOG_PRINTLN("  l0|l1       : LED off/on");
  LOG_PRINTLN("  pNN         : set radio output pin (e.g., p25)");
  LOG_PRINTLN("  f           : self-test: jumper radio pin to GPIO33, measure carrier");
  LOG_PRINTLN("  n0|n1       : (WWVB) legacy; ignored (txtempus defaults)");
  LOG_PRINTLN("  x0|x1|x2    : force DST STD/DST/AUTO (DCF/MSF only; WWVB ignores)");
  LOG_PRINTLN("  q0|q1|q2    : (WWVB) legacy; ignored (txtempus defaults)");
  LOG_PRINTLN("  sX          : set station to X (one of):");
  for (int i = 0; i < 7; i++) {
    LOG_PRINTF("    s%c : %s\n", stationCmds[i], stationNames[i]);
  }
}


// ------------------------- Web server ---------------------------------
String generateStatusText(void) {
  String s;
  s.reserve(512);
  // Capture current time at generation
  getlocaltime();
  s += "Status:\n";
  s += "  Station : "; s += stationNames[currentStation]; s += "\n";
  int chz = radiodiv * 500;
  s += "  Carrier : "; s += String(chz); s += " Hz ("; s += String((float)chz / 1000.0f, 1); s += " kHz)\n";
  s += "  Pin     : GPIO "; s += String(pinRadio); s += "\n";
  s += "  NTP     : "; s += (ntpsync ? "on" : "off"); s += "\n";
  s += "  TZ      : "; s += TZ; s += "\n";
  s += "  Buzzer  : "; s += (buzzsw ? "on" : "off"); s += "\n";
  s += "  LED     : "; s += (ledEnabled ? "on" : "off"); s += "\n";
  s += "  DST ov  : "; s += (dstOverride==2?"auto":(dstOverride?"DST":"STD")); s += " (applies to DCF/MSF; WWVB ignores)\n";
  s += "  WWVB    : txtempus framing (UTC; DST bits now/tomorrow). Overrides ignored.\n";
  char tbuf[48];
  snprintf(tbuf, sizeof(tbuf), "  Now     : %04d-%02d-%02d %02d:%02d:%02d (local)\n",
           nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday,
           nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec);
  s += tbuf;
  s += "\nCommands:\n";
  s += "  h           : show this help\n";
  s += "  y0|y1       : NTP sync off/on\n";
  s += "  dYYMMDD     : set date (interpreted in local TZ)\n";
  s += "  tHHmmSS     : set time and restart tick (local TZ)\n";
  s += "  z0|z1       : buzzer off/on\n";
  s += "  l0|l1       : LED off/on\n";
  s += "  pNN         : set radio output pin (e.g., p25)\n";
  s += "  f           : self-test: jumper radio pin to GPIO33, measure carrier\n";
  s += "  n0|n1       : (WWVB) legacy; ignored (txtempus defaults)\n";
  s += "  x0|x1|x2    : force DST STD/DST/AUTO (DCF/MSF only; WWVB ignores)\n";
  s += "  q0|q1|q2    : (WWVB) legacy; ignored (txtempus defaults)\n";
  s += "  sX          : set station to X (one of):\n";
  for (int i = 0; i < 7; i++) {
    s += "    s"; s += stationCmds[i]; s += " : "; s += stationNames[i]; s += "\n";
  }
  return s;
}

void handleStatus(void) {
  String s = generateStatusText();
  server.send(200, "text/plain", s);
}

void handleCmd(void) {
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "missing c param\n");
    return;
  }
  String arg = server.arg("c");
  if (arg.length() == 0 || arg.length() >= 127) {
    server.send(400, "text/plain", "bad c param\n");
    return;
  }
  char buf[128];
  memcpy(buf, arg.c_str(), arg.length());
  buf[arg.length()] = '\0';
  int ok = docmd(buf);
  String s;
  if (ok) {
    s = String("OK\n");
    if (lastCmdResp.length() > 0) {
      s += lastCmdResp;
      lastCmdResp = String();
    }
  } else {
    s = String("ERR\n");
  }
  server.send(200, "text/plain", s);
}

void handleRoot(void) {
  String s;
  s.reserve(1024);
  s += "<!doctype html><meta charset=\"utf-8\"><title>"; s += DEVICENAME; s += "</title>";
  s += "<style>body{font-family:monospace;white-space:pre-wrap;margin:16px;}input,button{font-family:monospace;}#msg{margin-left:8px;color:#080}</style>";
  s += "<h3>"; s += DEVICENAME; s += "</h3>";
  s += "<div><form id=\"cmdform\">cmd: <input id=\"cmd\" name=\"c\" size=\"20\" autocomplete=\"off\"><button type=\"submit\">Send</button><span id=\"msg\"></span></form></div>";
  s += "<hr><div><a href=\"/status.txt\" target=\"_blank\">status.txt</a></div><pre id=\"status\">";
  s += generateStatusText();
  s += "</pre><script>(function(){\n";
  s += "function fetchStatus(){return fetch('/status.txt',{cache:'no-store'}).then(r=>r.text()).then(t=>{document.getElementById('status').textContent=t;});}\n";
  s += "document.getElementById('cmdform').addEventListener('submit',function(ev){ev.preventDefault();var i=document.getElementById('cmd');var m=document.getElementById('msg');m.textContent='';var v=i.value.trim();if(!v){return;}fetch('/cmd?c='+encodeURIComponent(v)).then(r=>r.text()).then(t=>{m.style.color=(t.indexOf('OK')===0)?'#080':'#a00';m.textContent=t.trim();i.select();return fetchStatus();}).catch(()=>{m.style.color='#a00';m.textContent='ERR';});});\n";
  // No periodic refresh to minimize WiFi usage
  s += "})();</script>";
  server.send(200, "text/html", s);
}

void setupWebServer(void) {
  server.on("/", handleRoot);
  server.on("/status.txt", HTTP_GET, handleStatus);
  server.on("/cmd", HTTP_GET, handleCmd);
  server.onNotFound([](){ server.send(404, "text/plain", "not found\n"); });
  server.begin();
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(DEVICENAME)) {
      MDNS.addService("http", "tcp", 80);
      LOG_PRINTF("HTTP server: http://%s.local/\n", DEVICENAME);
    } else {
      LOG_PRINTLN("mDNS start failed");
    }
  } else {
    LOG_PRINTLN("HTTP server started (WiFi not connected yet)");
  }
}


void ntpstart(void) {
  int i;

  // WiFi, NTP setup
  LOG_PRINT("Attempting to connect to Network named: ");
  LOG_PRINTLN(ssid);  // print the network name (SSID);
  WiFi.begin(ssid, passwd);
  for (i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) {
    LOG_PRINT(".");
    delay(1000);
  }
  if (i == 10) {
    ntpsync = 0;
    return;
  }
  IPAddress ip = WiFi.localIP();
  LOG_PRINTF("IP Address: ");
  LOG_PRINTLN(ip);
  LOG_PRINTF("configuring NTP timezone...");
  // Use POSIX TZ string for automatic DST handling (see TZ macro)
  configTzTime(TZ, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 10 && !getLocalTime(&nowtm); i++) {
    LOG_PRINTF(".");
    delay(1000);
  }
  if (i == 10) {
    ntpsync = 0;
    return;
  }
  LOG_PRINTF("done\n");
}


void ntpstop(void) {
  ntpsync = 0;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}


void setlocaltime(void) {
  time_t nowtime = mktime(&nowtm) /*+ TZ */;
  struct timeval tv = {
    .tv_sec = nowtime
  };
  settimeofday(&tv, NULL);
  getlocaltime();  // to make wday/yday
}


void getlocaltime(void) {
  if (ntpsync) {
    getLocalTime(&nowtm);
  } else {
    time_t nowtime;
    time(&nowtime);
    struct tm *ntm;
    ntm = localtime(&nowtime);
    nowtm = *ntm;
  }
}

// Allowed radio output pins on Atom Lite (avoid 12, 27, 39, bootstraps)
int isSafeAtomPin(int p) {
  // Common breakouts and HY2.0
  int allowed[] = { 19, 21, 22, 23, 25, 26, 32, 33 };
  for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
    if (p == allowed[i]) {
      return 1;
    }
  }
  return 0;
}

// ------------------------- Persistence (Preferences) -------------------------
void saveSettings(void) {
  prefs.begin("clocksync", false);
  prefs.putInt("station", currentStation);
  prefs.putInt("pin", pinRadio);
  prefs.putInt("ntp", ntpsync);
  prefs.putInt("buzz", buzzsw);
  prefs.putInt("dst", dstOverride);
  prefs.putInt("wwvbPend", wwvbPendingOverride);
  prefs.putInt("led", ledEnabled);
  prefs.end();
}

void loadSettings(void) {
  prefs.begin("clocksync", true);
  int v;
  v = prefs.getInt("station", -1);
  if (v >= 0 && v <= SN_BPC) {
    currentStation = v;
  }
  v = prefs.getInt("pin", -1);
  if (v >= 0 && isSafeAtomPin(v)) {
    pinRadio = v;
  }
  v = prefs.getInt("ntp", -1);
  if (v == 0 || v == 1) {
    ntpsync = v;
  }
  v = prefs.getInt("buzz", -1);
  if (v == 0 || v == 1) {
    buzzsw = v;
  }
  v = prefs.getInt("dst", -1);
  if (v == 0 || v == 1 || v == 2) {
    dstOverride = v;
  }
  v = prefs.getInt("wwvbPend", -1);
  if (v == 0 || v == 1 || v == 2) {
    wwvbPendingOverride = v;
  }
  v = prefs.getInt("led", -1);
  if (v == 0 || v == 1) {
    ledEnabled = v;
  }
  prefs.end();
}

void applyDefaultSettings(void) {
  // Reset radio pin if changed
  int defaultPin = PIN_RADIO;
  if (pinRadio != defaultPin) {
    stoptimer();
    pinMode(pinRadio, OUTPUT);
    digitalWrite(pinRadio, LOW);
    pinRadio = defaultPin;
    pinMode(pinRadio, OUTPUT);
    digitalWrite(pinRadio, LOW);
    gpio_set_drive_capability((gpio_num_t)pinRadio, GPIO_DRIVE_CAP_3);
  }
  // Defaults
  buzzsw = 1;
  dstOverride = 2;
  wwvbPendingOverride = 2;
  // NTP default: on
  ntpstop();
  ntpsync = 1;
  ntpstart();
  // Station default
  setstation(SN_DEFAULT);
}
