// Host unit tests for timecode.cpp. Run with `make -C test`.
//
// Three kinds of checks:
//   1. Golden frames derived by hand from the protocol specs.
//   2. Round trips: independent decoders (written from the specs, not from
//      the encoder) decode thousands of encoded minutes.
//   3. BSF/BPC, which have no public spec, are compared against the v1.3
//      implementation so the port cannot drift.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>

#include "timecode.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    g_checks++;                                                       \
    if (!(cond)) {                                                    \
      g_failures++;                                                   \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                 \
  } while (0)

#define CHECK_STR(actual, expected)                                               \
  do {                                                                            \
    g_checks++;                                                                   \
    std::string a_ = (actual), e_ = (expected);                                   \
    if (a_ != e_) {                                                               \
      g_failures++;                                                               \
      printf("FAIL %s:%d:\n  got      %s\n  expected %s\n", __FILE__, __LINE__, \
             a_.c_str(), e_.c_str());                                             \
    }                                                                             \
  } while (0)

static const char *TZ_US = "PST8PDT,M3.2.0,M11.1.0";
static const char *TZ_JP = "JST-9";
static const char *TZ_DE = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char *TZ_UK = "GMT0BST,M3.5.0/1,M10.5.0";
static const char *TZ_CN = "CST-8";

static void use_tz(const char *tz) {
  setenv("TZ", tz, 1);
  tzset();
}

static time_t utc(int y, int mo, int d, int h, int mi) {
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  return timegm(&t);
}

static std::string frame_str(int station, time_t t, int dst_mode = TC_DST_AUTO) {
  uint8_t f[60];
  tc_build_minute(station, t, dst_mode, f);
  std::string s;
  for (int i = 0; i < 60; i++) {
    s += f[i] == TC_SYM_MARK ? 'M' : (char)('0' + f[i]);
  }
  return s;
}

static int sum(const uint8_t *b, int from, int to) {
  int s = 0;
  for (int i = from; i <= to; i++) s += b[i];
  return s;
}

// -- golden frames ----------------------------------------------------------

static void test_golden_jjy() {
  use_tz(TZ_JP);
  // Sunday 2026-11-01 12:34 JST, day of year 305.
  time_t t = utc(2026, 11, 1, 3, 34);
  std::string expected = std::string("M01100100M") + "000100010M" + "001100000M" +
                         "010100010M" + "000100110M" + "000000000M";
  CHECK_STR(frame_str(TC_JJY40, t), expected);
  CHECK_STR(frame_str(TC_JJY60, t), expected);
  // Any second within the minute selects the same frame.
  CHECK_STR(frame_str(TC_JJY40, t + 59), expected);
}

static void test_golden_wwvb() {
  use_tz(TZ_US);
  // 2026-11-01 08:30 UTC, the day US DST ends: DST bits 57,58 = "01".
  std::string expected = std::string("M01100000M") + "000001000M" + "001100000M" +
                         "010100101M" + "000000010M" + "011000001M";
  CHECK_STR(frame_str(TC_WWVB, utc(2026, 11, 1, 8, 30)), expected);
}

static void test_golden_dcf77() {
  use_tz(TZ_DE);
  // Sent during Sunday 2026-11-01 12:34 CET; announces 12:35.
  std::string expected = std::string("0000000000") + "0000000010" + "1101011000" +
                         "1001001000" + "0011110001" + "011001001M";
  CHECK_STR(frame_str(TC_DCF77, utc(2026, 11, 1, 11, 34)), expected);
}

static void test_golden_msf() {
  use_tz(TZ_UK);
  // Sent during Sunday 2026-11-01 12:34 GMT; announces 12:35.
  std::string expected = std::string("M000000000") + "0000000001" + "0011010001" +
                         "0000010000" + "1001001101" + "0101113310";
  CHECK_STR(frame_str(TC_MSF, utc(2026, 11, 1, 12, 34)), expected);
}

// -- WWVB DST bits ------------------------------------------------------------

static std::string wwvb_dst(time_t t, int dst_mode = TC_DST_AUTO) {
  std::string s = frame_str(TC_WWVB, t, dst_mode);
  return s.substr(57, 2);
}

