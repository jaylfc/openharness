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
        state = json.loads(get(f"{base}/state.json?t={int(time.time())}"))
        if state.get("tag") == args.tag:
            break
        if time.time() > deadline:
            die(f"{base}/state.json still names {state.get('tag')}, not {args.tag}, after {args.wait}s")
        print(f"waiting for {args.tag} (branch serves {state.get('tag')})")
        time.sleep(15)
    print(f"state.json: {json.dumps(state)}")

    checked = 0
    for rel in ("desktop", "cli", "esp32/ota"):
        manifest = json.loads(get(f"{base}/{rel}/metadata.json?t={int(time.time())}"))
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
