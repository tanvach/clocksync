# clocksync (fork of nisejjy)

ESP32 fake radio clock station. This repository is a maintained fork of the original `nisejjy` project by SASAKI Taroh.

- Original project: [tarohs/nisejjy](https://github.com/tarohs/nisejjy)
- Original author: SASAKI Taroh (taroh)

The original copyright notice remains in the source file header.

## Changes in this fork

- Renamed sketch to `clocksync.ino` and device name to `clocksync`.
- Added `secrets.h` handling (with fallback to `secrets_example.h`) to keep credentials out of git.
- Switched to POSIX TZ string via `#define TZ` in the source, with README guidance per station.
- Added simple HTTP server and mDNS service (`http` on port 80) for basic control/status.
- Updated default pins/hardware notes (e.g., M5 Atom Lite) and cleaned comments.
- Added `.gitignore` for common Arduino/PlatformIO artifacts and `secrets.h`.
- Aligned WWVB minute framing and DST bit semantics with the proven reference implementation `txtempus` for predictability.

## WiFi credentials (secrets)

This project reads WiFi credentials from a local header that is not committed.

1) Copy `secrets_example.h` to `secrets.h`
2) Edit `secrets.h` and set your credentials:

```
#define WIFI_SSID "YourSSID"
#define WIFI_PASS "YourPassword"
```

Notes:
- `secrets.h` is listed in `.gitignore` to avoid accidental commits.
- If `secrets.h` is missing, the build will fall back to `secrets_example.h` so compilation still works (with placeholder values).

## Timezone (TZ)

This sketch uses a POSIX TZ string via the `TZ` macro in `clocksync.ino` to configure local time for NTP and encoding. Recommended settings:

- JJY (Japan): `"JST-9"` (no DST)
- DCF77 (Germany): `"CET-1CEST,M3.5.0/2,M10.5.0/3"`
- MSF (UK): `"GMT0BST,M3.5.0/1,M10.5.0/2"`
- BSF (Taiwan): `"CST-8"` (no DST)
- BPC (China): `"CST-8"` (no DST)
- WWVB (US): `"UTC0"` (WWVB framing follows UTC with DST bits now/tomorrow)

Note: There is no runtime command to change TZ; set the `#define TZ` in the source and rebuild. Date/time commands (`dYYMMDD`, `tHHmmSS`) are interpreted in the configured local TZ.

## Stations

Emulates the following radio clock stations:

* JJY (Japan): [Wikipedia](https://ja.wikipedia.org/wiki/JJY)
* WWVB (US): [Wikipedia](https://en.wikipedia.org/wiki/WWVB)
  * For WWVB, set TZ to UTC (`"UTC0"`).
* DCF77 (Germany): [UDel notes](https://www.eecis.udel.edu/~mills/ntp/dcf77.html)
* (HBG (Switzerland) discontinued: [msys.ch](https://msys.ch/decoding-time-signal-stations))
* BSF (Taiwan): [Wikipedia](https://en.wikipedia.org/wiki/BSF_(time_service))
* MSF (UK): [Wikipedia](https://en.wikipedia.org/wiki/Time_from_NPL_(MSF))
* BPC (China): [51CTO article](https://harmonyos.51cto.com/posts/1731)

## Protocol reference and compatibility notes

- The data stream (minute frame layout and amplitude patterns) is based on and cross-checked with `txtempus`, a well-known Raspberry Pi/JETSON transmitter reference implementation. See: [hzeller/txtempus](https://github.com/hzeller/txtempus).
- When this project mentions “txtempus framing” or “txtempus defaults” in logs/help, it means the on-air bit layout matches `txtempus`’s interpretation of the respective time service. Legacy toggles such as WWVB next-minute or pending overrides are accepted for compatibility but have no effect here; clocksync always encodes WWVB per the standard frame (UTC time-base; DST-now/tomorrow bits set automatically).

## Attribution

This is a fork of the original `nisejjy` project by SASAKI Taroh. See the source file header for the retained copyright notice. Changes introduced in this fork are listed above.
