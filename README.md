# clocksync (v1.3.1)

**ESP32 Fake Radio Clock Station**

`clocksync` allows an ESP32 to emulate various Low-Frequency (LF) time signal stations, allowing you to sync radio-controlled clocks (JJY, WWVB, DCF77, MSF, etc.) even if you are out of range of the actual transmitters.

This project is a maintained fork of the original `nisejjy` project by SASAKI Taroh, customized for modern hardware (M5 Atom Lite, ESP32-S3) with improved Web UI, stability, and signal framing.

### What's New in v1.3.1

- **Prevent NVS flash wear**: Added `WiFi.persistent(false)` to stop the WiFi library from writing credentials to flash on every connect/disconnect cycle.
- **Buzzer default off**: Buzzer now defaults to off on fresh install and reset.
- **Tristate radio pin when TX off**: Radio output pin is now set to high-impedance (INPUT) when TX is disabled or when switching pins, instead of being driven LOW.

### What's New in v1.3

- **Fix WWVB DST bits**: Changed default TZ from `"UTC0"` to `"PST8PDT,M3.2.0,M11.1.0"`. With `"UTC0"`, DST status bits were always 0, causing clocks to stay on standard time after DST started. Updated documentation and status display to clarify that WWVB DST bits are derived from the TZ setting.
- **Remove dead `q` command**: Removed unused `q0/q1/q2` WWVB pending override command and related dead code (`wwvbPendingOverride`, `getWWVBPendFlag`).

## Features

- **Multi-Station Support**: Emulates JJY (Japan), WWVB (USA), DCF77 (Germany), MSF (UK), BSF (Taiwan), and BPC (China).
- **Web Control**: Simple web interface for status monitoring and configuration.
- **Home Assistant Integration**: REST API for scheduling transmission (e.g., run only at night).
- **Precise Timing**: Uses ESP32 hardware timers for accurate carrier generation and modulation.
- **OTA Updates**: Update firmware wirelessly via Arduino IDE.

## Getting Started

### 1. Hardware Setup

**Supported Boards**:
- **M5Stack Atom Lite** (Recommended): Works out of the box. The RGB LED indicates status, and the button toggles transmission.
- **Generic ESP32 / ESP32-S3**: Compatible with standard DevKits.

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

1.  **Pin**: Connect one end of your antenna to `GPIO 32` (default).
    *   *Note*: On M5Stack Atom Lite, this is a pin on the bottom header.
2.  **Ground**: Connect the other end of the antenna to a **Current Limiting Resistor** (220Ω - 330Ω), and then to `GND`.
    *   *Why a resistor?* It protects your ESP32 from drawing too much current, as the antenna coil has very low resistance.
3.  **Antenna Types**:
    *   **Ferrite Rod (Recommended)**: Scavenge one from an old radio clock or buy a ferrite antenna tuned to your target frequency. **Important**: You must match the frequency! (Tested range > 1m)
        *   **60 kHz**: For WWVB (USA), MSF (UK), JJY (Japan).
        *   **40 kHz**: For JJY (Japan).
        *   **77.5 kHz**: For DCF77 (Germany), BSF (Taiwan).
        *   **68.5 kHz**: For BPC (China).
    *   **Wire Loop**: A simple coil of wire (e.g., 30 turns of magnet wire around a water bottle). Short range, but works for any frequency.
4.  **Placement**: Place your target watch/clock **inside** or **immediately next to** the antenna coil. This is a low-power near-field emulator; range is typically < 10cm.

### 2. Software Configuration
1.  **WiFi Secrets**:
    *   Find the file `secrets_example.h` in the file list.
    *   **Duplicate** it and rename the copy to `secrets.h`.
    *   Open `secrets.h` and fill in your WiFi details:
        ```cpp
        #define WIFI_SSID "Your_WiFi_Name"
        #define WIFI_PASS "Your_WiFi_Password"
        ```
2.  **Timezone (Critical)**:
    *   Open `clocksync.ino`.
    *   Find the line `#define TZ "..."`.
    *   Change the string to match your station. ESP32 requires a POSIX TZ string (not Olson names like `America/Los_Angeles`).
    *   **WWVB (USA)**: Set TZ to your US timezone. WWVB frame time is always UTC (via `gmtime()`), so there is no double-offset risk — TZ only affects the DST status bits.

        | Region | POSIX TZ string |
        | :--- | :--- |
        | Pacific (LA, SF, Seattle) | `PST8PDT,M3.2.0,M11.1.0` |
        | Mountain (Denver, Phoenix†) | `MST7MDT,M3.2.0,M11.1.0` |
        | Central (Chicago, Dallas) | `CST6CDT,M3.2.0,M11.1.0` |
        | Eastern (NYC, Miami) | `EST5EDT,M3.2.0,M11.1.0` |
        | Hawaii | `HST10` |

        †Arizona (no DST): use `MST7` instead.

    *   **Other stations**: These transmit local time directly, so TZ must match the station's native timezone:

        | Station | Region | POSIX TZ string |
        | :--- | :--- | :--- |
        | JJY | Japan | `JST-9` |
        | DCF77 | Germany / Central Europe | `CET-1CEST,M3.5.0/2,M10.5.0/3` |
        | MSF | United Kingdom | `GMT0BST,M3.5.0/1,M10.5.0` |
        | BSF | Taiwan | `CST-8` |
        | BPC | China | `CST-8` |

    *   **Lookup**: Find any POSIX TZ string by city name in [this table](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv). On Mac/Linux: `tail -1 /etc/localtime`
