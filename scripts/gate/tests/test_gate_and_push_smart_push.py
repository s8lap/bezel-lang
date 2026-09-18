"""#smart-push (user directive 2026-09-18) — the push boundary must distinguish a
remote-advance race from a real push failure.

USER DIRECTIVE (verbatim): "the gate shouldn't stop just because the remote
branch advances, it should still continue and finish running, just not push.
in that case, you should devide how to push a clean gate to remote if the
remote advanced"

PRE-FIX BEHAVIOUR. A bare `git push` at the end of `gate_and_push.sh`
non-fast-forward-fails (exit 3) when origin/main has advanced between the
gate's verdict and the push step — even though the gate itself passed
cleanly, even though the local commit is already superseded, and even though
the only sensible response is "fetch + rebase". The gate is reported FAILED
with no diagnostics distinguishing that from a network failure or a
permission denial, so the coordinator learns nothing useful.

FOUR CASES the new block distinguishes:

    local == remote                  -> NO-OP,      exit 0, "GATE PASSED -> NO-OP ..."
    local is ancestor of remote      -> SKIPPED,    exit 0, "GATE PASSED -> SKIPPED PUSH (remote advanced ...)"
    remote is ancestor of local      -> fall through, "PUSHED origin/main now ..." (unchanged)
    diverged                         -> SKIPPED,    exit 4, "GATE PASSED -> SKIPPED PUSH (DIVERGED ...)"

All three new markers carry `GATE PASSED`, which gate_wait.sh's TERMINAL_RE
(`GATE PASSED|GATE FAILED .*->|PUSHED origin/main now`) accepts. The normal-
push path still emits `PUSHED origin/main now` byte-identically. A fetch
failure (no network, missing remote, rate limit) leaves `_remote_head` empty
and falls through to the existing push — no new failure mode is masked.

WHAT IS PINNED HERE, and why each pin exists rather than a comment:

  SourcePinsTest               — the block is in the right place (after
                                destination is resolved, before the existing
                                `git push`) and emits every documented
                                marker exactly once. A prose-only description
                                of a bash block is not a ratchet.

  FunctionalTest               — the EXTRACTED block (sourced verbatim into
                                a temp cwd with stubs for `git push` and
                                `gc_scratch.sh`) is driven against a fresh
                                git repo shaped as each of the four cases.
                                Each case asserts the exact marker + exit
                                code. The block is the production bash, not
                                a rewrite — a regression that ships a wrong
                                marker / wrong exit / wrong branch direction
                                fails here, even when the source pins pass.

  GateWaitRegexTest            — every new marker carries `GATE PASSED`, which
                                gate_wait.sh accepts. This is the property
                                the design relies on for the waiter to keep
                                classifying a `GATE PASSED -> SKIPPED PUSH`
                                as accept (not the bail that `GATE FAILED ->`
                                would read as).

  FetchFailureFallsThroughTest — `git fetch` failure is graceful: empty
                                `_remote_head`, fall through to existing
                                push (no new failure mode introduced).

  NonMainPushStillSmartPushedTest — the `BEZEL_GATE_PUSH_DEST_OK=1` escape
                                still works; smart-push runs the same race
                                detection on a non-main destination (the
                                race is structurally identical, the
                                destination ref is interchangeable).

Stdlib only (unittest, subprocess, tempfile, pathlib). No build. No new
dependencies. Every test here FAILED at the parent commit (e8a921fd) — the
block did not exist. Pinned functionally against a real git repo so the
merge-base arithmetic is exercised end to end, not just spelled.
"""

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent.parent                         # scripts/
REPO = HERE.parents[2]                               # repo root
GATE_AND_PUSH = SCRIPTS / "gate_and_push.sh"
GATE_WAIT = SCRIPTS / "gate_wait.sh"

# Same git env as the sibling tests (no operator config, no leaked identity).
GIT_ENV = dict(
    os.environ,
    GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
    GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t",
    GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null",
)


def _git(cwd, *args, env=None, timeout=30):
    e = dict(env or GIT_ENV)
    return subprocess.run(("git",) + args, cwd=str(cwd), env=e,
                          capture_output=True, text=True, timeout=timeout)


