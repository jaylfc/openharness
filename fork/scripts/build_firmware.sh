#!/usr/bin/env bash
# Build the dial firmware image. Reproduces step 2 of
# devices/harness-device/firmware/scripts/upload-firmware.sh (production build, throwaway sdkconfig
# regenerated from sdkconfig.defaults), minus the GCS upload. Run where idf.py is on PATH: the
# espressif/idf:v5.5.1 container the workflow uses, after sourcing its export.sh.
# Usage: build_firmware.sh <X.Y.Z> <out-dir>
set -euo pipefail
VER="${1:?version X.Y.Z}"; OUT="${2:?output dir}"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "error: version '$VER' must look like X.Y.Z" >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FW="$ROOT/devices/harness-device/firmware"
[ -d "$FW" ] || { echo "no firmware directory in this tree; skipping"; exit 0; }
command -v idf.py >/dev/null || { echo "error: idf.py not on PATH (source the ESP-IDF export.sh)" >&2; exit 1; }
mkdir -p "$OUT"

# The version stays a plain number: fwPush.ts only offers ^v?\d+\.\d+\.\d+$ and fw_update.c compares the
# offered string with the image's own esp_app_desc version. The file is a working-tree file on the runner.
printf '%s\n' "$VER" > "$FW/version.txt"
touch "$FW/version.txt"

# The dial (esp32s3) is the `commander` entry; upload-firmware.sh names the chip out loud for the same
# reason: a fresh checkout has no sdkconfig for IDF to guess from.
BUILD_DIR="$FW/build-prod"
rm -rf "$BUILD_DIR"
# DEVICE_FORCE_PROD=1: a published binary is always production, whatever provisioned_config.h says.
idf.py -C "$FW" -B "$BUILD_DIR" -DIDF_TARGET=esp32s3 -DSDKCONFIG="$BUILD_DIR/sdkconfig.release" -DDEVICE_FORCE_PROD=1 build

BIN="$BUILD_DIR/interns_commander.bin"
[ -f "$BIN" ] || { echo "error: $BIN not produced" >&2; exit 1; }

# esp_app_desc sits right after the 24-byte image header and 8-byte first segment header: magic word at
# 0x20, version string at 0x30. This is what the dial compares with the offered version.
python3 - "$BIN" "$VER" <<'PY'
import struct, sys
image = open(sys.argv[1], "rb").read()
magic, = struct.unpack_from("<I", image, 0x20)
version = image[0x30:0x50].split(b"\0")[0].decode()
if magic != 0xABCD5432:
    sys.exit(f"error: no esp_app_desc at 0x20 (magic {magic:#x})")
if version != sys.argv[2]:
    sys.exit(f"error: the image carries version '{version}', expected '{sys.argv[2]}'")
print(f">> image esp_app_desc.version = {version}")
PY
cp "$BIN" "$OUT/interns_commander.bin"
ls -l "$OUT/interns_commander.bin"
