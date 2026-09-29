#!/usr/bin/env bash
# Build the Apple Silicon macOS app, ad-hoc signed, and zip it the way DesktopUpdater unpacks it.
#
# Reproduces desktop/scripts/upload-desktop.sh (build, version stamp, `ditto` zip) and the apple-silicon
# row of desktop/scripts/publish-macos-variant.sh (Impeller), minus Developer ID signing, notarization,
# the dmg and the GCS upload. Nothing here needs a secret.
#
# Usage: build_macos.sh <X.Y.Z> <out-dir> <desktop-manifest-url>
#   <desktop-manifest-url> is baked in as DESKTOP_UPDATE_METADATA_URL, the build-time define
#   desktop_updater.dart reads, so this build polls the fork channel and not upstream's bucket.
set -euo pipefail

VER="${1:?version X.Y.Z}"; OUT="${2:?output dir}"; MANIFEST_URL="${3:?desktop manifest url}"
[[ "$VER" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || { echo "error: version '$VER' must look like X.Y.Z" >&2; exit 1; }
# Same formula as build_number_for() in upload-desktop.sh, so CFBundleVersion matches what upstream stamps.
BUILD_NUM=$(( 10#${BASH_REMATCH[1]} * 10000 + 10#${BASH_REMATCH[2]} * 100 + 10#${BASH_REMATCH[3]} ))

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP="$ROOT/desktop/build/macos/Build/Products/Release/Harness.app"
PLIST="$APP/Contents/Info.plist"
[ "$(uname -s)" = Darwin ] || { echo "error: a macOS build needs a macOS host" >&2; exit 1; }
[ "$(uname -m)" = arm64 ] || echo "warning: building on $(uname -m); the fork ships Apple Silicon" >&2
mkdir -p "$OUT"

python3 "$ROOT/fork/scripts/macos_adhoc.py" "$ROOT/desktop/macos/Runner.xcodeproj/project.pbxproj"

cd "$ROOT/desktop"
# Removed first, for the reason upload-desktop.sh gives: an incremental Xcode build can skip re-stamping
# Info.plist and leave an older version inside. The STAMPED check below is the backstop.
rm -rf "$APP"
echo ">> flutter build macos $VER (build $BUILD_NUM), polling $MANIFEST_URL"
flutter build macos --release --build-name="$VER" --build-number="$BUILD_NUM" \
  --dart-define=DESKTOP_UPDATE_METADATA_URL="$MANIFEST_URL"
[ -d "$APP" ] || { echo "error: app bundle missing: $APP" >&2; exit 1; }

STAMPED="$(plutil -extract CFBundleShortVersionString raw "$PLIST")"
[ "$STAMPED" = "$VER" ] || { echo "error: CFBundleShortVersionString is '$STAMPED', expected '$VER'" >&2; exit 1; }

# Apple Silicon build renders on Impeller, the engine default; only rule out an opt-out
# (publish-macos-variant.sh does the same check).
IMPELLER="$(/usr/libexec/PlistBuddy -c "Print :FLTEnableImpeller" "$PLIST" 2>/dev/null || true)"
{ [ -z "$IMPELLER" ] || [ "$IMPELLER" = true ]; } || { echo "error: FLTEnableImpeller=$IMPELLER; must render on Impeller" >&2; exit 1; }

# Ad hoc, over the whole bundle, so every nested framework carries one consistent signature. Apple
# Silicon refuses to run unsigned code at all; ad hoc is the minimum. Entitlements are not carried over:
# they only matter to the sandbox and the hardened runtime, and the fork uses neither.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"
codesign -dv "$APP" 2>&1 | grep -E '^(Identifier|Signature|CodeDirectory)' || true
echo ">> archs: $(lipo -archs "$APP/Contents/MacOS/Harness")"

# The define must really be in the binary: a build that silently kept upstream's URL would poll the
# wrong channel forever and nothing would look broken. Dart AOT keeps string constants verbatim.
if ! grep -aqF "$MANIFEST_URL" "$APP/Contents/Frameworks/App.framework/Versions/A/App"; then
  echo "error: the manifest URL is not in the built app; DESKTOP_UPDATE_METADATA_URL did not reach the build" >&2
  exit 1
fi

ZIP="$OUT/Harness-macos-arm64.zip"
rm -f "$ZIP"
# Exactly the archive command upload-desktop.sh uses; DesktopUpdater unpacks it with `ditto -x -k`.
( cd "$(dirname "$APP")" && ditto -c -k --sequesterRsrc --keepParent "$(basename "$APP")" "$ZIP" )
echo ">> $ZIP $(wc -c < "$ZIP" | tr -d ' ') bytes"
