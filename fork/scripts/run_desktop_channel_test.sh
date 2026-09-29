#!/usr/bin/env bash
# Run fork/tests/desktop_channel_test.dart inside the desktop package (it needs `package:harness`).
# The file is copied in for the run and removed after, so the working tree is left as it was.
# Usage: FORK_DESKTOP_MANIFEST_URL=... [FORK_STAGE=1] [FORK_EXPECT_VERSION=x.y.z] run_desktop_channel_test.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${FORK_DESKTOP_MANIFEST_URL:?set FORK_DESKTOP_MANIFEST_URL}"
cp "$ROOT/fork/tests/desktop_channel_test.dart" "$ROOT/desktop/test/fork_desktop_channel_test.dart"
trap 'rm -f "$ROOT/desktop/test/fork_desktop_channel_test.dart"' EXIT
cd "$ROOT/desktop"
flutter test test/fork_desktop_channel_test.dart
