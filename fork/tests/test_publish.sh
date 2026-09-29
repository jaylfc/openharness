#!/usr/bin/env bash
# Exercises fork/scripts/publish_channel.sh against a scratch repository with a stub `gh`: the first
# publish (creates the orphan manifest branch), a second one on top of it, and the refusals
# (reused tag, wrong branch). No network. Run: bash fork/tests/test_publish.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
fail() { echo "FAIL: $*" >&2; exit 1; }; ok() { echo "ok: $*"; }

mkdir "$W/bin"
cat > "$W/bin/gh" <<'STUB'
#!/usr/bin/env bash
echo "gh $*" >> "$STUB_LOG"
case "$1 $2" in
  "release view")
    if [[ " $* " == *" --json "* ]]; then ( cd "$STUB_ASSETS" && for f in *; do echo "$f $(wc -c < "$f" | tr -d ' ')"; done ); exit 0; fi
    grep -qx "$3" "$STUB_RELEASES" 2>/dev/null && exit 0 || exit 1 ;;
  "release create") echo "$3" >> "$STUB_RELEASES" ;;
esac
STUB
chmod +x "$W/bin/gh"
export PATH="$W/bin:$PATH" STUB_LOG="$W/gh.log" STUB_RELEASES="$W/releases" STUB_ASSETS="$W/assets"; : > "$STUB_LOG"

git init -q --bare "$W/remote.git"
git clone -q "$W/remote.git" "$W/work" 2>/dev/null; cd "$W/work"
echo x > f; git add f; git -c init.defaultBranch=main commit -qm base; git push -q origin HEAD:main
SRC="$(git rev-parse HEAD)"
mkdir "$W/assets"; echo cli > "$W/assets/cli.js"; echo notify > "$W/assets/notify.mjs"; echo bin > "$W/assets/interns_commander.bin"
echo '{"desktop":{"version":"1.2.28","upstream_latest":"1.2.27","fork_latest":null},"cli":{"version":"0.3.28","upstream_latest":"0.3.27","fork_latest":null},"firmware":{"version":"0.0.87","upstream_latest":"0.0.86","fork_latest":null}}' > "$W/plan.json"

run() { TAG="$1" CHANNEL_BRANCH="${2:-fork-updates-dryrun}" PLAN="$W/plan.json" ASSETS="$W/assets" SOURCE_SHA="$SRC" UPSTREAM_SHA="" \
        PRERELEASE=true GITHUB_REPOSITORY=o/r bash "$HERE/scripts/publish_channel.sh" 2>&1; }

out="$(run dryrun-1)" || fail "first publish: $out"
git ls-remote origin fork-updates-dryrun | grep -q . || fail "manifest branch was not created"
git fetch -q origin fork-updates-dryrun; git show FETCH_HEAD:harness/cli/metadata.json | grep -q '/download/dryrun-1/cli.js' || fail "manifest does not name the release"
[ "$(git rev-list --count FETCH_HEAD)" = 1 ] && [ -z "$(git rev-list --parents FETCH_HEAD | awk 'NF>1')" ] || fail "branch should be a single orphan commit"
ok "first publish creates the orphan branch with one commit"

out="$(run dryrun-2)" || fail "second publish: $out"
git fetch -q origin fork-updates-dryrun
[ "$(git rev-list --count FETCH_HEAD)" = 2 ] || fail "second publish should add a commit on top"
git show FETCH_HEAD:harness/state.json | grep -q '"tag": "dryrun-2"' || fail "state.json not updated"
ok "second publish adds one commit on the existing branch"

grep -qn "release create" "$STUB_LOG" && [ "$(grep -c 'release create' "$STUB_LOG")" = 2 ] || fail "expected two releases"
set +e; out="$(run dryrun-2)"; rc=$?; set -e
[ "$rc" != 0 ] && echo "$out" | grep -q "already exists" || fail "a reused tag must be refused: $out"
set +e; out="$(run dryrun-9 main)"; rc=$?; set -e
[ "$rc" != 0 ] && echo "$out" | grep -q "refusing to publish" || fail "only the two manifest branches may be published to: $out"
ok "reused tag and foreign branch are refused"
echo "all publish tests passed"
