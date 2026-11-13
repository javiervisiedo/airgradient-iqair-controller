#!/usr/bin/env python3
"""PreToolUse guard for run_command (project-local).

- deny:      committing/staging secrets (include/secrets.h, .env)
- force_ask: flashing / erasing the physically attached AtomS3
- ask:       everything else (normal permission flow)
"""
import json
import re
import subprocess
import sys


def out(decision, reason=""):
    print(json.dumps({"decision": decision, "reason": reason}))
    sys.exit(0)


try:
    payload = json.load(sys.stdin)
except Exception:
    out("ask")

args = (payload.get("toolCall") or {}).get("args") or {}
cmd = args.get("CommandLine") or args.get("commandLine") or ""
cwd = args.get("Cwd") or (payload.get("workspacePaths") or ["."])[0]

SECRET_FILES = r"(include/secrets\.h(?!\.example\b)|(^|[\s/])\.env(?!\.example\b)\b)"

# 1. Never stage secrets explicitly.
if re.search(r"\bgit\s+add\b", cmd) and re.search(SECRET_FILES, cmd):
    out("deny", "Refusing to stage secrets (include/secrets.h / .env). They must stay gitignored.")

# 2. Block commits if secrets are staged (e.g. via `git add -A` with a broken .gitignore).
if re.search(r"\bgit\s+(commit|push)\b", cmd):
    try:
        staged = subprocess.run(["git", "diff", "--cached", "--name-only"], cwd=cwd,
                                capture_output=True, text=True, timeout=5).stdout
        tracked = subprocess.run(["git", "ls-files", "include/secrets.h", ".env"], cwd=cwd,
                                 capture_output=True, text=True, timeout=5).stdout
    except Exception:
        staged = tracked = ""
    if re.search(r"^(include/secrets\.h|\.env)$", staged + tracked, flags=re.M):
        out("deny", "include/secrets.h or .env is staged/tracked. Run "
                    "`git rm --cached include/secrets.h .env` and check .gitignore first.")

# 3. Flashing / erasing real hardware always needs explicit confirmation.
FLASH = [r"\bpio\s+run\b.*-t\s*(upload|uploadfs|erase)\b",
         r"\bpio\s+run\b.*--target\s*(upload|uploadfs|erase)\b",
         r"\besptool(\.py)?\b.*\b(erase_flash|erase_region|write_flash)\b",
         r"pio\.sh\s+upload\b"]
if any(re.search(p, cmd) for p in FLASH):
    out("force_ask", "This flashes/erases the physically attached AtomS3 Lite. Confirm the "
                     "port and that no serial monitor is holding it.")

out("ask")