static void test_wwvb_dst_bits() {
  use_tz(TZ_US);
  // 2026: DST starts Sunday March 8 and ends Sunday November 1.
  CHECK_STR(wwvb_dst(utc(2026, 1, 15, 12, 0)), "00");
  CHECK_STR(wwvb_dst(utc(2026, 3, 7, 12, 0)), "00");  // day before: still 00 all day
  CHECK_STR(wwvb_dst(utc(2026, 3, 7, 23, 59)), "00");
  CHECK_STR(wwvb_dst(utc(2026, 3, 8, 0, 0)), "10");  // begins today, from 00:00 UTC
  CHECK_STR(wwvb_dst(utc(2026, 3, 8, 9, 0)), "10");
  CHECK_STR(wwvb_dst(utc(2026, 3, 8, 23, 59)), "10");
  CHECK_STR(wwvb_dst(utc(2026, 3, 9, 0, 0)), "11");
  CHECK_STR(wwvb_dst(utc(2026, 7, 4, 12, 0)), "11");
  CHECK_STR(wwvb_dst(utc(2026, 10, 31, 12, 0)), "11");  // day before: still 11 all day
  CHECK_STR(wwvb_dst(utc(2026, 11, 1, 0, 0)), "01");    // ends today
  CHECK_STR(wwvb_dst(utc(2026, 11, 1, 23, 59)), "01");
  CHECK_STR(wwvb_dst(utc(2026, 11, 2, 0, 0)), "00");
  // Eastern time switches on the same UTC day.
  use_tz("EST5EDT,M3.2.0,M11.1.0");
  CHECK_STR(wwvb_dst(utc(2026, 3, 8, 12, 0)), "10");
  CHECK_STR(wwvb_dst(utc(2026, 11, 1, 12, 0)), "01");
  // Arizona has no DST.
  use_tz("MST7");
  CHECK_STR(wwvb_dst(utc(2026, 7, 4, 12, 0)), "00");
  // Manual overrides.
  use_tz(TZ_US);
  CHECK_STR(wwvb_dst(utc(2026, 7, 4, 12, 0), TC_DST_STANDARD), "00");
  CHECK_STR(wwvb_dst(utc(2026, 1, 4, 12, 0), TC_DST_SUMMER), "11");
}

static void test_wwvb_misc_bits() {
  use_tz(TZ_US);
  std::string s = frame_str(TC_WWVB, utc(2028, 12, 31, 23, 59));
  CHECK_STR(s.substr(36, 3), "101");  // DUT1 sign: +
  CHECK(s[55] == '1');                // 2028 is a leap year
  CHECK(frame_str(TC_WWVB, utc(2026, 6, 1, 0, 0))[55] == '0');
}

// -- DCF77 / MSF specifics ------------------------------------------------------

static void test_dcf77_flags() {
  use_tz(TZ_DE);
  // Summer: Z1=1, Z2=0.
  std::string s = frame_str(TC_DCF77, utc(2026, 7, 1, 10, 0));
  CHECK(s[17] == '1' && s[18] == '0');
  // Winter: Z1=0, Z2=1.
  s = frame_str(TC_DCF77, utc(2026, 1, 1, 10, 0));
  CHECK(s[17] == '0' && s[18] == '1');
  // A1 is set during the hour before the change (2026-03-29 01:00 UTC).
  CHECK(frame_str(TC_DCF77, utc(2026, 3, 28, 23, 58))[16] == '0');  // announces 00:59 CET
  CHECK(frame_str(TC_DCF77, utc(2026, 3, 28, 23, 59))[16] == '1');  // announces 01:00 CET
  CHECK(frame_str(TC_DCF77, utc(2026, 3, 29, 0, 58))[16] == '1');   // announces 01:59 CET
  CHECK(frame_str(TC_DCF77, utc(2026, 3, 29, 0, 59))[16] == '0');   // announces 03:00 CEST
  // Override forces the flags and suppresses A1.
  s = frame_str(TC_DCF77, utc(2026, 3, 28, 23, 59), TC_DST_SUMMER);
  CHECK(s[16] == '0' && s[17] == '1' && s[18] == '0');
}

static void test_msf_flags() {
  use_tz(TZ_UK);
  std::string s = frame_str(TC_MSF, utc(2026, 7, 1, 10, 0));
  CHECK(s[58] == '3');  // A=1, B=1: summer time
  s = frame_str(TC_MSF, utc(2026, 1, 1, 10, 0));
  CHECK(s[58] == '1');  // A=1, B=0: winter
  // 53B warns during the 61 minutes before the change at 01:00 UTC.
  CHECK(frame_str(TC_MSF, utc(2026, 3, 28, 23, 57))[53] == '1');  // announces 23:58
  CHECK(frame_str(TC_MSF, utc(2026, 3, 28, 23, 58))[53] == '3');  // announces 23:59
  CHECK(frame_str(TC_MSF, utc(2026, 3, 29, 0, 58))[53] == '3');
  CHECK(frame_str(TC_MSF, utc(2026, 3, 29, 0, 59))[53] == '1');
}

