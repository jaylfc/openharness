#!/usr/bin/env bash
# Bundle the CLI (dist/cli.js + dist/notify.mjs) with its version baked in.
# Reproduces steps 2 of cli/scripts/upload-cli.sh, checks included, minus the upload.
# Usage: build_cli.sh <X.Y.Z> <out-dir>
set -euo pipefail
VER="${1:?version X.Y.Z}"; OUT="${2:?output dir}"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "error: version '$VER' must look like X.Y.Z" >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$OUT"
cd "$ROOT/cli"
[ -d node_modules ] || npm ci --no-audit --no-fund
ADAPTER_VERSION="$VER" npm run bundle
head -1 dist/cli.js | grep -q '^#!' || { echo "error: dist/cli.js lost its shebang" >&2; exit 1; }
[ "$(node dist/cli.js version)" = "$VER" ] || { echo "error: bundled version is not $VER (ADAPTER_VERSION not injected)" >&2; exit 1; }
# The channel patch must be in the bundle: without it this CLI would poll upstream's bucket.
grep -qF 'fork-updates/harness/cli/metadata.json' dist/cli.js \
  || { echo "error: the bundle does not carry the fork's ADAPTER_UPDATE_URL default; is the update-channel patch applied?" >&2; exit 1; }
grep -qF 'fork-updates/harness/esp32/ota/metadata.json' dist/cli.js \
  || { echo "error: the bundle does not carry the fork's CABLE_FW_MANIFEST_URL default" >&2; exit 1; }
cp dist/cli.js dist/notify.mjs "$OUT/"
ls -l "$OUT/cli.js" "$OUT/notify.mjs"
