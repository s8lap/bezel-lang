"""Non-author review follow-up (r21-jobserver) on scripts/jobserver_ctl.sh:
`start` must refuse from a linked worktree (a lane, or any `git worktree add`)
unless the operator opts in with BEZEL_JOBSERVER_ALLOW_WORKTREE=1.

WHY THIS MATTERS. `start` embeds build_jobserver.py's resolved path in the
transient systemd unit's ExecStart, and a later Restart=on-failure restart
re-execs that SAME path. A linked worktree can be torn down while the daemon
it started keeps running (the daemon PROCESS survives — python already read
the whole file into memory once `serve` starts), but the next automatic
restart would then try to exec a path that no longer exists, and the daemon
would silently stop coming back with nothing to notice at the time. An
earlier design tried resolving build_jobserver.py to a "stable" path (git's
common dir, i.e. the main checkout) instead of refusing — that was measured
to be WORSE here: this repo routinely runs several worktrees on independent
branches/commits at once (CLAUDE.md, "Dual-coordinator runs"), so the main
checkout's own working tree can be on a branch that predates the script
entirely (measured while building this fix: this repo's own main checkout had
no scripts/build_jobserver.py at all at the time). Refusing is simpler and
does not depend on what the main checkout happens to have checked out.

HOW THIS IS TESTED, SAFELY. jobserver_ctl.sh's `start` talks to the REAL
`git`, `python3`, `systemctl` and `systemd-run` — none of which this test may
touch for real: a stray `systemd-run --user --unit=bezel-jobserver` would
collide with the unit name the machine's live production daemon actually
uses (confirmed by hand while building this fix — see the module for
BEZEL_JOBSERVER_ALLOW_WORKTREE below). So every test here runs the REAL
jobserver_ctl.sh with PATH pointing at throwaway stubs for exactly those four
tools, and reads back the stub's own log to confirm what was (or, for the
refused cases, was NOT) invoked. `git` is stubbed to report a configurable
--git-dir / --git-common-dir pair (the only inputs the guard reads), so the
guard's own comparison — not git's real worktree machinery — is what is
under test. `python3`'s stub always answers "alive", which makes a permitted
`start` short-circuit at jobserver_ctl.sh's own "already running" branch
before it would ever reach systemctl/systemd-run — so a permitted-start test
proves the guard did not block, without needing to fake an actual daemon
bring-up. `systemctl`/`systemd-run` are stubbed as harmless no-ops purely as
a second line of defense: if that short-circuit assumption is ever wrong, the
test's own log would show it, rather than silently reaching the real tools.

Stdlib only (subprocess/tempfile/unittest). No test here writes to
$BEZEL_JOBSERVER_DIR or a real systemd unit at any point. Neither class here
touches the gate's push entry point (a separate script that publishes the
running gate's own pid/exit-status markers), so this module is out of scope
for test_gate_markers.SelfTestSuiteIsolationTest.KNOWN.
"""

import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent.parent          # scripts/
JOBSERVER_CTL_SH = SCRIPTS / "jobserver_ctl.sh"

REFUSAL_NEEDLE = "refusing to start from a LINKED worktree"

_GIT_STUB = """#!/usr/bin/env bash
# Reports a fixed --git-dir / --git-common-dir pair from the environment;
# never touches a real repository. See test_jobserver_ctl_worktree_guard.py.
echo "git $*" >> "${STUB_LOG:?}"
for a in "$@"; do
  case "$a" in
    --git-dir) echo "${STUB_GIT_DIR:?}"; exit 0 ;;
    --git-common-dir) echo "${STUB_GIT_COMMON_DIR:?}"; exit 0 ;;
  esac
done
echo "git stub: unhandled args: $*" >&2
exit 1
"""

_PYTHON3_STUB = """#!/usr/bin/env bash
# Only understands `<script> alive ...` (always "yes") and
# `<script> status ...` (prints one harmless line). This makes a `start`
# that gets PAST the worktree guard short-circuit at jobserver_ctl.sh's own
# "already running" branch, so it never reaches systemctl/systemd-run.
echo "python3 $*" >> "${STUB_LOG:?}"
for a in "$@"; do
  case "$a" in
    alive) exit 0 ;;
    status)
      echo "build_jobserver: pid=1 tokens=1 available=1 held~0 makes=0 fifo=/dev/null"
      exit 0 ;;
  esac
done
exit 0
"""

