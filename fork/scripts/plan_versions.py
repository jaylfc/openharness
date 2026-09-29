#!/usr/bin/env python3
"""Decide the version each fork component will publish as.

  next = patch + 1 of max(upstream's latest published, the fork channel's latest published)

The updaters compare only X.Y.Z, so the fork build must be strictly higher than BOTH channels' latest:
higher than upstream, or a machine moving onto the fork channel is never offered it, and higher than the
fork's own previous release, or nothing on the fork channel ever updates.

One version string can therefore exist in both channels with different content (upstream ships 1.2.28
later; the fork's 1.2.28 was different bytes). That is harmless: an install polls exactly one channel.
The rule above makes the fork's NEXT release skip past whatever upstream took.

Usage:
  plan_versions.py --upstream-base URL_OR_DIR --fork-base URL_OR_DIR [--fork-base ...] [--out plan.json]

Each base holds desktop/metadata.json, cli/metadata.json and esp32/ota/metadata.json. A --fork-base that
does not exist yet is "nothing published" (first ever release); an unreadable UPSTREAM is fatal.
"""

import argparse
import json
import os
import sys

from forklib import UPSTREAM_BASE, die, fmt, load_json, next_version, parse_version

# component -> (manifest path under a base, which manifest keys count toward "latest published")
COMPONENTS = {
    "desktop": ("desktop/metadata.json", lambda key: key.startswith("desktop-")),
    "cli": ("cli/metadata.json", lambda key: key == "cli"),
    "firmware": ("esp32/ota/metadata.json", lambda key: key == "commander"),
}


def join(base, path):
    return base.rstrip("/") + "/" + path if "://" in base else os.path.join(base, path)


def latest(manifest, wanted):
    """Highest X.Y.Z among the wanted keys of one manifest, or None."""
    best = None
    for key, entry in (manifest or {}).items():
        if not wanted(key) or not isinstance(entry, dict):
            continue
        v = parse_version(entry.get("version", ""))
        if v and (best is None or v > best):
            best = v
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--upstream-base", default=UPSTREAM_BASE)
    ap.add_argument("--fork-base", action="append", default=[])
    ap.add_argument("--out")
    args = ap.parse_args()

    plan = {}
    for name, (path, wanted) in COMPONENTS.items():
        up = latest(load_json(join(args.upstream_base, path), required=True), wanted)
        if up is None:
            die(f"upstream {path} has no usable {name} version; refusing to guess one")
        fork_versions = []
        for base in args.fork_base:
            v = latest(load_json(join(base, path), required=False), wanted)
            if v:
                fork_versions.append(v)
        fork = max(fork_versions) if fork_versions else None
        floor = max([up] + fork_versions)
        plan[name] = {
            "version": fmt(next_version(floor)),
            "upstream_latest": fmt(up),
            "fork_latest": fmt(fork) if fork else None,
        }

    text = json.dumps(plan, indent=2) + "\n"
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
