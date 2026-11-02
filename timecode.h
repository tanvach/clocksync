//
// timecode.h - time signal frame encoders (JJY, WWVB, DCF77, MSF, BSF, BPC)
//
// Pure C++ with no Arduino dependencies so it can be unit tested on a
// desktop machine (see test/). Local-time fields use the process time
// zone, i.e. whatever setenv("TZ", ...) + tzset() last selected.
//
#pragma once

#include <stdint.h>
#include <time.h>

enum {
  TC_JJY40 = 0,  // JJY Fukushima, Japan
  TC_JJY60,      // JJY Fukuoka, Japan
  TC_WWVB,       // WWVB Colorado, USA
  TC_DCF77,      // DCF77 Mainflingen, Germany
  TC_BSF,        // BSF Taiwan (uncertified)
  TC_MSF,        // MSF Anthorn, UK
  TC_BPC,        // BPC Shangqiu, China (uncertified)
  TC_STATION_COUNT
};

// DST flag handling for stations that carry a DST bit in local time (DCF77, MSF).
enum { TC_DST_STANDARD = 0, TC_DST_SUMMER = 1, TC_DST_AUTO = 2 };

// Symbols. Their carrier shape depends on the station (see tc_shape()).
enum { TC_SYM_0 = 0, TC_SYM_1 = 1, TC_SYM_2 = 2, TC_SYM_3 = 3, TC_SYM_MARK = 4, TC_SYM_COUNT };

struct tc_station {
  char cmd;               // legacy command letter: "s" + cmd selects it
  const char *key;        // short id used by the HTTP API
  const char *name;       // human readable name
  const char *region;     // where the real transmitter is
  uint32_t freq_hz;       // carrier frequency
  const char *native_tz;  // IANA zone whose time the clocks expect
  const char *native_posix;
  int encodes_utc;        // 1: frame carries UTC (WWVB), 0: local time
};

extern const tc_station tc_stations[TC_STATION_COUNT];

// Fill out[60] with the symbols sent during the minute that starts at
// minute_start (UTC seconds, a multiple of 60).
void tc_build_minute(int station, time_t minute_start, int dst_mode, uint8_t out[60]);

// Carrier shape of one symbol: 10 characters, one per 100 ms, 'H' for full
// power and 'L' for reduced power.
const char *tc_shape(int station, uint8_t symbol);

// 1 if the carrier is at full power during tenth (0..9) of a second.
int tc_carrier_high(int station, uint8_t symbol, int tenth);

// Station index for a legacy command letter or API key, or -1.
int tc_station_by_cmd(char cmd);
int tc_station_by_key(const char *key);
