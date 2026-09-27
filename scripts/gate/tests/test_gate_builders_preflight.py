"""gate.sh's own zero-builders preflight recognises ninja-build (#5087).

`scripts/gate.sh`'s BUILDERS alternation (matched via `pgrep -x "$BUILDERS"`)
had the exact same gap #5080/#5083 already fixed elsewhere: Fedora's ninja
binary is `/usr/bin/ninja-build` (the kernel sets a process's comm from the
invoked pathname's basename, not a symlink target), so a real Ninja build's
comm is `ninja-build` — which the bare `ninja` alternative never matched
under `-x`'s whole-string anchoring. The preflight is the gate's own
cold-cache-flake defense (see the comment above BUILDERS in gate.sh): it must
read a live `ninja-build` build as "builders active", not CLEAN.

This test does not invoke gate.sh itself (it builds/tests for real and has
many side effects) or run pgrep against a live process table (nondeterministic
and not hermetic). Instead it extracts the literal `BUILDERS='...'` value
from the script and reproduces `pgrep -x`'s matching semantics directly:
`-x` anchors the ERE to the WHOLE comm string, which Python's `re.fullmatch`
models exactly for this pattern (plain alternation plus one optional group,
no POSIX character classes, backreferences, or other ERE/PCRE divergences)."""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GATE_SH = os.path.normpath(os.path.join(HERE, "..", "..", "gate.sh"))


def _builders_pattern():
    """Extract the literal value of `BUILDERS='...'` from scripts/gate.sh."""
    with open(GATE_SH) as f:
        text = f.read()
    m = re.search(r"^BUILDERS='([^']*)'\s*$", text, re.MULTILINE)
    if m is None:
        raise AssertionError(
            "BUILDERS='...' not found in scripts/gate.sh — has it been "
            "renamed or restructured?")
    return m.group(1)


class BuildersPatternTest(unittest.TestCase):
    def test_pattern_is_present(self):
        self.assertTrue(_builders_pattern(), "BUILDERS pattern is empty")

    def test_ninja_build_comm_matches_under_pgrep_x_semantics(self):
        pattern = _builders_pattern()
        # pgrep -x anchors the ERE to the whole comm; re.fullmatch models that.
        self.assertIsNotNone(
            re.fullmatch(pattern, "ninja-build"),
            f"BUILDERS={pattern!r} does not match comm 'ninja-build' under "
            "pgrep -x whole-string semantics (#5087) — a live Fedora Ninja "
            "build would read as CLEAN")

    def test_other_known_builder_comms_still_match(self):
        pattern = _builders_pattern()
        for comm in ("cmake", "cc1plus", "ld", "mold", "lld", "ninja", "make",
                     "bezel_tests", "bezel_tests_codegen", "bezel_tests_sema"):
            with self.subTest(comm=comm):
                self.assertIsNotNone(
                    re.fullmatch(pattern, comm),
                    f"BUILDERS={pattern!r} no longer matches {comm!r}")

    def test_non_builder_comms_do_not_spuriously_match(self):
        pattern = _builders_pattern()
        # Includes the gate driver names themselves, confirming the pattern
        # still cannot self-match (per gate.sh's own comment on BUILDERS).
        for comm in ("bash", "python3", "sshd", "gate.sh", "gate_and_push.sh",
                     "bezelc"):
            with self.subTest(comm=comm):
                self.assertIsNone(
                    re.fullmatch(pattern, comm),
                    f"BUILDERS={pattern!r} spuriously matches {comm!r}")


if __name__ == "__main__":
    unittest.main()
