"""no_builders.sh counts every builder by its process name (#5083).

Fedora installs ninja as `ninja-build`, so that is the comm a live Ninja
build shows. The gate's zero-builders precondition read it as CLEAN, so a
gate could launch while a Ninja build ran. A fake `ps` on PATH feeds the
script a fixed process table of "SID COMM" lines.

A live gate's own builders (its session, named by the gate pidfile) are
excluded: run_status.sh used to flag every running gate as "running WITH
builders" because it counted the gate's suite binaries (R26 2026-10-04)."""

import os
import stat
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.normpath(os.path.join(HERE, "..", "..", "no_builders.sh"))


def _run(comms, gate_pid=None):
    """`comms` is a list of comm names (session 1) or (sid, comm) pairs.
    `gate_pid` is written to the gate pidfile; None means no pidfile."""
    rows = [c if isinstance(c, tuple) else (1, c) for c in comms]
    with tempfile.TemporaryDirectory() as d:
        ps = os.path.join(d, "ps")
        with open(ps, "w") as f:
            f.write("#!/bin/sh\nprintf '%s\\n' " +
                    " ".join(f"'{sid} {c}'" for sid, c in rows) + "\n")
        os.chmod(ps, os.stat(ps).st_mode | stat.S_IXUSR)
        pidfile = os.path.join(d, "gate.pid")
        if gate_pid is not None:
            with open(pidfile, "w") as f:
                f.write(f"{gate_pid}\n")
        env = dict(os.environ, PATH=d + os.pathsep + os.environ["PATH"],
                   BEZEL_GATE_PIDFILE=pidfile)
        return subprocess.run(["bash", SCRIPT], env=env,
                              capture_output=True, text=True)


def _dead_pid():
    """A pid with no process behind it (kill -0 fails)."""
    p = subprocess.Popen(["true"])
    p.wait()
    return p.pid


class NoBuildersTest(unittest.TestCase):
    def test_idle_box_is_clean(self):
        r = _run(["bash", "python3", "sshd"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("CLEAN", r.stdout)

    def test_each_builder_name_is_counted(self):
        for comm in ("make", "gmake", "ninja", "ninja-build", "clang++",
                     "cc1plus", "cmake", "ld.mold", "bezel_tests_cod",
                     "bezelc"):
            with self.subTest(comm=comm):
                r = _run(["bash", comm])
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("1 builder process(es)", r.stdout)

    def test_live_gate_session_builders_are_excluded(self):
        g = os.getpid()  # alive for the whole test
        r = _run([(g, "bezel_tests_cod"), (g, "clang++"), (g, "bezelc")],
                 gate_pid=g)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("CLEAN", r.stdout)
        self.assertIn("3 gate-owned process(es)", r.stdout)

    def test_builder_outside_the_gate_session_still_counts(self):
        g = os.getpid()
        r = _run([(g, "bezel_tests_cod"), (g + 1, "clang++")], gate_pid=g)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("1 builder process(es)", r.stdout)
        self.assertIn("1 gate-owned process(es)", r.stdout)

    def test_dead_gate_pidfile_excludes_nothing(self):
        g = _dead_pid()
        r = _run([(g, "bezel_tests_cod")], gate_pid=g)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("1 builder process(es)", r.stdout)
        self.assertNotIn("gate-owned", r.stdout)


if __name__ == "__main__":
    unittest.main()
