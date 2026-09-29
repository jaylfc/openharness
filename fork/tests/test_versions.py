"""Unit checks for the version rule and manifest generation. Run: python3 -m unittest discover fork/tests"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(HERE, "..", "scripts")
sys.path.insert(0, SCRIPTS)

from forklib import fmt, next_version, parse_version  # noqa: E402


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f)


def plan(upstream, fork):
    with tempfile.TemporaryDirectory() as d:
        up, fk = os.path.join(d, "up"), os.path.join(d, "fork")
        for base, versions in ((up, upstream), (fk, fork)):
            if versions is None:
                continue
            write(os.path.join(base, "desktop/metadata.json"), {"desktop-macos-arm64": {"version": versions[0]}, "desktop-linux-x64": {"version": versions[0]}})
            write(os.path.join(base, "cli/metadata.json"), {"cli": {"version": versions[1]}})
            write(os.path.join(base, "esp32/ota/metadata.json"), {"commander": {"version": versions[2]}, "flasher": {"version": "99.0.0"}})
        out = subprocess.run([sys.executable, os.path.join(SCRIPTS, "plan_versions.py"), "--upstream-base", up, "--fork-base", fk],
                             check=True, capture_output=True, text=True).stdout
        return json.loads(out)


class Versions(unittest.TestCase):
    def test_patch_bump_and_rollover(self):
        self.assertEqual(fmt(next_version(parse_version("1.2.27"))), "1.2.28")
        self.assertEqual(fmt(next_version(parse_version("1.2.99"))), "1.3.1")

    def test_rejects_non_plain_versions(self):
        self.assertIsNone(parse_version("1.2.3-dev.abc"))
        self.assertIsNone(parse_version("v1.2.3"))

    def test_first_release_is_above_upstream(self):
        p = plan(("1.2.27", "0.3.27", "0.0.86"), None)
        self.assertEqual((p["desktop"]["version"], p["cli"]["version"], p["firmware"]["version"]), ("1.2.28", "0.3.28", "0.0.87"))
        self.assertIsNone(p["desktop"]["fork_latest"])

    def test_takes_the_higher_of_the_two_channels(self):
        # fork ahead of upstream: fork + 1
        self.assertEqual(plan(("1.2.27", "0.3.27", "0.0.86"), ("1.2.30", "0.3.28", "0.0.90"))["desktop"]["version"], "1.2.31")
        # upstream overtook the fork: upstream + 1, so the fork build stays strictly higher than both
        p = plan(("1.2.40", "0.3.50", "0.0.95"), ("1.2.30", "0.3.28", "0.0.90"))
        self.assertEqual((p["desktop"]["version"], p["cli"]["version"], p["firmware"]["version"]), ("1.2.41", "0.3.51", "0.0.96"))

    def test_other_manifest_keys_do_not_count(self):
        # `flasher` in the firmware manifest carries 99.0.0 above; it must not drag the dial's version up
        self.assertEqual(plan(("1.2.27", "0.3.27", "0.0.86"), None)["firmware"]["version"], "0.0.87")


class Manifests(unittest.TestCase):
    def test_partial_build_keeps_previous_entries_and_never_names_missing_assets(self):
        with tempfile.TemporaryDirectory() as d:
            assets, channel = os.path.join(d, "a"), os.path.join(d, "c")
            os.makedirs(assets)
            for name in ("cli.js", "notify.mjs"):
                open(os.path.join(assets, name), "w").write("x")
            write(os.path.join(channel, "harness/desktop/metadata.json"), {"desktop-linux-x64": {"version": "1.2.28", "url": "old", "sha256": "0", "size": 1}})
            planfile = os.path.join(d, "plan.json")
            write(planfile, {"desktop": {"version": "1.2.29"}, "cli": {"version": "0.3.28"}, "firmware": {"version": "0.0.87"}})
            subprocess.run([sys.executable, os.path.join(SCRIPTS, "make_manifests.py"), "--assets", assets, "--plan", planfile,
                            "--channel", channel, "--repo", "o/r", "--tag", "fork-20260101-1"], check=True, capture_output=True)
            desktop = json.load(open(os.path.join(channel, "harness/desktop/metadata.json")))
            self.assertEqual(desktop["desktop-linux-x64"]["version"], "1.2.28")      # kept, not blanked
            self.assertNotIn("desktop-macos-arm64", desktop)                            # not built, not named
            self.assertFalse(os.path.exists(os.path.join(channel, "harness/esp32/ota/metadata.json")))
            cli = json.load(open(os.path.join(channel, "harness/cli/metadata.json")))["cli"]
            self.assertEqual(cli["cli"]["url"], "https://github.com/o/r/releases/download/fork-20260101-1/cli.js")
            state = json.load(open(os.path.join(channel, "harness/state.json")))
            self.assertEqual(state["versions"], {"cli": "0.3.28"})

    def test_half_a_cli_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            assets = os.path.join(d, "a"); os.makedirs(assets)
            open(os.path.join(assets, "cli.js"), "w").write("x")
            planfile = os.path.join(d, "plan.json")
            write(planfile, {"desktop": {"version": "1.2.29"}, "cli": {"version": "0.3.28"}, "firmware": {"version": "0.0.87"}})
            r = subprocess.run([sys.executable, os.path.join(SCRIPTS, "make_manifests.py"), "--assets", assets, "--plan", planfile,
                                "--channel", os.path.join(d, "c"), "--repo", "o/r", "--tag", "t"], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
