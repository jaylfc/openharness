#!/usr/bin/env python3
"""Make the macOS Runner's Release configuration buildable without upstream's signing identity.

The Xcode project signs Release with team 54DJVWMJCC's "Developer ID Application" certificate and asks for
the hardened runtime plus a secure timestamp, all for notarization. CI on the fork has none of that, and
`flutter build macos` passes no signing overrides through to xcodebuild, so the settings have to change in
the project file itself. This edits the WORKING TREE of a CI runner only; nothing is committed, so the
patch stack never touches macos/Runner.xcodeproj.

Each substitution must match exactly once. If upstream reshapes these settings the script fails loudly
(and the build stops) rather than building something signed with the wrong identity or not at all.

Hardened runtime is switched off because it enables library validation: an ad-hoc signed app cannot load
its own ad-hoc signed frameworks under it (no shared Team ID), so the app would die at launch. Nothing
needs it: the fork is not notarized and the updater downloads without a quarantine flag.
"""
import re
import sys

PATH = "desktop/macos/Runner.xcodeproj/project.pbxproj"

EDITS = [
    (r'CODE_SIGN_IDENTITY = "Developer ID Application";', 'CODE_SIGN_IDENTITY = "-";'),
    (r"\n\t*DEVELOPMENT_TEAM = 54DJVWMJCC;", ""),
    (r'\n\t*OTHER_CODE_SIGN_FLAGS = "--timestamp";', ""),
    (r"ENABLE_HARDENED_RUNTIME = YES;", "ENABLE_HARDENED_RUNTIME = NO;"),
]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else PATH
    with open(path) as f:
        text = f.read()
    for pattern, replacement in EDITS:
        text, n = re.subn(pattern, replacement, text)
        if n != 1:
            sys.exit(f"error: expected exactly one match for {pattern!r} in {path}, found {n}; "
                     "upstream changed the Runner signing settings, update fork/scripts/macos_adhoc.py")
    with open(path, "w") as f:
        f.write(text)
    print(f"{path}: Release signing set to ad hoc (no team, no hardened runtime, no timestamp)")


if __name__ == "__main__":
    main()