// -- round trips with independent decoders ----------------------------------

static int w(const uint8_t *b, int pos, const int *weights, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) v += b[pos + i] ? weights[i] : 0;
  return v;
}

static const int W8421[] = { 8, 4, 2, 1 };
static const int W_LSB[] = { 1, 2, 4, 8, 10, 20, 40, 80 };

static void decode_check_jjy(time_t t) {
  uint8_t f[60];
  tc_build_minute(TC_JJY40, t, TC_DST_AUTO, f);
  static const int marks[] = { 0, 9, 19, 29, 39, 49, 59 };
  for (int m : marks) CHECK(f[m] == TC_SYM_MARK);
  static const int W421[] = { 4, 2, 1 }, W21[] = { 2, 1 }, W8[] = { 80, 40, 20, 10, 8, 4, 2, 1 };
  int minute = 10 * w(f, 1, W421, 3) + w(f, 5, W8421, 4);
  int hour = 10 * w(f, 12, W21, 2) + w(f, 15, W8421, 4);
  int doy = 100 * w(f, 22, W21, 2) + 10 * w(f, 25, W8421, 4) + w(f, 30, W8421, 4);
  int year = w(f, 41, W8, 8);
  int wday = w(f, 50, W421, 3);
  CHECK(f[36] == sum(f, 12, 18) % 2);
  CHECK(f[37] == sum(f, 1, 8) % 2);
  struct tm lt;
  localtime_r(&t, &lt);
  bool ok = minute == lt.tm_min && hour == lt.tm_hour && doy == lt.tm_yday + 1 &&
            year == lt.tm_year % 100 && wday == lt.tm_wday;
  CHECK(ok);
}

static void decode_check_wwvb(time_t t) {
  uint8_t f[60];
  tc_build_minute(TC_WWVB, t, TC_DST_AUTO, f);
  static const int marks[] = { 0, 9, 19, 29, 39, 49, 59 };
  for (int m : marks) CHECK(f[m] == TC_SYM_MARK);
  static const int W421[] = { 4, 2, 1 }, W21[] = { 2, 1 };
  int minute = 10 * w(f, 1, W421, 3) + w(f, 5, W8421, 4);
  int hour = 10 * w(f, 12, W21, 2) + w(f, 15, W8421, 4);
  int doy = 100 * w(f, 22, W21, 2) + 10 * w(f, 25, W8421, 4) + w(f, 30, W8421, 4);
  int year = 10 * w(f, 45, W8421, 4) + w(f, 50, W8421, 4);
  struct tm ut;
  gmtime_r(&t, &ut);
  bool ok = minute == ut.tm_min && hour == ut.tm_hour && doy == ut.tm_yday + 1 &&
            year == ut.tm_year % 100;
  CHECK(ok);
}

static void decode_check_dcf77(time_t t) {
  uint8_t f[60];
  tc_build_minute(TC_DCF77, t, TC_DST_AUTO, f);
  CHECK(f[59] == TC_SYM_MARK);
  CHECK(f[0] == 0 && f[20] == 1);
  CHECK(f[17] != f[18]);
  int minute = w(f, 21, W_LSB, 7);
  int hour = w(f, 29, W_LSB, 6);
  int mday = w(f, 36, W_LSB, 6);
  int wday = w(f, 42, W_LSB, 3);
  int month = w(f, 45, W_LSB, 5);
  int year = w(f, 50, W_LSB, 8);
  CHECK(sum(f, 21, 28) % 2 == 0);  // even parity
  CHECK(sum(f, 29, 35) % 2 == 0);
  CHECK(sum(f, 36, 58) % 2 == 0);
  time_t enc = t + 60;
  struct tm lt;
  localtime_r(&enc, &lt);
  bool ok = minute == lt.tm_min && hour == lt.tm_hour && mday == lt.tm_mday &&
            wday == (lt.tm_wday == 0 ? 7 : lt.tm_wday) && month == lt.tm_mon + 1 &&
            year == lt.tm_year % 100 && f[17] == (lt.tm_isdst > 0);
  CHECK(ok);
}

