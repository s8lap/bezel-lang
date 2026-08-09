"""Tests for the gate_full_cycle.sh STALE-preflight suppression (this run).

The gate's preflight (gate_full_cycle.sh:228-307) used to fire on EVERY gate
because every lane commit edits src/ and at least one source becomes newer
than build/bezelc — the warning was noise. The warning is load-bearing ONLY
for the #2426 unreliable-incremental case (cmake --build prints
"Built target bezelc" without actually relinking). The structural
fingerprint of #2426 is: source X newer than bezelc AND X's .o ALSO newer
than X — cmake thinks the library is up-to-date and will not relink.

These tests pin the suppression logic over the 4 acceptance cases from the
brief, plus a header-only change and an edge case the brief didn't list
(no bezelc.dir at all — fresh checkout where the binary's own .o tree
hasn't been populated yet). Every case is constructed in a hermetic
tempdir so the real build/ tree is untouched.

How the bash logic is exercised: the preflight is inlined in the gate
script between two `say` lines and the function call, so a simple
`bash -c` with the same conditions (REPO, build/bezelc, .o layout)
reproduces the decision. We extract the inner predicate (everything
between `fire_stale=0` and the final `say` block) into a heredoc so the
test stays hermetic and is the verbatim bash the gate runs.

Acceptance grid (from the brief, restated):

  Case 1  Fresh checkout         no build/bezelc             suppress (no binary)
  Case 2  Lane commit, no rebuild  X.o older than X          suppress (rebuild fixes)
  Case 3  #2426 incremental lied   X.o NEWER than X          FIRE
  Case 4  Fresh build              bezelc > all sources      suppress (no warning needed)

Plus:
  Hdr    Header-only change       any newer .o              FIRE
  Fresh  Lane commit, only .cpp    bezel_core.dir missing    suppress (cmake rebuilds)

The test names mirror the case labels so a failing case tells you which
shape regressed.

MTIME DETERMINISM: every test sets file mtimes against a single BASELINE
(seconds-since-epoch, computed once per fixture) so the ordering is a
function of the offset passed to write_*, NOT of wall-clock time between
sequential touches. Without this, a slow test machine could see delta >
the offset between two fixtures' mtimes and silently flip an assertion.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))


# The verbatim preflight predicate extracted from gate_full_cycle.sh.
# Keep this in lockstep with the script — divergence is the test failing
# the cross-check below, not a test-only rewrite. The function is the
# only thing the harness runs; everything else is fixture plumbing.
#
# bezelc links THREE object trees (link.txt): libbezel_core.a,
# libbezel_runtime.a, and the binary's own main.cpp.o. The #2426
# fingerprint applies to ANY of them.
_PREFLIGHT_BASH = r"""
REPO="$1"
fire_stale=0
newest_src="$(find "$REPO/src" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
                -newer "$REPO/build/bezelc" -print -quit 2>/dev/null)"
CORE_OBJ_ROOTS="$REPO/build/CMakeFiles/bezel_core.dir $REPO/build/CMakeFiles/bezel_runtime.dir $REPO/build/CMakeFiles/bezelc.dir"
if [ -n "$newest_src" ]; then
  case "$newest_src" in
    *.cpp)
      src_rel="${newest_src#$REPO/}"
      fire_stale=0
      for root in $CORE_OBJ_ROOTS; do
        o="$root/$src_rel.o"
        if [ -f "$o" ] && [ "$o" -nt "$newest_src" ]; then
          fire_stale=1; break
        fi
      done
      ;;
    *)
      newer_o="$(find $CORE_OBJ_ROOTS -name '*.o' -newer "$newest_src" \
                     -print -quit 2>/dev/null || true)"
      [ -n "$newer_o" ] && fire_stale=1
      ;;
  esac
