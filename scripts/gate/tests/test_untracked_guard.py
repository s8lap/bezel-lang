r"""#5265: pregate preflight and gate_and_push refuse untracked files in build inputs.

CMake globs src/ and tests/ into the gate binary, so an untracked file there
means the gate tests a binary that is not the commit it reports (gate B run 1
false FAIL, R25 2026-10-02). `scripts/untracked_guard.sh` is the shared check;
pregate.sh's preflight and gate_and_push.sh both call it. Untracked files
outside src/ tests/ scripts/ CMakeLists.txt cmake/ only WARN. Each test builds
a throwaway git repo."""

import os
import shutil
import subprocess
import tempfile
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.normpath(os.path.join(HERE, "..", ".."))
GUARD = os.path.join(SCRIPTS, "untracked_guard.sh")
sys.path.insert(0, HERE)
from marker_isolation import isolated_markers  # noqa: E402


def _git(repo, *args):
    subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@t",
                    *args], check=True, capture_output=True)


class UntrackedGuardTest(unittest.TestCase):
    def setUp(self):
        self.repo = tempfile.mkdtemp(prefix="untracked-guard-")
        self.addCleanup(shutil.rmtree, self.repo, True)
        os.makedirs(os.path.join(self.repo, "tests"))
        for rel, body in (("tests/a.cpp", "x\n"), (".gitignore", "*.out\n")):
            with open(os.path.join(self.repo, rel), "w") as f:
                f.write(body)
        _git(self.repo, "init", "-q")
        _git(self.repo, "add", "tests/a.cpp", ".gitignore")
        _git(self.repo, "commit", "-q", "-m", "base")

    def _touch(self, rel):
        path = os.path.join(self.repo, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w").close()

    def _guard(self):
        return subprocess.run(["bash", GUARD, self.repo], capture_output=True, text=True)

    def test_clean_tree_passes_silently(self):
        r = self._guard()
        self.assertEqual((r.returncode, r.stdout), (0, ""))

    def test_untracked_under_tests_fails_and_names_file(self):
        self._touch("tests/module/stray_5251_test.cpp")
        r = self._guard()
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("FAIL tests/module/stray_5251_test.cpp", r.stdout)
        self.assertIn("Quarantine", r.stdout)

    def test_untracked_under_src_fails(self):
        self._touch("src/sema/new.cpp")
        r = self._guard()
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL src/sema/new.cpp", r.stdout)

    def test_lit_in_tree_output_does_not_trip(self):
        # lit.cfg.py falls back to test_exec_root = test_source_root and writes
        # these in-tree; the repo's real .gitignore must cover them (#5265).
        shutil.copy(os.path.join(SCRIPTS, "..", ".gitignore"),
                    os.path.join(self.repo, ".gitignore"))
        self._touch("tests/lit/.lit_test_times.txt")
        self._touch("tests/lit/Output/x.script")
        self._touch("tests/lit/sub/Output/y.tmp")
        r = self._guard()
        self.assertEqual((r.returncode, r.stdout), (0, ""))

    def test_untracked_only_at_repo_root_warns_not_fails(self):
        self._touch("repro_5242.bzl")
        r = self._guard()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("WARN repro_5242.bzl", r.stdout)
        self.assertNotIn("FAIL", r.stdout)

    def test_gitignored_untracked_file_does_not_trip(self):
        self._touch("tests/ignored.out")
        self._touch("junk.out")
        r = self._guard()
        self.assertEqual((r.returncode, r.stdout), (0, ""))


class WiringTest(unittest.TestCase):
    def _read(self, name):
        with open(os.path.join(SCRIPTS, name)) as f:
            return f.read()

    def test_pregate_preflight_calls_guard(self):
        self.assertIn("scripts/untracked_guard.sh", self._read("pregate.sh"))

    def test_gate_and_push_refuses_untracked_test_file_before_building(self):
        repo = tempfile.mkdtemp(prefix="untracked-gap-")
        self.addCleanup(shutil.rmtree, repo, True)
        os.makedirs(os.path.join(repo, "scripts"))
        os.makedirs(os.path.join(repo, "tests"))
        for n in ("gate_and_push.sh", "untracked_guard.sh"):
            shutil.copy(os.path.join(SCRIPTS, n), os.path.join(repo, "scripts", n))
        open(os.path.join(repo, "tests", "a.cpp"), "w").close()
        _git(repo, "init", "-q")
        _git(repo, "add", "scripts", "tests")
        _git(repo, "commit", "-q", "-m", "base")
        open(os.path.join(repo, "tests", "stray_test.cpp"), "w").close()
        with isolated_markers(dict(os.environ, GATE_SKIP_LOCK="1")) as env:
            r = subprocess.run(["bash", "scripts/gate_and_push.sh"], cwd=repo,
                               env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 65, r.stdout + r.stderr)
        self.assertIn("tests/stray_test.cpp", r.stderr)


if __name__ == "__main__":
    unittest.main()
