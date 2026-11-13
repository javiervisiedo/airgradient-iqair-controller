#!/usr/bin/env python3
"""Mock AirGradient server for simulation and bench testing (stdlib only).

Serves both API shapes so firmware can be pointed at it instead of the real
service (never send the real token through Wokwi's public gateway):

  Cloud shape: GET /public/api/v1/ping
               GET /public/api/v1/locations/<id>/measures/current?token=...
  Local shape: GET /measures/current

Usage:
  mock_airgradient.py [--port 8080] [--scenario ok] [--pm25 8.5] [--nox 1]
                      [--serial mock00000001]
                      [--pm25-series 5,12,40,60,20,5]   # one value per request, loops
                      [--sequence ok:3,frozen:4,ok:1]   # scenario per request, loops

Runtime control (no restart needed):
  curl 'localhost:8080/_mock/set?scenario=frozen'
  curl 'localhost:8080/_mock/set?pm25=42.0&tvoc=180&nox=6'
  curl 'localhost:8080/_mock/state'

Scenarios:
  ok            200, full payload, fresh timestamp, advancing boot counter
  frozen        200, valid JSON but local 'boot' counter stops advancing (TC-09)
  wrong_serial  200, mismatched serialno (test SERIAL_MISMATCH)
  null_pm       200, pm02 / pm02Compensated / pm02_corrected are null
  missing       200, only rco2 present
  stale         200, cloud timestamp 30 min old (local API has no timestamp; use 'frozen')
  unauthorized  401
  no_data       404
  server_error  500
  slow          sleeps 20 s before answering (exercise client timeouts)
  malformed     200, truncated JSON
  html          200, text/html body (captive-portal style)
Token handling: any non-empty token is accepted except "bad" (-> 401). Tokens are never logged.
"""
import argparse
import itertools
import json
import re
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

SCENARIOS = ["ok", "frozen", "wrong_serial", "null_pm", "missing", "stale", "unauthorized",
             "no_data", "server_error", "slow", "malformed", "html"]
STATE = {"scenario": "ok", "pm25": 8.5, "tvoc": 100, "nox": 1, "co2": 650,
         "serial": "mock00000001", "boot": 1, "requests": 0}
LOCK = threading.Lock()
SERIES = {"pm25": None, "seq": None}


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%S.000Z")


def next_values():
    with LOCK:
        STATE["requests"] += 1
        if SERIES["pm25"]:
            STATE["pm25"] = next(SERIES["pm25"])
        if SERIES["seq"]:
            STATE["scenario"] = next(SERIES["seq"])
        if STATE["scenario"] != "frozen":
            STATE["boot"] += 1
        return dict(STATE)


def cloud_payload(s, loc_id):
    now = datetime.now(timezone.utc)
    ts = now - timedelta(minutes=30) if s["scenario"] == "stale" else now - timedelta(seconds=20)
    pm = s["pm25"]
    serial = "0000deadbeef" if s["scenario"] == "wrong_serial" else s["serial"]
    body = {"locationId": loc_id, "locationName": "Mock room", "locationType": "indoor",
            "serialno": serial, "model": "I-9PSL", "firmwareVersion": "3.3.9",
            "pm01": round(pm * 0.7, 1), "pm02": round(pm * 1.15, 1), "pm10": round(pm * 1.3, 1),
            "pm01_corrected": round(pm * 0.6, 1), "pm02_corrected": round(pm, 1),
            "pm10_corrected": round(pm * 1.2, 1), "pm003Count": int(pm * 60),
            "atmp": 26.4, "atmp_corrected": 25.9, "rhum": 58, "rhum_corrected": 60,
            "rco2": s["co2"], "rco2_corrected": s["co2"], "tvocIndex": s["tvoc"], "noxIndex": s["nox"],
            "wifi": -55, "timestamp": iso(ts)}
    if s["scenario"] == "null_pm":
        body.update(pm02=None, pm02_corrected=None)
    if s["scenario"] == "missing":
        body = {"locationId": loc_id, "serialno": serial, "rco2": s["co2"], "timestamp": iso(ts)}
    return body


