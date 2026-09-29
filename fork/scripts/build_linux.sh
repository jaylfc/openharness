#!/usr/bin/env bash
# Build the Linux desktop app for THIS host's architecture and package the single-file AppImage.
#
# Reproduces steps 2-3 of desktop/scripts/upload-desktop-linux.sh (build, version.txt, AppDir,
# appimagetool), minus the GCS upload. flutter build linux cannot cross-compile, so the architecture is
# the host's: x64 on ubuntu-24.04, arm64 on ubuntu-24.04-arm.
#
# Usage: build_linux.sh <X.Y.Z> <out-dir> <desktop-manifest-url>   (APPIMAGETOOL = path to the tool)
set -euo pipefail

VER="${1:?version X.Y.Z}"; OUT="${2:?output dir}"; MANIFEST_URL="${3:?desktop manifest url}"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "error: version '$VER' must look like X.Y.Z" >&2; exit 1; }
: "${APPIMAGETOOL:?set APPIMAGETOOL to an appimagetool-<x86_64|aarch64>.AppImage}"
[ "$(uname -s)" = Linux ] || { echo "error: this must run on Linux" >&2; exit 1; }

case "$(uname -m)" in
  x86_64|amd64)  ARCH=x64;   APPIMAGE_ARCH=x86_64 ;;
  aarch64|arm64) ARCH=arm64; APPIMAGE_ARCH=aarch64 ;;
  *) echo "error: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUNDLE="$ROOT/desktop/build/linux/$ARCH/release/bundle"
mkdir -p "$OUT"

cd "$ROOT/desktop"
rm -rf "$BUNDLE"
echo ">> flutter build linux $VER ($ARCH), polling $MANIFEST_URL"
flutter build linux --release --dart-define=DESKTOP_UPDATE_METADATA_URL="$MANIFEST_URL"
[ -x "$BUNDLE/harness" ] || { echo "error: no harness executable in $BUNDLE" >&2; exit 1; }

# flutter build linux has no Info.plist to stamp; app_version.dart and DesktopUpdater read this file.
echo "$VER" > "$BUNDLE/version.txt"
[ "$(cat "$BUNDLE/version.txt")" = "$VER" ] || { echo "error: version.txt does not read back as $VER" >&2; exit 1; }

if ! grep -aqF "$MANIFEST_URL" "$BUNDLE/lib/libapp.so"; then
  echo "error: the manifest URL is not in libapp.so; DESKTOP_UPDATE_METADATA_URL did not reach the build" >&2
  exit 1
fi

# AppDir exactly as upload-desktop-linux.sh lays it out: the bundle verbatim under usr/bin, AppRun a
# symlink to the executable (the Flutter runner finds lib/ and data/ next to /proc/self/exe).
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
APPDIR="$STAGE/AppDir"
[ -f "$BUNDLE/harness.png" ] || { echo "error: no harness.png in $BUNDLE" >&2; exit 1; }
mkdir -p "$APPDIR/usr/bin"
cp -a "$BUNDLE/." "$APPDIR/usr/bin/"
ln -s usr/bin/harness "$APPDIR/AppRun"
cp "$BUNDLE/harness.png" "$APPDIR/harness.png"
cat > "$APPDIR/harness.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Harness
Comment=Attach terminals to the agents running on your Harness machines
Exec=harness
Icon=harness
Categories=Development;
Terminal=false
DESKTOP

IMAGE="$OUT/Harness-linux-$ARCH.AppImage"
rm -f "$IMAGE"
ARCH="$APPIMAGE_ARCH" "$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "$IMAGE"
chmod +x "$IMAGE"

# Evidence for the log, and a real check: unpack the finished AppImage and read the version back out.
( cd "$STAGE" && "$IMAGE" --appimage-extract usr/bin/version.txt >/dev/null )
GOT="$(cat "$STAGE/squashfs-root/usr/bin/version.txt")"
echo ">> AppImage usr/bin/version.txt = $GOT"
[ "$GOT" = "$VER" ] || { echo "error: the AppImage carries version '$GOT', expected '$VER'" >&2; exit 1; }
echo ">> $IMAGE $(wc -c < "$IMAGE" | tr -d ' ') bytes"