# ---------------------------------------------------------------------------
# Sentinels + extraction (the smart-push block's exact start/end).
# ---------------------------------------------------------------------------
# A double-sentinel block + a stub-bash harness is the house style here
# (test_gate_full_cycle_pins.py uses the same shape). The block's text
# between these two sentinels is sourced into a controlled cwd and run
# against a temp repo; every test in FunctionalTest exercises a different
# branch of the block on the SAME bash text.

_SMART_PUSH_START = "# \u2500\u2500 SMART-PUSH (#smart-push, user directive 2026-09-18)"
_SMART_PUSH_END = "  if git push \"$_push_remote\" \"$_head:$_push_ref\""


def _slice_block(text, start, end, what):
    """Extract `text[start..end]` (start inclusive, end inclusive). Both
    sentinels must occur exactly once; a miss is a loud failure, never an
    empty block."""
    s = text.find(start)
    if s < 0 or text.find(start, s + 1) >= 0:
        raise AssertionError("%s: start sentinel %r not found exactly once" % (what, start))
    e = text.find(end, s + len(start))
    if e < 0:
        raise AssertionError("%s: end sentinel %r not found after start" % (what, end))
    return text[s:e + len(end)], s, e


def _code_lines(text):
    """`(line_no, line)` for every line that is NOT blank or a full-line
    comment. Inline `# …` comments are NOT stripped — the block has strings
    like `"-fsanitize=address"` a naive strip would truncate, and the
    block's own echo lines carry the marker text we want to count. A
    marker that appears inside a comment counts in `block.count(marker)`
    but NOT in this view, which is what the marker-counting ratchets
    need (the design says "emit each marker exactly once", not "the
    substring appears exactly once").
    """
    out = []
    for n, ln in enumerate(text.splitlines(), 1):
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        out.append((n, ln))
    return out


def _block():
    """Return (full_text, smart_push_block_text, block_start_idx, block_end_idx).

    The block is the smart-push bash BETWEEN the start sentinel
    (`# ── SMART-PUSH …`) and the existing `if git push …` line. The
    trailing `if git push` line is the FIRST line of the existing
    push path and is NOT part of smart-push; the extraction strips it
    so the block can be sourced as a self-contained unit (without an
    unterminated `if`/`then`/`fi`).
    """
    text = GATE_AND_PUSH.read_text()
    block, s_idx, e_idx = _slice_block(text, _SMART_PUSH_START, _SMART_PUSH_END,
                                      "smart-push block")
    # Strip the trailing `if git push` line — that's the existing push
    # path, not part of smart-push.
    lines = block.rstrip().split("\n")
    while lines and re.match(r"\s*if\s+git\s+push\b", lines[-1]):
        lines.pop()
    block = "\n".join(lines) + "\n"
    # Structural controls: the real body, not a fragment. Every shape
    # mentioned in the design doc is present.
    for needle in ("git fetch", "merge-base --is-ancestor \"$_head\" \"$_remote_head\"",
                   "merge-base --is-ancestor \"$_remote_head\" \"$_head\"",
                   "GATE PASSED -> NO-OP",
                   "GATE PASSED -> SKIPPED PUSH (remote advanced:",
                   "GATE PASSED -> SKIPPED PUSH (DIVERGED:",
                   "exit 0", "exit 4"):
        if needle not in block:
            raise AssertionError("smart-push block lacks %r" % needle)
    return text, block, s_idx, e_idx


# ---------------------------------------------------------------------------
# 1. Source pins — block placement + marker emission.
# ---------------------------------------------------------------------------


