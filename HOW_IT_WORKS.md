## clocksync: How it Works

An ESP32 pretends to be a low‑frequency time station. It generates a carrier at 40/60/68.5/77.5 kHz and wiggles the amplitude so your radio‑controlled clocks think they’re listening to JJY, WWVB, DCF77, MSF, BSF, or BPC. This document explains the common architecture and the nitty‑gritty differences between station protocols.

Fun fact: despite the mystique, these systems are glorified minute‑long barcodes sent at 1 symbol per second.

### What all stations have in common

- **Carrier generation**: One ESP32 `LEDC` channel generates the carrier. Amplitude modulation is done by toggling between two PWM duty levels:
  - High power ≈ 50% duty; Low power ≈ 5–6% duty (approximate “reduced carrier”).
- **Timing**: A high‑resolution `esp_timer` aligns to wall‑clock second boundaries, then ticks every 100 ms. Each second is 10 sub‑slots of 100 ms.
- **Amplitude schedule**: At every 100 ms tick, the sketch selects High or Low according to the current station’s symbol pattern for that second.
- **Minute framing**: All services use a 60‑second frame with a special “marker” second and data seconds encoding BCD or quaternary digits.
- **Time source**: NTP sets the ESP32’s RTC using a POSIX `TZ` string. The modulation uses the local time (`nowtm`) for most stations; WWVB uses UTC for numerical fields (details below).
- **Control/Status**:
  - USB Serial at 115200 for commands and debug.
  - Optional web UI (mDNS `http://clocksync.local/`) with a simple status page and a `GET /cmd?c=...` command endpoint.

### The on‑air second (symbol) shapes

Each second is encoded by how long the carrier is reduced (Low) within that second. Diagrams below show 10 ticks of 100 ms from left (0.0 s) to right (1.0 s).

Legend: `#` = High (normal carrier), `.` = Low (reduced carrier)

```text
WWVB (start‑Low, classic):
  0-bit    ..########      (Low 0.2 s, then High 0.8 s)
  1-bit    .....#####      (Low 0.5 s, then High 0.5 s)
  Marker   ........##      (Low 0.8 s, then High 0.2 s)

DCF77 (start‑Low):
  0-bit    .#########      (Low 0.1 s, then High 0.9 s)
  1-bit    ..########      (Low 0.2 s, then High 0.8 s)
  Marker   ##########      (Special: full‑second High; frame ends with missing 59th second)

MSF (start‑Low, 4 symbols; parity uses special variants):
  0-bit    .#########      (0.1 s Low)
  1-bit    ..########      (0.2 s Low)
  M-bit    .....#####      (0.5 s Low)  [minute marker]
  P0/P1    ...#######      (parity seconds use distinct shapes; see MSF section)

JJY (end‑Low in this sketch):
  0-bit    ########..      (High 0.8 s, Low 0.2 s)
  1-bit    #####.....      (High 0.5 s, Low 0.5 s)
  Marker   ##........      (High 0.2 s, Low 0.8 s)

BSF/BPC (quaternary, start‑Low with 4 symbols + marker): patterns map 2‑bit values to distinct Low durations.
```

Note: JJY here uses end‑Low shapes (Low at the tail of the second), which many clocks still accept since integration windows vary. DCF77/WWVB/MSF use start‑Low shapes as per common practice.

## Protocols by station

Below, bit/field positions refer to the minute frame defined in the code; unless noted, bit 0 is the first second of the minute. “Marker” seconds appear regularly to delimit fields.

### JJY (Japan)

- **Frequencies**: `JJY_E` 40 kHz (Fukushima), `JJY_W` 60 kHz (Fukuoka)
- **Symbol shapes**: end‑Low (see above)
- **Frame layout (60 seconds)**, markers at seconds 0, 9, 19, 29, 39, 49, 59.

