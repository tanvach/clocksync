# clocksync (v1.2) - fork of nisejjy

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
- Added GPIO drive-strength control (0-3) with web UI + persistence for safer antenna drive.
  * Default drive strength is now `2`.

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

## Hardware notes: M5 Atom Lite vs. generic ESP32/ESP32‑S3

This fork is customized for the M5 Atom Lite by default (RGB LED via M5Atom library, default radio pin/power assumptions). It works great out-of-the-box on that device. For other ESP32 boards, a couple of tweaks may be required.

### M5 Atom Lite (default)
- Install the M5Atom library (Arduino Library Manager: "M5Atom").
- Default wiring: connect a small loop antenna (e.g., ~30 cm wire) from `GPIO32` to GND via ~330 Ω.
- The on-device RGB LED shows activity.
- The built-in button (or button on GPIO 39) toggles the radio signal (TX on/off).

### Generic ESP32 (e.g., DevKit v1/WROOM)
1) Libraries
   - EITHER install the M5Atom library so the sketch compiles unchanged, OR remove M5-specific bits:
     - Delete the include `#include <M5Atom.h>` and comment out lines that call `M5.begin()`, `M5.update()`, and `M5.dis.drawpix(...)` (search for `M5.` in `clocksync.ino`).
2) Pins
   - Set the radio output pin near the top of `clocksync.ino`: `#define PIN_RADIO (32)` is the default and works well on many boards; `GPIO25`/`GPIO26`/`GPIO33` are also typical.
   - Avoid strapping pins and input-only pins:
     - Classic ESP32 input-only: `GPIO34..GPIO39` (don’t use for `PIN_RADIO`).
     - Boot/strap-sensitive: `GPIO0`, `GPIO2`, `GPIO12`, `GPIO15`.
   - Optional buzzer: set `#define PIN_BUZZ` to a valid GPIO or `-1` to disable (default off).
   - Optional button: connect a button between `PIN_BUTTON` (default 39) and GND to toggle TX. If you are not using M5Atom library, you may need to implement the button read logic in `loop()`.
3) Wiring
   - Connect a small loop antenna from `PIN_RADIO` to GND via ~330 Ω resistor (very low-power magnetic coupling). Observe local RF regulations.
4) Build/Flash
   - Board: "ESP32 Dev Module" (Arduino IDE) or corresponding PlatformIO env.
   - Open Serial Monitor at 115200 baud; browse `http://clocksync.local/` if mDNS is available.

### ESP32‑S3 boards
1) Board setup
   - Board: "ESP32S3 Dev Module". If needed, enable USB CDC on boot in board options.
2) Pins
   - Choose any regular output-capable GPIO for `PIN_RADIO` (most S3 pins qualify). Default is `GPIO32`. Avoid dedicated/strap pins like `GPIO0`, `GPIO2`, `GPIO46` depending on your module.
3) M5Atom library
   - If you’re not using an M5 device, install the M5Atom library just to satisfy includes, or comment out the `M5.` calls as noted above.
4) Notes
   - The carrier is generated with LEDC on all variants; the built-in `f` self-test (`frequency`) can verify your actual carrier by jumpering `PIN_RADIO` to `GPIO33` (or adjust the measurement pin in code if your board lacks `GPIO33`). Expected is ~40/60/77.5/68.5 kHz depending on station.

### Minimal checklist to port
- [ ] Set `PIN_RADIO` to a safe, output-capable GPIO for your board (`32` by default).
- [ ] Install M5Atom library or comment out `M5.` calls.
- [ ] Optionally set `PIN_BUZZ` or keep `-1`.
- [ ] Configure `TZ` in `clocksync.ino` per your station.
- [ ] **Wiring**: Connect an antenna from `PIN_RADIO` to GND via ~330 Ω resistor.
    - **Recommended**: Use a **ferrite core antenna** tuned to your target frequency (see below).
    - **Easy Way**: Buy a radio time receiver module (Amazon/eBay), desolder the ferrite antenna, and wire it to your ESP32.
    - **Simple**: A long loop of wire (~30cm) works for very close range testing.

## Frequencies
- **40 kHz**: JJY (Fukushima)
- **60 kHz**: JJY (Fukuoka), WWVB (US), MSF (UK)
- **68.5 kHz**: BPC (China)
- **77.5 kHz**: DCF77 (Germany), BSF (Taiwan)


## Protocol reference and compatibility notes

