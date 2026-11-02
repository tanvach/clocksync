#!/usr/bin/env python3
"""Serve the clocksync web UI from webui.h with a fake device behind it.

Lets you try the UI in a desktop browser without flashing anything:

    python3 tools/mock_server.py            # first-boot setup mode
    python3 tools/mock_server.py --configured
    open http://127.0.0.1:8765/

In the fake Wi-Fi, passwords starting with "wrong" fail and others connect.
"""
import argparse
import datetime
import json
import os
import re
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from zoneinfo import ZoneInfo

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_page():
    src = open(os.path.join(ROOT, "webui.h"), encoding="utf-8").read()
    return src.split('R"HTML(', 1)[1].rsplit(')HTML"', 1)[0]


def read_zones():
    src = open(os.path.join(ROOT, "tzdata.h"), encoding="utf-8").read()
    lines = re.findall(r'^  "(.*)\\n"$', src, re.M)
    return [l.replace("\\t", "\t") for l in lines]


JJY = ["HHHHHHHHLL", "HHHHHLLLLL", "HHHHHHHHLL", "HHHHHHHHLL", "HHLLLLLLLL"]
SHAPES = {
    "jjy40": JJY, "jjy60": JJY,
    "wwvb": ["LLHHHHHHHH", "LLLLLHHHHH", "LLHHHHHHHH", "LLHHHHHHHH", "LLLLLLLLHH"],
    "dcf77": ["LHHHHHHHHH", "LLHHHHHHHH", "LHHHHHHHHH", "LHHHHHHHHH", "HHHHHHHHHH"],
    "bsf": ["LLHHHHHHHH", "LLLLHHHHHH", "LLLLLLLLHH", "LLLLLLHHHH", "HHHHHHHHHH"],
    "msf": ["LHHHHHHHHH", "LLHHHHHHHH", "LHLHHHHHHH", "LLLHHHHHHH", "LLLLLHHHHH"],
    "bpc": ["LHHHHHHHHH", "LLHHHHHHHH", "LLLHHHHHHH", "LLLLHHHHHH", "LLLLLLLLLL"],
}
STATIONS = [
    dict(key="jjy40", name="JJY 40 kHz", region="Japan (Fukushima)", freq=40000, tz="Asia/Tokyo", utc=False),
    dict(key="jjy60", name="JJY 60 kHz", region="Japan (Fukuoka)", freq=60000, tz="Asia/Tokyo", utc=False),
    dict(key="wwvb", name="WWVB", region="USA (Colorado)", freq=60000, tz="America/Los_Angeles", utc=True),
    dict(key="dcf77", name="DCF77", region="Germany (Mainflingen)", freq=77500, tz="Europe/Berlin", utc=False),
    dict(key="bsf", name="BSF", region="Taiwan", freq=77500, tz="Asia/Taipei", utc=False),
    dict(key="msf", name="MSF", region="UK (Anthorn)", freq=60000, tz="Europe/London", utc=False),
    dict(key="bpc", name="BPC", region="China (Shangqiu)", freq=68500, tz="Asia/Shanghai", utc=False),
]
# Golden frames from test/test_timecode.cpp, good enough for the display.
FRAMES = {
    "jjy40": "M01100100M000100010M001100000M010100010M000100110M000000000M",
    "wwvb": "M01100000M000001000M001100000M010100101M000000010M011000001M",
    "dcf77": "0000000000" "0000000010" "1101011000" "1001001000" "0011110001" "011001001M",
    "msf": "M000000000" "0000000001" "0011010001" "0000010000" "1001001101" "0101113310",
}


class Device:
    def __init__(self, configured):
        self.zones = dict(z.split("\t") for z in read_zones())
        self.station = "wwvb"
        self.tx = True
        self.tz = ""
        self.posix = ""
        self.dst = 2
        self.ntp = True
        self.ntp_server = "pool.ntp.org"
        self.pin = 32
        self.drive = 2
        self.led = True
        self.host = "clocksync"
        self.ssid = "HomeNet" if configured else ""
        self.connected = configured
        self.ap = not configured
        self.ap_manual = False
        self.ap_close_at = None  # like the firmware: the setup hotspot closes 20 s after connecting
        self.time_valid = configured
        self.clock_offset = 0.0
        self.pending = None  # (ssid, ok, when)
        self.error = ""
        self.scan_started = 0.0
        self.boot = time.time()
        self.last_sync = time.time() if configured else None

    def now(self):
        return time.time() + self.clock_offset

    def step(self):
        if self.pending and time.time() >= self.pending[2]:
            ssid, ok, _ = self.pending
            self.pending = None
            self.connected = ok
            self.error = "" if ok else "wrong password?"
            if ok:
                self.ap_manual = False
                self.ap_close_at = time.time() + 20 if self.ap else None
                self.time_valid = True
                self.last_sync = time.time()

    def station_info(self):
        return next(s for s in STATIONS if s["key"] == self.station)

    def status(self):
        self.step()
        if self.ap_close_at and time.time() >= self.ap_close_at:
            self.ap, self.ap_close_at = False, None
        st = self.station_info()
        zone = self.tz if self.tz and self.tz != "custom" else (st["tz"] if not self.tz else "UTC")
        now = self.now()
        local = datetime.datetime.fromtimestamp(now, ZoneInfo(zone))
        transmitting = self.tx and self.time_valid
        return dict(
            ok=True, version="2.0.0", board="Mock board", host=self.host, uptime=int(time.time() - self.boot),
            hostSaved=self.host, ms=int(now * 1000), timeValid=self.time_valid, station=self.station, tx=self.tx,
            state="transmitting" if transmitting else ("waiting" if self.tx else "off"),
            freq=st["freq"], ledcHz=st["freq"] if transmitting else 0,
            tz=self.tz, tzEff="custom" if self.tz == "custom" else zone,
            posix=self.posix if self.tz == "custom" else self.zones.get(zone, ""),
            offset=int(local.utcoffset().total_seconds()), isDst=bool(local.dst()), abbr=local.tzname(),
            dstMode=self.dst, ntp=self.ntp, ntpServer=self.ntp_server,
            lastSync=int(time.time() - self.last_sync) if self.last_sync and self.ntp else -1,
            pin=self.pin, pins=[19, 21, 22, 23, 25, 26, 32, 33], drive=self.drive, led=self.led, measPin=33,
            frame=FRAMES.get(self.station, FRAMES["jjy40"]) if transmitting else "",
            wifi=dict(ssid=self.ssid, connected=self.connected, connecting=bool(self.pending),
                      error="" if self.connected else self.error, ip="192.168.1.50" if self.connected else "",
                      rssi=-58 if self.connected else 0, ap=self.ap, apManual=self.ap_manual, apSsid=self.host + "-setup",
                      apIp="192.168.4.1"),
            stations=[dict(s, shapes=SHAPES[s["key"]]) for s in STATIONS],
        )


