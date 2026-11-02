#pragma once

// Optional. clocksync needs no source changes: on first boot it opens a
// "clocksync-setup" Wi-Fi hotspot where you choose your network.
//
// To pre-load settings at build time instead, copy this file to secrets.h
// (which is gitignored) and keep the lines you need. Wi-Fi credentials from
// secrets.h are applied once each time you change them and reflash; changes
// made later in the web UI win until then.

#define WIFI_SSID "YourSSID"
#define WIFI_PASS "YourPassword"

// #define DEFAULT_STATION TC_DCF77       // TC_JJY40, TC_JJY60, TC_WWVB, TC_DCF77, TC_MSF, TC_BSF, TC_BPC
// #define DEFAULT_HOSTNAME "clocksync"   // http://<name>.local and the "<name>-setup" hotspot
// #define DEFAULT_NTP_SERVER "pool.ntp.org"
// #define AP_PASSWORD "at-least-8-chars" // protect the setup hotspot (open by default)
