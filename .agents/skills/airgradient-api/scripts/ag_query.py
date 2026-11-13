#!/usr/bin/env python3
"""Host-side AirGradient API explorer (stdlib only).

Token is read from $AG_TOKEN or from a .env file in the project root
(AG_TOKEN=...). The token is masked in all output.

Usage:
  ag_query.py ping
  ag_query.py place
  ag_query.py locations                 # summary table of all locations
  ag_query.py current <locationId>      # full JSON for one location
  ag_query.py raw <locationId> [N]      # N most recent raw measures (default 5)
  ag_query.py alarms
  ag_query.py local <serialno>          # LAN API: http://airgradient_<serial>.local
"""
import json
import os
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

BASE = "https://api.airgradient.com/public/api/v1"
ROOT = pathlib.Path(__file__).resolve().parents[4]


def load_token(required=True):
    tok = os.environ.get("AG_TOKEN")
    env = ROOT / ".env"
    if not tok and env.exists():
        for line in env.read_text().splitlines():
            if line.strip().startswith("AG_TOKEN="):
                tok = line.split("=", 1)[1].strip().strip('"').strip("'")
    if required and not tok:
        sys.exit("ERROR: set AG_TOKEN env var or add AG_TOKEN=... to .env")
    return tok


def mask(url):
    return url.split("token=")[0] + "token=***" if "token=" in url else url


def get(url, timeout=10):
    print(f"GET {mask(url)}", file=sys.stderr)
    req = urllib.request.Request(url, headers={"Accept": "application/json",
                                               "User-Agent": "iqair-controller-dev"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read().decode()
            print(f"HTTP {r.status} ({len(body)} bytes)", file=sys.stderr)
            return json.loads(body) if body.strip() else None
    except urllib.error.HTTPError as e:
        hint = {401: "bad/missing token", 404: "no data for this location"}.get(e.code, "")
        sys.exit(f"HTTP {e.code} {hint}")
    except urllib.error.URLError as e:
        sys.exit(f"Network error: {e.reason}")


def api(path, **params):
    params["token"] = load_token()
    return get(f"{BASE}{path}?{urllib.parse.urlencode(params)}")


def age(ts):
    if not ts:
        return "?"
    t = datetime.fromisoformat(ts.replace("Z", "+00:00"))
    return f"{int((datetime.now(timezone.utc) - t).total_seconds())}s"


def pick(m, *keys):
    for k in keys:
        if m.get(k) is not None:
            return m[k]
    return None


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    if cmd == "ping":
        get(f"{BASE}/ping")
        print("ok")
    elif cmd == "place":
        print(json.dumps(api("/place"), indent=2))
    elif cmd == "locations":
        rows = api("/locations/measures/current") or []
        print(f"{'locationId':>10}  {'name':<22} {'serial':<14} {'model':<10} "
              f"{'pm2.5':>6} {'co2':>5} {'age':>6}")
        for m in rows:
            print(f"{m.get('locationId', ''):>10}  {str(m.get('locationName', ''))[:22]:<22} "
                  f"{str(m.get('serialno', '')):<14} {str(m.get('model', '')):<10} "
                  f"{str(pick(m, 'pm02_corrected', 'pm02')):>6} "
                  f"{str(pick(m, 'rco2_corrected', 'rco2')):>5} {age(m.get('timestamp')):>6}")
    elif cmd == "current" and len(argv) > 2:
        m = api(f"/locations/{argv[2]}/measures/current")
        print(json.dumps(m, indent=2))
        print(f"# age={age(m.get('timestamp'))}", file=sys.stderr)
    elif cmd == "raw" and len(argv) > 2:
        n = int(argv[3]) if len(argv) > 3 else 5
        print(json.dumps((api(f"/locations/{argv[2]}/measures/raw") or [])[:n], indent=2))
    elif cmd == "alarms":
        print(json.dumps(api("/alarms/triggered"), indent=2))
    elif cmd == "local" and len(argv) > 2:
        print(json.dumps(get(f"http://airgradient_{argv[2].lower()}.local/measures/current"),
                         indent=2))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