_NOOP_STUB = """#!/usr/bin/env bash
# Second line of defense (see module docstring): logs the call but never
# does anything real. A permitted `start` should never reach this in these
# tests (the python3 stub's "alive" short-circuits first).
echo "%(name)s $*" >> "${STUB_LOG:?}"
exit 0
"""


class JobserverCtlWorktreeGuardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bezel-jsctl-guard-test-")
        self.stub_dir = os.path.join(self.tmp, "bin")
        os.makedirs(self.stub_dir)
        self.log = os.path.join(self.tmp, "calls.log")
        self._write_stub("git", _GIT_STUB)
        self._write_stub("python3", _PYTHON3_STUB)
        self._write_stub("systemctl", _NOOP_STUB % {"name": "systemctl"})
        self._write_stub("systemd-run", _NOOP_STUB % {"name": "systemd-run"})

    def _write_stub(self, name, body):
        p = os.path.join(self.stub_dir, name)
        with open(p, "w") as f:
            f.write(body)
        os.chmod(p, os.stat(p).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)

    def _run(self, args, *, linked, allow_worktree=False, extra_env=None):
        """Run the REAL jobserver_ctl.sh with git/python3/systemctl/
        systemd-run all stubbed. `linked=True` reports a --git-dir nested
        under --git-common-dir (a linked worktree); `linked=False` reports
        the two as equal (the main checkout)."""
        env = dict(os.environ)
        env["PATH"] = self.stub_dir + os.pathsep + env.get("PATH", "")
        env["STUB_LOG"] = self.log
        common = os.path.join(self.tmp, "repo", ".git")
        env["STUB_GIT_COMMON_DIR"] = common
        env["STUB_GIT_DIR"] = (
            os.path.join(common, "worktrees", "somelane") if linked else common)
        env["BEZEL_JOBSERVER_DIR"] = os.path.join(self.tmp, "jsdir")  # never touched for real
        if allow_worktree:
            env["BEZEL_JOBSERVER_ALLOW_WORKTREE"] = "1"
        else:
            env.pop("BEZEL_JOBSERVER_ALLOW_WORKTREE", None)
        if extra_env:
            env.update(extra_env)
        return subprocess.run([str(JOBSERVER_CTL_SH)] + list(args),
                              capture_output=True, text=True, env=env, timeout=20)

    def _log_text(self):
        try:
            with open(self.log) as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def test_start_from_linked_worktree_is_refused(self):
        r = self._run(["start", "5"], linked=True)
        self.assertNotEqual(r.returncode, 0,
                            "start from a linked worktree must fail, not succeed")
        self.assertIn(REFUSAL_NEEDLE, r.stderr)
        self.assertIn("BEZEL_JOBSERVER_ALLOW_WORKTREE", r.stderr)
        log = self._log_text()
        self.assertIn("git ", log, "the guard itself must have consulted git")
        for tool in ("python3", "systemctl", "systemd-run"):
            self.assertNotIn(tool + " ", log,
                             "a refused start must never reach %s — log: %r"
                             % (tool, log))

    def test_start_from_main_checkout_is_not_refused(self):
        r = self._run(["start", "5"], linked=False)
        self.assertNotIn(REFUSAL_NEEDLE, r.stderr)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("already running", r.stdout)
        log = self._log_text()
        self.assertIn("python3", log)
        self.assertNotIn("systemd-run", log,
                         "the alive-stub short-circuit means systemd-run should "
                         "never be reached in this test")

    def test_start_from_linked_worktree_with_override_proceeds(self):
        r = self._run(["start", "5"], linked=True, allow_worktree=True)
        self.assertNotIn(REFUSAL_NEEDLE, r.stderr)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("already running", r.stdout)
        self.assertIn("python3", self._log_text())

    def test_stop_is_never_refused_regardless_of_worktree(self):
        r = self._run(["stop"], linked=True)
        self.assertNotIn(REFUSAL_NEEDLE, r.stderr)

    def test_status_is_never_refused_regardless_of_worktree(self):
        r = self._run(["status"], linked=True)
        self.assertNotIn(REFUSAL_NEEDLE, r.stderr)

    def test_default_subcommand_status_is_never_refused(self):
        """`${1:-status}` — no argument at all must behave like `status`,
        not fall through to the start guard."""
        r = self._run([], linked=True)
        self.assertNotIn(REFUSAL_NEEDLE, r.stderr)


if __name__ == "__main__":
    unittest.main()