```text
Secs  0.. 9 : (M), MIN10[3], 0, MIN1[4], (M)
     10..19 : 0, 0, HOUR10[2], 0, HOUR1[4], (M)
     20..29 : 0, 0, DOY100[2], DOY10[4], (M)
     30..39 : DOY1[4], 0, 0, PA1, PA2, 0, (M)
     40..49 : 0, YEAR[8], (M)
     50..59 : DOW[3], LS1, LS2, 0, 0, 0, 0, (M)
```

- **Parity**: `PA1 = parity(12..18)` minutes/hours; `PA2 = parity(1..8)` minutes.
- **Leap seconds**: `LS1/LS2` fields present but not actively set by the sketch.
- **Time base**: local time (`TZ`).

Disparity: JJY’s symbol shapes are end‑Low here; many references show start‑Low. If you find a receiver picky about edge placement, this is the knob.

### WWVB (United States)

- **Frequency**: 60 kHz
- **Symbol shapes**: start‑Low (classic WWVB 0/1/Marker at 0.2/0.5/0.8 s)
- **Time base**: UTC for numeric fields; DST flags represent US local DST “now” and “tomorrow”.

Frame content prepared exactly once per minute (bit 59 = first second):

```text
Minutes   (BCD, 2 digits)
Hours     (BCD, 2 digits)
Day-of-yr (BCD, 3 digits)
Year      (BCD, 2 digits)
LeapYear  (1 = leap year)
DST bits  bit57 = DST tomorrow?  bit58 = DST now?
Markers   seconds 0 and 9 are markers
```

Symbol timing (per second):

```text
Sec=0 or 9 : Marker  (Low 0.8 s)
Data 0-bit : Low 0.2 s
Data 1-bit : Low 0.5 s
```

Important nuance: The code computes DST flags using the process’ local `TZ` rather than hard‑coded US rules. If you set `TZ="UTC0"` (as the README suggests for framing), `tm_isdst` is always 0 → both flags transmit as 0. Use a US DST‑aware `TZ` if you need accurate WWVB DST bits, or adjust code to compute US DST independently of `TZ`.

Compatibility toggles (`n` next‑minute, `q` pending) are accepted but have no effect; framing follows the standard “txtempus” interpretation.

### DCF77 (Germany)

- **Frequency**: 77.5 kHz
- **Symbol shapes**: start‑Low (0 = 100 ms, 1 = 200 ms)
- **Time base**: local time (`TZ`), including seasonal flags Z1/Z2.

Frame layout (LSB→MSB within fields):

```text
 0..14 : reserved (0)
 15..19: reserved, then Z flags
   17  : Z1 (DST=1/STD=0)
   18  : Z2 (DST=0/STD=1)
 21..27: Minutes (BCD, LSB→MSB), P1 at 28
 29..34: Hours   (BCD),         P2 at 35
 36..41: Day     (BCD)
 42..44: DOW     (binary)
 45..49: Month   (BCD)
 50..57: Year    (BCD),         P3 at 58
 59    : Missing second (minute marker)
```

Note: Original comment hints DCF/MSF “forward 1 minute”; this sketch encodes the current minute values as shown (no +1 minute advance).

### MSF (United Kingdom)

- **Frequency**: 60 kHz
- **Symbol shapes**: 4 symbols; parity seconds use special variants.
- **Time base**: local time (`TZ`).

Fields (high‑level):

```text
Year (BCD, 8) → Month (BCD, 5) → Day (BCD, 6) → DOW (3)
→ Hour (BCD, 6) → Minute (BCD, 7)
Parities (4 seconds): Pyear, Pdate, Pdow, Ptime
Summertime indicator: dedicated second uses P0/P1 shape
```

Parity seconds transmit as `SP_P0` (parity 0) or `SP_P1` (parity 1), which have distinct Low durations so receivers can identify parity vs data.

### BSF (Taiwan)

- **Frequency**: 77.5 kHz
- **Encoding**: quaternary (2‑bit symbols) + marker symbol.
- **Status**: Marked “not certified” in code.

High‑level mapping (quaternary BCD across fields; parity bits qparity indicated in comments): minutes, hours×2, day×2, DOW split, month, year×2, with two parity checks. Minute markers at 0, 39, 59 seconds.

