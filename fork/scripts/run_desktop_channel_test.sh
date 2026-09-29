#!/usr/bin/env bash
# Run fork/tests/desktop_channel_test.dart inside the desktop package (it needs `package:harness`).
# The file is copied in for the run and removed after, so the working tree is left as it was.
# Usage: FORK_DESKTOP_MANIFEST_URL=... [FORK_STAGE=1] [FORK_EXPECT_VERSION=x.y.z] run_desktop_channel_test.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${FORK_DESKTOP_MANIFEST_URL:?set FORK_DESKTOP_MANIFEST_URL}"
# raw.githubusercontent.com can serve an older copy for a few minutes after a push. When the caller says
# which version to expect, wait (fresh cache-buster each try) until the manifest shows it, so a slow edge
# is not reported as a broken release.
if [ -n "${FORK_EXPECT_VERSION:-}" ]; then
  base="${FORK_DESKTOP_MANIFEST_URL%%\?*}"
  for _ in $(seq 1 60); do
    if curl -fsS "$base?t=$(date +%s)$RANDOM" | grep -q "\"version\": \"$FORK_EXPECT_VERSION\""; then break; fi
    echo "manifest does not show $FORK_EXPECT_VERSION yet; waiting"; sleep 10
  done
  FORK_DESKTOP_MANIFEST_URL="$base?t=$(date +%s)$RANDOM"
  export FORK_DESKTOP_MANIFEST_URL
fi
cp "$ROOT/fork/tests/desktop_channel_test.dart" "$ROOT/desktop/test/fork_desktop_channel_test.dart"
trap 'rm -f "$ROOT/desktop/test/fork_desktop_channel_test.dart"' EXIT
cd "$ROOT/desktop"
flutter test test/fork_desktop_channel_test.dart
