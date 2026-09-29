#!/usr/bin/env bash
# Rebase the fork's patch stack onto upstream/main and push it, or stop cleanly if it does not apply.
#
# Runs inside a checkout of the fork with the stack branch checked out (the workflow does that).
# The whole script is driven by environment variables so it can be exercised against scratch repos:
#
#   FORK_BRANCH     the stack branch                                     (default harness-fork)
#   UPSTREAM_URL    where upstream lives                                 (default the public repo)
#   UPSTREAM_BRANCH                                                      (default main)
#   PUSH_REMOTE     remote holding FORK_BRANCH and the manifest branch   (default origin)
#   STATE_BRANCH    the manifest branch whose harness/state.json says what was last built
#                                                                        (default fork-updates)
#   FORCE           'true' = rebuild even when nothing changed
#   ISSUE_TITLE     title of the ONE issue used to report a blocked rebase
#                                                                        (default "Fork rebase blocked")
#   GH_REPO         owner/name for `gh`; unset = the checkout's own repo
#   NO_ISSUE        'true' = skip the issues (local experiments without gh)
#   REVIEW_ISSUE_TITLE  title of the ONE issue listing upstream changes that overlap the patches, when the
#                   rebase itself was clean                       (default "Fork sync: upstream overlap to review")
#   OVERLAP_REPORT  where to write the overlap report             (default a temp file; path is emitted)
#
# Prints key=value lines (skip, source_sha, upstream_sha, rebased) to $GITHUB_OUTPUT when set, and to
# stdout. Exit codes: 0 done or nothing to do, 1 rebase blocked or a hard failure.
set -euo pipefail

FORK_BRANCH="${FORK_BRANCH:-harness-fork}"
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/autonomous-ai/openharness.git}"
UPSTREAM_BRANCH="${UPSTREAM_BRANCH:-main}"
PUSH_REMOTE="${PUSH_REMOTE:-origin}"
STATE_BRANCH="${STATE_BRANCH:-fork-updates}"
FORCE="${FORCE:-false}"
ISSUE_TITLE="${ISSUE_TITLE:-Fork rebase blocked}"
REVIEW_ISSUE_TITLE="${REVIEW_ISSUE_TITLE:-Fork sync: upstream overlap to review}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NO_ISSUE="${NO_ISSUE:-false}"

emit() {
  echo "$1=$2"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "$1=$2" >> "$GITHUB_OUTPUT"; fi
}

current="$(git rev-parse --abbrev-ref HEAD)"
[ "$current" = "$FORK_BRANCH" ] || { echo "error: expected $FORK_BRANCH checked out, found $current" >&2; exit 1; }
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "error: working tree has uncommitted changes; a rebase would trip over them" >&2
  exit 1
fi

# Fetched by URL into a tracking ref, NOT added as a remote: `gh` treats a remote named upstream as the
# repository to open issues on, and this script must only ever write to the fork.
git fetch --quiet "$UPSTREAM_URL" "+$UPSTREAM_BRANCH:refs/remotes/upstream/$UPSTREAM_BRANCH"
# Be explicit anyway. Inside Actions the repository is known; elsewhere the caller may set GH_REPO.
if [ -z "${GH_REPO:-}" ] && [ -n "${GITHUB_REPOSITORY:-}" ]; then export GH_REPO="$GITHUB_REPOSITORY"; fi
upstream_sha="$(git rev-parse "upstream/$UPSTREAM_BRANCH")"
old_head="$(git rev-parse HEAD)"

# What the last successful build was made from. Missing = never built, so build.
state_json="$(git fetch --quiet "$PUSH_REMOTE" "$STATE_BRANCH" 2>/dev/null \
  && git show "FETCH_HEAD:harness/state.json" 2>/dev/null || echo '{}')"
state_field() {
  printf '%s' "$state_json" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get(sys.argv[1], ""))
except Exception: print("")' "$1"
}
last_upstream="$(state_field upstream_sha)"
last_source="$(state_field source_sha)"

# Nothing to do when neither side moved since the last build: upstream is where it was AND the stack
# (including any patch added since) is the commit we built. Comparing the stack's own head, not only
# upstream's, is what makes "I added a patch" trigger a release without upstream doing anything.
if [ "$FORCE" != "true" ] && [ "$last_upstream" = "$upstream_sha" ] && [ "$last_source" = "$old_head" ]; then
  echo "up to date: upstream $upstream_sha and stack $old_head were already built"
  emit skip true; emit source_sha "$old_head"; emit upstream_sha "$upstream_sha"; emit rebased false
  exit 0
fi

# Which upstream commits since the last sync touch what the patches touch. Computed BEFORE the rebase
# (it needs the stack's current base) and reported whether or not the rebase applies: a clean rebase can
# still mean upstream rewrote, or now duplicates, something a patch does. The fork side is what gets
# reworked, so the owner is told each time.
OVERLAP_REPORT="${OVERLAP_REPORT:-$(mktemp)}"
old_base="$(git merge-base HEAD "upstream/$UPSTREAM_BRANCH")"
python3 "$HERE/overlap.py" --old-base "$old_base" --upstream "$upstream_sha" --head "$old_head" \
  --watch "$HERE/../overlap-watch.txt" > "$OVERLAP_REPORT" || { echo "warning: the overlap check failed; continuing without it" >&2; : > "$OVERLAP_REPORT"; }