### BPC (China)

- **Frequency**: 68.5 kHz
- **Encoding**: quaternary (2‑bit symbols) + marker symbol, with three repeats.

Layout (conceptual):

```text
Preamble/Marker; P1; P2; Hour(12h); Minute; DOW; Par <hmDOW>
Date block: Day; Month; Year; Par <DMY>
The 20-second payload repeats 3× to fill 60 seconds
```

The sketch copies seconds 2..19 into 20..39 and 40..59, yielding 3 identical sub‑frames per minute.

## Architecture details

- **Aligned start**: A one‑shot timer fires exactly at the next real‑time second boundary (computed from `gettimeofday()`), then starts a periodic 100 ms timer.
- **Carrier control**: Each 100 ms tick toggles the LEDC duty between high and low power. WWVB computes “low‑ticks” directly; other stations use a per‑symbol `secpattern` lookup.
- **Persistence**: Selected station, radio pin, NTP on/off, LED, DST override, and WWVB pending override persist via `Preferences`.
- **Self‑test**: `f` command measures carrier by counting edges on a measurement pin for ~200 ms.

## Command quick reference (Serial and HTTP `/cmd?c=`)

```text
h            help
y0|y1        NTP sync off/on
dYYMMDD      set date (local TZ)
tHHmmSS      set time (local TZ)
z0|z1        buzzer off/on
l0|l1        LED off/on
pNN          set radio GPIO (e.g., p25)
f            frequency self-test (jumper radio to meas pin; default meas pin GPIO33)
n0|n1        WWVB next-minute (compat only; no effect)
q0|q1|q2     WWVB pending (compat only; no effect)
x0|x1|x2     DST STD/DST/AUTO (applies to DCF/MSF)
s[jkwtmc]    station: JJY_E, JJY_W, WWVB, DCF77, MSF, BPC (BSF via 't')
```

## Rough edges, disparities, and notes

- **WWVB DST bits vs TZ**: DST “now/tomorrow” flags are derived from the process’ `TZ`. If `TZ` is `UTC0` (as recommended for WWVB framing), both flags will be 0 all year. Consider either (a) setting a US DST‑aware `TZ`, or (b) computing US DST independently of `TZ` for the WWVB flags.
- **JJY symbol phase**: JJY uses end‑Low symbols here; many references show start‑Low. Most receivers will accept either; a few might be edge‑sensitive.
- **DCF77/MSF minute advance**: The header comment says “forwards 1 minute”, but the code encodes the current minute. If your receiver expects “next minute” encoding, you may need to advance fields by 1 minute at frame build time.
- **Leap second flags (JJY)**: `LS1/LS2` are present in the frame map but not populated by this sketch.
- **Amplitude ratio**: The reduced carrier is approximated by a lower PWM duty, not a calibrated dB reduction. This is usually fine for near‑field coupling.
- **BSF/BPC**: Marked as “not certified” by the original author; expect receiver variability.
- **Marker conventions**: DCF77 ends the minute with a missing 59th second. WWVB uses 0 and 9 as marker seconds. JJY uses markers every 10 seconds. MSF uses a distinct “M” symbol at its marker second.

## Quick mental model (ASCII)

One minute, 60 columns, with marker seconds (M) and data seconds (D):

```text
JJY:  M D D D D D D D D M  D D D D D D D D M  D D D D D D D D M  D D D D D D D D M  D D D D D D D D M  D D D D D D D D M
WWVB: M D D D D D D D D M  D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D D
DCF : D D D D D D D D D D  D D D D D D D D D D  D D D D D D D D D D  D D D D D D D D D D  D D D D D D D D D D  D D D D D D  -
MSF : D D D D D D D D D D  D D D D D D D D D D  D D D D D D D D D D  D D D D D D D D D D  P P P P S  (P=parity, S=summertime)
```

If you can picture those markers and the Low‑duration patterns per second, you already “get” how these stations work.


