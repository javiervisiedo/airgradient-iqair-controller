#!/usr/bin/env bash
# Bump, test, build, commit, and create an annotated Git tag after hardware verification.
# Usage: ./scripts/release.sh <MAJOR.MINOR.PATCH> [-m "Release notes"] [--push]
set -euo pipefail

usage() {
  echo "Usage: $0 <MAJOR.MINOR.PATCH> [-m \"Release notes\"] [--push]" >&2
  exit 1
}

if [[ $# -lt 1 ]]; then
  usage
fi

VERSION="$1"
shift

if [[ ! "$VERSION" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  echo "Error: version '$VERSION' must match MAJOR.MINOR.PATCH (e.g. 0.9.1)" >&2
  exit 1
fi
MAJOR="${BASH_REMATCH[1]}"
MINOR="${BASH_REMATCH[2]}"
PATCH="${BASH_REMATCH[3]}"

NOTES="Release v$VERSION"
DO_PUSH=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    -m)
      [[ $# -lt 2 ]] && usage
      NOTES="$2"
      shift 2
      ;;
    --push)
      DO_PUSH=true
      shift
      ;;
    -h|--help)
      usage
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      ;;
  esac
done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "Error: tracked working tree has uncommitted changes. Commit or stash them before releasing." >&2
  exit 1
fi

TAG="v$VERSION"
if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "Error: git tag '$TAG' already exists." >&2
  exit 1
fi

if command -v pio >/dev/null 2>&1; then
  PIO_BIN="pio"
elif [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
  PIO_BIN="$HOME/.platformio/penv/bin/pio"
else
  echo "Error: PlatformIO ('pio') not found in PATH or ~/.platformio/penv/bin/pio" >&2
  exit 1
fi

if [[ -x "$HOME/.platformio/penv/bin/python" ]]; then
  PY_BIN="$HOME/.platformio/penv/bin/python"
elif command -v python3 >/dev/null 2>&1; then
  PY_BIN="python3"
else
  echo "Error: python3 not found" >&2
  exit 1
fi

echo "Updating include/fw_version.h to $VERSION ..."
"$PY_BIN" - "$MAJOR" "$MINOR" "$PATCH" "$VERSION" <<'PYEOF'
import pathlib, re, sys

major, minor, patch, ver = sys.argv[1:5]
p = pathlib.Path("include/fw_version.h")
text = p.read_text(encoding="utf-8")
text, n1 = re.subn(r"^#define FW_VERSION_MAJOR \d+$", f"#define FW_VERSION_MAJOR {major}", text, flags=re.M)
text, n2 = re.subn(r"^#define FW_VERSION_MINOR \d+$", f"#define FW_VERSION_MINOR {minor}", text, flags=re.M)
text, n3 = re.subn(r"^#define FW_VERSION_PATCH \d+$", f"#define FW_VERSION_PATCH {patch}", text, flags=re.M)
text, n4 = re.subn(r'^#define FW_VERSION_STR "[^"]+"$', f'#define FW_VERSION_STR "{ver}"', text, flags=re.M)
if (n1, n2, n3, n4) != (1, 1, 1, 1):
    sys.exit("Error: failed to update one or more macros in include/fw_version.h")
p.write_text(text, encoding="utf-8")
PYEOF

echo "Running native host tests..."
"$PIO_BIN" test -e native

echo "Building AtomS3 firmware (pre-commit check)..."
"$PIO_BIN" run -e atoms3

git add include/fw_version.h
git commit -m "chore(release): $TAG"
git tag -a "$TAG" -m "$NOTES"

echo "Rebuilding AtomS3 firmware with clean release commit metadata..."
"$PIO_BIN" run -e atoms3

if [[ "$DO_PUSH" == true ]]; then
  echo "Pushing commit and tag $TAG to origin..."
  git push origin HEAD --follow-tags
else
  echo
  echo "Release $TAG committed and tagged locally."
  echo "To push: git push origin HEAD --follow-tags"
fi

echo "Firmware binary ready at: .pio/build/atoms3/firmware.bin"
