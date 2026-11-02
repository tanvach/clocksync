//
// timecode.cpp - time signal frame encoders
//
// Protocol references: NICT (JJY), NIST SP 432 (WWVB), PTB (DCF77) and
// NPL "MSF 60 kHz time and date code" (MSF). See HOW_IT_WORKS.md.
// BSF/BPC were reverse engineered by the original nisejjy author and are
// ported unchanged.
//
#include "timecode.h"

#include <string.h>

const tc_station tc_stations[TC_STATION_COUNT] = {
  { 'j', "jjy40", "JJY 40 kHz", "Japan (Fukushima)", 40000, "Asia/Tokyo", "JST-9", 0 },
  { 'k', "jjy60", "JJY 60 kHz", "Japan (Fukuoka)", 60000, "Asia/Tokyo", "JST-9", 0 },
  { 'w', "wwvb", "WWVB", "USA (Colorado)", 60000, "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0", 1 },
  { 'd', "dcf77", "DCF77", "Germany (Mainflingen)", 77500, "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3", 0 },
  { 't', "bsf", "BSF", "Taiwan", 77500, "Asia/Taipei", "CST-8", 0 },
  { 'm', "msf", "MSF", "UK (Anthorn)", 60000, "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0", 0 },
  { 'c', "bpc", "BPC", "China (Shangqiu)", 68500, "Asia/Shanghai", "CST-8", 0 },
};

// Carrier shapes, indexed by symbol (0, 1, 2, 3, marker).
// One character per 100 ms: H = full power, L = reduced power.
static const char *const SHAPES[TC_STATION_COUNT][TC_SYM_COUNT] = {
  // JJY: the second starts at full power; the low tail encodes the symbol.
  { "HHHHHHHHLL", "HHHHHLLLLL", "HHHHHHHHLL", "HHHHHHHHLL", "HHLLLLLLLL" },
  { "HHHHHHHHLL", "HHHHHLLLLL", "HHHHHHHHLL", "HHHHHHHHLL", "HHLLLLLLLL" },
  // WWVB: 0.2 s / 0.5 s / 0.8 s low.
  { "LLHHHHHHHH", "LLLLLHHHHH", "LLHHHHHHHH", "LLHHHHHHHH", "LLLLLLLLHH" },
  // DCF77: 0.1 s / 0.2 s low; the minute mark is an unmodulated second.
  { "LHHHHHHHHH", "LLHHHHHHHH", "LHHHHHHHHH", "LHHHHHHHHH", "HHHHHHHHHH" },
  // BSF: quaternary.
  { "LLHHHHHHHH", "LLLLHHHHHH", "LLLLLLLLHH", "LLLLLLHHHH", "HHHHHHHHHH" },
  // MSF: symbol = A bit + 2 * B bit; the minute mark is 0.5 s off.
  { "LHHHHHHHHH", "LLHHHHHHHH", "LHLHHHHHHH", "LLLHHHHHHH", "LLLLLHHHHH" },
  // BPC: quaternary, 0.1 s .. 0.4 s low.
  { "LHHHHHHHHH", "LLHHHHHHHH", "LLLHHHHHHH", "LLLLHHHHHH", "LLLLLLLLLL" },
};

// -- field helpers ----------------------------------------------------------

// Binary value, most significant bit first.
static void put_bin(uint8_t *f, int v, int pos, int len) {
  for (int i = pos + len - 1; i >= pos; i--) {
    f[i] = (uint8_t)(v & 1);
    v >>= 1;
  }
}

// Binary value, least significant bit first.
static void put_bin_lsb(uint8_t *f, int v, int pos, int len) {
  for (int i = pos; i < pos + len; i++) {
    f[i] = (uint8_t)(v & 1);
    v >>= 1;
  }
}

