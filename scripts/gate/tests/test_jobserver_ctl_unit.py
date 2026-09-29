"""Tests for the systemd unit name scripts/jobserver_ctl.sh uses (#4990).

The script used to hardcode UNIT=bezel-jobserver, so a `start` against any
other token dir (a test, a lane, a second pool) targeted the production
daemon's unit. Pinned here:
  1. The default token dir (unset, spelled out, or with a trailing slash) still
     maps to `bezel-jobserver`, for start AND stop. That instance is already
     running, so status/stop must keep finding it.
  2. Any other dir gets its own `bezel-jobserver-<8 hex>` unit, which is
     deterministic and differs between dirs.
  3. BEZEL_JOBSERVER_UNIT overrides verbatim; an invalid name is refused
     (exit 64) before any systemd call.

systemd-run, systemctl and python3 are PATH stubs that only log their argv, so
nothing here starts, stops or even queries a real unit. The production daemon
cannot be touched. BEZEL_JOBSERVER_ALLOW_WORKTREE=1 is set so the worktree
guard (a sibling change) does not refuse `start` when this runs from a lane.
"""

import os
import re
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.normpath(os.path.join(HERE, "..", "..", "jobserver_ctl.sh"))
DEFAULT_DIR = "/var/tmp/bezel-scratch/jobserver"

# python3 stub: `alive --dir D` is true only once the systemd-run stub has
# "started" a daemon for D (a per-dir marker file), and false again after the
# systemctl stub's `stop`, so neither the start poll loop nor the stop guard
# waits or fails.
_MARK = 'mark() { printf "%s/started-%s" "$STUB_DIR" "$(printf %s "$1" | cksum | cut -d" " -f1)"; }'
_PY_STUB = """#!/bin/bash
echo "python3 $*" >> "$STUB_LOG"
""" + _MARK + """
case "$2" in
  alive) [ -e "$(mark "$4")" ] ;;
  *) exit 0 ;;
esac
"""
_SYSTEMD_RUN_STUB = """#!/bin/bash
echo "systemd-run $*" >> "$STUB_LOG"
""" + _MARK + """
while [ $# -gt 0 ]; do
  if [ "$1" = --dir ]; then touch "$(mark "$2")"; fi
  shift
done
"""
_SYSTEMCTL_STUB = """#!/bin/bash
echo "systemctl $*" >> "$STUB_LOG"
[ "$2" = stop ] && rm -f "$STUB_DIR"/started-*
exit 0
"""


class JobserverCtlUnitNameTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="jobserver-ctl-unit-")
        self.tmp = self._tmp.name
        self.bin = os.path.join(self.tmp, "bin")
        os.mkdir(self.bin)
        for name, body in (("python3", _PY_STUB),
                           ("systemd-run", _SYSTEMD_RUN_STUB),
                           ("systemctl", _SYSTEMCTL_STUB)):
            p = os.path.join(self.bin, name)
            with open(p, "w") as f:
                f.write(body)
            os.chmod(p, 0o755)
        self.log = os.path.join(self.tmp, "calls.log")

    def tearDown(self):
        self._tmp.cleanup()

    def _run(self, action, **env_over):
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("BEZEL_JOBSERVER")}
        env.update(PATH=self.bin + os.pathsep + env.get("PATH", "/usr/bin:/bin"),
                   STUB_LOG=self.log, STUB_DIR=self.tmp,
                   BEZEL_JOBSERVER_ALLOW_WORKTREE="1")
        env.update(env_over)
        if os.path.exists(self.log):
            os.unlink(self.log)
        r = subprocess.run(["bash", SCRIPT, action], env=env, cwd=self.tmp,
                           capture_output=True, text=True, timeout=60)
        calls = []
        if os.path.exists(self.log):
            with open(self.log) as f:
                calls = f.read().splitlines()
        return r, calls

    def _units(self, calls):
        """Every unit name systemd-run / systemctl was pointed at."""
        units = []
        for c in calls:
            m = re.match(r"systemd-run .*--unit=(\S+)", c)
            if m:
                units.append(m.group(1))
            m = re.match(r"systemctl --user (?:stop|reset-failed) (\S+)", c)
            if m:
                units.append(m.group(1))
        return units

    def _start_units(self, **env):
        r, calls = self._run("start", **env)
        self.assertEqual(r.returncode, 0, r.stderr)
        units = self._units(calls)
        self.assertTrue(any(c.startswith("systemd-run ") for c in calls), calls)
        self.assertEqual(len(set(units)), 1, calls)
        return units[0]

    def _stop_units(self, **env):
        r, calls = self._run("stop", **env)
        self.assertEqual(r.returncode, 0, r.stderr)
        units = self._units(calls)
        self.assertTrue(units, calls)
        self.assertEqual(len(set(units)), 1, calls)
        return units[0]

    # 1. default dir -> the production unit name, unchanged
    def test_default_dir_keeps_production_unit_name(self):
        self.assertEqual(self._start_units(), "bezel-jobserver")
        self.assertEqual(self._stop_units(), "bezel-jobserver")

    def test_explicit_default_dir_keeps_production_unit_name(self):
        for d in (DEFAULT_DIR, DEFAULT_DIR + "/", DEFAULT_DIR + "//"):
            with self.subTest(dir=d):
                self.assertEqual(self._start_units(BEZEL_JOBSERVER_DIR=d),
                                 "bezel-jobserver")
                self.assertEqual(self._stop_units(BEZEL_JOBSERVER_DIR=d),
                                 "bezel-jobserver")

    # 2. any other dir -> its own derived unit
    def test_other_dir_gets_distinct_derived_unit(self):
        a = os.path.join(self.tmp, "pool-a")
        b = os.path.join(self.tmp, "pool-b")
        ua = self._start_units(BEZEL_JOBSERVER_DIR=a)
        ub = self._start_units(BEZEL_JOBSERVER_DIR=b)
        self.assertRegex(ua, r"^bezel-jobserver-[0-9a-f]{8}$")
        self.assertRegex(ub, r"^bezel-jobserver-[0-9a-f]{8}$")
        self.assertNotEqual(ua, ub)
        # Deterministic: the same dir maps to the same unit for stop,
        # otherwise stop could never find what start created.
        self.assertEqual(self._stop_units(BEZEL_JOBSERVER_DIR=a), ua)
        self.assertEqual(self._start_units(BEZEL_JOBSERVER_DIR=a + "/"), ua)

    # 3. explicit override, and validation
    def test_unit_env_overrides_verbatim(self):
        other = os.path.join(self.tmp, "pool-c")
        self.assertEqual(self._start_units(BEZEL_JOBSERVER_UNIT="bz-js-test",
                                           BEZEL_JOBSERVER_DIR=other),
                         "bz-js-test")
        self.assertEqual(self._stop_units(BEZEL_JOBSERVER_UNIT="bz-js-test",
                                          BEZEL_JOBSERVER_DIR=other),
                         "bz-js-test")

    def test_invalid_unit_name_refused_before_systemd(self):
        r, calls = self._run("start", BEZEL_JOBSERVER_UNIT="bad name;rm")
        self.assertEqual(r.returncode, 64, r.stdout + r.stderr)
        self.assertIn("invalid systemd unit name", r.stderr)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
