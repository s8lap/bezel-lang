"""Non-author review follow-up (r21-jobserver) on the machine-wide build
jobserver (scripts/build_jobserver.py, introduced #... "Add an opt-in
machine-wide build jobserver for lane builds", refined "jobserver: address
non-author review notes"). scripts/gate/tests/test_build_jobserver.py already
covers build_jobserver.py itself (rewrite-args, the token pool, self-repair,
liveness); this module covers the two OTHER scripts the jobserver design
depends on, neither of which had a test:

  1. scripts/dev_heavy.sh's jobserver hook must strip -j/--parallel and
     export MAKEFLAGS with the live daemon's fifo ONLY when ALL of: a live
     daemon exists, BEZEL_JOBSERVER isn't "off", MAKEFLAGS isn't already set,
     and the command is one of cmake/make/gmake/ninja — and must leave
     argv/env untouched in every other case.        -> DevHeavyJobserverHookTest
  2. scripts/gate.sh must export BEZEL_JOBSERVER=off before its first heavy
     build: a gate build runs with zero other builders (CLAUDE.md,
     "Concurrency discipline") and must not depend on a daemon's health.
                                                      -> GateShJobserverOptOutTest

HOW #1 WORKS. dev_heavy.sh's only exit for a `build`/`suite` kind is `exec
systemd-run …`, which needs a live systemd --user session and actually
launches the given command — neither is appropriate for a unit test. So
dev_heavy.sh grew a minimal, documented test seam: BEZEL_DEV_HEAVY_DRY_RUN=1
makes it print the final argv and the MAKEFLAGS/CMAKE_BUILD_PARALLEL_LEVEL it
would export, then exit 0, instead of exec'ing systemd-run. This module drives
the REAL script through that seam — nothing here re-implements or greps the
hook's own logic — with a REAL build_jobserver.py `serve` daemon in a
tempdir for the "live daemon" cases (same pattern as test_build_jobserver.py's
DaemonTestBase).

HOW #2 WORKS. gate.sh's build is many minutes of the real compiler; nothing
here builds it. The test reads gate.sh's own TEXT and pins that `export
BEZEL_JOBSERVER=off` appears exactly once, strictly before the first `"$DH"
build` call — the property the whole opt-out depends on, since dev_heavy.sh
only consults BEZEL_JOBSERVER for `kind = build`. Same source-only pin style
as test_gate_full_cycle_pins.py's GateShNoTidyTest.

Stdlib only (subprocess/tempfile/unittest). Neither class here touches the
gate's push entry point (a separate script that publishes the running gate's
own pid/exit-status markers), so this module is out of scope for
test_gate_markers.SelfTestSuiteIsolationTest.KNOWN.
"""

import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent.parent          # scripts/
DEV_HEAVY_SH = SCRIPTS / "dev_heavy.sh"
BUILD_JOBSERVER_PY = SCRIPTS / "build_jobserver.py"
GATE_SH = SCRIPTS / "gate.sh"