overlap=false
if [ -s "$OVERLAP_REPORT" ]; then
  overlap=true
  echo "::warning title=Upstream overlap to review::$(head -1 "$OVERLAP_REPORT")"
  cat "$OVERLAP_REPORT"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then { echo "### Upstream overlap to review"; echo; echo '```'; cat "$OVERLAP_REPORT"; echo '```'; } >> "$GITHUB_STEP_SUMMARY"; fi
fi
emit overlap "$overlap"; emit overlap_report "$OVERLAP_REPORT"

# upsert_issue <title> <body>: open the ONE issue with this title, or update it if it is already open.
find_issue() {
  gh issue list --state open --search "\"$1\" in:title" --json number,title \
    --jq "[.[] | select(.title == \"$1\")][0].number // empty" 2>/dev/null || true
}
upsert_issue() {
  local existing; existing="$(find_issue "$1")"
  if [ -n "$existing" ]; then
    gh issue edit "$existing" --body "$2" >/dev/null && echo "updated issue #$existing"
  else
    gh issue create --title "$1" --body "$2" \
      || echo "::warning::could not open the issue; the failed run is the only alert. Forks have Issues switched off by default: enable them under Settings, General, Features." >&2
  fi
}
run_link="${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-local}"

rebased=false
rebase_log="$(mktemp)"
if git merge-base --is-ancestor "upstream/$UPSTREAM_BRANCH" HEAD; then
  echo "stack is already on top of upstream $upstream_sha; no rebase needed"
else
  echo "rebasing $FORK_BRANCH ($old_head) onto upstream/$UPSTREAM_BRANCH ($upstream_sha)"
  if ! git rebase "upstream/$UPSTREAM_BRANCH" >"$rebase_log" 2>&1; then
    conflicted="$(git diff --name-only --diff-filter=U || true)"
    stopped_at="$(git log -1 --format='%h %s' REBASE_HEAD 2>/dev/null || echo unknown)"
    git rebase --abort
    rm -f "$rebase_log"
    # Abort restores the branch exactly; prove it, because the rest of the pipeline trusts it.
    [ "$(git rev-parse HEAD)" = "$old_head" ] || { echo "error: HEAD moved after rebase --abort" >&2; exit 1; }
    echo "::error title=Fork rebase blocked::$FORK_BRANCH does not apply on upstream $upstream_sha" >&2
    echo "conflicting files:" >&2
    printf '%s\n' "$conflicted" | sed 's/^/  /' >&2

    if [ "$NO_ISSUE" != "true" ]; then
      body="$(printf '%s\n' \
        "The daily sync could not rebase \`$FORK_BRANCH\` onto upstream \`$UPSTREAM_BRANCH\`." \
        "" \
        "- upstream commit: \`$upstream_sha\`" \
        "- stack head (unchanged): \`$old_head\`" \
        "- patch that did not apply: $stopped_at" \
        "" \
        "Conflicting files:" \
        "" \
        "$(printf '%s\n' "$conflicted" | sed 's/^/- `/; s/$/`/')" \
        "" \
        "Nothing was pushed and nothing was published. Rework the fork's patch on top of upstream (never the other way round), push the stack, and the next run picks it up and closes this issue." \
        "" \
        "Upstream changes that overlap the patches:" \
        "" \
        "$(if [ -s "$OVERLAP_REPORT" ]; then sed 's/^/    /' "$OVERLAP_REPORT"; else echo '    none found by file or function'; fi)" \
        "" \
        "Last updated by workflow run: $run_link")"
      upsert_issue "$ISSUE_TITLE" "$body"
    fi
    emit skip true; emit source_sha "$old_head"; emit upstream_sha "$upstream_sha"; emit rebased false
    exit 1
  fi
  rm -f "$rebase_log"
  rebased=true
fi

new_head="$(git rev-parse HEAD)"
if [ "$new_head" != "$old_head" ]; then
  # Lease pinned to the sha we started from: if someone pushed the stack while we were rebasing, this
  # refuses instead of overwriting their work.
  git push --force-with-lease="refs/heads/$FORK_BRANCH:$old_head" "$PUSH_REMOTE" "HEAD:refs/heads/$FORK_BRANCH"
fi

# A rebase that works closes any earlier "blocked" issue. Overlap with upstream is reported in its own
# issue, opened or updated while there is something to review and closed when there is not.
if [ "$NO_ISSUE" != "true" ]; then
  existing="$(find_issue "$ISSUE_TITLE")"
  if [ -n "$existing" ]; then
    gh issue close "$existing" --comment "Rebased cleanly onto upstream \`$upstream_sha\`; stack head is now \`$new_head\`." >/dev/null || true
  fi
  if [ "$overlap" = true ]; then
    upsert_issue "$REVIEW_ISSUE_TITLE" "$(printf '%s\n' \
      "The stack rebased cleanly onto upstream \`$upstream_sha\`, but upstream changed code the fork's patches also touch. A clean rebase does not prove the patches still do the right thing: upstream may have superseded or now duplicates a fork feature. The fork's patch is reworked, never upstream's." \
      "" \
      "$(sed 's/^/    /' "$OVERLAP_REPORT")" \
      "" \
      "New stack head: \`$new_head\`. Last updated by workflow run: $run_link")"
  else
    existing="$(find_issue "$REVIEW_ISSUE_TITLE")"
    if [ -n "$existing" ]; then
      gh issue close "$existing" --comment "No upstream change since the last sync overlaps the patches." >/dev/null || true
    fi
  fi
fi

emit skip false; emit source_sha "$new_head"; emit upstream_sha "$upstream_sha"; emit rebased "$rebased"
