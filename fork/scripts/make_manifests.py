#!/usr/bin/env python3
"""Write the fork's manifests from the artifacts one build produced.

Reads   --assets DIR      the files that were (or are about to be) attached to the GitHub Release
        --plan FILE       plan_versions.py output: the version each component publishes as
        --channel DIR     a checkout of the manifest branch (fork-updates or fork-updates-dryrun)
Writes  DIR/harness/desktop/metadata.json, cli/metadata.json, esp32/ota/metadata.json, state.json

Entry shapes are exactly what the running clients parse:
  desktop   {"desktop-macos-arm64": {version,url,sha256,size}, "desktop-linux-x64": ..., ...}
            (desktop/lib/update/desktop_updater.dart)
  cli       {"cli": {version, cli: {url,sha256,size}, notify: {url,sha256,size}}}
            (cli/src/lib/selfUpdate.ts)
  esp32     {"commander": {version,url,sha256,size}}
            (cli/src/cable/fwPush.ts)

A component whose artifacts are absent from --assets (a skipped or failed arch, firmware not present in
this tree) keeps whatever entry the channel already had; it is never blanked. Only entries for artifacts
that exist are written, so no manifest names an asset that was not uploaded.

Deliberately NOT copied from upstream: the Intel `desktop-macos`, the dmg keys, `commander-square`,
`flasher`. The fork builds Apple Silicon macOS and Linux only; an app that finds no entry for its key
simply sees no update, which is the correct answer for a build the fork does not make.
"""

import argparse
import json
import os
import sys
from datetime import datetime, timezone

from forklib import die, sha256_and_size

# manifest key -> asset file name
DESKTOP_ASSETS = {
    "desktop-macos-arm64": "Harness-macos-arm64.zip",
    "desktop-linux-x64": "Harness-linux-x64.AppImage",
    "desktop-linux-arm64": "Harness-linux-arm64.AppImage",
}
FIRMWARE_ASSET = "interns_commander.bin"
CLI_ASSETS = {"cli": "cli.js", "notify": "notify.mjs"}


def read(path):
    try:
        with open(path) as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--assets", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--channel", required=True)
    ap.add_argument("--repo", required=True, help="owner/name")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--source-sha", default="")
    ap.add_argument("--upstream-sha", default="")
    args = ap.parse_args()

    with open(args.plan) as f:
        plan = json.load(f)
    base = f"https://github.com/{args.repo}/releases/download/{args.tag}"
    root = os.path.join(args.channel, "harness")

    def ref(name):
        path = os.path.join(args.assets, name)
        sha, size = sha256_and_size(path)
        return {"url": f"{base}/{name}", "sha256": sha, "size": size}

    def have(name):
        return os.path.isfile(os.path.join(args.assets, name))

    built = []
    components = set()

    # desktop: every platform shares one version, like upstream's release
    path = os.path.join(root, "desktop", "metadata.json")
    manifest = read(path)
    for key, name in DESKTOP_ASSETS.items():
        if have(name):
            manifest[key] = {"version": plan["desktop"]["version"], **ref(name)}
            built.append(key)
            components.add("desktop")
    if manifest:
        write(path, manifest)

    # cli: two files under one key; both or neither
    path = os.path.join(root, "cli", "metadata.json")
    manifest = read(path)
    present = [have(n) for n in CLI_ASSETS.values()]
    if all(present):
        manifest["cli"] = {"version": plan["cli"]["version"], **{k: ref(n) for k, n in CLI_ASSETS.items()}}
        built.append("cli")
        components.add("cli")
    elif any(present):
        die("only one of cli.js / notify.mjs was produced; refusing to publish half a CLI")
    if manifest:
        write(path, manifest)

    # firmware: the dial's key is `commander`
    path = os.path.join(root, "esp32", "ota", "metadata.json")
    manifest = read(path)
    if have(FIRMWARE_ASSET):
        manifest["commander"] = {"version": plan["firmware"]["version"], **ref(FIRMWARE_ASSET)}
        built.append("commander")
        components.add("firmware")
    if manifest:
        write(path, manifest)

    if not built:
        die(f"no artifacts found in {args.assets}; nothing to publish")

    state_path = os.path.join(root, "state.json")
    state = read(state_path)
    state.update(
        {
            "source_sha": args.source_sha,
            "upstream_sha": args.upstream_sha,
            "tag": args.tag,
            "built_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            # only what THIS run published; a skipped component keeps its previous version
            "versions": {**state.get("versions", {}), **{c: plan[c]["version"] for c in components}},
            "entries_updated": built,
        }
    )
    write(state_path, state)
    print("wrote entries: " + ", ".join(built), file=sys.stderr)


if __name__ == "__main__":
    main()
