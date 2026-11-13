#!/usr/bin/env bash
# Team onboarding / toolchain bootstrap for iqair-controller.
#
# Installs PlatformIO Core 6.2.0 (into ~/.platformio/penv), installs project
# toolchains/libraries for [env:native] and [env:atoms3], repairs the known
# pioarduino Core version overwrite (`6.2.0.post1` -> `6.2.0`), and runs
# verification tests (plus optional firmware build with --build).
#
# Usage:
#   ./scripts/bootstrap.sh          # setup toolchain + run native tests
#   ./scripts/bootstrap.sh --build  # also build all firmware environments
#   ./scripts/bootstrap.sh --check  # verify local prerequisites only

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PIO_HOME="${HOME}/.platformio"
PIO_BIN="${PIO_HOME}/penv/bin/pio"
PIO_PY="${PIO_HOME}/penv/bin/python"
REQUIRED_PIO_VER="6.2.0"

BUILD_FW=0
CHECK_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --build) BUILD_FW=1 ;;
    --check) CHECK_ONLY=1 ;;
    -h|--help)
      sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "ERROR: unknown option '$arg' (expected --build, --check, or --help)" >&2
      exit 2
      ;;
  esac
done

say()  { printf '==> %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

find_python310() {
  local cand
  for cand in python3 python3.13 python3.12 python3.11 python3.10; do
    if command -v "$cand" >/dev/null 2>&1; then
      if "$cand" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; then
        command -v "$cand"
        return 0
      fi
    fi
  done
  if command -v uv >/dev/null 2>&1; then
    local uv_py
    uv_py="$(uv python find '>=3.10' 2>/dev/null || true)"
    if [[ -n "$uv_py" && -x "$uv_py" ]]; then
      echo "$uv_py"
      return 0
    fi
  fi
  if [[ -x "$HOME/.local/bin/uv" ]]; then
    local uv_py
    uv_py="$("$HOME/.local/bin/uv" python find '>=3.10' 2>/dev/null || true)"
    if [[ -n "$uv_py" && -x "$uv_py" ]]; then
      echo "$uv_py"
      return 0
    fi
  fi
  return 1
}

check_host_prereqs() {
  local missing=0

  command -v git  >/dev/null 2>&1 || { warn "git not found in PATH"; missing=1; }
  command -v curl >/dev/null 2>&1 || { warn "curl not found in PATH"; missing=1; }

  if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    warn "No host C++ compiler (g++ or clang++) found. Required for 'pio test -e native'."
    warn "  Ubuntu/Debian: sudo apt-get install build-essential"
    warn "  macOS:         xcode-select --install"
    missing=1
  fi

  if ! find_python310 >/dev/null 2>&1; then
    warn "Python >= 3.10 not found (required by pioarduino)."
    warn "  Install Python >= 3.10 via OS package manager or 'uv python install 3.12' (see README.md)."
    missing=1
  fi

  if [[ $missing -ne 0 ]]; then
    die "Missing host prerequisites listed above."
  fi
}

fix_pio_version_if_needed() {
  [[ -x "$PIO_BIN" && -x "$PIO_PY" ]] || return 0
  local ver
  ver="$("$PIO_BIN" --version 2>/dev/null || true)"
  if [[ "$ver" != *"${REQUIRED_PIO_VER}" || "$ver" == *"post"* ]]; then
    say "Restoring PlatformIO Core ${REQUIRED_PIO_VER} (was: ${ver:-unknown})"
    "$PIO_PY" -m pip uninstall -y pioarduino >/dev/null 2>&1 || true
    "$PIO_PY" -m pip install --force-reinstall "platformio==${REQUIRED_PIO_VER}"
  fi
}

install_platformio() {
  if [[ ! -x "$PIO_BIN" ]]; then
    local py
    py="$(find_python310)"
    say "Installing PlatformIO Core ${REQUIRED_PIO_VER} using ${py}"
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN
    curl -fsSL -o "${tmp}/get-platformio.py" \
      https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
    "$py" "${tmp}/get-platformio.py"
  fi
  fix_pio_version_if_needed
  say "PlatformIO ready: $("$PIO_BIN" --version)"
}

check_host_prereqs
if [[ $CHECK_ONLY -eq 1 ]]; then
  if [[ -x "$PIO_BIN" ]]; then
    say "Host prereqs OK; PlatformIO: $("$PIO_BIN" --version)"
  else
    say "Host prereqs OK; PlatformIO not yet installed (run ./scripts/bootstrap.sh)"
  fi
  exit 0
fi

install_platformio

say "Installing PlatformIO platforms & library dependencies (native + atoms3)..."
"$PIO_BIN" pkg install -e native -e atoms3
fix_pio_version_if_needed

say "Running host unit tests (pio test -e native)..."
"$PIO_BIN" test -e native

if [[ $BUILD_FW -eq 1 ]]; then
  say "Building firmware environments (atoms3, atoms3_debug, atoms3_sim)..."
  "$PIO_BIN" run -e atoms3 -e atoms3_debug -e atoms3_sim
  fix_pio_version_if_needed
fi

say "Bootstrap complete."
echo "Next steps:"
echo "  export PATH=\"\$HOME/.platformio/penv/bin:\$PATH\""
echo "  pio test -e native"
echo "  pio run -e atoms3"
