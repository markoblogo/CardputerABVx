#!/bin/zsh
# Install the optional Needle runtime used by ABVx Companion on macOS.
set -euo pipefail

if (( $# != 0 )); then
  print -u2 "Usage: zsh tools/setup_abvx_companion_needle.zsh"
  exit 2
fi

SUPPORT_DIR="${ABVX_COMPANION_SUPPORT_DIR:-$HOME/Library/Application Support/ABVx Companion}"
VENV_DIR="${ABVX_COMPANION_VENV_DIR:-$SUPPORT_DIR/.venv}"
BOOTSTRAP_PYTHON="${ABVX_COMPANION_BOOTSTRAP_PYTHON:-python3}"

if ! command -v "$BOOTSTRAP_PYTHON" >/dev/null 2>&1; then
  print -u2 "Python interpreter not found: $BOOTSTRAP_PYTHON"
  exit 1
fi

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  "$BOOTSTRAP_PYTHON" -m venv "$VENV_DIR"
fi

"$VENV_DIR/bin/python" -m pip install --upgrade pip
"$VENV_DIR/bin/python" -m pip install --upgrade cactus-needle
"$VENV_DIR/bin/python" - <<'PY'
import needle

print(f"Needle runtime ready: {getattr(needle, '__version__', 'installed')}")
PY

print "Companion runtime: $VENV_DIR/bin/python"
print "Start with: ABVX_INTENT_ADAPTER=needle '$VENV_DIR/bin/python' tools/abvx_companion_app.py"
