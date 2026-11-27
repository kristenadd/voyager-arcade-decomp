#!/bin/sh
# Translates the three programs of a game folder to C++ (build/x86tr/<program>/gen).
# The output is derived from the player's files and is never committed.
#
#   tools/prepare.sh "<game folder>"
set -e
GAME="$1"
if [ -z "$GAME" ] || [ ! -f "$GAME/trek" ]; then
    echo "usage: tools/prepare.sh <game folder holding trek, stvsnd, stvserv>" >&2
    exit 2
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/x86tr"
if [ ! -d "$ROOT/build/pylib/capstone" ]; then
    python3 -m pip install --quiet --target "$ROOT/build/pylib" capstone==5.0.3
fi
for p in trek stvsnd stvserv; do
    PYTHONPATH="$ROOT/build/pylib" python3 "$ROOT/tools/x86tr/discover.py" "$GAME/$p" "$OUT/$p" > "$OUT.$p.discover.log"
    PYTHONPATH="$ROOT/build/pylib" python3 "$ROOT/tools/x86tr/translate.py" "$GAME/$p" "$OUT/$p/functions.json" "$OUT/$p/gen" | tail -1
done
