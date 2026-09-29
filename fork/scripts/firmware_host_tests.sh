#!/usr/bin/env bash
# Run upstream's firmware host tests (devices/harness-device/firmware/test/run.sh) on a Linux runner.
#
# That suite is developed on macOS with clang and compiles everything with -Werror. On Ubuntu's gcc the
# unmodified upstream tree already trips a -Wformat-truncation warning in ui/habitat/terminal.c, so the
# suite fails there for a reason that has nothing to do with the code under test. A `cc` shim that drops
# -Werror, and asks for gnu11 instead of c11 (a test uses getline/ssize_t, which strict c11 hides on
# glibc), keeps every assertion and the sanitizers while stopping a compiler difference from failing
# the run. Real failures (a test that runs and fails) still fail this script.
#
# Run where the ESP-IDF environment is exported (IDF_PATH set), so the tests that link the SDK's cJSON run
# as well. Usage: firmware_host_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/devices/harness-device/firmware/test/run.sh"
[ -f "$RUN" ] || { echo "no firmware host tests in this tree; skipping"; exit 0; }

shim="$(mktemp -d)"; trap 'rm -rf "$shim"' EXIT
cat > "$shim/cc" <<'SHIM'
#!/usr/bin/env bash
args=(); for a in "$@"; do case "$a" in -Werror) ;; -std=c11) args+=(-std=gnu11) ;; *) args+=("$a") ;; esac; done
exec gcc "${args[@]}"
SHIM
chmod +x "$shim/cc"
PATH="$shim:$PATH" bash "$RUN"
