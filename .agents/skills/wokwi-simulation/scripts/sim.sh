#!/usr/bin/env bash
# Wokwi simulation wrapper for the AtomS3 Lite firmware (run from anywhere).
#
#   sim.sh check                         # prerequisites: wokwi-cli, token, wokwi.toml, diagram.json, firmware
#   sim.sh lint                          # validate diagram.json
#   sim.sh run [--timeout MS] [--scenario FILE] [--expect TEXT] [--fail TEXT] [-- extra wokwi-cli args]
#   sim.sh ir  [--expect speed_1,power_off]   # decode NEC frames from the last run's VCD
#   sim.sh usage                         # simulated minutes used in the last 30 days (local ledger)
#
# Outputs go to sim-out/: serial.log, ir.vcd, usage.log.
# Token: $WOKWI_CLI_TOKEN, or WOKWI_CLI_TOKEN=... in the gitignored .env. Never printed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
SKILL="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/sim-out"
FREE_QUOTA_MIN=50
cd "$ROOT"

load_token() {
  if [[ -z "${WOKWI_CLI_TOKEN:-}" && -f .env ]]; then
    WOKWI_CLI_TOKEN="$(grep -E '^WOKWI_CLI_TOKEN=' .env | head -1 | cut -d= -f2- | tr -d "\"'" || true)"
  fi
  [[ -n "${WOKWI_CLI_TOKEN:-}" ]] || { echo "ERROR: WOKWI_CLI_TOKEN not set (env or .env). Get one at https://wokwi.com/dashboard/ci" >&2; return 1; }
  [[ "$WOKWI_CLI_TOKEN" == wok_* && ${#WOKWI_CLI_TOKEN} -eq 44 ]] || echo "WARN: token doesn't look like wok_ + 40 chars" >&2
  export WOKWI_CLI_TOKEN
}

firmware_path() {  # first firmware = '...' in wokwi.toml
  sed -n "s/^[[:space:]]*firmware[[:space:]]*=[[:space:]]*['\"]\(.*\)['\"].*/\1/p" wokwi.toml | head -1
}

check() {
  local ok=1
  command -v wokwi-cli >/dev/null && echo "ok   wokwi-cli $(wokwi-cli --version 2>/dev/null | head -1)" \
    || { echo "MISS wokwi-cli  -> curl -L https://wokwi.com/ci/install.sh | sh"; ok=0; }
  load_token >/dev/null 2>&1 && echo "ok   WOKWI_CLI_TOKEN set" || { echo "MISS WOKWI_CLI_TOKEN"; ok=0; }
  [[ -f wokwi.toml ]] && echo "ok   wokwi.toml" || { echo "MISS wokwi.toml (architecture phase)"; ok=0; }
  [[ -f diagram.json ]] && echo "ok   diagram.json" || { echo "MISS diagram.json (architecture phase)"; ok=0; }
  if [[ -f wokwi.toml ]]; then
    local fw; fw="$(firmware_path)"
    if [[ -n "$fw" && -f "$fw" ]]; then echo "ok   firmware $fw ($(date -r "$fw" '+%F %T'))"
    else echo "MISS firmware '$fw' -> build first (pio run -e <sim env>)"; ok=0; fi
  fi
  [[ $ok == 1 ]]
}

run() {
  local timeout=30000 scenario="" extra=() args=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --timeout) timeout="$2"; shift 2 ;;
      --scenario) scenario="$2"; shift 2 ;;
      --expect) args+=(--expect-text "$2"); shift 2 ;;
      --fail) args+=(--fail-text "$2"); shift 2 ;;
      --) shift; extra=("$@"); break ;;
      *) echo "unknown arg $1" >&2; exit 1 ;;
    esac
  done
  check >/dev/null || { check; exit 2; }
  load_token
  mkdir -p "$OUT"
  [[ -n "$scenario" ]] && args+=(--scenario "$scenario")
  grep -q '"wokwi-logic-analyzer"' diagram.json && args+=(--vcd-file "sim-out/ir.vcd")
  echo ">> wokwi-cli timeout=${timeout}ms ${scenario:+scenario=$scenario}" >&2
  echo "$(date -u +%s) $timeout" >> "$OUT/usage.log"   # upper bound of simulated time
  set +e
  wokwi-cli . --timeout "$timeout" --serial-log-file sim-out/serial.log "${args[@]}" "${extra[@]+"${extra[@]}"}"
  local rc=$?
  set -e
  case $rc in
    0)  echo ">> PASS (exit 0)" >&2 ;;
    42) echo ">> TIMEOUT (exit 42): no --expect matched before ${timeout}ms" >&2 ;;
    *)  echo ">> FAIL (exit $rc)" >&2 ;;
  esac
  echo ">> serial log: sim-out/serial.log" >&2
  return $rc
}

usage_report() {
  [[ -f "$OUT/usage.log" ]] || { echo "no runs recorded"; return; }
  awk -v now="$(date -u +%s)" -v q=$FREE_QUOTA_MIN '
    now - $1 <= 30*86400 { ms += $2; n++ }
    END { printf "last 30 days: %d runs, <= %.1f simulated min (free plan: %d min / rolling 30 days)\n", n, ms/60000, q }' "$OUT/usage.log"
}

cmd="${1:-check}"; shift || true
case "$cmd" in
  check) check ;;
  lint)  wokwi-cli lint ;;
  run)   run "$@" ;;
  ir)    python3 "$SKILL/scripts/vcd_nec_decode.py" "$OUT/ir.vcd" "$@" ;;
  usage) usage_report ;;
  *) sed -n '2,12p' "$0"; exit 1 ;;
esac