// BCD, most significant digit and bit first. The field is filled from its
// end in 4-bit digits, so a 7-bit minute field is 3 tens bits + 4 ones bits.
static void put_bcd(uint8_t *f, int v, int pos, int len) {
  int end = pos + len;
  while (len > 0) {
    int l = len < 4 ? len : 4;
    put_bin(f, v % 10, end - l, l);
    v /= 10;
    end -= l;
    len -= l;
  }
}

// BCD, least significant digit and bit first.
static void put_bcd_lsb(uint8_t *f, int v, int pos, int len) {
  while (len > 0) {
    int l = len < 4 ? len : 4;
    put_bin_lsb(f, v % 10, pos, l);
    v /= 10;
    pos += l;
    len -= l;
  }
}

// Base-4 value, most significant digit first.
static void put_quad(uint8_t *f, int v, int pos, int len) {
  for (int i = pos + len - 1; i >= pos; i--) {
    f[i] = (uint8_t)(v & 3);
    v >>= 2;
  }
}

// 1 if the number of one bits in f[pos..pos+len-1] is odd (even parity bit).
static int parity(const uint8_t *f, int pos, int len) {
  int s = 0;
  for (int i = pos; i < pos + len; i++) {
    s += f[i] & 1;
  }
  return s & 1;
}

// Parity over the bits of base-4 digits (BSF/BPC).
static int quad_parity(const uint8_t *f, int pos, int len) {
  int s = 0;
  for (int i = pos; i < pos + len; i++) {
    s += (f[i] & 1) + ((f[i] >> 1) & 1);
  }
  return s & 1;
}

static int is_dst_at(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  return lt.tm_isdst > 0;
}

static int dst_flag(int isdst, int dst_mode) {
  if (dst_mode == TC_DST_STANDARD) return 0;
  if (dst_mode == TC_DST_SUMMER) return 1;
  return isdst ? 1 : 0;
}

static void clear_frame(uint8_t *f) {
  memset(f, TC_SYM_0, 60);
}

// -- stations ---------------------------------------------------------------

// JJY: local (Japan) time of the current minute.
static void build_jjy(time_t t, uint8_t *f) {
  struct tm lt;
  localtime_r(&t, &lt);
  clear_frame(f);
  f[0] = f[9] = f[19] = f[29] = f[39] = f[49] = f[59] = TC_SYM_MARK;
  put_bin(f, lt.tm_min / 10, 1, 3);
  put_bin(f, lt.tm_min % 10, 5, 4);
  put_bin(f, lt.tm_hour / 10, 12, 2);
  put_bin(f, lt.tm_hour % 10, 15, 4);
  int doy = lt.tm_yday + 1;  // JJY counts days from 1
  put_bin(f, doy / 100, 22, 2);
  put_bin(f, doy / 10 % 10, 25, 4);
  put_bin(f, doy % 10, 30, 4);
  f[36] = (uint8_t)parity(f, 12, 7);  // PA1: hours
  f[37] = (uint8_t)parity(f, 1, 8);   // PA2: minutes
  int yy = lt.tm_year % 100;
  put_bin(f, yy / 10, 41, 4);
  put_bin(f, yy % 10, 45, 4);
  put_bin(f, lt.tm_wday, 50, 3);
}

