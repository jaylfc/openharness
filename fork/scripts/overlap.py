#!/usr/bin/env python3
"""Which upstream changes since the last sync touch what the fork's patches touch.

A clean rebase does not mean nothing happened: upstream can rewrite the function a patch edits, or add a
feature the patch now duplicates, and git will still apply both without complaint. The policy is that the
fork's side is reworked, never upstream's, so the person owning the stack needs to hear about every such
overlap while the change is fresh. This lists them, per patch, in the alert format the sync uses.

  overlap.py --old-base SHA --upstream SHA --head SHA [--watch FILE] [--json] [--repo DIR]

  --old-base  where the stack currently sits on upstream (git merge-base HEAD upstream/main, taken BEFORE
              the rebase); the upstream commits considered are old-base..upstream
  --head      the stack (patch commits are old-base..head); commits that only add fork tooling
              (fork/, fork-*.yml) are left out, they cannot overlap upstream
  --watch     extra areas to flag by name: lines of `label :: path regex :: content regex`

Two levels, strongest first:
  same function  a hunk of an upstream commit and a hunk of a patch sit in the same function of the same
                 file (git's own hunk header names it), so upstream likely rewrote or superseded that code
  same file      upstream changed a file the patch changed, elsewhere in it
plus the watch list, which flags an upstream diff that mentions a named symbol in a named place even when
no patch hunk overlaps it yet.

Exit status is 0 always; the report is empty when there is nothing to review.
"""
import argparse
import json
import re
import subprocess
import sys

FORK_ONLY = re.compile(r"^(fork/|\.github/workflows/fork-)")


def git(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], check=True, capture_output=True, text=True).stdout


def commits(repo, rng):
    out = git(repo, "rev-list", "--reverse", "--no-merges", rng).split()
    return out


def subject(repo, sha):
    return git(repo, "show", "-s", "--format=%s", sha).strip()


def parse(repo, sha):
    """files touched, and {(path, function header)} for every hunk, of one commit."""
    text = git(repo, "show", "--format=", "-U0", "--no-color", "--no-ext-diff", sha)
    files, funcs, lines = set(), set(), {}
    path = None
    for line in text.splitlines():
        if line.startswith("diff --git "):
            path = line.split(" b/", 1)[1]
            files.add(path)
        elif line.startswith("@@") and path:
            m = re.match(r"^@@ -\S+ \+\S+ @@ ?(.*)$", line)
            header = (m.group(1) if m else "").strip()
            if header:
                funcs.add((path, header))
        elif path and line[:1] in "+-" and not line.startswith(("+++", "---")):
            lines.setdefault(path, []).append(line[1:])
    return files, funcs, lines


def load_watch(path):
    entries = []
    if not path:
        return entries
    try:
        with open(path) as f:
            for raw in f:
                raw = raw.strip()
                if not raw or raw.startswith("#"):
                    continue
                label, pathre, contentre = [p.strip() for p in raw.split(" :: ", 2)]
                entries.append((label, re.compile(pathre), re.compile(contentre)))
    except FileNotFoundError:
        pass
    return entries


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old-base", required=True)
    ap.add_argument("--upstream", required=True)
    ap.add_argument("--head", required=True)
    ap.add_argument("--watch")
    ap.add_argument("--repo", default=".")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    repo = args.repo

    upstream = commits(repo, f"{args.old_base}..{args.upstream}")
    patches = [c for c in commits(repo, f"{args.old_base}..{args.head}")]
    info = {}
    for c in patches:
        files, funcs, _ = parse(repo, c)
        if files and all(FORK_ONLY.match(f) for f in files):
            continue
        info[c] = (files, funcs)

    result = {"upstream_commits": len(upstream), "patches": [], "watch": []}
    watch = load_watch(args.watch)

    for u in upstream:
        ufiles, ufuncs, ulines = parse(repo, u)
        for label, pathre, contentre in watch:
            for f, body in ulines.items():
                if pathre.search(f) and any(contentre.search(b) for b in body):
                    result["watch"].append({"label": label, "commit": u[:8], "subject": subject(repo, u), "file": f})
                    break
        for p, (pfiles, pfuncs) in info.items():
            same_funcs = sorted(ufuncs & pfuncs)
            same_files = sorted(ufiles & pfiles)
            if not same_files:
                continue
            entry = next((e for e in result["patches"] if e["patch"] == p[:8]), None)
            if entry is None:
                entry = {"patch": p[:8], "subject": subject(repo, p), "hits": []}
                result["patches"].append(entry)
            entry["hits"].append({
                "commit": u[:8], "subject": subject(repo, u),
                "level": "function" if same_funcs else "file",
                "files": same_files,
                "functions": [f"{path}: {hdr}" for path, hdr in same_funcs],
            })

    if args.json:
        json.dump(result, sys.stdout, indent=2)
        print()
        return

    if not result["patches"] and not result["watch"]:
        return
    out = []
    n_hit = len({h["commit"] for e in result["patches"] for h in e["hits"]})
    out.append(f"Upstream changed {len(upstream)} commit(s) since the last sync; {n_hit} touch files the fork's patches touch.")
    out.append("The fork side is reworked, never upstream's. Review each item below.")
    for level, title in (("function", "review: may supersede or interfere (same function)"), ("file", "review: touches the same file")):
        section = []
        for e in result["patches"]:
            hits = [h for h in e["hits"] if h["level"] == level]
            # a commit already listed at the stronger level is not repeated at the weaker one
            if level == "file":
                strong = {h["commit"] for h in e["hits"] if h["level"] == "function"}
                hits = [h for h in hits if h["commit"] not in strong]
            if not hits:
                continue
            section.append(f"- patch {e['patch']} {e['subject']}")
            for h in hits:
                detail = "; ".join(h["functions"]) if level == "function" else ", ".join(h["files"])
                section.append(f"  - upstream {h['commit']} {h['subject']} ({detail})")
        if section:
            out.append("")
            out.append(title)
            out.extend(section)
    if result["watch"]:
        out.append("")
        out.append("review: watched areas changed upstream")
        for w in result["watch"]:
            out.append(f"- {w['label']}: upstream {w['commit']} {w['subject']} ({w['file']})")
    print("\n".join(out))


if __name__ == "__main__":
    main()