static void decode_check_msf(time_t t) {
  uint8_t f[60], a[60], b[60];
  tc_build_minute(TC_MSF, t, TC_DST_AUTO, f);
  CHECK(f[0] == TC_SYM_MARK);
  for (int i = 1; i < 60; i++) {
    CHECK(f[i] <= 3);
    a[i] = f[i] & 1;
    b[i] = (f[i] >> 1) & 1;
  }
  static const int WY[] = { 80, 40, 20, 10, 8, 4, 2, 1 };
  int year = w(a, 17, WY, 8);
  int month = w(a, 25, WY + 3, 5);
  int mday = w(a, 30, WY + 2, 6);
  int wday = w(a, 36, WY + 5, 3);
  int hour = w(a, 39, WY + 2, 6);
  int minute = w(a, 45, WY + 1, 7);
  CHECK(a[52] == 0 && a[59] == 0);
  CHECK(sum(a, 53, 58) == 6);
  CHECK((sum(a, 17, 24) + b[54]) % 2 == 1);  // odd parity
  CHECK((sum(a, 25, 35) + b[55]) % 2 == 1);
  CHECK((sum(a, 36, 38) + b[56]) % 2 == 1);
  CHECK((sum(a, 39, 51) + b[57]) % 2 == 1);
  CHECK(sum(b, 1, 52) == 0);
  time_t enc = t + 60;
  struct tm lt;
  localtime_r(&enc, &lt);
  bool ok = year == lt.tm_year % 100 && month == lt.tm_mon + 1 && mday == lt.tm_mday &&
            wday == lt.tm_wday && hour == lt.tm_hour && minute == lt.tm_min &&
            b[58] == (lt.tm_isdst > 0);
  CHECK(ok);
}

static void test_round_trips() {
  time_t start = utc(2026, 1, 1, 0, 0), end = utc(2029, 1, 1, 0, 0);
  const time_t step = 7 * 3600 + 13 * 60;  // walks through all hours and minutes
  use_tz(TZ_JP);
  for (time_t t = start; t < end; t += step) decode_check_jjy(t);
  use_tz(TZ_US);
  for (time_t t = start; t < end; t += step) decode_check_wwvb(t);
  use_tz(TZ_DE);
  for (time_t t = start; t < end; t += step) decode_check_dcf77(t);
  use_tz(TZ_UK);
  for (time_t t = start; t < end; t += step) decode_check_msf(t);

  // Edges: new year, leap day, day 366, and the DST change minutes.
  const time_t edges[] = { utc(2026, 12, 31, 23, 59), utc(2027, 1, 1, 0, 0),  utc(2028, 2, 29, 12, 0),
                           utc(2028, 12, 31, 23, 59), utc(2026, 3, 29, 0, 59), utc(2026, 10, 25, 0, 59),
                           utc(2026, 3, 8, 9, 59),    utc(2026, 11, 1, 8, 59) };
  for (time_t t : edges) {
    for (time_t d = -120; d <= 120; d += 60) {
      use_tz(TZ_JP);
      decode_check_jjy(t + d);
      use_tz(TZ_US);
      decode_check_wwvb(t + d);
      use_tz(TZ_DE);
      decode_check_dcf77(t + d);
      use_tz(TZ_UK);
      decode_check_msf(t + d);
    }
  }
}

// -- regressions against v1.3 for BSF and BPC ---------------------------------

namespace legacy {
// Verbatim from clocksync.ino v1.3.1 (globals renamed into a namespace).
#define SP_0 (0)
#define SP_1 (1)
#define SP_2 (2)
#define SP_M4 (4)
int8_t bits_bsf[] = {
  SP_M4, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M4,
  SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M4
};
int8_t bits_bpc[] = {
  SP_M4, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_M4, SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_M4, SP_2, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0
};
int8_t sp_jjy[] = { 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
int8_t sp_wwvb[] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1 };
int8_t sp_dcf[] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
int8_t sp_bsf[] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                    0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
int8_t sp_msf[] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
                    0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1 };
int8_t sp_bpc[] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,
                    1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
int8_t *bits60;
struct tm nowtm;
void quadize(int v, int pos, int len) {
  for (pos = pos + len - 1; 0 < len; pos--, len--) {
    bits60[pos] = (uint8_t)(v & 3);
    v >>= 2;
  }
}
int qparity(int pos, int len) {
  int s = 0;
  for (; 0 < len; pos++, len--) {
    s += (bits60[pos] & 1) + ((bits60[pos] & 2) >> 1);
  }
  return (s % 2);
}
void mb_bsf(void) {
  quadize(nowtm.tm_min, 41, 3);
  quadize(nowtm.tm_hour * 2, 44, 3);
  bits60[46] |= qparity(41, 6);
  quadize(nowtm.tm_mday * 2, 47, 3);
  bits60[49] |= nowtm.tm_wday / 4;
  quadize(nowtm.tm_wday % 4, 50, 1);
  quadize(nowtm.tm_mon + 1, 51, 2);
  quadize((nowtm.tm_year - 100) * 2, 53, 4);
  bits60[56] |= qparity(47, 10);
}
void mb_bpc(void) {
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
}
}  // namespace legacy

