# The Mac mini runner

`.github/workflows/mobile-mac.yml` builds the phone app on a self-hosted Mac: Xcode is the only
thing that compiles the iOS runner's Swift, and no hosted job here builds `mobile/` at all. This page
is how that Mac is set up. It is written to be followed by an agent working on the Mac itself, top
to bottom, and each step says what it expects to find.

**Scope.** Everything this creates lives in one folder, `~/claude-workspace/actions-runner`, plus the
LaunchAgent `svc.sh` installs. Nothing else in the home folder, the shell profile, Homebrew or the
global git config is changed. If a step seems to need that, stop and ask.

## What the Mac already needs

Check, don't install — each of these is already on the Mac mini this was written against:

| Tool | Check | Expected there |
|---|---|---|
| Flutter | `~/Development/flutter/bin/flutter --version` | 3.47.x stable |
| Xcode | `xcodebuild -version` | Xcode 27, first launch done (`xcodebuild -runFirstLaunch` if not) |
| JDK 17 | `/opt/homebrew/opt/openjdk@17/bin/java -version` | 17.x |
| Android SDK | `ls ~/Library/Android/sdk/platforms` | android-35 or newer |

No CocoaPods: the iOS project uses Swift Package Manager. No git-lfs: nothing under `mobile/` is in
LFS, and the workflow checks out with `lfs: false`.

## 1. The runner

```sh
mkdir -p ~/claude-workspace/actions-runner && cd ~/claude-workspace/actions-runner
```

Download the latest **osx-arm64** runner from <https://github.com/actions/runner/releases>, check its
SHA-256 against the one printed on the release page, and unpack it here:

```sh
curl -fLo runner.tar.gz https://github.com/actions/runner/releases/download/v<VERSION>/actions-runner-osx-arm64-<VERSION>.tar.gz
shasum -a 256 runner.tar.gz        # must match the release page
tar xzf runner.tar.gz && rm runner.tar.gz
```

## 2. Register it

A registration token is valid for an hour. The repository owner gets one from **Settings → Actions →
Runners → New self-hosted runner** (the token is in the `config.sh` line shown there), or with a
logged-in `gh`:

```sh
gh api -X POST repos/jaylfc/openharness/actions/runners/registration-token --jq .token
```

```sh
./config.sh --unattended --replace \
  --url https://github.com/jaylfc/openharness \
  --token <TOKEN> \
  --name mac-mini \
  --labels openharness-mac \
  --work _work
```

`openharness-mac` is the label the workflow asks for; `self-hosted`, `macOS` and `ARM64` are added
by the runner itself. Builds happen in `~/claude-workspace/actions-runner/_work`.

## 3. Its environment

A LaunchAgent does not read `~/.zshrc`, so the runner is told where the tools are. Both files live
in the runner folder and are read when it starts.

`.path` — one line, searched in order:

```
/Volumes/NVMe/Users/jay/Development/flutter/bin:/opt/homebrew/opt/openjdk@17/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
```

`.env`:

```
LANG=en_US.UTF-8
JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
ANDROID_HOME=/Volumes/NVMe/Users/jay/Library/Android/sdk
GIT_CONFIG_GLOBAL=/Volumes/NVMe/Users/jay/claude-workspace/actions-runner/gitconfig
```

⚠️ **`GIT_CONFIG_GLOBAL` is not optional on this Mac.** The account's own `~/.gitconfig` sets
`core.hooksPath` to hooks that call `git-lfs`, which is not installed, so every checkout the runner
made would fail in a post-checkout hook. Pointing the runner at its own file leaves the account's
config exactly as it is:

```sh
cat > ~/claude-workspace/actions-runner/gitconfig <<'EOF'
[user]
	name = mac-mini runner
	email = runner@localhost
[init]
	defaultBranch = main
EOF
```

## 4. Run it as a service

```sh
./svc.sh install     # a LaunchAgent for this user
./svc.sh start
./svc.sh status
```

A LaunchAgent runs while the user is logged in. On a headless Mac mini that means automatic login
for this account (System Settings → Users & Groups), or the runner is offline after every reboot
until someone logs in.

## 5. Check it

- **Settings → Actions → Runners** shows `mac-mini` as Idle.
- **Actions → Mobile (Mac) → Run workflow** on `main`. A green (or known-red, below) run with all
  five steps executed is done.

Known state on `main` at the time of writing: Analyze, Build iOS and Build Android pass;
**Test** fails in four `agent_pager_attach_test.dart` cases, which fail the same way on Linux. That
is a real failure to fix in the tests' own PR, not a runner problem.

## Keeping it healthy

- **Disk.** `_work` keeps the last checkout and its `build/` output, a few GB. Delete
  `~/claude-workspace/actions-runner/_work/openharness/openharness/mobile/build` whenever space is
  short; the next run rebuilds it. Gradle's and pub's caches (`~/.gradle`, `~/.pub-cache`) are
  shared with anything else on the account and are left alone.
- **Updates.** The runner updates itself. Flutter, Xcode and the SDK are the Mac's own and update
  when the person using the Mac updates them; the workflow's first step prints what it built with.
- **One job at a time.** One runner, so PRs queue. `concurrency` in the workflow cancels a run that
  a newer push to the same PR has replaced.

## Security

The repository is public, and a self-hosted runner runs whatever code a workflow checks out, as
`jay`, with that account's files in reach. What keeps a stranger's code off it:

- The workflow uses `pull_request`, never `pull_request_target`, and has no secrets.
- Its job-level `if:` skips any PR whose branch is in a fork. Don't remove it. A fork's change is
  checked by reading it, pushing it to a branch in this repository, and running the workflow there.
- Only this one workflow names the `openharness-mac` label. A new workflow that targets the runner
  deserves the same `if:` guard; review for it.

A separate macOS user for the runner would contain it further, at the cost of installing Flutter,
the JDK and the Android SDK for that user too. Not done here; worth doing if the repository starts
taking outside contributions.

## Removing it

```sh
cd ~/claude-workspace/actions-runner
./svc.sh stop && ./svc.sh uninstall
./config.sh remove --token <REMOVAL TOKEN>   # Settings → Actions → Runners → mac-mini → Remove
cd ~ && rm -rf ~/claude-workspace/actions-runner
```
