# The fork channel

This directory, `.github/workflows/fork-sync.yml` and two small patches are everything that turns
`jaylfc/openharness` into a release channel of its own: a daily rebuild of upstream plus the fork's patch
stack, released on GitHub, with the installed apps polling the fork for updates instead of upstream.

Upstream's pipeline (Google Cloud Storage, Workload Identity, Developer ID signing and notarization) cannot
run here. This is a lighter one that needs no secret except a single personal access token for the rebase.

## The pieces

| Thing | Where | Notes |
|---|---|---|
| `main` | fork | Untouched. Old, stale. Never used by this pipeline. |
| `harness-fork` | fork | Upstream `main` plus a small linear stack of patch commits. Becomes the default branch at go-live. The daily job rebases it and force-pushes it (with a lease). |
| `fork-ops-dev` | fork | Where this pipeline is developed. Every push is a DRY run. |
| `fork-updates` | fork, orphan | The manifests the apps poll, plus `harness/state.json`. Written only by a real release. |
| `fork-updates-dryrun` | fork, orphan | The same, written only by dry runs. |
| Releases `fork-<yyyymmdd>-<n>` | fork | One per build; the artifacts are its assets. |
| Prereleases `dryrun-<n>` | fork | Dry-run artifacts. Safe to delete. |

Manifests are served from `https://raw.githubusercontent.com/jaylfc/openharness/fork-updates/harness/`:
`desktop/metadata.json`, `cli/metadata.json`, `esp32/ota/metadata.json`, `state.json`.
Artifacts are at `https://github.com/jaylfc/openharness/releases/download/<tag>/<asset>`.

Everything fork-only lives under `fork/` or in `.github/workflows/fork-*.yml`, so the stack rebases
without conflicts. Two files upstream tracks are patched, each in its own commit so each can be its own
patch in the stack (see "The two channel patches").

## How a release happens

`fork-sync.yml` runs daily at 04:17 UTC, but only when the repository variable `FORK_AUTOSYNC` is `true`.
It can also be started by hand (Actions, Fork sync, Run workflow; inputs `force` and `publish`).

1. **sync**: fetch upstream, rebase `harness-fork` onto `upstream/main`, push it with
   `--force-with-lease`. If neither upstream nor the stack changed since the last build (compared with
   `state.json`) it stops there, unless `force` is set. If the rebase conflicts it aborts, leaves
   `harness-fork` exactly as it was, opens or updates ONE issue titled "Fork rebase blocked" (listing the
   conflicting files and the upstream sha), fails the run, and stops. A later clean rebase closes the issue.
   Whether or not the rebase applies, it also runs the overlap check (below).
2. **plan**: work out the versions (below) and the release tag once, so every build stamps the same numbers.
   Toolchain pins (Flutter, Node, appimagetool) are read from upstream's own workflow files, so a bump
   upstream arrives with the next rebase.
3. **test**: `tsc`, the CLI's `src/cable` tests, the fork's own specs (the real updater code against a
   manifest built by the real generator), the script tests, and `flutter analyze` on any Dart file the
   stack changed.
4. **build**, in parallel: CLI bundle; macOS arm64 app; Linux x64 and arm64 AppImages; dial firmware (in
   the `espressif/idf:v5.5.1` container, skipped if the tree has no firmware directory).
5. **publish**: create the Release with every asset and check they all arrived at the right size; only
   then write the three manifests and `state.json` to `fork-updates` in one commit. A manifest never names
   an asset that does not exist yet.
6. **verify**: downloads every artifact every manifest names and checks its sha256; unpacks the macOS zip
   with `ditto`, checks `Info.plist` and `codesign --verify`; extracts the AppImage and reads `version.txt`;
   and runs the real CLI updater, the firmware offer and the real Dart `DesktopUpdater` against the
   published manifests.