static void test_legacy_bsf_bpc() {
  use_tz(TZ_CN);
  time_t start = utc(2026, 1, 1, 0, 0), end = utc(2028, 1, 1, 0, 0);
  int mismatches = 0;
  for (time_t t = start; t < end; t += 3 * 3600 + 7 * 60) {
    uint8_t f[60];
    localtime_r(&t, &legacy::nowtm);
    legacy::bits60 = legacy::bits_bsf;
    legacy::mb_bsf();
    tc_build_minute(TC_BSF, t, TC_DST_AUTO, f);
    for (int i = 0; i < 60; i++) mismatches += f[i] != (uint8_t)legacy::bits_bsf[i];
    legacy::bits60 = legacy::bits_bpc;
    legacy::mb_bpc();
    tc_build_minute(TC_BPC, t, TC_DST_AUTO, f);
    for (int i = 0; i < 60; i++) mismatches += f[i] != (uint8_t)legacy::bits_bpc[i];
  }
  CHECK(mismatches == 0);
}

static void check_shapes(int station, const int8_t *legacy_sp, const int *legacy_syms, const int *new_syms,
                         int n) {
  for (int k = 0; k < n; k++) {
    for (int tenth = 0; tenth < 10; tenth++) {
      CHECK(tc_carrier_high(station, (uint8_t)new_syms[k], tenth) == legacy_sp[legacy_syms[k] * 10 + tenth]);
    }
  }
}

static void test_shapes() {
  // Legacy symbol order was {0, 1, marker} for JJY/WWVB/DCF77 and
  // {0, 1/A, marker, AB} for MSF.
  const int l3[] = { 0, 1, 2 }, n3[] = { TC_SYM_0, TC_SYM_1, TC_SYM_MARK };
  check_shapes(TC_JJY40, legacy::sp_jjy, l3, n3, 3);
  check_shapes(TC_JJY60, legacy::sp_jjy, l3, n3, 3);
  check_shapes(TC_WWVB, legacy::sp_wwvb, l3, n3, 3);
  check_shapes(TC_DCF77, legacy::sp_dcf, l3, n3, 3);
  const int lm[] = { 0, 1, 2, 3 }, nm[] = { 0, 1, TC_SYM_MARK, 3 };
  check_shapes(TC_MSF, legacy::sp_msf, lm, nm, 4);
  const int l5[] = { 0, 1, 2, 3, 4 }, n5[] = { 0, 1, 2, 3, TC_SYM_MARK };
  check_shapes(TC_BSF, legacy::sp_bsf, l5, n5, 5);
  check_shapes(TC_BPC, legacy::sp_bpc, l5, n5, 5);
  // MSF 0B=1 with A=0: off, on, off.
  CHECK_STR(tc_shape(TC_MSF, 2), "LHLHHHHHHH");
  // Out of range falls back to an unmodulated carrier.
  CHECK(tc_carrier_high(TC_WWVB, 9, 0) == 1);
  CHECK(tc_carrier_high(99, 0, 0) == 1);
}

static void test_station_table() {
  // The legacy "sX" command letters keep their meaning.
  const char *letters = "jkwdtmc";
  for (int i = 0; i < TC_STATION_COUNT; i++) {
    CHECK(tc_station_by_cmd(letters[i]) == i);
    CHECK(tc_station_by_cmd((char)(letters[i] - 'a' + 'A')) == i);
    CHECK(tc_station_by_key(tc_stations[i].key) == i);
  }
  CHECK(tc_station_by_cmd('x') == -1);
  CHECK(tc_station_by_key("nope") == -1);
  CHECK(tc_stations[TC_JJY40].freq_hz == 40000);
  CHECK(tc_stations[TC_JJY60].freq_hz == 60000);
  CHECK(tc_stations[TC_WWVB].freq_hz == 60000);
  CHECK(tc_stations[TC_DCF77].freq_hz == 77500);
  CHECK(tc_stations[TC_BSF].freq_hz == 77500);
  CHECK(tc_stations[TC_MSF].freq_hz == 60000);
  CHECK(tc_stations[TC_BPC].freq_hz == 68500);
}

int main() {
  test_golden_jjy();
  test_golden_wwvb();
  test_golden_dcf77();
  test_golden_msf();
  test_wwvb_dst_bits();
  test_wwvb_misc_bits();
  test_dcf77_flags();
  test_msf_flags();
  test_round_trips();
  test_legacy_bsf_bpc();
  test_shapes();
  test_station_table();
  printf("test_timecode: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
