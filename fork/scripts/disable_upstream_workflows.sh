#!/usr/bin/env bash
# List (default) or disable (--apply) the workflows inherited from upstream that must not run on the fork.
#
# Go-live step; needs a token that may change workflow state, which is why it is a separate, explicit
# command and not something the sync workflow does. Only workflows registered on the fork are touched,
# and fork-sync is never on the list. Re-runnable: disabling a disabled workflow is a no-op.
#
# Why each one:
#   tag-triggered releases  (v*_desktop, v*_cli, v*_web, v*_backend): they publish to upstream's GCS bucket
#                           with upstream's cloud credentials. On the fork they can only fail, but a tag
#                           pushed by mistake (for example `git push --tags` after fetching upstream's)
#                           would start them.
#   dispatch-only releases  (grid runtime, tmux runtime, tui): the same bucket, started by hand.
#   desktop-internal-build  any push to a branch named internal/**.
#   publish-store-catalog   pushes to main; guarded by a repository check, disabled anyway.
#   *-checks                pull request and push-to-main checks for upstream's website, store and
#                           agents: cost minutes on the fork's PRs for no benefit.
#   mobile-mac              the stale workflow left on the fork's old main; needs a self-hosted runner.
set -euo pipefail

APPLY=false
[ "${1:-}" = "--apply" ] && APPLY=true
REPO="${GITHUB_REPOSITORY:-jaylfc/openharness}"

# Matched by file name: `name:` fields get edited upstream, file names rarely do. A workflow the fork has
# not registered yet (GitHub indexes a file the first time a branch containing it is pushed) is reported
# as such, so run this AFTER harness-fork is the default branch and has been pushed.
FILES=(
  release-desktop.yml
  release.yml
  release-web.yml
  release-tui.yml
  release-grid-runtime.yml
  release-tmux-runtime.yml
  production-be-build.yaml
  desktop-internal-build.yml
  publish-store-catalog.yml
  authoring-browser-checks.yml
  experience-checks.yml
  home-assistant-checks.yml
  website-checks.yml
  mobile-mac.yml
)

registered="$(gh workflow list --repo "$REPO" --all --json name,state,id,path --jq '.[] | "\(.id)\t\(.state)\t\(.path)\t\(.name)"')"
for file in "${FILES[@]}"; do
  row="$(printf '%s\n' "$registered" | awk -F'\t' -v f=".github/workflows/$file" '$3 == f {print; exit}')"
  if [ -z "$row" ]; then echo "not registered on $REPO: $file"; continue; fi
  id="$(printf '%s' "$row" | cut -f1)"; state="$(printf '%s' "$row" | cut -f2)"; name="$(printf '%s' "$row" | cut -f4)"
  if [ "$state" != active ]; then echo "already $state: $file"; continue; fi
  if $APPLY; then gh workflow disable "$id" --repo "$REPO" && echo "disabled: $file ($name)"; else echo "would disable: $file ($name)"; fi
done
$APPLY || echo "(dry listing; pass --apply to disable)"
