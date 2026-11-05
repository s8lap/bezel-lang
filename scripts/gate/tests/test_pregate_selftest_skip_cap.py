"""pregate must enforce the gate's self-test skip cap (shared file)
(R39 gate 1: `OK (skipped=6)` passed pregate, then failed the cycle's cap of 4).
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
LIMITS = os.path.join(GATE, "selftest_limits.sh")
sys.path.insert(0, SCRIPTS)


def _scratch():
    d = os.path.join(ROOT, "scratch")
    os.makedirs(d, exist_ok=True)
    return tempfile.TemporaryDirectory(dir=d)


def _bash(snippet):
    return subprocess.run(["bash", "-c", ". %s; %s" % (LIMITS, snippet)],
                          capture_output=True, text=True).stdout.strip()


class SkipCapTest(unittest.TestCase):
    def _count(self, text):
        with _scratch() as d:
            p = os.path.join(d, "log")
            with open(p, "w") as f:
                f.write(text)
            return int(_bash("gate_selftest_skip_count %s" % p))

    def test_parse_skipped(self):
        self.assertEqual(self._count("Ran 9 tests in 1s\n\nOK (skipped=6)\n"), 6)
        self.assertEqual(self._count("Ran 9 tests in 1s\n\nOK\n"), 0)
        self.assertEqual(self._count("OK (skipped=4, expected failures=1)\n"), 4)

    def test_cap_in_gate_and_in_pregate(self):
        cap = int(_bash('echo "$GATE_SELFTEST_SKIP_CAP"'))
        gate_only = int(_bash('echo "$GATE_SELFTEST_GATE_ONLY_SKIPS"'))
        self.assertEqual(int(_bash("gate_selftest_skip_cap_for 1")), cap)
        self.assertEqual(int(_bash("gate_selftest_skip_cap_for 0")), cap - gate_only)
        self.assertGreater(cap - gate_only, 0)

    def test_incident_six_skips_exceeds_both_caps(self):
        # R39 gate 1: 4 token tests + 2 lock tests = 6 inside a gate.
        self.assertGreater(6, int(_bash("gate_selftest_skip_cap_for 1")))
        self.assertGreater(4, int(_bash("gate_selftest_skip_cap_for 0")))

    def test_cap_has_one_home(self):
        for name in ("pregate.sh", "gate_full_cycle.sh"):
            with open(os.path.join(SCRIPTS, name)) as f:
                src = f.read()
            self.assertIn("gate/selftest_limits.sh", src, name)
            self.assertFalse(re.search(r"^_gs_skip_cap=\d", src, re.M), name)


if __name__ == "__main__":
    unittest.main()
