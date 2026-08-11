"""Smoke test for the scripts/gate package scaffold (#3010, Phase 0).

Confirms the package imports and exposes the io helpers the later phases build
on. A failure here means the package layout itself broke (sys.path, __init__,
or a syntax error in a module) — not a logic regression.
"""

import os
import sys
import unittest

# Make `gate` importable when discovery runs this file directly: this test lives
# at scripts/gate/tests/, so two levels up is scripts/ (where the `gate` package
# sits). stdlib-only — no pytest, no editable install.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

import gate   # noqa: E402
from gate import io  # noqa: E402


class SmokeTest(unittest.TestCase):
    def test_package_imports(self):
        self.assertTrue(hasattr(gate, "__all__"))

    def test_io_exposes_helpers(self):
        self.assertTrue(callable(io.atomic_json_write))
        self.assertTrue(callable(io.read_jsonl))


if __name__ == "__main__":
    unittest.main()