class SourcePinsTest(unittest.TestCase):
    """The block must be where the design put it, with every documented
    marker present once. A prose-only description is not a ratchet."""

    def test_block_sits_after_destination_resolution(self):
        """The smart-push block reads $_push_remote and $_push_ref that
        the destination-resolution branch establishes. It must come
        AFTER the destination-refusal (upstream-not-main) check."""
        text, block, s_idx, e_idx = _block()
        refusal = 'REFUSING TO PUSH \u2014 upstream is not main'
        i_refusal = text.find(refusal)
        self.assertGreaterEqual(i_refusal, 0,
                                "guard: the destination-refusal branch is "
                                "missing from gate_and_push.sh")
        self.assertLess(i_refusal, s_idx,
                        "smart-push block (line %d) must come AFTER the "
                        "destination refusal (line %d) so $_push_ref/"
                        "$_push_remote are settled when the block runs"
                        % (text[:s_idx].count("\n") + 1,
                           text[:i_refusal].count("\n") + 1))

    def test_block_sits_before_existing_push(self):
        """The bare `git push` line below the block is unchanged; smart-
        push must NOT emit a marker that competes with it. The block's
        END sentinel is that push line."""
        text, _, s_idx, _ = _block()
        self.assertIn(_SMART_PUSH_END, text[s_idx:],
                      "the bare `git push` line must be in the source "
                      "after the smart-push block's start sentinel")

    def test_no_marker_appears_twice_in_code_lines(self):
        """Each of the three skip/no-op markers is emitted exactly once
        in code (not in the block's header comment). The header has the
        marker text quoted verbatim, so a whole-text count would always
        return >= 2."""
        _, block, _, _ = _block()
        code_lines = [ln for _n, ln in _code_lines(block)]
        code_text = "\n".join(code_lines)
        for marker in ("GATE PASSED -> NO-OP",
                       "GATE PASSED -> SKIPPED PUSH (remote advanced:",
                       "GATE PASSED -> SKIPPED PUSH (DIVERGED:"):
            self.assertEqual(code_text.count(marker), 1,
                             "smart-push block emits %r %d times in CODE "
                             "(expected 1) — duplication means either a "
                             "copy-paste regression or a marker race:\n%s"
                             % (marker, code_text.count(marker), code_text))
        # The `PUSHED origin/main now` marker belongs to the post-block
        # push path; the smart-push block must NOT emit it (that would
        # race the existing waiter's accept-matcher).
        self.assertNotIn("PUSHED origin/main now", code_text,
                         "smart-push block must not emit the waiter's "
                         "accept marker — that belongs to the unchanged "
                         "push path below the block")

    def test_existing_push_path_still_emits_pushed_origin_main(self):
        """The byte-identical PUSHED line gate_wait.sh has always matched
        must still be present on the normal-push path, AND must come
        AFTER the smart-push block. Smart-push sits above it; it does
        not touch it."""
        text = GATE_AND_PUSH.read_text()
        code_lines = [(n, ln) for n, ln in _code_lines(text)
                      if "PUSHED origin/main now" in ln]
        self.assertEqual(len(code_lines), 1,
                         "the accept marker must appear in CODE exactly "
                         "once everywhere — once, in the post-block push path")
        i_marker_line = code_lines[0][0]
        _, _, e_idx = _slice_block(text, _SMART_PUSH_START,
                                   _SMART_PUSH_END, "smart-push block")
        e_idx_line = text[:e_idx].count("\n") + 1
        self.assertGreater(i_marker_line, e_idx_line,
                           "the accept marker (line %d) must come AFTER "
                           "the smart-push block (line %d), on the "
                           "unchanged existing push path"
                           % (i_marker_line, e_idx_line))

    def test_block_emits_every_documented_exit_code(self):
        """NO-OP + REMOTE-ADVANCED both exit 0 (the design doc's case 1
        and case 2 share an exit class — a 'gate passed' admission).
        DIVERGED exits 4 (distinct from the existing exit 3 "push
        failed" so an observer can tell them apart)."""
        _, block, _, _ = _block()
        code_text = "\n".join(ln for _n, ln in _code_lines(block))
        self.assertEqual(code_text.count("exit 0"), 2,
                         "NO-OP + REMOTE-ADVANCED must each emit `exit 0` "
                         "in code (2 total); got %d:\n%s"
                         % (code_text.count("exit 0"), code_text))
        self.assertEqual(code_text.count("exit 4"), 1,
                         "DIVERGED must emit `exit 4` exactly once in code")

    def test_block_runs_after_all_other_push_safety_checks(self):
        """Smart-push is push-TIME, not push-DECISION-TIME. The
        destination-refusal + HEAD-moved + verdict cross-checks are
        unchanged; the block only fires when they all passed. Pin by
        source position: the block must come AFTER every such check."""
        text, _, s_idx, _ = _block()
        sentinels = [
            ("HEAD moved after the gate's verdict",
             "the verdict-described-HEAD / push-HEAD check"),
            ("REFUSING TO PUSH \u2014 the verdict describes a DIFFERENT COMMIT",
             "verdict-head cross-check"),
            ("REFUSING TO PUSH \u2014 the verdict describes a DIFFERENT WORKING TREE",
             "verdict-tree cross-check"),
            ("REFUSING TO PUSH \u2014 no upstream configured",
             "destination refusal (no upstream)"),
            ("REFUSING TO PUSH \u2014 upstream is not main",
             "destination refusal (non-main upstream)"),
        ]
        ln_smart = text[:s_idx].count("\n") + 1
        for needle, what in sentinels:
            i = text.find(needle)
            self.assertGreaterEqual(i, 0,
                                    "guard: %s sentinel %r not found in "
                                    "gate_and_push.sh" % (what, needle))
            ln = text[:i].count("\n") + 1
            self.assertLess(ln, ln_smart,
                            "smart-push block (line %d) must come AFTER "
                            "the %s (line %d); a push-time check that "
                            "fires before the safety checks would defeat "
                            "the design" % (ln_smart, what, ln))

    def test_gate_wait_terminal_regex_matches_every_new_marker(self):
        """The waiter's accept pattern (`GATE PASSED|...|PUSHED origin/
        main now`) must match each of the three new SKIPPED/NO-OP
        markers. Otherwise a smart-push that ran cleanly would spin the
        waiter to its timeout — the exact failure mode #3010 measured."""
        re_text = GATE_WAIT.read_text()
        m = re.search(r"TERMINAL_RE='([^']+)'", re_text)
        self.assertIsNotNone(m, "gate_wait.sh: TERMINAL_RE sentinel not found")
        terminal_re = m.group(1)
        rgx = re.compile(terminal_re)
        for marker in (
            "GATE PASSED -> NO-OP (already current on origin/main abc1234)",
            "GATE PASSED -> SKIPPED PUSH (remote advanced: origin/main=abc1234, local HEAD=def5678)",
            "GATE PASSED -> SKIPPED PUSH (DIVERGED: origin/main=abc1234, local HEAD=def5678)",
        ):
            self.assertRegex(marker, rgx,
                             "gate_wait.sh's TERMINAL_RE=%r does not "
                             "match %r — a waiter would bail on the "
                             "smart-push success path"
                             % (terminal_re, marker))


