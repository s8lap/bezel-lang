"""scripts/lane_setup.sh build/ layout (#5010).

A worktree that already lives on the fast filesystem must get a REAL in-tree
build/ (the precondition for CMake's relocatable ccache keys); a worktree on
another filesystem keeps the build/ -> <fast root>/<name> symlink.

The "fast disk" here is /dev/shm: lane_setup only requires that the fast root's
PARENT be a mount point, and /dev/shm is one on every Linux box, so the test
needs no real NVMe and never writes to /mnt/fast. Everything it creates is
removed in tearDown. BEZEL_AOT_FAST_ROOT points at a non-mount so the AOT
bincache symlinks under /var/tmp/bezel-scratch are never touched.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from orch_tools_support import Repo, git_env, run

SHM = "/dev/shm"


def _is_mount(p):
    return subprocess.run(["mountpoint", "-q", p]).returncode == 0


@unittest.skipUnless(os.path.isdir(SHM) and _is_mount(SHM),
                     "/dev/shm is not a mount point on this host")
class LaneSetupBuildLayoutTest(unittest.TestCase):
    def setUp(self):
        self._slow = tempfile.TemporaryDirectory(prefix="lane-layout-")
        self._fast = tempfile.mkdtemp(prefix="bezel-lane-layout-", dir=SHM)
        self.env = git_env(self._slow.name)
        self.fast_root = self._fast + "-lane-builds"   # parent = /dev/shm (a mount)
        self.env.update(BEZEL_LANE_BUILD_ROOT=self.fast_root,
                        BEZEL_AOT_FAST_ROOT=self._slow.name + "/nofast")

    def tearDown(self):
        self._slow.cleanup()
        shutil.rmtree(self._fast, ignore_errors=True)
        shutil.rmtree(self.fast_root, ignore_errors=True)

    def _setup(self, repo):
        r = run("lane_setup.sh", cwd=repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        return r

    def test_worktree_on_the_fast_disk_gets_a_real_in_tree_build_dir(self):
        repo = Repo(self._fast + "/repo", self.env)
        r = self._setup(repo)
        build = repo.root / "build"
        self.assertTrue(build.is_dir(), r.stdout)
        self.assertFalse(build.is_symlink(),
                         "a worktree already on the fast disk must not get the "
                         "symlink: relocatable ccache keys need an in-tree "
                         "build dir (#5010)\n" + r.stdout)
        self.assertIn("REAL in-tree", r.stdout)
        leftovers = list(pathlib.Path(self.fast_root).glob("*")) \
            if os.path.isdir(self.fast_root) else []
        self.assertEqual(leftovers, [], "no out-of-tree build dir may be created")

    def test_rerun_on_the_fast_disk_is_idempotent(self):
        repo = Repo(self._fast + "/repo", self.env)
        self._setup(repo)
        (repo.root / "build" / "CMakeCache.txt").write_text("# configured\n")
        r = self._setup(repo)
        self.assertFalse((repo.root / "build").is_symlink())
        self.assertTrue((repo.root / "build" / "CMakeCache.txt").exists(),
                        "a configured in-tree build must be left alone\n" + r.stdout)

    def test_worktree_on_another_disk_keeps_the_symlink(self):
        repo = Repo(self._slow.name + "/repo", self.env)
        if os.stat(repo.root).st_dev == os.stat(SHM).st_dev:
            self.skipTest("TMPDIR is on the same filesystem as /dev/shm")
        r = self._setup(repo)
        build = repo.root / "build"
        self.assertTrue(build.is_symlink(), r.stdout)
        self.assertTrue(os.readlink(build).startswith(self.fast_root + "/"),
                        os.readlink(build))


if __name__ == "__main__":
    unittest.main()
