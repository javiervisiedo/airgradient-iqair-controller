#!/usr/bin/env python3
"""Query Home Assistant's REST API from the host (stdlib only).

Config: HA_URL (e.g. http://homeassistant.local:8123) and HA_TOKEN (long-lived
access token, HA profile > Security), from env or the gitignored .env.
The token is sent as a Bearer header and is never printed.

  ha_query.py ping                 # GET /api/  -> {"message": "API running."}
  ha_query.py states [prefix]      # entity_id, state, friendly_name (filter by entity_id prefix)
  ha_query.py state <entity_id>    # full JSON for one entity (exit 1 if missing)
"""
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[4]


def cfg(key):
    v = os.environ.get(key)
    env = ROOT / ".env"
    if not v and env.exists():
        for line in env.read_text().splitlines():
            if line.strip().startswith(key + "="):
                v = line.split("=", 1)[1].strip().strip('"').strip("'")
    if not v:
        sys.exit(f"ERROR: set {key} (env or .env)")
    return v


def get(path):
    url = cfg("HA_URL").rstrip("/") + path
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {cfg('HA_TOKEN')}",
                                               "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read().decode() or "null")
    except urllib.error.HTTPError as e:
        hint = {401: "bad token", 404: "not found"}.get(e.code, "")
        print(f"HTTP {e.code} {hint} for {path}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as e:
        sys.exit(f"Network error: {e.reason}")


def main(a):
    if len(a) < 2:
        sys.exit(__doc__)
    if a[1] == "ping":
        print(json.dumps(get("/api/")))
    elif a[1] == "states":
        prefix = a[2] if len(a) > 2 else ""
        for s in sorted(get("/api/states"), key=lambda s: s["entity_id"]):
            if s["entity_id"].startswith(prefix):
                print(f"{s['entity_id']:<50} {str(s['state']):<15} "
                      f"{s.get('attributes', {}).get('friendly_name', '')}")
    elif a[1] == "state" and len(a) > 2:
        print(json.dumps(get(f"/api/states/{a[2]}"), indent=2))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