def local_payload(s):
    pm = s["pm25"]
    serial = "0000deadbeef" if s["scenario"] == "wrong_serial" else s["serial"]
    body = {"wifi": -55, "serialno": serial, "rco2": s["co2"], "pm01": round(pm * 0.7),
            "pm02": round(pm * 1.15), "pm10": round(pm * 1.3), "pm02Compensated": round(pm, 1),
            "pm003Count": int(pm * 60), "atmp": 26.4, "atmpCompensated": 25.9, "rhum": 58,
            "rhumCompensated": 60, "tvocIndex": s["tvoc"], "tvocRaw": 31000, "noxIndex": s["nox"],
            "noxRaw": 16000, "boot": s["boot"], "bootCount": s["boot"], "ledMode": "pm",
            "firmware": "3.3.9", "model": "I-9PSL"}
    if s["scenario"] == "null_pm":
        body.update(pm02=None, pm02Compensated=None)
    if s["scenario"] == "missing":
        body = {"serialno": serial, "rco2": s["co2"]}
    return body


class Handler(BaseHTTPRequestHandler):
    server_version = "mock-airgradient/1"

    def log_message(self, fmt, *args):  # mask tokens in access log
        msg = re.sub(r"token=[^&\s\"]*", "token=***", fmt % args)
        print(f"{time.strftime('%H:%M:%S')} {self.client_address[0]} {msg}", flush=True)

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else (
            json.dumps(body).encode() if not isinstance(body, str) else body.encode())
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/_mock/state":
            return self.send(200, STATE)
        if u.path == "/_mock/set":
            with LOCK:
                if "scenario" in q:
                    if q["scenario"] not in SCENARIOS:
                        return self.send(400, {"error": f"scenarios: {SCENARIOS}"})
                    STATE["scenario"] = q["scenario"]
                    SERIES["seq"] = None
                for k in ("pm25", "tvoc", "nox", "co2", "serial"):
                    if k in q:
                        if k == "serial":
                            STATE[k] = q[k]
                        elif k == "pm25":
                            STATE[k] = float(q[k])
                            SERIES["pm25"] = None
                        else:
                            STATE[k] = int(q[k])
            return self.send(200, STATE)
        if u.path == "/public/api/v1/ping":
            return self.send(200, b"", "text/plain")

        m = re.fullmatch(r"/public/api/v1/locations/(\d+)/measures/current", u.path)
        is_local = u.path == "/measures/current"
        if not m and not is_local:
            return self.send(404, {"message": "not found"})
        if m and (not q.get("token") or q.get("token") == "bad"):
            return self.send(401, {"message": "Unauthorized"})

        s = next_values()
        sc = s["scenario"]
        if sc == "unauthorized":
            return self.send(401, {"message": "Unauthorized"})
        if sc == "no_data":
            return self.send(404, {"message": "No data available"})
        if sc == "server_error":
            return self.send(500, {"message": "Internal error"})
        if sc == "slow":
            time.sleep(20)
        if sc == "malformed":
            return self.send(200, '{"pm02": 12.3, "rco2": 6')
        if sc == "html":
            return self.send(200, "<html><body>Login required</body></html>", "text/html")
        self.send(200, local_payload(s) if is_local else cloud_payload(s, int(m.group(1))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--scenario", choices=SCENARIOS, default="ok")
    ap.add_argument("--pm25", type=float, default=8.5)
    ap.add_argument("--nox", type=int, default=1)
    ap.add_argument("--serial", default="mock00000001")
    ap.add_argument("--pm25-series", help="comma list of PM2.5 values, one per data request (loops)")
    ap.add_argument("--sequence", help="e.g. ok:3,stale:2 (scenario:count, loops)")
    a = ap.parse_args()
    STATE.update(scenario=a.scenario, pm25=a.pm25, nox=a.nox, serial=a.serial)
    if a.pm25_series:
        SERIES["pm25"] = itertools.cycle(float(x) for x in a.pm25_series.split(","))
    if a.sequence:
        seq = []
        for part in a.sequence.split(","):
            name, _, n = part.partition(":")
            if name not in SCENARIOS:
                ap.error(f"unknown scenario {name}")
            seq += [name] * int(n or 1)
        SERIES["seq"] = itertools.cycle(seq)
    print(f"mock AirGradient on http://{a.bind}:{a.port}  scenario={a.scenario}", flush=True)
    ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