# ---------------------------------------------------------------------------
# 2. Functional tests — drive the EXTRACTED block against a real git repo.
# ---------------------------------------------------------------------------


_FUNCTIONAL_HARNESS = r'''
set -u
cd "$PIN_TMP"
# The block reads two runtime variables the wrapper script would have
# established; stub the rest so the extraction is self-contained.
_push_remote="$PIN_REMOTE"
_push_ref="$PIN_REF"
_head="$PIN_HEAD"
git() {
  if [ "$1" = "fetch" ]; then
    # The block's `git fetch … 2>/dev/null || true` is allowed to fail;
    # PIN_FETCH_FAIL=1 simulates that.
    if [ "${PIN_FETCH_FAIL:-0}" = "1" ]; then return 1; fi
    "$(which git)" "$@"
    return $?
  fi
  "$(which git)" "$@"
}
# The block calls `scripts/gc_scratch.sh …` (a relative path). The
# caller in _run_block stages a stub script at $PIN_TMP/scripts/
# gc_scratch.sh which touches PIN_GC_RAN_FILE on invocation. Bash
# resolves the relative path to that stub.
%(block)s
# If the block did NOT exit (no `exit` hit), the harness falls through
# to here — record that as the fall-through case (normal push).
if [ "${PIN_FALLTHROUGH:-0}" = "1" ]; then
  echo "PUSH_RAN" >> "$PIN_TMP/PUSH_RAN"
fi
echo "AFTER_BLOCK rc=$?"
'''