- The data stream (minute frame layout and amplitude patterns) is based on and cross-checked with `txtempus`, a well-known Raspberry Pi/JETSON transmitter reference implementation. See: [hzeller/txtempus](https://github.com/hzeller/txtempus).
- When this project mentions “txtempus framing” or “txtempus defaults” in logs/help, it means the on-air bit layout matches `txtempus`’s interpretation of the respective time service. Legacy toggles such as WWVB next-minute or pending overrides are accepted for compatibility but have no effect here; clocksync always encodes WWVB per the standard frame (UTC time-base; DST-now/tomorrow bits set automatically).
    
## OTA Updates

This firmware supports Over-The-Air (OTA) updates via the Arduino IDE/PlatformIO.

1.  **First Flash**: Must be done via USB to install the OTA-capable firmware.
2.  **Network Port**: Once connected to WiFi, the device will appear as a network port named `clocksync` (or your configured `DEVICENAME`) in the Arduino IDE "Port" menu.
3.  **Upload**: Select the network port and click Upload.
    -   **Password**: If asked, enter `clocksync`.
    -   The device will receive the new firmware and reboot.

> [!NOTE]
> OTA is only enabled if the device successfully connects to WiFi on boot.
>
> **Important**: The Serial Monitor is **not available** over the network port. To view logs or debugging information, you must connect the device via USB.

## Home Assistant scheduling (optional)

If you want to run the sync signal only during a daily window (e.g., at night), you can keep this firmware and have Home Assistant call the built-in HTTP command endpoint.

Prereqs:
- Device is on WiFi and reachable at `http://clocksync.local/` (mDNS/Bonjour), or replace `clocksync.local` with the device IP address.
- You can verify the API by visiting `http://clocksync.local/status.txt` in a browser.

### TX enable/disable command

This fork adds a TX enable/disable command:
- Button press: toggle TX on/off
- `e1`: enable TX (start carrier + modulation scheduler)
- `e0`: disable TX (stop carrier + modulation scheduler)

Commands work over both USB serial and HTTP:
- `GET http://clocksync.local/cmd?c=e1`
- `GET http://clocksync.local/cmd?c=e0`

### Home Assistant `rest_command`

Add this to your `configuration.yaml`:

```yaml
rest_command:
  clocksync_cmd:
    url: "http://clocksync.local/cmd?c={{ cmd | urlencode }}"
    method: GET
```

Restart Home Assistant (or reload YAML if you prefer).

### Example automation: run TX daily for 10 minutes

```yaml
automation:
  - id: clocksync_daily_tx_window
    alias: "clocksync: daily TX window"
    mode: single
    trigger:
      - platform: time
        at: "03:00:00"
    action:
      - service: rest_command.clocksync_cmd
        data:
          cmd: "e1"
      - delay: "00:10:00"
      - service: rest_command.clocksync_cmd
        data:
          cmd: "e0"
```

Tip: many radio clocks need multiple full minute frames to sync reliably. If you have trouble, increase the window (e.g., 15–30 minutes).

### Other useful commands for automations

You can combine these with the same `rest_command` (one call per command):
- Station select: `sX` where X is one of `j` (JJY_E), `k` (JJY_W), `w` (WWVB), `d` (DCF77), `t` (BSF), `m` (MSF), `c` (BPC). Example: `cmd: "sw"` for WWVB.
- Drive strength: `g0|g1|g2|g3` (0 = weakest, 3 = strongest). Example: `cmd: "g1"`.
- DST override (DCF/MSF only): `x0|x1|x2` for STD / DST / AUTO.

Example automation that sets station + drive, then runs TX for 15 minutes:

```yaml
automation:
  - id: clocksync_nightly_dcf77
    alias: "clocksync: nightly DCF77 window"
    trigger:
      - platform: time
        at: "02:30:00"
    action:
      - service: rest_command.clocksync_cmd
        data: { cmd: "sd" }   # DCF77
      - service: rest_command.clocksync_cmd
        data: { cmd: "g1" }   # moderate drive
      - service: rest_command.clocksync_cmd
        data: { cmd: "e1" }   # TX on
      - delay: "00:15:00"
      - service: rest_command.clocksync_cmd
        data: { cmd: "e0" }   # TX off
```

## Attribution

This is a fork of the original `nisejjy` project by SASAKI Taroh. See the source file header for the retained copyright notice. Changes introduced in this fork are listed above.
