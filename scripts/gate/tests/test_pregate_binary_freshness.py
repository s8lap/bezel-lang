"""pregate must catch a test binary older than the build inputs (R39 gate 1: a
stale bezel_tests_support made four gate self-tests skip), WITHOUT going red on
commits that touch no build input (the remedy build would be a no-op, so the
row would stay FAIL forever). mtime-based, not commit-time-based.
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.dirname(HERE)
SCRIPTS = os.path.dirname(GATE)
ROOT = os.path.dirname(SCRIPTS)
sys.path.insert(0, SCRIPTS)
from gate import binary_freshness as bf  # noqa: E402

T0 = 1_700_000_000  # all times below are offsets from this, set with os.utime


def _scratch():
    d = os.path.join(ROOT, "scratch")
    os.makedirs(d, exist_ok=True)
    return tempfile.TemporaryDirectory(dir=d)


def _touch(path, t, text="x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.utime(path, (t, t))


class FreshnessTest(unittest.TestCase):
    def test_fresh_passes(self):
        with _scratch() as d:
            b = os.path.join(d, "bezelc")
            _touch(b, 2000)
            self.assertEqual(bf.check([b], 1000), [])

    def test_older_binary_fails_naming_it_and_both_dates(self):
        with _scratch() as d:
            b = os.path.join(d, "bezel_tests_support")
            _touch(b, 1000)
            probs = bf.check([b], 2000, "newest build input src/a.cpp")
            self.assertEqual(len(probs), 1)
            self.assertIn(b, probs[0])
            self.assertIn("OLDER", probs[0])
            self.assertEqual(len(re.findall(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", probs[0])), 2)

    def test_missing_binary_fails(self):
        with _scratch() as d:
            self.assertIn("MISSING", bf.check([os.path.join(d, "nope")], 0)[0])

    def _fake_repo(self, d, support_t):
        """A git repo whose sources are at T0, bezelc at T0+10, support at support_t."""
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        self.git = lambda *a: subprocess.run(["git", "-C", d, *a], env=env, check=True,
                                             capture_output=True)
        self.git("init", "-q")
        _touch(os.path.join(d, "src", "a.cpp"), T0)
        _touch(os.path.join(d, "tests", "t.cpp"), T0)
        _touch(os.path.join(d, "tests", "conformance", "c.txt"), T0)
        _touch(os.path.join(d, "CMakeLists.txt"), T0)
        self.git("add", ".")
        self.git("commit", "-q", "-m", "init")
        sup = os.path.join(d, "build", "tests", "bezel_tests_support")
        _touch(os.path.join(d, "build", "bezelc"), T0 + 10)
        _touch(sup, support_t)
        with open(os.path.join(d, "build", "tests", "test_binaries.txt"), "w") as f:
            f.write("support\t%s\t46\n" % sup)
        return sup

    def _run(self, d):
        return subprocess.run([sys.executable, os.path.join(GATE, "binary_freshness.py"),
                               "--root", d], capture_output=True, text=True)

    def test_stale_support_fails_via_cli(self):
        with _scratch() as d:
            sup = self._fake_repo(d, support_t=T0 - 100)
            r = self._run(d)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn(sup, r.stdout)
            self.assertIn("scripts/dev_heavy.sh build cmake --build build --target bezel_tests bezelc",
                          r.stdout)

    def test_up_to_date_build_passes_via_cli(self):
        with _scratch() as d:
            self._fake_repo(d, support_t=T0 + 10)
            r = self._run(d)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_conformance_only_commit_after_build_passes(self):
        # [3] a harvest/corpus commit touches no build input: the build is
        # still up to date, so the row must stay PASS (a rebuild is a no-op).
        with _scratch() as d:
            self._fake_repo(d, support_t=T0 + 10)
            _touch(os.path.join(d, "tests", "conformance", "c.txt"), T0 + 50, "new case")
            _touch(os.path.join(d, "docs", "d.md"), T0 + 50)
            self.git("add", ".")
            self.git("commit", "-q", "-m", "corpus bank")
            r = self._run(d)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_edit_build_then_commit_passes(self):
        # [4] the usual lane order: src edited at T0+5, built at T0+10, committed later.
        with _scratch() as d:
            self._fake_repo(d, support_t=T0 + 10)
            _touch(os.path.join(d, "src", "a.cpp"), T0 + 5, "edited")
            self.git("commit", "-q", "-am", "fix")
            r = self._run(d)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_source_edited_after_build_fails_naming_the_input(self):
        with _scratch() as d:
            self._fake_repo(d, support_t=T0 + 10)
            _touch(os.path.join(d, "src", "a.cpp"), T0 + 20, "edited")
            r = self._run(d)
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn("newest build input src/a.cpp", r.stdout)

    def test_untracked_new_test_cpp_and_cmakelists_count_but_not_data(self):
        with _scratch() as d:
            self._fake_repo(d, support_t=T0 + 10)
            _touch(os.path.join(d, "tests", "lit", "x.test"), T0 + 30)
            self.assertEqual(self._run(d).returncode, 0)
            _touch(os.path.join(d, "tests", "new_test.cpp"), T0 + 30)
            r = self._run(d)
            self.assertEqual(r.returncode, 1)
            self.assertIn("tests/new_test.cpp", r.stdout)
            os.remove(os.path.join(d, "tests", "new_test.cpp"))
            _touch(os.path.join(d, "tests", "sub", "CMakeLists.txt"), T0 + 30)
            self.assertEqual(self._run(d).returncode, 1)


if __name__ == "__main__":
    unittest.main()