def _make_pair(tmp, *, local_ahead=0, remote_ahead=0, diverged=False):
    """Build (bare_dir, work_dir) for the four smart-push cases.

    The two repos share initial history; we then add commits on one or
    both sides to shape the case:
      local_ahead>0, remote_ahead=0, not diverged -> normal push
      local_ahead=0, remote_ahead>0, not diverged -> remote advanced
      local_ahead>0, remote_ahead=0, diverged=True -> diverged
      local_ahead=0, remote_ahead=0              -> no-op (== remote)

    Returns (bare_dir, work_dir, head_short, head_full, remote_short,
    remote_full). The work repo's `origin` remote IS the bare dir, so
    `git fetch origin main` from the work repo fetches from bare.
    """
    d = pathlib.Path(tmp)
    bare = d / "remote.git"
    work = d / "work"
    subprocess.run(["git", "init", "-q", "--bare", "-b", "main", str(bare)],
                   env=GIT_ENV, check=True, timeout=30)
    work.mkdir()
    _git(work, "init", "-q", "-b", "main", ".")
    _git(work, "remote", "add", "origin", str(bare))
    _git(work, "config", "remote.origin.fetch",
         "+refs/heads/*:refs/remotes/origin/*")
    # Seed history: one commit, then push to bare so the work repo has
    # an upstream and bare is reachable.
    _git(work, "commit", "-q", "--allow-empty", "-m", "seed")
    _git(work, "push", "-q", "origin", "main")
    if remote_ahead and not diverged:
        # Advance remote by `remote_ahead` commits from a side repo that
        # pushes them; the work repo stays at seed.
        side = d / "side"
        side.mkdir()
        _git(side, "init", "-q", "-b", "main", ".")
        _git(side, "remote", "add", "origin", str(bare))
        _git(side, "fetch", "-q", "origin")
        _git(side, "reset", "-q", "--hard", "origin/main")
        for i in range(remote_ahead):
            _git(side, "commit", "-q", "--allow-empty", "-m", "remote %d" % i)
        _git(side, "push", "-q", "origin", "main")
        _git(work, "fetch", "-q", "origin")
        # Keep the work repo at its existing HEAD (no local_ahead).
    if local_ahead and not diverged:
        # Local gets a unique commit; remote is at seed.
        for i in range(local_ahead):
            _git(work, "commit", "-q", "--allow-empty", "-m", "local %d" % i)
        # Note: no push — local is ahead, remote is at seed.
    if diverged:
        side = d / "side"
        side.mkdir()
        _git(side, "init", "-q", "-b", "main", ".")
        _git(side, "remote", "add", "origin", str(bare))
        _git(side, "fetch", "-q", "origin")
        _git(side, "reset", "-q", "--hard", "origin/main")
        _git(side, "commit", "-q", "--allow-empty", "-m", "remote-div")
        _git(side, "push", "-q", "origin", "main")
        _git(work, "fetch", "-q", "origin")
        _git(work, "commit", "-q", "--allow-empty", "-m", "local-div")
    head = _git(work, "rev-parse", "HEAD").stdout.strip()
    remote = _git(bare, "rev-parse", "main").stdout.strip()
    return (str(bare), str(work),
            head[:7], head, remote[:7], remote)


class _FunctionalBase(unittest.TestCase):
    """Shared fixtures: the extracted block + a harness that runs it."""

    @staticmethod
    def _stage_gc_stub(work_dir, gc_ran_file):
        """Stage a fake `scripts/gc_scratch.sh` inside the work dir so
        the block's relative-path call resolves to a stub that records
        the GC invocation.

        The block calls `scripts/gc_scratch.sh …` from cwd. The real
        operator's setup runs that as the repo's own copy; we install a
        one-line executable that touches the run's PIN_GC_RAN_FILE.
        """
        scripts_dir = pathlib.Path(work_dir) / "scripts"
        scripts_dir.mkdir(exist_ok=True)
        stub = scripts_dir / "gc_scratch.sh"
        stub.write_text(
            "#!/usr/bin/env bash\n"
            "# Smart-push test stub — records GC invocation only.\n"
            "touch \"$PIN_GC_RAN_FILE\" 2>/dev/null || true\n"
            "exit 0\n")
        stub.chmod(0o755)
        # Carry the gc-ran filename through the stub via an env var the
        # caller sets on the harness subprocess — the stub picks it up.
        os.environ["PIN_GC_RAN_FILE"] = str(gc_ran_file) if gc_ran_file else ""

    def _run_block(self, work_dir, head_full, *,
                   fallthrough=True, fetch_fail=False, gc_ran_file=""):
        """Source the extracted block into a bash subprocess whose cwd is
        the work repo. The harness cds to PIN_TMP (== work_dir) before
        sourcing the block, so the git fetch + rev-parse calls resolve
        against `origin` (set to the bare repo via the work repo's own
        remote config)."""
        _, block, _, _ = _block()
        # Stage the GC stub before launching so the work_dir's
        # scripts/gc_scratch.sh exists when the block sources it.
        self._stage_gc_stub(work_dir, gc_ran_file)
        script = _FUNCTIONAL_HARNESS % {"block": block}
        env = dict(GIT_ENV)
        env["PIN_TMP"] = work_dir
        env["PIN_REMOTE"] = "origin"
        env["PIN_REF"] = "refs/heads/main"
        env["PIN_HEAD"] = head_full
        env["PIN_FETCH_FAIL"] = "1" if fetch_fail else "0"
        env["PIN_FALLTHROUGH"] = "1" if fallthrough else "0"
        env["PIN_GC_RAN_FILE"] = str(gc_ran_file) if gc_ran_file else ""
        r = subprocess.run(["bash", "-c", script],
                           cwd=work_dir, env=env,
                           capture_output=True, text=True, timeout=30)
        push_ran = os.path.exists(os.path.join(work_dir, "PUSH_RAN"))
        gc_ran_seen = bool(gc_ran_file) and os.path.exists(gc_ran_file)
        return r, push_ran, gc_ran_seen


