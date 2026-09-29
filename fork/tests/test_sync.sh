#!/usr/bin/env bash
# Exercises fork/scripts/sync.sh against throwaway repositories: a clean rebase, the no-op, a new
# patch on the stack, and a deliberately conflicting one. No network, no real GitHub: `gh` is a stub
# that records what it was asked to do. Run: bash fork/tests/test_sync.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYNC="$HERE/scripts/sync.sh"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
git() { command git -c init.defaultBranch=main "$@"; }

# A stub gh: logs every call; `issue list` returns the number in $W/open_issue when there is one.
mkdir "$W/bin"
cat > "$W/bin/gh" <<'STUB'
#!/usr/bin/env bash
echo "gh $*" >> "$STUB_LOG"
# two issue titles are in play: the blocked-rebase one and the overlap review one
file="$STUB_OPEN"; [[ "$*" == *"overlap to review"* ]] && file="$STUB_OPEN.review"
if [ "$1 $2" = "issue list" ] && [ -f "$file" ]; then cat "$file"; fi
if [ "$1 $2" = "issue create" ]; then [[ "$*" == *"overlap to review"* ]] && echo 8 > "$STUB_OPEN.review" || echo 7 > "$STUB_OPEN"; fi
if [ "$1 $2" = "issue close" ]; then [[ "$3" == 8 ]] && rm -f "$STUB_OPEN.review" || rm -f "$STUB_OPEN"; fi
STUB
chmod +x "$W/bin/gh"
export PATH="$W/bin:$PATH" STUB_LOG="$W/gh.log" STUB_OPEN="$W/open_issue"; : > "$STUB_LOG"

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }

# upstream -> fork bare -> working clone on harness-fork with two patch commits
git init -q --bare "$W/upstream.git"; git init -q --bare "$W/fork.git"
git clone -q "$W/upstream.git" "$W/up" 2>/dev/null
( cd "$W/up"; printf 'a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n' > file.txt; echo one > other.txt; git add .; git commit -qm base; git push -q origin HEAD:main )
git clone -q "$W/fork.git" "$W/work" 2>/dev/null
cd "$W/work"
git fetch -q "$W/upstream.git" main; git checkout -q -b harness-fork FETCH_HEAD
echo patch1 > fork-a.txt; git add .; git commit -qm "patch one"
sed -i.bak 's/^b$/b-fork/' file.txt; rm file.txt.bak; git commit -qam "patch two edits line b"
git push -q origin harness-fork
export UPSTREAM_URL="$W/upstream.git" ISSUE_TITLE="[dry-run] Fork rebase blocked" REVIEW_ISSUE_TITLE="[dry-run] Fork sync: upstream overlap to review"
run() { bash "$SYNC" 2>&1; }

out="$(run)"; echo "$out" | grep -q '^skip=false' || fail "first run should build: $out"
ok "first run (never built) proceeds"

# record state as the workflow would after publishing
state_branch() {
  local src="$1" up="$2"
  git -C "$W/work" fetch -q origin harness-fork
  ( cd "$W"; rm -rf st; git init -q st; cd st; git checkout -q -b fork-updates; mkdir -p harness
    printf '{"source_sha":"%s","upstream_sha":"%s"}\n' "$src" "$up" > harness/state.json
    git add .; git commit -qm state; git push -q -f "$W/fork.git" fork-updates )
}
head_now="$(git rev-parse HEAD)"; up_now="$(git rev-parse upstream/main)"
state_branch "$head_now" "$up_now"
out="$(run)"; echo "$out" | grep -q '^skip=true' || fail "unchanged should skip: $out"
[ "$(FORCE=true run | grep '^skip=')" = "skip=false" ] || fail "FORCE should build"
ok "no-op skips, FORCE builds"

