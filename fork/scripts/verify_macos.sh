#!/usr/bin/env bash
# Check a downloaded macOS zip the way the updater and the OS will treat it.
# Usage: verify_macos.sh <Harness-macos-arm64.zip> <expected X.Y.Z>
set -euo pipefail
ZIP="${1:?zip}"; VER="${2:?version}"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
ditto -x -k "$ZIP" "$D"                      # what DesktopUpdater runs
APP="$D/Harness.app"
[ -d "$APP" ] || { echo "error: no Harness.app in the archive" >&2; exit 1; }
GOT="$(plutil -extract CFBundleShortVersionString raw "$APP/Contents/Info.plist")"
echo "Info.plist CFBundleShortVersionString = $GOT"
[ "$GOT" = "$VER" ] || { echo "error: bundle says $GOT, manifest says $VER" >&2; exit 1; }
codesign --verify --deep --strict --verbose=2 "$APP"
codesign -dv "$APP" 2>&1 | grep -E '^(Identifier|Signature|TeamIdentifier)'
lipo -archs "$APP/Contents/MacOS/Harness"
# No quarantine attribute is expected: the updater's download never sets one, so Gatekeeper is not asked.
xattr -l "$APP" | grep -q quarantine && { echo "error: unexpected quarantine attribute" >&2; exit 1; } || true
echo "macOS bundle ok"