class Handler(BaseHTTPRequestHandler):
    dev = None

    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def json(self, obj, code=200):
        self.send(code, json.dumps(obj))

    def error(self, msg, code=400):
        self.json(dict(ok=False, error=msg), code)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        d = self.dev
        if url.path == "/":
            self.send(200, read_page(), "text/html; charset=utf-8")
        elif url.path == "/api/status":
            self.json(d.status())
        elif url.path == "/api/zones":
            self.send(200, "".join(z + "\n" for z in read_zones()), "text/plain")
        elif url.path == "/api/scan":
            if "start" in q:
                d.scan_started = time.time()
            if time.time() - d.scan_started < 2:
                self.json(dict(ok=True, scanning=True))
            else:
                self.json(dict(ok=True, networks=[
                    dict(ssid="HomeNet", rssi=-48, open=False), dict(ssid="Neighbor 5G", rssi=-71, open=False),
                    dict(ssid="Cafe \"Guest\" <wifi>", rssi=-80, open=True)]))
        elif url.path == "/cmd":
            self.send(200, "OK\nmock device: %s\n" % q.get("c", ""), "text/plain")
        elif url.path == "/status.txt":
            self.send(200, json.dumps(d.status(), indent=1), "text/plain")
        else:
            self.send(404, "not found\n", "text/plain")

    def do_POST(self):
        if "X-Clocksync" not in self.headers:
            return self.error("missing X-Clocksync header", 403)
        n = int(self.headers.get("Content-Length") or 0)
        p = dict(urllib.parse.parse_qsl(self.rfile.read(n).decode(), keep_blank_values=True))
        d = self.dev
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/set":
            if "station" in p:
                if p["station"] not in [s["key"] for s in STATIONS]:
                    return self.error("unknown station")
                d.station = p["station"]
            if "tz" in p:
                if p["tz"] == "custom":
                    if not re.match(r"^[A-Za-z<][\x21-\x7e]{0,62}$", p.get("posix", "")):
                        return self.error("invalid POSIX TZ string")
                    d.posix = p["posix"]
                elif p["tz"] and p["tz"] not in d.zones:
                    return self.error("unknown time zone")
                d.tz = p["tz"]
            for key, attr, conv in (("tx", "tx", lambda v: v == "1"), ("ntp", "ntp", lambda v: v == "1"),
                                    ("led", "led", lambda v: v == "1"), ("dst", "dst", int), ("pin", "pin", int),
                                    ("drive", "drive", int), ("ntpServer", "ntp_server", str), ("host", "host", str)):
                if key in p:
                    setattr(d, attr, conv(p[key]))
            return self.json(d.status())
        if path == "/api/wifi":
            if not p.get("ssid"):
                return self.error("network name must be 1-32 characters")
            if 0 < len(p.get("pass", "")) < 8:
                return self.error("password must be 8-63 characters (or empty for an open network)")
            d.ssid = p["ssid"]
            d.connected = False
            d.error = ""
            d.pending = (p["ssid"], not p.get("pass", "").startswith("wrong"), time.time() + 3)
            return self.json(dict(ok=True))
        if path == "/api/time":
            d.clock_offset = int(p["ms"]) / 1000 - time.time()
            d.time_valid = True
            return self.json(d.status())
        if path == "/api/setup":
            d.ap, d.ap_manual = True, True
            return self.json(dict(ok=True))
        if path == "/api/reset":
            d.station, d.tz, d.tx, d.dst = "wwvb", "", True, 2
            return self.json(d.status())
        if path in ("/api/reboot", "/api/forget"):
            return self.json(dict(ok=True))
        self.error("not found", 404)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--configured", action="store_true", help="start connected instead of in setup mode")
    args = ap.parse_args()
    Handler.dev = Device(args.configured)
    print("mock clocksync on http://127.0.0.1:%d/" % args.port)
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