# upstream moves without touching our lines: rebases and pushes
( cd "$W/up"; echo new > upstream-new.txt; git add .; git commit -qm "upstream adds a file"; git push -q origin HEAD:main )
out="$(run)"; echo "$out" | grep -q '^rebased=true' || fail "should rebase: $out"
[ "$(git rev-parse origin/harness-fork)" = "$(git rev-parse HEAD)" ] || fail "rebased stack was not pushed"
git merge-base --is-ancestor "$(git -C "$W/up" rev-parse HEAD)" HEAD || fail "stack is not on upstream tip"
ok "clean rebase pushed with a lease"

grep -q 'issue create.*overlap to review' "$STUB_LOG" && fail "a change that touches none of the patches' files must not open a review issue"
ok "no overlap, no review issue"

# upstream edits ANOTHER part of a file a patch edits: rebases cleanly, but is flagged for review
( cd "$W/up"; sed -i.bak 's/^i$/i-upstream/' file.txt; rm file.txt.bak; git commit -qam "upstream edits line i"; git push -q origin HEAD:main )
: > "$STUB_LOG"; out="$(run)"; echo "$out" | grep -q '^rebased=true' || fail "still a clean rebase: $out"
echo "$out" | grep -q 'review: touches the same file' || fail "overlap not reported: $out"
echo "$out" | grep -q 'patch .* patch two edits line b' || fail "patch not named: $out"
grep -q '^gh issue create --title \[dry-run\] Fork sync: upstream overlap to review' "$STUB_LOG" || fail "review issue not opened: $(cat "$STUB_LOG")"
ok "clean rebase that overlaps is flagged for review and opens the one review issue"

# upstream edits the SAME line our patch edits: the rebase must block, abort, and leave everything alone
( cd "$W/up"; sed -i.bak 's/^b$/b-upstream/' file.txt; rm file.txt.bak; git commit -qam "upstream edits line b"; git push -q origin HEAD:main )
before_local="$(git rev-parse HEAD)"; before_remote="$(git -C "$W/fork.git" rev-parse harness-fork)"
: > "$STUB_LOG"
set +e; out="$(run)"; rc=$?; set -e
[ "$rc" = 1 ] || fail "conflict should exit 1, got $rc: $out"
[ "$(git rev-parse HEAD)" = "$before_local" ] || fail "local branch moved"
[ "$(git -C "$W/fork.git" rev-parse harness-fork)" = "$before_remote" ] || fail "remote branch moved"
[ -z "$(git status --porcelain)" ] || fail "working tree not clean after abort"
[ ! -d .git/rebase-merge ] && [ ! -d .git/rebase-apply ] || fail "rebase still in progress"
echo "$out" | grep -q 'file.txt' || fail "conflicting file not listed: $out"
grep -q '^gh issue create --title \[dry-run\] Fork rebase blocked' "$STUB_LOG" || fail "issue not opened: $(cat "$STUB_LOG")"
ok "conflict aborts cleanly, branch and remote untouched, issue opened"

# a second blocked run updates the same issue instead of opening another
: > "$STUB_LOG"; set +e; run >/dev/null; set -e
{ grep -q "^gh issue edit 7" "$STUB_LOG" && ! grep -q "issue create" "$STUB_LOG"; } || fail "should edit issue 7: $(cat "$STUB_LOG")"
ok "repeat block updates the one issue"

# resolve by dropping the conflicting patch: next run rebases and closes the issue
git reset -q --hard HEAD~1; git push -q -f origin harness-fork
: > "$STUB_LOG"; out="$(run)"; echo "$out" | grep -q '^skip=false' || fail "resolved stack should proceed: $out"
grep -q '^gh issue close 7' "$STUB_LOG" || fail "issue not closed: $(cat "$STUB_LOG")"
ok "resolved stack rebases and closes the issue"
grep -q '^gh issue close 8' "$STUB_LOG" || fail "review issue should close once nothing overlaps: $(cat "$STUB_LOG")"
ok "review issue closes when nothing overlaps"
echo "all sync tests passed"
