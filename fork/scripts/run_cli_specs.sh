#!/usr/bin/env bash
# Run the fork's TypeScript specs (fork/tests/*.spec.ts) with the CLI's own vitest and setup.
# The specs import the real client code from cli/src, so they must resolve `vitest` from the CLI's
# node_modules; a symlink is the least invasive way to give them that (it is gitignored).
# Usage (from anywhere, after `npm ci` in cli/):  fork/scripts/run_cli_specs.sh [vitest args]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -d "$ROOT/cli/node_modules" ] || { echo "error: run 'npm ci' in cli/ first" >&2; exit 1; }
ln -sfn ../../cli/node_modules "$ROOT/fork/tests/node_modules"
cd "$ROOT/cli"
exec npx vitest run --config ../fork/tests/vitest.config.ts "$@"
