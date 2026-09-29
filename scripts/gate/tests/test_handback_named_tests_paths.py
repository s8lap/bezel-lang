"""check_named_tests: scripts/gate/tests/*.py paths and Suite.name from scripts/ are not false-positives.

Repro for #5098: check_named_tests had two compounding bugs:
1. The path-extraction regex \btests/... matched the / boundary inside
   scripts/gate/tests/..., silently stripping the scripts/gate/ prefix.
2. The Suite.name fallback grep was restricted to -- tests/ only, never
   searching scripts/gate/tests/*.py.
"""
import re
import tempfile
import unittest

from orch_tools_support import WANT, Repo, git_env, run


class HandbackScriptsGatePathNotFalsePositive(unittest.TestCase):
    """Test that scripts/gate/tests/*.py paths in commit messages don't false-positive."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="handback-scripts-gate-")
        self.env = git_env(self._tmp.name)
        self.repo = Repo(self._tmp.name + "/repo", self.env)
        self.repo.git("switch", "-q", "-c", "lane")
        self.repo.change("src/sema/a.cpp", "fix #5098: a\n\n" + WANT)
        # Create the scripts/gate/tests/ dir and a Python gate test in it
        self.repo.write("scripts/gate/tests/test_gate_foo.py",
                        "# gate test\nSelfTestSuiteIsolationTest.KNOWN = True\n")
        self.repo.commit("fix #5098 part 2\n\n"
                         "Adds SelfTestSuiteIsolationTest.KNOWN\n"
                         "and scripts/gate/tests/test_gate_foo.py.\n\n" + WANT,
                         ["scripts/gate/tests/test_gate_foo.py"])
        self.tip = self.repo.out("rev-parse", "HEAD")

    def tearDown(self):
        self._tmp.cleanup()

    def check(self, *args, branch="lane"):
        return run("handback_check.sh", branch, "--base", "main",
                   "--repo", str(self.repo.root), *args, cwd=self.repo.root, env=self.env)

    def row(self, out, name):
        m = re.search(r"^(PASS|WARN|FAIL|SKIP)\s+%s\b.*$" % re.escape(name), out, re.M)
        self.assertIsNotNone(m, "no %s row in:\n%s" % (name, out))
        return m.group(0)

    def test_scripts_gate_python_test_path_not_false_positive(self):
        """scripts/gate/tests/test_gate_foo.py referenced in a commit message must not WARN.

        Bug #5098: the path-extraction regex \btests/... matched the / boundary
        inside scripts/gate/tests/, so grep -o extracted only
        tests/test_gate_foo.py (stripping scripts/gate/), and the subsequent
        git cat-file check looked for the wrong path. The fallback now also tries
        scripts/gate/<path>.
        """
        # Add a commit that *mentions* the scripts/gate/tests/ path
        self.repo.change("src/sema/b.cpp",
                         "fix #5098: b\n\n"
                         "See scripts/gate/tests/test_gate_foo.py for details.\n\n" + WANT)
        r = self.check()
        # named-tests should PASS (the path exists at the tip)
        nt = self.row(r.stdout, "named-tests")
        self.assertTrue(nt.startswith("PASS"), nt)

    def test_suite_name_from_scripts_gate_tests_not_false_positive(self):
        """A Suite.name that lives only in scripts/gate/tests/*.py must not WARN.

        Bug #5098: the Suite.name fallback grep used -- tests/ only, so a name
        defined in scripts/gate/tests/test_gate_markers.py was never searched
        there and was always reported missing. The pathspec now includes
        scripts/gate/tests/.
        """
        # The setUp commit already added SelfTestSuiteIsolationTest.KNOWN
        # to scripts/gate/tests/test_gate_foo.py.
        # Add a commit that names it (as if a commit message referenced the class attr).
        self.repo.change("src/sema/c.cpp",
                         "fix #5098: c\n\n"
                         "Uses SelfTestSuiteIsolationTest.KNOWN.\n\n" + WANT)
        r = self.check()
        nt = self.row(r.stdout, "named-tests")
        self.assertTrue(nt.startswith("PASS"), nt)


class HandbackRealMissingTestStillWarns(unittest.TestCase):
    """Test that a genuinely missing tests/ path still WARNs after the fix."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="handback-real-missing-")
        self.env = git_env(self._tmp.name)
        self.repo = Repo(self._tmp.name + "/repo", self.env)
        self.repo.git("switch", "-q", "-c", "lane")
        self.repo.change("src/sema/a.cpp", "fix #5098: a\n\n" + WANT)
        self.repo.change("src/sema/d.cpp",
                         "fix #5098: d\n\n"
                         "No such file: tests/sema/nonexistent_5098_buggy.cpp.\n\n" + WANT)
        self.tip = self.repo.out("rev-parse", "HEAD")

    def tearDown(self):
        self._tmp.cleanup()

    def check(self, *args, branch="lane"):
        return run("handback_check.sh", branch, "--base", "main",
                   "--repo", str(self.repo.root), *args, cwd=self.repo.root, env=self.env)

    def row(self, out, name):
        m = re.search(r"^(PASS|WARN|FAIL|SKIP)\s+%s\b.*$" % re.escape(name), out, re.M)
        self.assertIsNotNone(m, "no %s row in:\n%s" % (name, out))
        return m.group(0)

    def test_real_missing_test_still_warns(self):
        """A genuinely missing tests/ path must still WARN after the fix."""
        r = self.check()
        nt = self.row(r.stdout, "named-tests")
        self.assertTrue(nt.startswith("WARN"), nt)
        self.assertIn("tests/sema/nonexistent_5098_buggy.cpp", nt)


if __name__ == "__main__":
    unittest.main()
