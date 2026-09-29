#!/usr/bin/env python3
"""Check a published channel the way a client would see it, and save the artifacts it names.

  verify_channel.py --base https://raw.githubusercontent.com/<repo>/<branch>/harness --tag <tag> --out DIR

  1. waits until harness/state.json on the branch names --tag (raw.githubusercontent.com caches for
     about five minutes; a cache-busting query string gets a fresh copy)
  2. for EVERY entry of the three manifests, downloads its url and checks size and sha256, so no
     manifest can name an asset that is not there (this includes entries kept from earlier releases)
  3. saves what it downloaded under DIR by manifest key, for the platform checks that follow
"""
import argparse
import hashlib
import json
import os
import sys
import time
import urllib.request

from forklib import die


def get(url, retries=3):
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url), timeout=120) as r:
                return r.read()
        except Exception as e:  # noqa: BLE001
            if attempt == retries - 1:
                die(f"GET {url}: {e}")
            time.sleep(3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--wait", type=int, default=600, help="seconds to wait for the branch to serve the new state")
    args = ap.parse_args()
    base = args.base.rstrip("/")

    deadline = time.time() + args.wait
    while True:
        try:
            state = json.loads(get(f"{base}/state.json?t={int(time.time())}", retries=1))
        except SystemExit:
            state = {}
        if state.get("tag") == args.tag:
            break
        if time.time() > deadline:
            die(f"{base}/state.json still names {state.get('tag')}, not {args.tag}, after {args.wait}s")
        print(f"waiting for {args.tag} (branch serves {state.get('tag')})")
        time.sleep(15)
    print(f"state.json: {json.dumps(state)}")

    updated = set(state.get("entries_updated", []))
    checked = 0
    for rel in ("desktop", "cli", "esp32/ota"):
        # raw.githubusercontent.com serves each file from its own cache, so right after a push one file
        # can be new while another is still a 404 or the previous release. Retry until this manifest shows
        # the entries THIS release wrote (they name the tag in their urls), within the same deadline.
        while True:
            try:
                manifest = json.loads(get(f"{base}/{rel}/metadata.json?t={int(time.time())}", retries=1))
                stale = [k for k, e in manifest.items() if k in updated and f"/download/{args.tag}/" not in json.dumps(e)]
                if not stale:
                    break
                print(f"{rel}: still serving the previous release for {stale}")
            except SystemExit:
                print(f"{rel}: not served yet")
            if time.time() > deadline:
                die(f"{base}/{rel}/metadata.json never showed release {args.tag} within {args.wait}s")
            time.sleep(15)
        for key, entry in manifest.items():
            files = {key: entry} if "url" in entry else {f"{key}.{k}": v for k, v in entry.items() if isinstance(v, dict)}
            for name, ref in files.items():
                data = get(ref["url"])
                got = hashlib.sha256(data).hexdigest()
                if got != ref["sha256"] or len(data) != ref["size"]:
                    die(f"{ref['url']}: sha256/size {got}/{len(data)} != manifest {ref['sha256']}/{ref['size']}")
                dest = os.path.join(args.out, name)
                os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
                with open(dest, "wb") as f:
                    f.write(data)
                print(f"ok {rel}/{name}: {ref['url'].rsplit('/', 1)[-1]} {len(data)} bytes sha256 {got[:12]}")
                checked += 1
    if not checked:
        die("the channel has no entries")
    print(f"{checked} artifacts verified against their manifests")


if __name__ == "__main__":
    main()
