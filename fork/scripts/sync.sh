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
#   NO_ISSUE        'true' = skip the issue (local experiments without gh)
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

git remote remove upstream 2>/dev/null || true
git remote add upstream "$UPSTREAM_URL"
git fetch --quiet upstream "$UPSTREAM_BRANCH"
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
        "Nothing was pushed and nothing was published. Rebase locally, resolve, and push the stack; the next run picks it up and closes this issue." \
        "" \
        "Last updated by workflow run: ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-local}")"
      existing="$(gh issue list --state open --search "\"$ISSUE_TITLE\" in:title" --json number,title \
        --jq "[.[] | select(.title == \"$ISSUE_TITLE\")][0].number // empty" 2>/dev/null || true)"
      if [ -n "$existing" ]; then
        gh issue edit "$existing" --body "$body" >/dev/null && echo "updated issue #$existing"
      else
        gh issue create --title "$ISSUE_TITLE" --body "$body" \
          || echo "::warning::could not open the issue; the failed run is the only alert. Forks have Issues switched off by default: enable them under Settings, General, Features." >&2
      fi
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

# A rebase that works closes any earlier "blocked" issue.
if [ "$NO_ISSUE" != "true" ]; then
  existing="$(gh issue list --state open --search "\"$ISSUE_TITLE\" in:title" --json number,title \
    --jq "[.[] | select(.title == \"$ISSUE_TITLE\")][0].number // empty" 2>/dev/null || true)"
  if [ -n "$existing" ]; then
    gh issue close "$existing" --comment "Rebased cleanly onto upstream \`$upstream_sha\`; stack head is now \`$new_head\`." >/dev/null || true
  fi
fi

emit skip false; emit source_sha "$new_head"; emit upstream_sha "$upstream_sha"; emit rebased "$rebased"
