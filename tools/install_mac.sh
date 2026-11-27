#!/bin/sh
# Installs the Mac app with the game inside it, so it needs nothing else:
#
#   tools/install_mac.sh "<game folder>" [destination folder, default /Applications]
#
# Copies build/mac/Voyager Arcade.app, puts the game folder in
# Contents/Resources/Game (a clone on the same APFS volume, so it takes no
# extra space), and signs the result ad hoc. The installed app holds your
# game files: keep it to yourself.
set -e
GAME="$1"
DEST="${2:-/Applications}"
if [ -z "$GAME" ] || [ ! -f "$GAME/trek" ]; then
    echo "usage: tools/install_mac.sh <game folder holding trek, stvsnd, stvserv> [destination]" >&2
    exit 2
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/mac/Voyager Arcade.app"
APP="$DEST/Voyager Arcade.app"
if [ ! -d "$SRC" ]; then
    echo "build the Mac app first (cmake --build --preset mac-host)" >&2
    exit 1
fi
# Only system libraries and frameworks: the app must run on a Mac without
# Homebrew or anything else installed.
if otool -L "$SRC/Contents/MacOS/Voyager Arcade" | tail -n +2 | grep -v -E '^\s+/(usr/lib|System/Library)/'; then
    echo "the app links libraries outside the system (above); not installing" >&2
    exit 1
fi
rm -rf "$APP"
ditto "$SRC" "$APP"
mkdir -p "$APP/Contents/Resources"
cp -cR "$GAME" "$APP/Contents/Resources/Game"
rm -rf "$APP/Contents/Resources/Game/lost+found"
codesign --force --sign - "$APP"
codesign --verify "$APP"
echo "installed $APP"
