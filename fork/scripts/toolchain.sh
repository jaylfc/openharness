#!/usr/bin/env bash
# Print the toolchain pins the fork builds with, READ FROM UPSTREAM'S OWN FILES so a bump upstream
# (Flutter, Node, appimagetool) is picked up on the next rebase instead of drifting here.
# Output is KEY=value lines, ready for $GITHUB_OUTPUT. Fails if a pin cannot be found: a silently
# empty version would make setup-flutter install "latest" and ship an engine nobody has run.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

pin() { # name file regex
  local v
  v="$(sed -nE "$3" "$2" | head -1)"
  [ -n "$v" ] || { echo "error: could not read $1 from $2 (upstream changed the file; update fork/scripts/toolchain.sh)" >&2; exit 1; }
  echo "$1=$v"
}

pin flutter_version    "$ROOT/.github/workflows/release-desktop.yml" "s/^  FLUTTER_VERSION: '([^']+)'.*/\1/p"
pin appimagetool       "$ROOT/.github/workflows/release-desktop.yml" "s/^ *APPIMAGETOOL_VERSION: '([^']+)'.*/\1/p"
pin node_version       "$ROOT/.github/workflows/release.yml"         "s/^ *node-version: ([0-9.]+) *$/\1/p"