// WWVB: UTC of the current minute; the time zone only drives the DST bits.
static void build_wwvb(time_t t, int dst_mode, uint8_t *f) {
  struct tm ut;
  gmtime_r(&t, &ut);
  clear_frame(f);
  f[0] = f[9] = f[19] = f[29] = f[39] = f[49] = f[59] = TC_SYM_MARK;
  put_bin(f, ut.tm_min / 10, 1, 3);
  put_bin(f, ut.tm_min % 10, 5, 4);
  put_bin(f, ut.tm_hour / 10, 12, 2);
  put_bin(f, ut.tm_hour % 10, 15, 4);
  int doy = ut.tm_yday + 1;
  put_bin(f, doy / 100, 22, 2);
  put_bin(f, doy / 10 % 10, 25, 4);
  put_bin(f, doy % 10, 30, 4);
  // DUT1 = +0.0 s: sign bits 36..38 are "101" for positive.
  f[36] = 1;
  f[38] = 1;
  int yy = ut.tm_year % 100;
  put_bin(f, yy / 10, 45, 4);
  put_bin(f, yy % 10, 50, 4);
  int year = ut.tm_year + 1900;
  f[55] = (uint8_t)((year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 1 : 0);
  // DST bits change at 00:00 UTC: bit 57 is the DST state at the end of
  // the current UTC day and bit 58 the state at its start, so the pair reads
  // 10 on the day DST begins and 01 on the day it ends.
  time_t day = t - (t % 86400);
  f[57] = (uint8_t)dst_flag(is_dst_at(day + 86400), dst_mode);
  f[58] = (uint8_t)dst_flag(is_dst_at(day), dst_mode);
}

// DCF77: local (German) time of the *following* minute.
static void build_dcf77(time_t t, int dst_mode, uint8_t *f) {
  time_t enc = t + 60;
  struct tm lt;
  localtime_r(&enc, &lt);
  clear_frame(f);
  f[59] = TC_SYM_MARK;
  int dst = dst_flag(lt.tm_isdst > 0, dst_mode);
  if (dst_mode == TC_DST_AUTO) {
    f[16] = (uint8_t)(is_dst_at(enc) != is_dst_at(enc + 3600));  // A1: change within the hour
  }
  f[17] = (uint8_t)dst;   // Z1: CEST
  f[18] = (uint8_t)!dst;  // Z2: CET
  f[20] = 1;              // S: start of time information
  put_bcd_lsb(f, lt.tm_min, 21, 7);
  f[28] = (uint8_t)parity(f, 21, 7);
  put_bcd_lsb(f, lt.tm_hour, 29, 6);
  f[35] = (uint8_t)parity(f, 29, 6);
  put_bcd_lsb(f, lt.tm_mday, 36, 6);
  put_bin_lsb(f, lt.tm_wday == 0 ? 7 : lt.tm_wday, 42, 3);  // Monday=1 .. Sunday=7
  put_bcd_lsb(f, lt.tm_mon + 1, 45, 5);
  put_bcd_lsb(f, lt.tm_year % 100, 50, 8);
  f[58] = (uint8_t)parity(f, 36, 22);
}

// MSF: local (UK) time of the *following* minute. Each second carries an
// A bit and a B bit; the symbol is A + 2 * B.
static void build_msf(time_t t, int dst_mode, uint8_t *f) {
  time_t enc = t + 60;
  struct tm lt;
  localtime_r(&enc, &lt);
  uint8_t a[60], b[60];
  memset(a, 0, sizeof(a));
  memset(b, 0, sizeof(b));
  put_bcd(a, lt.tm_year % 100, 17, 8);
  put_bcd(a, lt.tm_mon + 1, 25, 5);
  put_bcd(a, lt.tm_mday, 30, 6);
  put_bin(a, lt.tm_wday, 36, 3);  // Sunday=0
  put_bcd(a, lt.tm_hour, 39, 6);
  put_bcd(a, lt.tm_min, 45, 7);
  for (int i = 53; i <= 58; i++) {
    a[i] = 1;  // 52A..59A = 01111110
  }
  if (dst_mode == TC_DST_AUTO) {
    b[53] = (uint8_t)(is_dst_at(enc) != is_dst_at(enc + 61 * 60));  // change within 61 minutes
  }
  // Odd parity: each parity bit makes the count of ones in its field odd.
  b[54] = (uint8_t)!parity(a, 17, 8);
  b[55] = (uint8_t)!parity(a, 25, 11);
  b[56] = (uint8_t)!parity(a, 36, 3);
  b[57] = (uint8_t)!parity(a, 39, 13);
  b[58] = (uint8_t)dst_flag(lt.tm_isdst > 0, dst_mode);
  for (int i = 0; i < 60; i++) {
    f[i] = (uint8_t)(a[i] | (b[i] << 1));
  }
  f[0] = TC_SYM_MARK;
}

// BSF: local (Taiwan) time of the current minute.
static void build_bsf(time_t t, uint8_t *f) {
  struct tm lt;
  localtime_r(&t, &lt);
  clear_frame(f);
  f[0] = f[39] = f[59] = TC_SYM_MARK;
  f[40] = TC_SYM_1;
  put_quad(f, lt.tm_min, 41, 3);
  put_quad(f, lt.tm_hour * 2, 44, 3);
  f[46] |= (uint8_t)quad_parity(f, 41, 6);
  put_quad(f, lt.tm_mday * 2, 47, 3);
  f[49] |= (uint8_t)(lt.tm_wday / 4);
  put_quad(f, lt.tm_wday % 4, 50, 1);
  put_quad(f, lt.tm_mon + 1, 51, 2);
  put_quad(f, (lt.tm_year % 100) * 2, 53, 4);
  f[56] |= (uint8_t)quad_parity(f, 47, 10);
}

// BPC: local (China) time of the current minute, as three 20 s sub-frames.
static void build_bpc(time_t t, uint8_t *f) {
  struct tm lt;
  localtime_r(&t, &lt);
  clear_frame(f);
  f[0] = f[20] = f[40] = TC_SYM_MARK;
  f[1] = TC_SYM_0;  // P1: sub-frame index
  f[21] = TC_SYM_1;
  f[41] = TC_SYM_2;
  put_quad(f, lt.tm_hour % 12, 3, 2);
  put_quad(f, lt.tm_min, 5, 3);
  put_quad(f, lt.tm_wday, 8, 2);
  f[10] = (uint8_t)((lt.tm_hour / 12) * 2 + quad_parity(f, 3, 7));
  put_quad(f, lt.tm_mday, 11, 3);
  put_quad(f, lt.tm_mon + 1, 14, 2);
  put_quad(f, lt.tm_year % 100, 16, 3);
  f[19] = (uint8_t)quad_parity(f, 11, 8);
  for (int i = 2; i < 20; i++) {
    f[20 + i] = f[i];
    f[40 + i] = f[i];
  }
}

// -- public API -------------------------------------------------------------

void tc_build_minute(int station, time_t minute_start, int dst_mode, uint8_t out[60]) {
  minute_start -= minute_start % 60;
  switch (station) {
    case TC_JJY40:
    case TC_JJY60: build_jjy(minute_start, out); break;
    case TC_WWVB: build_wwvb(minute_start, dst_mode, out); break;
    case TC_DCF77: build_dcf77(minute_start, dst_mode, out); break;
    case TC_BSF: build_bsf(minute_start, out); break;
    case TC_MSF: build_msf(minute_start, dst_mode, out); break;
    case TC_BPC: build_bpc(minute_start, out); break;
    default: memset(out, TC_SYM_0, 60); break;
  }
}

const char *tc_shape(int station, uint8_t symbol) {
  if (station < 0 || station >= TC_STATION_COUNT || symbol >= TC_SYM_COUNT) {
    return "HHHHHHHHHH";
  }
  return SHAPES[station][symbol];
}

int tc_carrier_high(int station, uint8_t symbol, int tenth) {
  if (tenth < 0 || tenth > 9) return 1;
  return tc_shape(station, symbol)[tenth] == 'H';
}

int tc_station_by_cmd(char cmd) {
  if (cmd >= 'A' && cmd <= 'Z') cmd = (char)(cmd - 'A' + 'a');
  for (int i = 0; i < TC_STATION_COUNT; i++) {
    if (tc_stations[i].cmd == cmd) return i;
  }
  return -1;
}

int tc_station_by_key(const char *key) {
  if (!key) return -1;
  for (int i = 0; i < TC_STATION_COUNT; i++) {
    if (strcmp(tc_stations[i].key, key) == 0) return i;
  }
  return -1;
}