class FunctionalTest(_FunctionalBase):
    """Drive the EXTRACTED smart-push block against a real bare remote.

    The block is sourced into the WORK repo with `git push` neutralised
    (the existing push line is below the block — the harness records a
    PUSH_RAN marker to observe fall-through). Each scenario shapes the
    (bare, work) pair differently and asserts the right exit code +
    the right marker line.
    """

    # -- Case 1: NO-OP -----------------------------------------------------

    def test_no_op_when_local_equals_remote(self):
        """Local and remote HEADs are the same sha; the gate passed but
        someone (maybe us, in a prior run) already pushed it. The
        block should NO-OP with exit 0 and the documented marker."""
        _, block, _, _ = _block()
        with tempfile.TemporaryDirectory() as tmp:
            gc_file = os.path.join(tmp, "gc_ran_noop")
            bare, work, h_s, h_f, r_s, r_f = _make_pair(tmp)
            self.assertEqual(h_f, r_f,
                             "guard: setup failed — local and remote HEADs "
                             "should be the same sha for the no-op case")
            r, push_ran, gc_ran_seen = self._run_block(
                work, h_f, gc_ran_file=gc_file)
        self.assertEqual(r.returncode, 0,
                         "NO-OP must exit 0:\nstdout=%r\nstderr=%r"
                         % (r.stdout, r.stderr))
        self.assertIn("GATE PASSED -> NO-OP", r.stdout,
                      "NO-OP marker missing:\n%s" % r.stdout)
        self.assertIn(h_s, r.stdout,
                      "the marker must name the gated commit (the short "
                      "sha %s):\n%s" % (h_s, r.stdout))
        self.assertFalse(push_ran,
                         "NO-OP must NOT fall through to the existing push")
        self.assertTrue(gc_ran_seen,
                        "NO-OP runs gc_scratch.sh --landed-only …")

    # -- Case 2: REMOTE ADVANCED -------------------------------------------

    def test_skipped_push_when_remote_advanced(self):
        """Someone else pushed commits between our verdict and our push
        step. The gated commit is already superseded; a bare push
        would non-fast-forward fail. Skip cleanly with exit 0."""
        _, block, _, _ = _block()
        with tempfile.TemporaryDirectory() as tmp:
            gc_file = os.path.join(tmp, "gc_ran_advanced")
            bare, work, h_s, h_f, r_s, r_f = _make_pair(
                tmp, remote_ahead=2)
            self.assertNotEqual(h_f, r_f,
                                "guard: setup failed — remote should be "
                                "ahead of local for the remote-advanced "
                                "case; local=%s remote=%s" % (h_s, r_s))
            self.assertTrue(
                _git(pathlib.Path(work), "merge-base", "--is-ancestor",
                     h_f, r_f).returncode == 0,
                "guard: local must be a fast-forward ancestor of remote "
                "for the remote-advanced case")
            r, push_ran, gc_ran_seen = self._run_block(
                work, h_f, gc_ran_file=gc_file)
        self.assertEqual(r.returncode, 0,
                         "REMOTE-ADVANCED must exit 0:\nstdout=%r\nstderr=%r"
                         % (r.stdout, r.stderr))
        self.assertIn("GATE PASSED -> SKIPPED PUSH (remote advanced:", r.stdout,
                      "REMOTE-ADVANCED marker missing:\n%s" % r.stdout)
        self.assertIn(h_s, r.stdout,
                      "the marker must name the gated local HEAD:\n%s"
                      % r.stdout)
        self.assertIn(r_s, r.stdout,
                      "the marker must name the remote HEAD:\n%s" % r.stdout)
        self.assertIn("Local is fast-forward ancestor of remote", r.stdout,
                      "the REMOTE-ADVANCED branch's diagnostic is missing:\n"
                      "%s" % r.stdout)
        self.assertFalse(push_ran,
                         "REMOTE-ADVANCED must NOT fall through to the "
                         "existing push")
        self.assertTrue(gc_ran_seen,
                        "REMOTE-ADVANCED runs gc_scratch.sh --builds --root "
                        "(best-effort, no --landed-only since we did not "
                        "push)")

    # -- Case 3: NORMAL PUSH -----------------------------------------------

    def test_normal_push_falls_through_to_existing_push_line(self):
        """Local is ahead of remote; merge-base direction is `remote is
        ancestor of local`. The block must NOT exit — control reaches
        the existing `git push` line, which the harness records via
        PUSH_RAN."""
        _, block, _, _ = _block()
        with tempfile.TemporaryDirectory() as tmp:
            bare, work, h_s, h_f, r_s, r_f = _make_pair(
                tmp, local_ahead=1)
            self.assertNotEqual(h_f, r_f,
                                "guard: setup failed — local should be "
                                "ahead of remote for the normal-push case")
            self.assertTrue(
                _git(pathlib.Path(work), "merge-base", "--is-ancestor",
                     r_f, h_f).returncode == 0,
                "guard: remote must be a fast-forward ancestor of local "
                "for the normal-push case")
            r, push_ran, gc_ran_seen = self._run_block(work, h_f)
        self.assertTrue(push_ran,
                        "NORMAL-PUSH must fall through to the existing "
                        "`git push` line (PUSH_RAN marker absent):\n"
                        "stdout=%r\nstderr=%r" % (r.stdout, r.stderr))
        self.assertNotIn("GATE PASSED -> NO-OP", r.stdout,
                         "NORMAL-PUSH must not emit the NO-OP marker")
        self.assertNotIn("GATE PASSED -> SKIPPED", r.stdout,
                         "NORMAL-PUSH must not emit a SKIPPED marker")

    # -- Case 4: DIVERGED --------------------------------------------------

    def test_diverged_refuses_with_exit_4(self):
        """Local and remote have each committed on top of a shared base.
        Neither is an ancestor of the other — a bare push would fail
        and a force-push would lose work. Skip cleanly with exit 4
        (distinct from the existing exit 3 "push failed" so an
        observer can tell them apart)."""
        _, block, _, _ = _block()
        with tempfile.TemporaryDirectory() as tmp:
            gc_file = os.path.join(tmp, "gc_ran_diverged")
            bare, work, h_s, h_f, r_s, r_f = _make_pair(
                tmp, diverged=True)
            self.assertNotEqual(h_f, r_f,
                                "guard: setup failed — diverged heads "
                                "should differ")
            anc1 = _git(pathlib.Path(work), "merge-base", "--is-ancestor",
                        h_f, r_f).returncode
            anc2 = _git(pathlib.Path(work), "merge-base", "--is-ancestor",
                        r_f, h_f).returncode
            self.assertNotEqual(anc1, 0,
                                "guard: local must NOT be an ancestor of "
                                "remote for the diverged case")
            self.assertNotEqual(anc2, 0,
                                "guard: remote must NOT be an ancestor of "
                                "local for the diverged case")
            r, push_ran, gc_ran_seen = self._run_block(
                work, h_f, gc_ran_file=gc_file)
        self.assertEqual(r.returncode, 4,
                         "DIVERGED must exit 4 (distinct from existing "
                         "push-failed exit 3):\nstdout=%r\nstderr=%r"
                         % (r.stdout, r.stderr))
        self.assertIn("GATE PASSED -> SKIPPED PUSH (DIVERGED:", r.stdout,
                      "DIVERGED marker missing:\n%s" % r.stdout)
        self.assertIn(h_s, r.stdout,
                      "the marker must name the local HEAD:\n%s" % r.stdout)
        self.assertIn(r_s, r.stdout,
                      "the marker must name the remote HEAD:\n%s" % r.stdout)
        self.assertIn("Neither is ancestor of the other", r.stdout,
                      "the DIVERGED branch's diagnostic is missing:\n%s"
                      % r.stdout)
        self.assertFalse(push_ran,
                         "DIVERGED must NOT fall through to the existing "
                         "push")
        self.assertFalse(gc_ran_seen,
                         "DIVERGED must NOT auto-GC: the tree is in a "
                         "mid-divergence state we do not want to reap from")