3.  **Dependencies & Upload**:
    *   **Using Arduino IDE**:
        1.  **Add Board URL**: Go to `File` -> `Preferences` -> `Additional boards manager URLs` and add `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`.
        2.  **Install Board**: Go to `Tools` -> `Board` -> `Boards Manager`, search for **M5Stack** and install it.
        3.  **Install Library**: Go to `Sketch` -> `Include Library` -> `Manage Libraries...`. Search for and install **M5Atom** by M5Stack.
        4.  **Select & Upload**: Select **M5Stack-ATOM** (or "ESP32 Dev Module") from the `Tools` -> `Board` menu, connect your device, and click Upload (→).
    *   **Using arduino-cli**:
        ```bash
        arduino-cli core update-index --additional-urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
        arduino-cli core install m5stack:esp32 --additional-urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
        arduino-cli lib install M5Atom
        arduino-cli compile --fqbn m5stack:esp32:m5stack_atom clocksync.ino
        arduino-cli upload -p /dev/ttyUSB0 --fqbn m5stack:esp32:m5stack_atom clocksync.ino
        ```

### 3. Usage

Once running, the device acts as a time signal transmitter.
- **Status LED**: Indicates transmission is active.
- **Button**: Press to toggle transmission ON/OFF.
- **Web UI**: Visit `http://clocksync.local` (or the device IP) to view status.

## Command Reference

Control the device via Serial Monitor (115200 baud) or via HTTP (`http://clocksync.local/cmd?c=<command>`).

| Command | Description | Example |
| :--- | :--- | :--- |
| **Stations** | | |
| `sj` | Set station to **JJY (40 kHz)** (Fukushima) | `sj` |
| `sk` | Set station to **JJY (60 kHz)** (Fukuoka) | `sk` |
| `sw` | Set station to **WWVB (60 kHz)** (USA) | `sw` |
| `sd` | Set station to **DCF77 (77.5 kHz)** (Germany) | `sd` |
| `sm` | Set station to **MSF (60 kHz)** (UK) | `sm` |
| `st` | Set station to **BSF (77.5 kHz)** (Taiwan) | `st` |
| `sc` | Set station to **BPC (68.5 kHz)** (China) | `sc` |
| **Control** | | |
| `e1` / `e0` | **Enable / Disable Transmission** (Carrier & Logic) | `e1` |
| `pNN` | Set Radio Output Pin (GPIO `NN`) | `p25` |
| `g0` - `g3` | Set Drive Strength (0=Weakest, 3=Strongest) | `g2` |
| **Settings** | | |
| `y1` / `y0` | NTP Sync On / Off | `y1` |
| `l1` / `l0` | LED On / Off | `l1` |
| `x0` - `x2` | DCF/MSF DST Override (0=Std, 1=DST, 2=Auto) | `x2` |
| **Manual Time** | | |
| `dYYMMDD` | Manually set Date (Year, Month, Day) | `d231225` |
| `tHHmmSS` | Manually set Time (Hour, Min, Sec) | `t123000` |
| **Debug** | | |
| `h` | Show Help | `h` |
| `f` | Frequency Self-Test (Requires jumper `PIN_RADIO` -> `PIN_MEAS`) | `f` |
| `status` | (HTTP only) Get text status summary | |

## Supported Stations

| Station | Freq (kHz) | Location | Notes |
| :--- | :--- | :--- | :--- |
| **JJY** | 40 / 60 | Japan | Default is end-Low symbol shape. |
| **WWVB** | 60 | USA | UTC framing; DST bits derived from TZ setting. |
| **DCF77** | 77.5 | Germany | "Start-Low" encoding. |
| **MSF** | 60 | UK | Includes parity and DST bits. |
| **BSF** | 77.5 | Taiwan | Quaternary encoding (uncertified). |
| **BPC** | 68.5 | China | Quaternary encoding (uncertified). |

## Troubleshooting
*   **Clock isn't syncing**:
    *   **Proximity**: Move the clock *closer* to the antenna. It usually needs to be touching.
    *   **Interference**: LED power supplies and monitors cause noise. Move away from them.
    *   **Volume**: Increase "Volume" (Drive Strength) to `g3` using the Serial/Web command.
    *   **Antenna**: Ensure your antenna wire isn't broken and the resistor is secure.
*   **Clock shows wrong time**:
    *   **Timezone**: Check the `#define TZ` line in `clocksync.ino`. For WWVB, use a US POSIX timezone (see setup step 2).
    *   **DST off by 1 hour (WWVB)**: If TZ is set to `"UTC0"`, DST bits will always be 0 and your clock won't spring forward. Set TZ to your US timezone.
*   **Compilation Error**:
    *   `M5Atom.h: No such file`: You missed installing the M5Atom library in step 3.

## Technical Details

For deep dives into the signal encoding and protocol specifications used by this emulator, see [HOW_IT_WORKS.md](./HOW_IT_WORKS.md).

## Credits

- **Original Author**: SASAKI Taroh (tarohs) - [nisejjy](https://github.com/tarohs/nisejjy)
- **Reference**: [txtempus](https://github.com/hzeller/txtempus) by Henner Zeller (used for WWVB frame verification).
