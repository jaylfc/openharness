#!/usr/bin/env bash
# Publish one build: GitHub Release first, then the manifests, in that order and only in that order.
#
# A manifest that names an asset which does not exist yet is a broken update for every client that polls
# in the gap (the CLI polls every minute). So the release and its assets are created and CHECKED, and
# only then are the three manifests and state.json written to the manifest branch in ONE commit.
#
# Versioned assets are immutable: a tag that already exists is an error, never an overwrite. A re-run
# gets a new tag and new versions from plan_versions.py.
#
# Environment (all required unless noted):
#   TAG             release tag (fork-yyyymmdd-n, or dryrun-n)
#   CHANNEL_BRANCH  manifest branch (fork-updates, or fork-updates-dryrun)
#   PLAN            plan_versions.py output
#   ASSETS          directory holding the built artifacts
#   SOURCE_SHA      the commit that was built
#   UPSTREAM_SHA    upstream/main it was rebased on (informational)
#   PRERELEASE      'true' for dry runs (optional, default false)
#   GH_TOKEN, GITHUB_REPOSITORY   as in Actions
set -euo pipefail

: "${TAG:?}" "${CHANNEL_BRANCH:?}" "${PLAN:?}" "${ASSETS:?}" "${SOURCE_SHA:?}" "${GITHUB_REPOSITORY:?}"
UPSTREAM_SHA="${UPSTREAM_SHA:-}"
PRERELEASE="${PRERELEASE:-false}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Every gh call below acts on THIS repository and nothing else. Without this gh infers the target from
# the git remotes and prefers one named upstream, which is how a first attempt aimed at the upstream
# project's releases (refused with a 403, luckily).
export GH_REPO="$GITHUB_REPOSITORY"

case "$CHANNEL_BRANCH" in
  fork-updates|fork-updates-dryrun) ;;
  *) echo "error: refusing to publish to '$CHANNEL_BRANCH'" >&2; exit 1 ;;
esac
[ "$PRERELEASE" = true ] || [[ "$TAG" =~ ^fork-[0-9]{8}-[0-9]+$ ]] || { echo "error: tag '$TAG' is not fork-yyyymmdd-n" >&2; exit 1; }
[ "$PRERELEASE" != true ] || [[ "$TAG" =~ ^dryrun- ]] || { echo "error: a dry run must publish under a dryrun-* tag" >&2; exit 1; }
ls "$ASSETS"/* >/dev/null 2>&1 || { echo "error: no artifacts in $ASSETS" >&2; exit 1; }

# --- 1. the release, assets included ---------------------------------------------------------------
if gh release view "$TAG" >/dev/null 2>&1 || git ls-remote --exit-code --tags origin "refs/tags/$TAG" >/dev/null 2>&1; then
  echo "error: $TAG already exists; published releases are immutable. Re-run to get a new tag." >&2
  exit 1
fi

notes="$(mktemp)"
{
  echo "Fork build of upstream plus the fork's patch stack."
  echo
  python3 - "$PLAN" <<'PY'
import json, sys
plan = json.load(open(sys.argv[1]))
for name, p in plan.items():
    print(f"- {name}: {p['version']} (upstream latest {p['upstream_latest']}, fork previous {p['fork_latest'] or 'none'})")
PY
  echo
  echo "- source: \`$SOURCE_SHA\`"
  [ -z "$UPSTREAM_SHA" ] || echo "- upstream main: \`$UPSTREAM_SHA\`"
  if [ -n "$UPSTREAM_SHA" ] && git cat-file -e "$UPSTREAM_SHA^{commit}" 2>/dev/null; then
    echo
    echo "Patch stack on top of upstream:"
    echo
    git log --format='- %h %s' "$UPSTREAM_SHA..$SOURCE_SHA"
  fi
} > "$notes"

flags=()
if [ "$PRERELEASE" = true ]; then flags+=(--prerelease); else flags+=(--latest); fi
gh release create "$TAG" "$ASSETS"/* --target "$SOURCE_SHA" --title "$TAG" --notes-file "$notes" "${flags[@]}"

# Every local file must be present on the release at the same size before any manifest may name it.
gh release view "$TAG" --json assets --jq '.assets[] | "\(.name) \(.size)"' | sort > "$notes.remote"
( cd "$ASSETS" && for f in *; do echo "$f $(wc -c < "$f" | tr -d ' ')"; done | sort ) > "$notes.local"
if ! diff "$notes.local" "$notes.remote"; then
  echo "error: the release's assets do not match what was built (left is local, right is the release)" >&2
  exit 1
fi
echo ">> release $TAG has all $(wc -l < "$notes.local" | tr -d ' ') assets"

# --- 2. the manifests, one commit ------------------------------------------------------------------
work="$(mktemp -d)"
git worktree add --detach "$work" >/dev/null
trap 'git worktree remove --force "$work" 2>/dev/null || true' EXIT
if git fetch --quiet origin "$CHANNEL_BRANCH" 2>/dev/null; then
  git -C "$work" checkout --quiet --detach FETCH_HEAD
  parent="$(git -C "$work" rev-parse HEAD)"
else
  echo ">> $CHANNEL_BRANCH does not exist yet; creating it as an orphan"
  git -C "$work" checkout --quiet --orphan channel
  git -C "$work" rm -rfq . 2>/dev/null || true
  parent=""
fi

python3 "$HERE/make_manifests.py" --assets "$ASSETS" --plan "$PLAN" --channel "$work" \
  --repo "$GITHUB_REPOSITORY" --tag "$TAG" --source-sha "$SOURCE_SHA" --upstream-sha "$UPSTREAM_SHA"

git -C "$work" add harness
if git -C "$work" diff --cached --quiet; then
  echo "error: manifests did not change; something is wrong with this publish" >&2; exit 1
fi
git -C "$work" commit --quiet -m "Publish $TAG"
# Plain push, no force: this branch has one writer at a time (the workflow's concurrency group), so a
# rejected push means something else wrote to it and must be looked at, not overwritten.
git -C "$work" push origin "HEAD:refs/heads/$CHANNEL_BRANCH"
echo ">> $CHANNEL_BRANCH updated (was ${parent:-empty}, now $(git -C "$work" rev-parse --short HEAD))"