# ---------------------------------------------------------------------------
# 3. Fetch-failure falls through gracefully.
# ---------------------------------------------------------------------------


class FetchFailureFallsThroughTest(_FunctionalBase):
    """A fetch failure (no network, missing remote, rate limit) must NOT
    silently swallow the push — it must fall through to the existing
    push line, where the existing diagnostics surface the real problem.

    The shape the user directive guards against is a smart-push that
    silently treats "could not determine remote state" as "OK to
    skip". That is the OPPOSITE of the design: when smart-push cannot
    see the remote, the existing push's clear failure modes (network /
    non-fast-forward / permission denied) take over.
    """

    def test_fetch_failure_falls_through_to_existing_push(self):
        with tempfile.TemporaryDirectory() as tmp:
            bare, work, h_s, h_f, r_s, r_f = _make_pair(
                tmp, local_ahead=1)
            # PIN_FETCH_FAIL=1: the harness's stub `git fetch` returns 1,
            # so `_remote_head` stays empty and the block falls through.
            r, push_ran, gc_ran_seen = self._run_block(
                work, h_f, fetch_fail=True)
        self.assertTrue(push_ran,
                        "a fetch failure must fall through to the "
                        "existing push line (PUSH_RAN marker absent):\n"
                        "stdout=%r\nstderr=%r" % (r.stdout, r.stderr))
        # No smart-push marker was emitted — the block saw no remote
        # state and stood down, exactly the documented behaviour.
        self.assertNotIn("GATE PASSED -> NO-OP", r.stdout)
        self.assertNotIn("GATE PASSED -> SKIPPED", r.stdout)


