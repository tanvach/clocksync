# clocksync (v2.0)

**ESP32 Fake Radio Clock Station**

`clocksync` lets an ESP32 emulate a Low-Frequency (LF) time signal station, so radio-controlled clocks and watches (JJY, WWVB, DCF77, MSF, BPC, BSF) can sync even out of range of the real transmitter.

This project is a maintained fork of `nisejjy` by SASAKI Taroh, customized for modern hardware (M5 Atom Lite, ESP32, ESP32-C3, ESP32-S3).

## What's New in v2.0

**Compile and go.** Nothing in the source needs editing any more.
- **Wi-Fi setup hotspot**: on first boot (or when the saved network can't be reached) the device opens a `clocksync-setup` hotspot. Join it from a phone, the setup page opens by itself, pick your network from a scan list. Idea from the [Rmni2 fork](https://github.com/Rmni2/clocksync-esp32-c3).
- **Time zone picker**: choose any IANA zone (Europe/Berlin, America/Denver, ...) from a list in the web UI. By default each station uses the zone its clocks expect, so DCF77 sends German time and MSF UK time without any setup.
- **Board detection**: pins, LED and button are picked automatically for M5 Atom, AtomS3, ESP32, ESP32-C3 and ESP32-S3. The M5Atom library is no longer needed.
- **New web UI**: live clock, a trace of the exact signal being sent, and all settings in one place, including Wi-Fi, antenna pin and NTP server.

**Signal fixes.** Several stations were not being decoded correctly before:
- **Carrier frequency**: v1.x transmitted 60 kHz for every station. JJY 40 kHz, DCF77, BSF and BPC now use their real frequency. The carrier is also more accurate (≤30 ppm instead of up to 370 ppm).
- **MSF**: parity bits were even instead of odd, so clocks rejected every frame. MSF also announces the *next* minute, and v1.x sent the current one.
- **DCF77**: announces the *next* minute (v1.x clocks ran a minute slow); Sunday was sent as day 0 instead of 7; the DST-change warning bit (A1) is now set.
- **JJY**: day of year was one day behind.
- **WWVB**: the "DST begins/ends today" bits could appear a day early (now evaluated per UTC day as NIST specifies); DUT1 sign bits are now valid.
- **No 1970 broadcasts**: the transmitter waits until the clock is actually set, instead of sending 1970 when NTP fails.
- **Steady timing**: modulation runs from a timer task aligned to the wall clock, so web or Wi-Fi activity can't delay pulses, and NTP corrections re-align on their own.
- **NTP**: v1.x could permanently disable NTP after one failed boot. NTP now re-syncs hourly and `y0` no longer turns Wi-Fi off.

### Upgrading from v1.x
- Your station, TX, pin, drive strength, LED and DST override settings carry over.
- An existing `secrets.h` is still read: its Wi-Fi credentials are copied into the device's settings on first boot.
- The `#define TZ` line is gone. Pick a zone in the web UI, or keep the automatic default (US Pacific rules for WWVB, which is what v1.3 used).
- NTP is switched back on, since v1.x may have turned it off by mistake.
- Removed commands: `z` (buzzer, no hardware for it) and `n` (did nothing).

## Getting Started

### 1. Hardware

**Supported boards** (detected automatically from the board you select when compiling):

| Board | Antenna pin | Button | Status LED |
| :--- | :--- | :--- | :--- |
| **M5Stack Atom Lite/Matrix/Echo** (recommended) | GPIO 32 | front button (G39) | RGB LED |
| M5Stack AtomS3 / AtomS3 Lite | GPIO 5 | front button (G41) | RGB LED (Lite) |
| ESP32 DevKit | GPIO 32 | BOOT (GPIO 0) | GPIO 2 |
| ESP32-C3 (e.g. SuperMini) | GPIO 6 | BOOT (GPIO 9) | GPIO 8 |
| ESP32-S3 DevKit | GPIO 4 | BOOT (GPIO 0) | onboard RGB |
| Other ESP32 variants | GPIO 4 | none | none |

The antenna pin can be changed in the web UI (Advanced → Antenna pin). Edit [`board.h`](./board.h) only to add a board.

**Wiring Diagram**:
```text
           ESP32 / M5Atom
          +-------------+
          |             |
          |     GND [ ]----(Resistor 220-330Ω)----+
          |             |                         |
          | GPIO 32 [ ]-----------------+         |
          |             |               |         |
          +-------------+               |         |
                                   [ Antenna ]    |
                                   [  Coil   ]    |
                                        |         |
                                        +---------+
```

1.  **Pin**: Connect one end of your antenna to the antenna pin (`GPIO 32` on an Atom).
2.  **Ground**: Connect the other end to a **current-limiting resistor** (220-330 Ω), then to `GND`. The coil has almost no resistance; the resistor protects the ESP32.
3.  **Antenna types**:
    *   **Ferrite rod (recommended)**: scavenge one from an old radio clock or buy one tuned to your station. The frequency must match:
        *   **40 kHz**: JJY (east Japan).
        *   **60 kHz**: WWVB (USA), MSF (UK), JJY (west Japan).
        *   **68.5 kHz**: BPC (China).
        *   **77.5 kHz**: DCF77 (Germany), BSF (Taiwan).
    *   **Wire loop**: e.g. 30 turns of magnet wire around a water bottle. Short range, but works for any frequency.
4.  **Placement**: put the clock or watch **next to** the coil. This is a low-power near-field emulator; range is typically a few cm to ~1 m with a tuned ferrite. Very close can be too strong for some watches: try a few positions.

### 2. Upload

**Easiest: install from the browser.** Open **[tanvach.github.io/clocksync](https://tanvach.github.io/clocksync/)** in Chrome or Edge on a computer, pick your board, plug it in by USB and press **Install clocksync**. Nothing to install, no code to compile.

**Updating** works the same way: open the page, press Install, and leave **Erase device** unticked. Wi-Fi, station, time zone and all other settings are kept: they live in a separate flash area that updates don't touch. (Uploading from the Arduino IDE keeps them too.)

**Or build it yourself.** No libraries beyond the ESP32 board package are needed (Arduino-ESP32 core 3.x).

*   **Arduino IDE**:
    1.  `File` → `Preferences` → `Additional boards manager URLs`: add `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
    2.  `Tools` → `Board` → `Boards Manager`: install **esp32** by Espressif (3.x).
    3.  Open `clocksync.ino`, select your board (e.g. **M5Atom**, **ESP32C3 Dev Module**, **ESP32 Dev Module**) and click Upload.
        On ESP32-C3/S3 boards with a USB connector, set `Tools` → `USB CDC On Boot` → `Enabled` to see serial output.
*   **arduino-cli**:
    ```bash
    arduino-cli core install esp32:esp32
    arduino-cli compile --fqbn esp32:esp32:m5stack_atom .
    arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:m5stack_atom .
    ```
    Other boards: `esp32:esp32:esp32`, `esp32:esp32:esp32c3`, `esp32:esp32:esp32s3`, `esp32:esp32:m5stack_atoms3`. The M5Stack board package (`m5stack:esp32:m5stack_atom`) works too.

### 3. Set up

1.  On your phone or computer, join the Wi-Fi network **`clocksync-setup`**. The setup page usually opens by itself; if not, browse to `http://192.168.4.1`.
2.  Pick your network, enter its password and press **Connect**. The device fetches the time over NTP and starts transmitting.
3.  Choose your **Station**, the one your clock is made for. It usually says so on the clock or in its manual.
4.  Rejoin your normal Wi-Fi. The page is now at **`http://clocksync.local`** (or the IP your router assigned).

No internet? Press **Set from this device** to copy the time from your phone, and turn off Internet time under Advanced.

## Using It

**Web UI** (`http://clocksync.local`): live clock, the signal being sent this minute, transmitter on/off, station, time zone and Wi-Fi. Advanced holds the NTP server, DST flag override, antenna pin, drive strength, LED, device name (up to 26 characters, applied after a restart), a command box and restart/reset.

**Button**:
- Short press: transmitter on/off.
- Hold 3 s (the LED turns blue after 1 s): open the setup hotspot, e.g. to change Wi-Fi.

**Status LED** (single-color LEDs show the same patterns without the colors; the setup blink is faster):
- Green, flickering with the signal: transmitting.
- Slow amber blink: waiting for the time.
- Blue blink: setup hotspot is on.
- Off: transmitter off (or LED disabled in the UI).

### Time Zones

The time zone decides which local time is encoded. Leave it on **Auto** unless you know you need something else:

| Station | Auto zone | Notes |
| :--- | :--- | :--- |
| WWVB | America/Los_Angeles | WWVB sends UTC; the zone only sets the DST flag. Any US zone with DST gives the same signal. Choose an Arizona or Hawaii zone, or DST flag "Always standard", if your clock should never change. |
| DCF77 | Europe/Berlin | Real DCF77 clocks expect German time. |
| MSF | Europe/London | |
| JJY | Asia/Tokyo | |
| BPC | Asia/Shanghai | |
| BSF | Asia/Taipei | |

Pick a different zone when you want a DCF77/MSF/JJY clock to show another local time (for example a DCF77 clock used in New York). A custom POSIX TZ rule can also be entered.

The device opens the setup hotspot again if it can't reach the saved network for 20 s after boot, or for 5 minutes later on. It keeps retrying in the background and closes the hotspot once connected, so a power cut that outlasts the router reboot recovers by itself.

## Command Reference

Commands work over USB serial (115200 baud) and HTTP (`http://clocksync.local/cmd?c=<command>`).

| Command | Description | Example |
| :--- | :--- | :--- |
| **Stations** | | |
| `sj` / `sk` | JJY 40 kHz (Fukushima) / 60 kHz (Fukuoka) | `sj` |
| `sw` | WWVB (USA) | `sw` |
| `sd` | DCF77 (Germany) | `sd` |
| `sm` | MSF (UK) | `sm` |
| `st` | BSF (Taiwan) | `st` |
| `sc` | BPC (China) | `sc` |
| **Control** | | |
| `e1` / `e0` | Transmit on / off | `e1` |
| `pNN` | Antenna pin (from the board's safe list) | `p25` |
| `g0` - `g3` | Drive strength (0 = weakest) | `g2` |
| **Settings** | | |
| `tz<zone>` | Time zone: IANA name, POSIX rule, or `tz` alone for auto | `tzEurope/Berlin` |
| `x0` / `x1` / `x2` | DST flag: standard / summer / auto | `x2` |
| `y1` / `y0` | NTP on / off | `y1` |
| `ss<name>` | Wi-Fi network name | `ssMy Network` |
| `ps<password>` | Wi-Fi password, then connect (`ps` alone for open networks) | `psSecret123` |
| `setup` | Open the setup hotspot | `setup` |
| `l1` / `l0` | Status LED on / off | `l1` |
| **Manual Time** | | |
| `dYYMMDD` | Set the date (local time) | `d261225` |
| `tHHMMSS` | Set the time (local time) | `t123000` |
| **Other** | | |
| `h` / `status` | Help and status | `h` |
| `f` | Frequency self-test (jumper the antenna pin to the measurement pin, GPIO 33 on Atom) | `f` |
| `r` | Reset settings to defaults (keeps Wi-Fi) | `r` |
| `reboot` | Restart | `reboot` |

### HTTP API (Home Assistant etc.)

- `GET /cmd?c=e0`: any command above. The reply starts with `OK` or `ERR`. Commands that change Wi-Fi, reset or restart (`ss`, `ps`, `r`, `reboot`, `setup`) also need the header `X-Clocksync: 1`.
- `GET /status.txt`: human-readable status.
- `GET /api/status`: JSON status (station, state, time, zone, Wi-Fi, ...).
- `POST /api/set`: form fields `station` (`wwvb`, `dcf77`, `msf`, `jjy40`, `jjy60`, `bpc`, `bsf`), `tx`, `tz`, `posix`, `dst`, `ntp`, `ntpServer`, `pin`, `drive`, `led`, `host`. POST requests must send the header `X-Clocksync: 1`, which stops other websites from changing settings through your browser.

Example: transmit only at night with two Home Assistant `rest_command`s calling `http://clocksync.local/cmd?c=e1` and `...?c=e0`.

### Build-time Defaults (optional)

Copy [`secrets_example.h`](./secrets_example.h) to `secrets.h` to pre-load Wi-Fi credentials, the default station, device name, NTP server or a password for the setup hotspot.

## Supported Stations

| Station | Freq (kHz) | Location | Notes |
| :--- | :--- | :--- | :--- |
| **JJY** | 40 / 60 | Japan | Japan time, current minute. |
| **WWVB** | 60 | USA | UTC; DST bits from the selected zone. |
| **DCF77** | 77.5 | Germany | German time, announces the next minute. |
| **MSF** | 60 | UK | UK time, announces the next minute; odd parity. |
| **BSF** | 77.5 | Taiwan | Quaternary encoding (uncertified). |
| **BPC** | 68.5 | China | Quaternary encoding (uncertified). |

## Troubleshooting
*   **Can't find the setup page**: join `clocksync-setup`, then open `http://192.168.4.1`. Hold the button 3 s to reopen the hotspot.
*   **`clocksync.local` doesn't open**: some Android versions don't support `.local` names. Use the IP address shown on the setup page or in your router.
*   **Clock isn't syncing**:
    *   **Station**: check it matches your clock (WWVB/DCF77/MSF/JJY...). With JJY, try both 40 and 60 kHz.
    *   **Status**: the web UI should say "Transmitting". "Waiting for time" means the device has no time yet.
    *   **Proximity**: try different distances and orientations; ferrite rods couple best when parallel to the clock's antenna.
    *   **Interference**: LED power supplies, chargers and monitors cause noise. Move away from them.
    *   **Strength**: raise Drive strength to 3 (Advanced).
    *   Many clocks only try to sync at night or when you hold their sync button.
*   **Clock shows the wrong time**:
    *   Leave the time zone on **Auto** for the station's own zone, or pick the zone you want the clock to show (not WWVB, which always sends UTC).
    *   **WWVB clock an hour off**: set the clock's own time zone/DST switch; WWVB carries UTC.
*   **Compile errors**: use the Espressif ESP32 core 3.x (Boards Manager → esp32).

## Development

```bash
make -C test                      # host unit tests for the encoders and the time zone table
python3 tools/mock_server.py      # web UI with a fake device at http://127.0.0.1:8765/
python3 tools/gen_tzdata.py > tzdata.h   # refresh time zones from the system's tzdata
tools/build_installer.sh _site    # build all boards and the browser installer into _site/
```

**Releases.** [`.github/workflows/release.yml`](./.github/workflows/release.yml) runs the tests and builds every board. Each push to `master` republishes the installer page on GitHub Pages; pushing a `v*` tag also attaches the images to a GitHub release. One-time setup: repository **Settings → Pages → Source: GitHub Actions**. To release, bump `VERSION` in `clocksync.ino`, push to `master`, then tag:

```bash
git tag v2.0.0 && git push origin v2.0.0
```

The installer writes the bootloader, partition table and app as separate parts, so the settings area (NVS at 0x9000) survives unless "Erase device" is ticked. A merged full image would overwrite it; those are only offered as downloads for a clean install.

See [HOW_IT_WORKS.md](./HOW_IT_WORKS.md) for the signal encoding, timing architecture and protocol tables.

## Credits

- **Original Author**: SASAKI Taroh (tarohs) - [nisejjy](https://github.com/tarohs/nisejjy)
- **Wi-Fi setup and runtime time zone ideas**: Rmni2 - [clocksync-esp32-c3](https://github.com/Rmni2/clocksync-esp32-c3)
- **Reference**: [txtempus](https://github.com/hzeller/txtempus) by Henner Zeller (used for WWVB frame verification).
- **Time zones**: [IANA tz database](https://www.iana.org/time-zones) (public domain).
