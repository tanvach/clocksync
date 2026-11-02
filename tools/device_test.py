#!/usr/bin/env python3
"""End-to-end checks against a running clocksync device over its HTTP API.

    python3 tools/device_test.py clocksync.local

Checks status, NTP, the clock, the WWVB frame being transmitted against the
current UTC minute, the API guards and validation, and the carrier frequency
the chip reports for each station. Briefly switches stations and time zone,
then restores the original settings.
"""
import datetime
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

if len(sys.argv) != 2:
    sys.exit(__doc__)
BASE = "http://%s" % sys.argv[1]
results = []


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def req(path, data=None, header=True, host_header=None):
    body = urllib.parse.urlencode(data).encode() if data is not None else None
    r = urllib.request.Request(BASE + path, data=body)
    if header and data is not None:
        r.add_header("X-Clocksync", "1")
    if host_header:
        r.add_header("Host", host_header)
    try:
        with OPENER.open(r, timeout=10) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def status():
    return json.loads(req("/api/status")[1])


def set_(**fields):
    code, body = req("/api/set", fields)
    return code, json.loads(body)


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + (" -- " + detail if detail else ""))


def decode_wwvb(frame):
    bit = lambda i: frame[i] == "1"
    val = lambda pos, weights: sum(w for i, w in enumerate(weights) if bit(pos + i))
    minute = 10 * val(1, [4, 2, 1]) + val(5, [8, 4, 2, 1])
    hour = 10 * val(12, [2, 1]) + val(15, [8, 4, 2, 1])
    doy = 100 * val(22, [2, 1]) + 10 * val(25, [8, 4, 2, 1]) + val(30, [8, 4, 2, 1])
    year = 10 * val(45, [8, 4, 2, 1]) + val(50, [8, 4, 2, 1])
    marks = all(frame[i] == "M" for i in (0, 9, 19, 29, 39, 49, 59))
    return minute, hour, doy, year, marks, frame[57:59], frame[36:39]


original = status()
restore = {"station": original["station"], "tz": original["tz"], "drive": original["drive"]}
if original["tz"] == "custom":
    restore["posix"] = original["posix"]

try:
    s = original
    check("status", s.get("ok") is True, "v%s on %s" % (s.get("version"), s.get("board")))
    check("Wi-Fi connected", s["wifi"]["connected"], "%s dBm" % s["wifi"]["rssi"])
    check("time valid and NTP synced", s["timeValid"] and s["lastSync"] >= 0, "last sync %s s ago" % s["lastSync"])
    skew = abs(s["ms"] / 1000 - time.time())
    check("device clock matches this computer", skew < 2, "%.3f s incl. request latency" % skew)
    check("transmitting", s["state"] == "transmitting", s["state"])

    # The WWVB frame being sent must decode to the current UTC minute.
    set_(station="wwvb", tz="")
    time.sleep(0.5)
    s = status()
    if s["frame"]:
        t = datetime.datetime.fromtimestamp(s["ms"] / 1000, datetime.timezone.utc)
        minute, hour, doy, year, marks, dst, dut1 = decode_wwvb(s["frame"])
        expect = (t.minute, t.hour, t.timetuple().tm_yday, t.year % 100)
        check("WWVB frame decodes to the current UTC minute", marks and (minute, hour, doy, year) == expect,
              "frame %02d:%02d day %d year %02d" % (hour, minute, doy, year))
        check("WWVB DST and DUT1 sign bits", dut1 == "101" and dst in ("00", "01", "10", "11"), "DST %s" % dst)
    else:
        check("WWVB frame present", False, "no frame; is TX on and the clock set?")

    # Guards and validation.
    code, _ = req("/api/set", {"led": "1"}, header=False)
    check("POST without X-Clocksync rejected", code == 403, str(code))
    code, _ = req("/cmd?c=reboot")
    check("/cmd reboot without header rejected", code == 403, str(code))
    code, body = req("/cmd?c=status")
    check("/cmd status allowed without header", code == 200 and body.startswith("OK"))
    for name, fields in (("invalid POSIX rule", {"tz": "custom", "posix": "US/Eastern"}),
                         ("non-numeric pin", {"pin": "abc"}),
                         ("over-long name", {"host": "a-very-long-device-name-over-limit"}),
                         ("unknown station", {"station": "nope"})):
        code, body = req("/api/set", fields)
        check(name + " rejected", code == 400, json.loads(body).get("error", ""))
    code, body = req("/status.txt")
    check("status.txt", code == 200 and body.startswith("clocksync v"))
    code, body = req("/api/zones")
    check("zone table", code == 200 and len(body.split("\n")) > 400, "%d zones" % len(body.strip().split("\n")))
    code, _ = req("/", host_header="example.com")
    check("LAN request with another Host is not captive-redirected", code == 200, str(code))

    # Time zones.
    code, j = set_(tz="Europe/Berlin")
    check("set zone Europe/Berlin", code == 200 and j["posix"].startswith("CET-1CEST") and j["offset"] in (3600, 7200),
          "%s, offset %s" % (j.get("abbr"), j.get("offset")))
    code, j = set_(tz="")
    check("automatic zone", code == 200 and j["tz"] == "", j.get("tzEff", ""))

    # Each station reconfigures the carrier; ledcHz is read back from the LEDC registers.
    for key, freq in (("jjy40", 40000), ("jjy60", 60000), ("dcf77", 77500), ("bsf", 77500), ("bpc", 68500),
                      ("msf", 60000), ("wwvb", 60000)):
        set_(station=key)
        time.sleep(0.5)
        j = status()
        ppm = (j["ledcHz"] - freq) / freq * 1e6 if j["ledcHz"] else float("inf")
        check("%s carrier" % key, j["station"] == key and j["state"] == "transmitting" and abs(ppm) < 100,
              "%s Hz (%+.0f ppm)" % (j["ledcHz"], ppm))
finally:
    code, j = set_(**restore)
    print("restored station %s, zone %s, drive %s" % (j.get("station"), j.get("tzEff"), j.get("drive")))

print("\n%d/%d checks passed" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