# ---------------------------------------------------------------------------
# 4. Non-main push with BEZEL_GATE_PUSH_DEST_OK=1.
# ---------------------------------------------------------------------------


class NonMainPushStillSmartPushedTest(unittest.TestCase):
    """The `BEZEL_GATE_PUSH_DEST_OK=1` escape lets an operator push to a
    non-main destination. Smart-push is push-time only — it must run
    regardless of destination, because the race it detects (origin/main
    advances WHILE the gate runs) is structurally identical for any
    ref the branch tracks.

    Pin by source: the smart-push block must not gate on the OK flag,
    and it must read its destination from `$_push_remote`/`$_push_ref`
    rather than a hardcoded "origin/main".
    """

    def test_block_does_not_gate_on_BEZEL_GATE_PUSH_DEST_OK(self):
        _, block, _, _ = _block()
        self.assertNotIn("BEZEL_GATE_PUSH_DEST_OK", block,
                         "smart-push must not reference the non-main "
                         "escape — it runs for every push destination")

    def test_block_uses_the_resolved_remote_and_ref_not_a_literal(self):
        _, block, _, _ = _block()
        # The block fetches `$_push_remote` and resolves
        # `${_push_remote}/${_push_ref##*/}` (the basename of the ref),
        # not `origin/main`. A regression to a hardcoded remote would
        # silently no-op on a non-main push destination — the "OK flag
        # bypasses smart-push" defect.
        self.assertIn('git fetch "$_push_remote"', block,
                      "smart-push must fetch from the branch's upstream "
                      "remote, not a literal `origin`")
        self.assertIn('"${_push_remote}/${_push_ref##*/}"', block,
                      "smart-push must resolve the destination's short "
                      "name (basename of $_push_ref) — a regression to "
                      "`origin/refs/heads/main` would resolve to a "
                      "non-existent `refs/remotes/origin/refs/heads/main`")
        self.assertNotIn('origin "$_push_ref"', block,
                         "smart-push must not hardcode `origin` as the "
                         "fetch remote")


if __name__ == "__main__":
    unittest.main()