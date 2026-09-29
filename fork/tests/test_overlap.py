"""overlap.py on a scratch repository. Run: python3 -m unittest discover -s fork/tests"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
OVERLAP = os.path.join(HERE, "..", "scripts", "overlap.py")
ENV = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
       "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid", "GIT_CONFIG_GLOBAL": "/dev/null"}

# two functions, each long enough that an edit near the top and one near the bottom do not touch
BODY = "".join(f"    step_{i}();\n" for i in range(12))
SOURCE = f"int render_home(void)\n{{\n{BODY}    return 0;\n}}\n\nint other(void)\n{{\n{BODY}    return 1;\n}}\n"


def run(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], check=True, capture_output=True, text=True, env=ENV).stdout.strip()


def edit(repo, path, old, new, message):
    full = os.path.join(repo, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    text = open(full).read() if os.path.exists(full) else ""
    open(full, "w").write(text.replace(old, new, 1) if old else text + new)
    run(repo, "add", "-A")
    run(repo, "commit", "-qm", message)


class Overlap(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        r = self.r = self.d.name
        run(r, "init", "-q", "-b", "main")
        edit(r, "ui.c", "", SOURCE, "base")
        self.base = run(r, "rev-parse", "HEAD")
        # the fork's patch edits the top of render_home
        run(r, "checkout", "-q", "-b", "stack")
        edit(r, "ui.c", "step_1();", "step_1_fork();", "patch: recap tap")
        edit(r, "fork/tool.py", "", "x = 1\n", "fork tooling")            # ignored: fork-only
        run(r, "checkout", "-q", "main")

    def tearDown(self):
        self.d.cleanup()

    def report(self, *extra):
        out = subprocess.run([sys.executable, OVERLAP, "--repo", self.r, "--old-base", self.base,
                              "--upstream", "main", "--head", "stack", *extra], check=True, capture_output=True, text=True)
        return out.stdout

    def test_nothing_new_upstream_is_silent(self):
        self.assertEqual(self.report(), "")

    def test_same_function_is_the_strong_flag(self):
        edit(self.r, "ui.c", "step_9();", "step_9_upstream();", "upstream rewrites render_home")
        text = self.report()
        self.assertIn("review: may supersede or interfere (same function)", text)
        self.assertIn("patch", text)
        self.assertIn("recap tap", text)
        self.assertIn("upstream rewrites render_home", text)
        self.assertIn("render_home", text)
        self.assertNotIn("fork tooling", text)

    def test_other_function_same_file_is_the_weak_flag(self):
        # step_9 also exists in other(); take the second occurrence
        path = os.path.join(self.r, "ui.c")
        text = open(path).read()
        head, tail = text.split("int other(void)")
        open(path, "w").write(head + "int other(void)" + tail.replace("step_9();", "step_9_upstream();", 1))
        run(self.r, "add", "-A"); run(self.r, "commit", "-qm", "upstream touches other")
        text = self.report()
        self.assertIn("review: touches the same file", text)
        self.assertNotIn("same function", text)

    def test_unrelated_file_is_silent(self):
        edit(self.r, "README.md", "", "hello\n", "upstream docs")
        self.assertEqual(self.report(), "")

    def test_watch_list_flags_a_named_symbol_without_a_patch_overlap(self):
        edit(self.r, "theme/colors.h", "", "#define HT_THEME_CANVAS 0x000000\n", "upstream moves the canvas colour")
        watch = os.path.join(self.r, "watch.txt")
        open(watch, "w").write("# comment\nblack canvas :: theme/ :: HT_THEME_CANVAS\n")
        text = self.report("--watch", watch)
        self.assertIn("review: watched areas changed upstream", text)
        self.assertIn("black canvas", text)

    def test_json_output(self):
        edit(self.r, "ui.c", "step_9();", "step_9_upstream();", "upstream rewrites render_home")
        data = json.loads(self.report("--json"))
        self.assertEqual(data["patches"][0]["hits"][0]["level"], "function")


if __name__ == "__main__":
    unittest.main()
