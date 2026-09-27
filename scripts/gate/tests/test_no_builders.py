"""no_builders.sh counts every builder by its process name (#5083).

Fedora installs ninja as `ninja-build`, so that is the comm a live Ninja
build shows. The gate's zero-builders precondition read it as CLEAN, so a
gate could launch while a Ninja build ran. A fake `ps` on PATH feeds the
script a fixed process table."""

import os
import stat
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.normpath(os.path.join(HERE, "..", "..", "no_builders.sh"))


def _run(comms):
    with tempfile.TemporaryDirectory() as d:
        ps = os.path.join(d, "ps")
        with open(ps, "w") as f:
            f.write("#!/bin/sh\nprintf '%s\\n' " +
                    " ".join(f"'{c}'" for c in comms) + "\n")
        os.chmod(ps, os.stat(ps).st_mode | stat.S_IXUSR)
        env = dict(os.environ, PATH=d + os.pathsep + os.environ["PATH"])
        return subprocess.run(["bash", SCRIPT], env=env,
                              capture_output=True, text=True)


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


if __name__ == "__main__":
    unittest.main()