fi
echo "fire_stale=$fire_stale"
echo "newest=$newest_src"
"""


def _run_preflight(repo):
    """Run the verbatim bash preflight against a tempdir `repo`. Returns
    (fire_stale: bool, newest_src: str)."""
    out = subprocess.run(
        ["bash", "-c", _PREFLIGHT_BASH, "_", repo],
        capture_output=True, text=True, timeout=10,
    )
    if out.returncode != 0:
        raise AssertionError(
            "preflight bash failed (rc=%d): %s" % (out.returncode, out.stderr))
    fire_stale = False
    newest = ""
    for line in out.stdout.strip().splitlines():
        k, _, v = line.partition("=")
        if k == "fire_stale":
            fire_stale = (v == "1")
        elif k == "newest":
            newest = v  # may be empty string (find -print -quit returns nothing)
    return fire_stale, newest


def _dedent(block):
    """Normalise leading whitespace so the script's nesting under
    `if [ -x ]; then` and the test's flat layout compare equal on the
    LOGIC, not on indentation.

    The first line of both sides is `fire_stale=0` (no leading
    whitespace) — that prevents textwrap.dedent from finding any
    common indent. So: keep the first line as-is, then strip leading
    whitespace from each subsequent line up to the common minimum
    indent of the remainder (ignoring blank lines that may have been
    left by comment-stripping)."""
    lines = block.splitlines()
    if not lines:
        return block
    # First line stays as-is (the anchor — both blocks start with the
    # same `fire_stale=0`).
    first = lines[0]
    rest = lines[1:]
    # Find the minimum indent among non-blank rest lines.
    non_blank = [ln for ln in rest if ln.strip()]
    if not non_blank:
        return first
    min_indent = min(len(ln) - len(ln.lstrip()) for ln in non_blank)
    dedented_rest = [ln[min_indent:] if ln.strip() else ln for ln in rest]
    return "\n".join([first] + dedented_rest)


class _Fixture:
    """Build a minimal repo/ tree the preflight can walk.

    Layout mirrors what the real preflight expects:
      <root>/src/<sub>/<file>.{cpp,hpp}
      <root>/build/bezelc
      <root>/build/CMakeFiles/bezel_core.dir/src/<sub>/<file>.cpp.o
      <root>/build/CMakeFiles/bezelc.dir/src/<sub>/<file>.cpp.o

    Mtimes are set against a single BASELINE (computed once at fixture
    init, far enough in the past that no offset underflows). The offset
    is in seconds since BASELINE; positive = newer. This keeps the
    ordering deterministic regardless of how long the fixture takes to
    build.
    """

    # A single offset unit. The widest ordering span any test uses is
    # ~30 units (offsets of 0/10/20/30); 1 unit = 10s means the span is
    # 5 min — far more than any sequential touch gap. Bumping this is a
    # no-op for correctness; smaller keeps `stat` honest on filesystems
    # with second-granular mtimes.
    _OFFSET_SECONDS = 10

    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="stale-test-")
        # Anchor well in the past so the largest offset is still a legal
        # mtime (positive seconds-since-epoch).
        self._t0 = time.time() - (10 ** 6)

    def repo(self):
        return self.root

    def _set_mtime(self, path, offset_units):
        mtime = self._t0 + offset_units * self._OFFSET_SECONDS
        # Both atime and mtime; atime doesn't matter for the predicate
        # but `find -newer` compares against mtime.
        os.utime(path, (mtime, mtime))

    def write_src(self, rel, mtime_offset=10):
        # mtime_offset=10 means "10 units newer than bezelc baseline";
        # the default models the post-lane-commit edit.
        path = os.path.join(self.root, "src", rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write("// test fixture\n")
        self._set_mtime(path, mtime_offset)

    def write_bezelc(self, mtime_offset=0):
        path = os.path.join(self.root, "build", "bezelc")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            # Need it to be executable for the outer `[ -x ]` guard in
            # the real gate, though our extracted predicate doesn't
            # include that guard. Match anyway so the fixture exercises
            # the gate's actual code path.
            f.write("#!/bin/sh\necho bezelc\n")
        os.chmod(path, 0o755)
        self._set_mtime(path, mtime_offset)

    def write_core_o(self, src_rel, mtime_offset=0):
        # The .o mirrors the source's path under bezel_core.dir/src/.
        path = os.path.join(self.root, "build/CMakeFiles/bezel_core.dir/src",
                            src_rel + ".o")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write("/* .o fixture */\n")
        self._set_mtime(path, mtime_offset)

    def write_bezelc_o(self, src_rel, mtime_offset=0):
        path = os.path.join(self.root, "build/CMakeFiles/bezelc.dir/src",
                            src_rel + ".o")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write("/* .o fixture */\n")
        self._set_mtime(path, mtime_offset)

    def write_runtime_o(self, src_rel, mtime_offset=0):
        # bezelc also links libbezel_runtime.a; the predicate walks
        # bezel_runtime.dir too (added when reviewer caught that link.txt
        # has TWO static libs, not one — a runtime .o newer than its
        # source is the same #2426 shape).
        path = os.path.join(self.root, "build/CMakeFiles/bezel_runtime.dir/src",
                            src_rel + ".o")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write("/* .o fixture */\n")
        self._set_mtime(path, mtime_offset)

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


class Case1FreshCheckoutTest(unittest.TestCase):
    """Case 1: Fresh checkout, no build/bezelc at all. Suppress.

    The outer `if [ -x "$REPO/build/bezelc" ]` guards the preflight, so
    missing bezelc means no warning. The test verifies the harness exits
    cleanly with fire_stale=0 even if src/ exists.
    """

    def test_no_bezelc_suppresses(self):
        fx = _Fixture()
        try:
            fx.write_src("foo/bar.cpp", mtime_offset=10)
            # Deliberately DO NOT write bezelc.
            fire, newest = _run_preflight(fx.repo())
            self.assertFalse(fire,
                             "Case 1 (no build/bezelc) must suppress, got fire_stale=True")
        finally:
            fx.cleanup()


class Case2LaneCommitPreRebuildTest(unittest.TestCase):
    """Case 2: Lane commit, .o files older than source. Suppress.

    The rebuild phase WILL recompile the stale .o (source > .o) and
    relink bezelc — the warning would be redundant.
    """

    def test_stale_o_suppresses(self):
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=0)        # bezelc at T0
            fx.write_src("foo/bar.cpp", mtime_offset=20)  # source at T0+200s
            fx.write_core_o("foo/bar.cpp", mtime_offset=10)  # .o at T0+100s — older than source
            fire, newest = _run_preflight(fx.repo())
            self.assertFalse(fire,
                             "Case 2 (X.o older than X) must suppress, got fire_stale=True")
            self.assertNotEqual(newest, "", "newest_src must be populated")
            self.assertTrue(newest.endswith("bar.cpp"))
        finally:
            fx.cleanup()

    def test_no_o_file_suppresses(self):
        """Case 2 variant: source exists, .o file does NOT exist yet.
        The rebuild will create it. Suppress."""
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=0)
            fx.write_src("foo/bar.cpp", mtime_offset=20)
            # No .o written at all.
            fire, _ = _run_preflight(fx.repo())
            self.assertFalse(fire,
                             "Case 2 variant (no .o) must suppress, got fire_stale=True")
        finally:
            fx.cleanup()


class Case3ReliabilityIncrementalLiedTest(unittest.TestCase):
    """Case 3: #2426 — cmake says OK but bezelc is stale. FIRE.

    Source X newer than bezelc AND X's .o ALSO newer than X means cmake
    rebuilt the .o and thinks everything is up-to-date — it will not
    relink bezelc. The warning is load-bearing here.
    """

    def test_fresh_o_fires(self):
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=0)        # bezelc at T0
            fx.write_src("foo/bar.cpp", mtime_offset=20)   # source at T0+200s
            fx.write_core_o("foo/bar.cpp", mtime_offset=30)  # .o at T0+300s — NEWER than source
            fire, newest = _run_preflight(fx.repo())
            self.assertTrue(fire,
                            "Case 3 (X.o newer than X) MUST fire — #2426 shape; got fire_stale=False")
            self.assertNotEqual(newest, "")
            self.assertTrue(newest.endswith("bar.cpp"))
        finally:
            fx.cleanup()

    def test_fresh_o_in_bezelc_dir_fires(self):
        """Case 3 variant: the .o is in bezelc.dir (e.g., main.cpp.o),
        not bezel_core.dir. The predicate checks BOTH candidate paths —
        this verifies the bezelc.dir branch is wired."""
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=0)
            fx.write_src("main.cpp", mtime_offset=20)
            # .o only exists in bezelc.dir/, NOT bezel_core.dir/.
            fx.write_bezelc_o("main.cpp", mtime_offset=30)
            fire, _ = _run_preflight(fx.repo())
            self.assertTrue(fire,
                            "Case 3 variant (.o in bezelc.dir) MUST fire")
        finally:
            fx.cleanup()

    def test_fresh_o_in_runtime_dir_fires(self):
        """Case 3 variant: the .o is in bezel_runtime.dir. bezelc links
        libbezel_runtime.a in addition to libbezel_core.a, so a runtime
        .o newer than its source is the SAME #2426 shape and must fire.

        This test is the structural witness for the reviewer-pre-empt:
        the original draft only walked bezel_core.dir, which would have
        silently swallowed this defect class."""
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=0)
            fx.write_src("runtime/foo.cpp", mtime_offset=20)
            # .o only in bezel_runtime.dir.
            fx.write_runtime_o("runtime/foo.cpp", mtime_offset=30)
            fire, _ = _run_preflight(fx.repo())
            self.assertTrue(fire,
                            "Case 3 variant (.o in bezel_runtime.dir) MUST fire")
        finally:
            fx.cleanup()


class Case4FreshBuildTest(unittest.TestCase):
    """Case 4: Everything fresh — bezelc newer than all sources. Suppress.

    `find -newer bezelc` returns nothing, so newest_src is empty, so the
    case branch is skipped and fire_stale stays 0.
    """

    def test_fresh_bezelc_suppresses(self):
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=20)  # bezelc at T0+200s
            fx.write_src("foo/bar.cpp", mtime_offset=0)  # source at T0 — OLDER
            fx.write_core_o("foo/bar.cpp", mtime_offset=0)
            fire, newest = _run_preflight(fx.repo())
            self.assertFalse(fire,
                             "Case 4 (fresh bezelc) must suppress, got fire_stale=True")
            self.assertEqual(newest, "", "newest_src should be empty")
        finally:
            fx.cleanup()


class HeaderOnlyChangeTest(unittest.TestCase):
    """Header-only changes: .hpp file with NO own .o file.

    A .hpp's consumer .o files are tracked by cmake's compiler_depend
    manifest. If ANY bezel-related .o is newer than the header, cmake
    processed it — #2426 shape, FIRE. If NO .o is newer, cmake will
    rebuild on the next pass — suppress.

    This case was NOT in the brief's 4-case grid but is a real edge
    shape: lane work commonly touches headers without touching .cpp
    files, and the original code's "find any source newer than bezelc"
    matched headers the same way it matched .cpp files.
    """

    def test_header_with_dependent_o_fires(self):
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=0)
            fx.write_src("foo/bar.hpp", mtime_offset=20)  # header newer
            # A consumer .o newer than the header: cmake processed it.
            fx.write_core_o("foo/consumer.cpp", mtime_offset=30)
            fire, newest = _run_preflight(fx.repo())
            self.assertTrue(fire,
                            "Header with newer dependent .o MUST fire")
            self.assertNotEqual(newest, "")
            self.assertTrue(newest.endswith("bar.hpp"),
                            "newest_src must identify the header that drove the staleness")
        finally:
            fx.cleanup()

    def test_header_with_no_newer_o_suppresses(self):
        """Header newer than bezelc, but NO .o is newer than the header.
        The rebuild will recompile consumers. Suppress."""
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=0)
            fx.write_src("foo/bar.hpp", mtime_offset=20)
            # Consumer .o exists but is OLDER than the header — will be rebuilt.
            fx.write_core_o("foo/consumer.cpp", mtime_offset=10)
            fire, _ = _run_preflight(fx.repo())
            self.assertFalse(fire,
                             "Header with no newer .o must suppress (rebuild will fix)")
        finally:
            fx.cleanup()


class FreshCheckoutNoCoreDirTest(unittest.TestCase):
    """Fresh lane checkout where bezel_core.dir hasn't been built yet.

    bezelc exists but no .o mirror exists in bezel_core.dir/. Source is
    newer than bezelc but the .o doesn't exist yet — the rebuild will
    create it. Suppress.

    This shape is rare in practice (lane work usually inherits a build
    dir) but is the second "fresh checkout" sub-case and ensures the
    predicate's `if [ -f "$o1" ]` guard handles the missing .o.
    """

    def test_suppresses_when_no_core_dir(self):
        fx = _Fixture()
        try:
            fx.write_bezelc(mtime_offset=0)
            fx.write_src("foo/bar.cpp", mtime_offset=20)
            # Deliberately do NOT create bezel_core.dir or any .o files.
            fire, _ = _run_preflight(fx.repo())
            self.assertFalse(fire,
                             "Missing bezel_core.dir must suppress (rebuild will populate)")
        finally:
            fx.cleanup()


class VerbatimScriptMatchTest(unittest.TestCase):
    """The preflight predicate in the test must match the script byte-
    for-byte (the test runs a copy of the predicate; the script runs the
    same). If a future edit to gate_full_cycle.sh's preflight is NOT
    mirrored here, this test catches it.

    The match is structural: extract the preflight block from the
    script via a sentinel pattern, assert it is identical to the
    inlined _PREFLIGHT_BASH (after normalising the say/printf wrappers
    and the surrounding context). The point is that a divergent edit
    is loud, not silent.
    """

    _SCRIPT = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "gate_full_cycle.sh"))

    def _extract_preflight_block(self):
        """Return the verbatim bash from `fire_stale=0` up to (but not
        including) the final `if [ \"$fire_stale\" -eq 1 ]; then`."""
        with open(self._SCRIPT) as f:
            text = f.read()
        start_marker = "fire_stale=0\n"
        end_marker = 'if [ "$fire_stale" -eq 1 ]; then'
        s = text.find(start_marker)
        e = text.find(end_marker)
        if s < 0 or e < 0:
            self.fail("could not locate preflight block in gate_full_cycle.sh "
                      "(start=%d end=%d)" % (s, e))
        return text[s:e].rstrip()

    def _normalised_test_predicate(self):
        """Return the test's _PREFLIGHT_BASH with test-only plumbing
        stripped: REPO="$1" assignment (test sets it) and trailing
        `echo fire_stale=...` (test parses it). What remains must equal
        the script's preflight block.

        Inline comments (`# Source file: ...`) are stripped too — the
        script has descriptive comments that the test omits, and a
        comment-only diff should not be a regression tripwire.
        Indentation is dedented (common leading whitespace dropped) so
        the script's nesting under `if [ -x ]; then` and the test's
        flat layout compare equal on the LOGIC, not on whitespace.
        """
        body = _PREFLIGHT_BASH.strip()
        lines = body.splitlines()
        body_lines = [ln for ln in lines if not ln.startswith('REPO="$1"')]
        while body_lines and body_lines[-1].startswith("echo "):
            body_lines.pop()
        # Strip inline comments + their indentation. Comments are the
        # only thing that legitimately differs; stripping here means
        # whitespace-only diffs in the script's `# ...` annotations
        # don't fail the test, but any LOGIC change does.
        stripped = []
        for ln in body_lines:
            stripped.append(ln.split(" #", 1)[0].rstrip() if " #" in ln else ln)
        # Drop fully-blank lines (comment-only lines become empty).
        return _dedent("\n".join(s for s in stripped if s.strip())).strip()

    def _strip_script_comments(self, block):
        """Strip inline `# ...` comments from the script-extracted block,
        matching the test-side stripping, then dedent."""
        stripped = []
        for ln in block.splitlines():
            stripped.append(ln.split(" #", 1)[0].rstrip() if " #" in ln else ln)
        return _dedent("\n".join(s for s in stripped if s.strip())).strip()

    def test_preflight_block_matches_inlined_predicate(self):
        script_block = self._strip_script_comments(self._extract_preflight_block())
        inlined = self._normalised_test_predicate()
        self.maxDiff = None
        self.assertEqual(script_block, inlined,
                         "gate_full_cycle.sh preflight block has drifted from "
                         "the test's _PREFLIGHT_BASH — update one or the other "
                         "(a divergence here means the test is no longer "
                         "exercising the actual gate logic).\n"
                         "--- script ---\n%s\n--- inlined ---\n%s" % (
                             script_block, inlined))


if __name__ == "__main__":
    unittest.main()