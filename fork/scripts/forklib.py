"""Shared helpers for the fork channel scripts (stdlib only, so CI needs no pip install)."""

import hashlib
import json
import re
import sys
import urllib.error
import urllib.request

# The same strict X.Y.Z the updaters compare (desktop_updater.dart semverGt, cli/src/lib/selfUpdate.ts
# semverGt). Anything after the third number is ignored by them, so we only ever publish plain X.Y.Z.
SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")

UPSTREAM_BASE = "https://storage.googleapis.com/s3-autonomous-upgrade-3/harness"


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def parse_version(text):
    m = SEMVER.match(str(text).strip())
    if not m:
        return None
    return tuple(int(g) for g in m.groups())


def fmt(v):
    return ".".join(str(n) for n in v)


def next_version(current):
    """patch + 1, rolling 99 over to the next minor at .1 (never .0).

    Mirrors next_desktop_version() in desktop/scripts/upload-desktop.sh, so "the next version" means the
    same thing here as upstream. Note a minor bump is a FORCED update in the desktop app.
    """
    major, minor, patch = current
    if patch >= 99:
        return (major, minor + 1, 1)
    return (major, minor, patch + 1)


def load_json(source, required):
    """Read a manifest from an https URL or a local path.

    required=True: any failure is fatal. Used for upstream's manifests, because computing a version from
    a manifest we could not read would quietly publish something LOWER than upstream, and the updaters
    would then never offer it.
    required=False: a missing document (HTTP 404 / no such file) means "nothing published yet" and
    returns None. Any other failure is still fatal, since a network blip must not look like an empty
    channel and restart the version numbers.
    """
    try:
        if re.match(r"^https?://", source):
            req = urllib.request.Request(source, headers={"Cache-Control": "no-cache"})
            with urllib.request.urlopen(req, timeout=30) as resp:
                raw = resp.read()
        else:
            with open(source, "rb") as f:
                raw = f.read()
        data = json.loads(raw)
    except urllib.error.HTTPError as e:
        if e.code == 404 and not required:
            return None
        die(f"could not read {source}: HTTP {e.code}")
    except FileNotFoundError:
        if not required:
            return None
        die(f"could not read {source}: no such file")
    except Exception as e:  # noqa: BLE001 - report whatever it was, then stop
        die(f"could not read {source}: {e}")
    if not isinstance(data, dict):
        die(f"{source} is not a JSON object")
    return data


def sha256_and_size(path):
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size