def _wait_for(pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return pred()


def _alive(d):
    return subprocess.run([sys.executable, str(BUILD_JOBSERVER_PY), "alive", "--dir", d],
                          capture_output=True).returncode == 0


class DevHeavyJobserverHookTest(unittest.TestCase):
    """dev_heavy.sh build <cmd> …: the jobserver rewrite must fire in exactly
    one configuration and leave argv/env untouched in every other one. One
    shared daemon for the whole class — the hook only ever reads it (`alive`,
    `rewrite-args`), never mutates its token pool, since BEZEL_DEV_HEAVY_DRY_RUN
    exits before any real build would take or return a token."""

    @classmethod
    def setUpClass(cls):
        cls.daemon_dir = tempfile.mkdtemp(prefix="bezel-js-hook-test-")
        cls.daemon = subprocess.Popen(
            [sys.executable, str(BUILD_JOBSERVER_PY), "serve", "--dir", cls.daemon_dir,
             "--tokens", "2", "--interval", "5"])
        if not _wait_for(lambda: _alive(cls.daemon_dir)):
            cls.daemon.terminate()
            cls.daemon.wait(timeout=10)
            raise RuntimeError("test daemon never came up")

    @classmethod
    def tearDownClass(cls):
        cls.daemon.terminate()
        try:
            cls.daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            cls.daemon.kill()
        shutil.rmtree(cls.daemon_dir, ignore_errors=True)

    def _run(self, argv, env_extra=None, daemon_dir=None):
        """Run `dev_heavy.sh build <argv...>` for real, in dry-run mode, and
        return (final_argv, MAKEFLAGS) as reported by the seam."""
        env = dict(os.environ)
        env["BEZEL_DEV_HEAVY_DRY_RUN"] = "1"
        env["BEZEL_ALLOW_TMPFS_CCACHE"] = "1"  # unrelated guard, not under test
        env["BEZEL_JOBSERVER_DIR"] = daemon_dir if daemon_dir is not None else self.daemon_dir
        for k in ("BEZEL_JOBSERVER", "MAKEFLAGS", "CMAKE_BUILD_PARALLEL_LEVEL"):
            env.pop(k, None)
        if env_extra:
            env.update(env_extra)
        r = subprocess.run([str(DEV_HEAVY_SH), "build"] + list(argv),
                           capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(
            r.returncode, 0,
            "dev_heavy.sh build %r: rc=%s\nstdout:\n%s\nstderr:\n%s"
            % (argv, r.returncode, r.stdout, r.stderr))
        lines = r.stdout.splitlines()
        argv_line = next((l for l in lines if l.startswith("DRY_RUN argv:")), None)
        mf_line = next((l for l in lines if l.startswith("DRY_RUN MAKEFLAGS=")), None)
        self.assertIsNotNone(argv_line, "no DRY_RUN argv line in:\n%s" % r.stdout)
        self.assertIsNotNone(mf_line, "no DRY_RUN MAKEFLAGS line in:\n%s" % r.stdout)
        final_argv = shlex.split(argv_line.split("DRY_RUN argv:", 1)[1])
        makeflags = mf_line.split("DRY_RUN MAKEFLAGS=", 1)[1]
        return final_argv, makeflags

    def test_live_daemon_strips_dash_j_and_exports_makeflags(self):
        argv, makeflags = self._run(["make", "-j16", "-f", "Makefile", "all"])
        self.assertEqual(argv, ["make", "-f", "Makefile", "all"],
                         "-j16 must be stripped when the build joins the shared pool "
                         "(an explicit -jN makes GNU make ignore an inherited pool)")
        self.assertEqual(makeflags,
                         "-j --jobserver-auth=fifo:%s/fifo" % self.daemon_dir)

    def test_bezel_jobserver_off_does_not_rewrite(self):
        argv, makeflags = self._run(["make", "-j16", "-f", "Makefile", "all"],
                                    env_extra={"BEZEL_JOBSERVER": "off"})
        self.assertEqual(argv, ["make", "-j16", "-f", "Makefile", "all"])
        self.assertEqual(makeflags, "<unset>")

    def test_preset_makeflags_is_left_exactly_as_the_caller_set_it(self):
        argv, makeflags = self._run(["make", "-j16", "-f", "Makefile", "all"],
                                    env_extra={"MAKEFLAGS": "-j4"})
        self.assertEqual(argv, ["make", "-j16", "-f", "Makefile", "all"])
        self.assertEqual(makeflags, "-j4")

    def test_no_daemon_does_not_rewrite(self):
        with tempfile.TemporaryDirectory() as empty:
            argv, makeflags = self._run(["make", "-j16", "-f", "Makefile", "all"],
                                        daemon_dir=empty)
            self.assertEqual(argv, ["make", "-j16", "-f", "Makefile", "all"])
            self.assertEqual(makeflags, "<unset>")

    def test_non_make_command_does_not_rewrite(self):
        argv, makeflags = self._run(["bezelc", "-o", "out", "foo.bzl"])
        self.assertEqual(argv, ["bezelc", "-o", "out", "foo.bzl"])
        self.assertEqual(makeflags, "<unset>")

    def test_cmake_gmake_ninja_are_also_recognized(self):
        """The hook keys on basename($1), not a hardcoded 'make'."""
        argv, makeflags = self._run(["cmake", "--build", "build", "-j16", "--target", "t"])
        self.assertEqual(argv, ["cmake", "--build", "build", "--target", "t"])
        self.assertNotEqual(makeflags, "<unset>")


class GateShJobserverOptOutTest(unittest.TestCase):
    """gate.sh must opt out of the shared jobserver (BEZEL_JOBSERVER=off)
    before its first `"$DH" build` invocation — a gate runs with zero other
    builders and its build must not depend on a daemon's health."""

    def test_opt_out_is_exported_exactly_once_before_the_first_dev_heavy_build_call(self):
        text = GATE_SH.read_text()
        opt_out = "export BEZEL_JOBSERVER=off"
        idx = text.find(opt_out)
        self.assertGreater(idx, -1,
                           "gate.sh no longer exports BEZEL_JOBSERVER=off at all")
        self.assertEqual(text.find(opt_out, idx + 1), -1,
                         "expected exactly one export of BEZEL_JOBSERVER=off "
                         "(a second, later one could shadow an earlier opt-out "
                         "with a value other than off)")
        first_dh_build = text.find('"$DH" build')
        self.assertGreater(first_dh_build, -1,
                           'gate.sh no longer calls "$DH" build at all — if the '
                           "heavy-op call convention changed, re-point this pin")
        self.assertLess(
            idx, first_dh_build,
            "BEZEL_JOBSERVER=off must be exported BEFORE the first \"$DH\" build "
            "call, or that call could join the shared pool and make a gate "
            "build depend on the daemon's health")


if __name__ == "__main__":
    unittest.main()