Push to `fork-ops-dev` runs the same jobs in DRY mode: no rebase, nothing pushed to `harness-fork`, release
is a prerelease tagged `dryrun-<n>`, manifests go to `fork-updates-dryrun`, and the desktop builds poll
that branch. `fork-updates` is never touched.

GitHub only lets `workflow_dispatch` start a workflow that exists on the default branch, and `schedule`
only fires from the default branch. Until `harness-fork` is the default, the only way to run this is a push
to `fork-ops-dev`, and the cron is inert.

## Versions

Per component (desktop, CLI, firmware): `next = patch + 1 of max(upstream's latest published, the fork's
latest published)`, plain `X.Y.Z`, with `.99` rolling to the next minor at `.1` like upstream's release
scripts. Upstream's real manifests are read at plan time; if they cannot be read the run fails rather than
guess (a version below upstream's would never be offered).

The updaters compare only `X.Y.Z`, so the fork build has to be strictly higher than what an install already
has. Being above upstream's latest is what lets an install move onto the fork; being above the fork's own
latest is what lets fork installs keep updating. Consequences worth knowing:

- One version string can exist in both channels with different bytes. Upstream ships 1.2.28 later; the
  fork's 1.2.28 was something else. This is harmless because an install polls exactly one channel, and the
  rule above makes the fork's next release skip past whatever upstream took (1.2.29).
- A MINOR bump is a forced update in the desktop app. It only happens when upstream's own minor moves or a
  patch number rolls over 99, same as upstream.
- Released assets are immutable. A re-run gets a new tag and new versions; the workflow refuses to reuse a
  tag.
- Firmware versions must stay plain numbers (`fwPush.ts` only offers `^v?\d+\.\d+\.\d+$`).

## The upstream overlap check

Policy: if upstream adds something that supersedes or interferes with a fork feature, the fork's
implementation is reworked, never upstream's. A rebase that applies cleanly does not tell you that, so on
every sync `scripts/overlap.py` lists the upstream commits since the last sync that touch what the fork's
patches touch, per patch, in three tiers:

- `review: may supersede or interfere (same function)`: an upstream hunk and a hunk of a patch sit in the
  same function of the same file (git's own hunk header names the function).
- `review: touches the same file`: upstream changed a file a patch changed, elsewhere in it.
- `review: watched areas changed upstream`: an upstream diff adds or removes a line matching a named
  symbol in a named place, from `fork/overlap-watch.txt` (recap reader and recap tap against the character
  tap in `ui_habitat.c`, PSRAM history, the LAN and WiFi transport, the black canvas `HT_THEME_CANVAS`,
  the BOOT and PWR handling in `ptt.c`, the daemon's dial WiFi and recap code). Edit that file as the
  features move; one `label :: path regex :: content regex` per line.

The report is a warning annotation and a section of the run summary. When the rebase is blocked it is part
of the "Fork rebase blocked" issue; when the rebase is clean it is the body of a second issue, "Fork sync:
upstream overlap to review", opened or updated while there is something to review and closed when there
is not. It is also appended to the release notes of the release that follows. It is a heuristic (file and
function granularity, plus the watch list), so a quiet report is not proof that nothing changed; a
noisy one costs a look.

## What the installed apps poll

| Client | Manifest | How it is pointed at the fork |
|---|---|---|
| Desktop app | `desktop/metadata.json` | Build-time define `DESKTOP_UPDATE_METADATA_URL`, set by `build_macos.sh` / `build_linux.sh`. Keys `desktop-macos-arm64`, `desktop-linux-x64`, `desktop-linux-arm64`. |
| CLI daemon | `cli/metadata.json`, key `cli` | Default of `ADAPTER_UPDATE_URL` in `cli/src/config/env.ts` (channel patch). |
| Dial firmware (via the daemon) | `esp32/ota/metadata.json`, key `commander` | Default of `CABLE_FW_MANIFEST_URL` in the same patch. |
| node / tmux / grid runtimes | upstream's GCS | Deliberately unchanged. |

The fork's desktop manifest has no Intel Mac (`desktop-macos`) and no `.dmg` entries: the fork builds Apple
Silicon only, and an app that finds no entry for its key sees no update. Likewise the fork's firmware
manifest names only the dial (`commander`); other boards (`commander-square`, Harness Pro) get no fork
image until the fork builds one.

raw.githubusercontent.com caches every file for about five minutes (`cache-control: max-age=300`), and the
CLI's manifest fetch adds no cache-buster. So an update reaches a running CLI within roughly five minutes of
publishing, not the one minute upstream's GCS gives.

## The two channel patches

Both are tiny and separate commits, kept at the bottom of the stack:

1. `cli/src/config/env.ts`: the `ADAPTER_UPDATE_URL` and `CABLE_FW_MANIFEST_URL` defaults.
2. `desktop/lib/update/desktop_updater.dart`: read the manifest as text and decode it in the updater.
   This one is needed, not cosmetic. Dio only parses a JSON body when the response declares a JSON content
   type, and raw.githubusercontent.com serves every file as `text/plain`; without the patch every desktop
   update check on the fork channel fails its cast and the app never sees an update (reproduced locally against a
   text/plain server). The change also accepts an already-decoded body, so upstream's own tests still pass.
   If upstream ever changes that function the rebase will stop on it and raise the "Fork rebase blocked"
   issue; the resolution is to re-apply the same idea.

## Go-live checklist

Do these in order. Nothing above needs to change in the repository.

1. **Make `harness-fork` the fork's default branch.** Put the stack on it first: upstream `main`, then the
   patch commits, then the two channel patches and this `fork/` directory plus
   `.github/workflows/fork-sync.yml` (cherry-pick from `fork-ops-dev`; they only touch `fork/`, the
   workflow file, and the two channel-patched files). Then Settings, General, Default branch. Do not
   copy tags from upstream to the fork (see 5).
2. **Enable Issues** (Settings, General, Features). Forks have them off (they are off today), and the "Fork rebase blocked"
   report is an issue. Without it the failed run is the only alert.
3. **Create the token and the secret.** A fine-grained personal access token, resource owner `jaylfc`,
   repository access limited to `jaylfc/openharness`, permissions: Contents read and write, Workflows read
   and write, Issues read and write. Add it as the Actions secret `FORK_SYNC_TOKEN`. (The built-in token
   cannot push commits that touch `.github/workflows`, which a rebased stack does whenever upstream edits a
   workflow. A run without the secret fails first and says so.) Set an expiry you will remember; an
   expired token makes the daily run fail at the checkout.
4. **Decide about the dial firmware before the first release.** The image is built from this tree, which
   is ahead of what upstream last published: upstream's published `commander` 0.0.86 (built 24 Sep, LVGL
   UI, 3.2 MB) predates upstream commit `977638b2`, which deleted LVGL and the Pro sources and moved to
   the habitat renderer; the image built from current main is about 0.9 MB. The fork is therefore the
   first channel to offer the post-LVGL firmware, and once the CLI is on the fork channel the daemon
   offers it to a dial running an older clean release. It was built and its `esp_app_desc` version
   checked, but never flashed to hardware in this work. To hold it back, set the repository variable
   `FORK_FIRMWARE` to `false` (the dial job is skipped and no `commander` entry is published; a previous
   entry is kept). Remove the variable when you are ready.
5. **Turn the schedule on.** Settings, Secrets and variables, Actions, Variables: `FORK_AUTOSYNC` = `true`.
   Before that the cron does nothing. To release once by hand first: Actions, Fork sync, Run workflow,
   `force` on.
6. **Disable the upstream workflows that must not fire on the fork.** After step 1 GitHub registers the
   inherited files; then `fork/scripts/disable_upstream_workflows.sh` lists what it would do and
   `--apply` does it. The list, from an audit of every `on:` trigger:
   - `release-desktop.yml` (tag `v*_desktop`), `release.yml` (tag `v*_cli`), `release-web.yml` (tag
     `v*_web`), `production-be-build.yaml` (tag `v*_backend`): publish to upstream's bucket with upstream's
     cloud credentials. A tag pushed by mistake, for example `git push --tags` after fetching upstream's,
     would start them.
   - `release-grid-runtime.yml`, `release-tmux-runtime.yml`, `release-tui.yml`: manual, same bucket.
   - `desktop-internal-build.yml`: any push to a branch named `internal/**`.
   - `publish-store-catalog.yml`: push to `main` (guarded by a repository check).
   - `authoring-browser-checks.yml`, `experience-checks.yml`, `home-assistant-checks.yml`,
     `website-checks.yml`: pull request and push-to-`main` checks for upstream's website and store.
   - `mobile-mac.yml`: left on the fork's old `main`, needs a self-hosted runner.
   - `ci.yml` is manual-only and harmless. There is no other `schedule`, `workflow_run` or
     `repository_dispatch` trigger. `fork-sync.yml` uses none of the tag patterns above.
7. **Check the first run.** Watch it to the `verify` jobs. The first fork release is `fork-<date>-1`.

If you ever rename the fork, update the URLs in `cli/src/config/env.ts` (baked into the CLI) and the
`repo` you dispatch against; the workflow itself uses `github.repository`.

## Moving an installed app onto the fork channel, once

An install polls one channel and never looks at the other, so each needs one manual step.

**CLI daemon.** The fork's CLI is higher than upstream's, so the running upstream CLI can be pointed at the
fork's manifest for a single update; the fork bundle it installs has the fork manifest as its default from
then on:

```bash
ADAPTER_UPDATE_URL=https://raw.githubusercontent.com/jaylfc/openharness/fork-updates/harness/cli/metadata.json harness update
```

`harness update` reads that variable (the launcher `install.sh` writes just execs `node cli.js`, so the
environment passes straight through), so it stops the daemon, swaps the bundle, and relaunches it on the new
bytes. `--force` is only needed to replace a build made by `install-cli.sh` (a `-dev` build); such a build
also carries `ADAPTER_UPDATE_DISABLE=true` in its launcher and will not follow the fork on its own. For a
machine that has no CLI yet, install upstream's with its `install.sh` and run the line above.

**macOS app.** Apple Silicon only. Download `Harness-macos-arm64.zip` from the latest `fork-*` release and
unpack it with `ditto -x -k` (or `curl -L` it: a file fetched by curl has no quarantine flag). If the
browser downloaded it, macOS will refuse to open the app as from an unidentified developer, since the build
is ad-hoc signed and not notarized; clear that with:

```bash
xattr -cr /Applications/Harness.app
```

Quit Harness first, replace `/Applications/Harness.app`, open it. From then on the app updates itself from
the fork: the updater downloads without a quarantine flag, so Gatekeeper is not consulted again.

What changes when the signing identity changes from upstream's Developer ID to ad hoc:

- Sign-in survives. The CLI keeps its session under `~/.harness/auth`, and the app's own settings are keyed
  by the unchanged bundle id (`ai.autonomous.harness`). Neither app nor CLI stores anything in the login
  Keychain; the one Keychain item the app touches is Claude Code's, read through `/usr/bin/security`, so the
  app's signature does not matter to it.
- macOS privacy permissions (folder access, microphone, local network, Automation and the like) are
  granted to a code identity. An ad-hoc signature is identified by the hash of the build, which changes
  with every release, so expect macOS to ask again after the first switch and possibly after each update.
  This is the price of not having a Developer ID; it does not lose data.
- Moving back to upstream's build later means installing upstream's zip the same way once, and polling
  upstream's manifest again.

**Linux.** Download `Harness-linux-<x64|arm64>.AppImage` from the release, `chmod +x`, run it in place of the
old one. It updates itself from the fork.

**Dial firmware.** Nothing to do once the CLI is on the fork: the daemon offers the fork's image to a dial
running an older clean release, exactly as it did with upstream's. A dial running a dev build is never
touched.

## Rollback

There is none in the sense of going back: every updater only moves forward, and deleting a release only
leaves clients pointing at a 404. To roll back, publish a HIGHER version that contains the older code:
revert or drop the offending commit from `harness-fork`, push, and run Fork sync with `force`. The new
release gets the next version numbers and installs over the bad one. A run that produced something worse
than the last release should be handled the same way, quickly, because the CLI updates within minutes.

## Adding a patch to the stack

Work on a branch off `harness-fork`, keep the change small and in its own commit (one idea per commit;
the stack is linear and is rebased daily), and land it by fast-forwarding `harness-fork`:

```bash
git fetch fork
git switch -c my-patch fork/harness-fork
# ... commit ...
git push fork my-patch:harness-fork      # a fast-forward; the daily job takes it from there
```

The next scheduled run (or a manual run) sees a stack head that differs from `state.json` and releases it.
Prefer edits that leave upstream's files alone (new files, `fork/`); each line changed in an upstream file
is a future conflict. If the daily rebase blocks, the issue names the patch that no longer applies: rebase
locally, fix that commit, and force-push the stack (`--force-with-lease`).

## Developing the pipeline itself

Push to `fork-ops-dev`; it is a dry run. Useful locally:

```bash
python3 fork/scripts/plan_versions.py --fork-base https://raw.githubusercontent.com/jaylfc/openharness/fork-updates/harness
python3 -m unittest discover -s fork/tests                # version rule and manifest generator
bash fork/tests/test_sync.sh                              # rebase, no-op, overlap, conflict and issue paths, on scratch repos
bash fork/tests/test_publish.sh                           # release then manifests, orphan branch, refusals, stub gh
bash fork/scripts/run_cli_specs.sh                        # real CLI updater and firmware offer on a fixture channel
FORK_DESKTOP_MANIFEST_URL=<url> bash fork/scripts/run_desktop_channel_test.sh   # real DesktopUpdater
```

Dry-run output is for testing only. Prereleases `dryrun-<n>` and the branch `fork-updates-dryrun` carry
version numbers that keep climbing (a dry run plans against both channels) and will be HIGHER than the
first real release, and a desktop app built by a dry run polls `fork-updates-dryrun` for ever. Do not
install them on a machine you care about; delete the prereleases when you are done.

One trap when working on these scripts locally: if the checkout has a git remote named `upstream`,
`gh` treats it as the repository to act on. An early run of this pipeline did exactly that and tried to
create its release on the upstream project (refused with a 403). The scripts therefore fetch upstream by
URL, never add the remote, and set `GH_REPO` before calling `gh`; keep doing the same.

Scripts are Python 3 (stdlib only) for JSON and Bash for git and packaging. They fail loudly with a message
that says what to do.

## Known limits

- Apple Silicon macOS and Linux only; no Intel Mac, no Windows, no `.dmg`.
- The dial (`commander`) is the only firmware image built. The firmware build is a production build
  (`DEVICE_FORCE_PROD=1`) and ignores any local `provisioned_config.h`, as upstream's release does; anything
  the WiFi transport needs at runtime has to come from provisioning, not from that header.
- Issues are disabled on the fork today, so the "Fork rebase blocked" issue cannot be opened until they
  are switched on; the rebase-conflict path was proven with a stubbed `gh`, and the run itself still
  fails loudly. Real-mode runs (rebase, push with the token, the `fork-updates` branch, the schedule)
  cannot run until `harness-fork` is the default branch and the secret exists; only dry runs have been
  exercised.
- Ad-hoc signing (above). Not notarized.
- The manifests are plain files on a branch: no cache control, five minutes of edge caching.
- A failed `verify` job does not undo a release. It is the alarm; the remedy is a newer version.
