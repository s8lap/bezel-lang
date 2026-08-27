"""#3413 — the gate must name, and follow, the tree it validates.

WHAT WENT WRONG. `scripts/gate_full_cycle.sh` opened with two lines:

    REPO=/home/<the-operator>/w/code/bezel-lang     (spelled out in git history)
    cd "$REPO" || exit 1

(the literal is deliberately broken up above: the ratchet below scans this very
directory, and a file that records the defect must not read as the defect)

`gate_and_push.sh` cds to `git rev-parse --show-toplevel`, so launching a gate
from a lane or integration worktree LOOKED like it gated that worktree. It then
called this script, which cd'd away to the literal and gated whatever the shared
checkout had checked out. The verdict described a tree the operator never
selected, and nothing in the banner, the log, or the verdict said so.

The failure is silent and biased toward FALSE GREEN: the shared checkout
normally sits on already-landed `main` — by construction the tree most likely to
be clean — so a gate that misses its target cannot fail on the new work.

MEASURED. Run 4 cycle 4 (2026-08-27) reported "BOTH oracles PASS, 19071/19071
gtests, 0 ASan" and pushed e9bd762, while its own gate-verdict.json recorded
head=d34c8b4 — the previous day's commit on cycle-2-integration-v3, 56 files /
+7775 lines away. Nine commits reached main ungated, and two defects escaped
through the hole: #2837's regression test, whose assertions compare an 8-bit
exit status against 501 and so could never pass, and a gap-pin superseded two
days earlier.

WHAT IS PINNED HERE, and why each pin exists rather than a comment:

  RootResolutionTest      — the root follows the SCRIPT, not a literal, and the
                            banner names it. A comment cannot stop the literal
                            coming back; this scans the source for it.
  MismatchRefusalTest     — the load-bearing proof. Two REAL git repos are
                            built, the gate script from repo A is invoked with
                            cwd inside repo B, and the refusal is observed:
                            non-zero exit, both roots and both HEADs named, and
                            no gate work started. The negative controls (same
                            tree, non-repo cwd, explicit override) are here too,
                            because a refusal that fires on the correct tree
                            would block every push — worse than the bug.
  VerdictNamesItsTreeTest — the verdict carries `repo`, and a reader can refuse
                            a verdict that describes another tree.
  PushBoundaryRefusalTest — the fail-open the Run 4 forensic actually walked
                            through: `verdict read --expect-head` collapses
                            missing/unparseable/stale into exit 2, and
                            gate_and_push.sh's exit-2 branch is "relying on $?"
                            — which pushes. A present verdict naming another
                            commit or tree must refuse instead.
  NoGrepPoisoningTest     — the identity banner must not disturb the two log
                            streams that gate decisions are made from: the ASan
                            evidence grep over gate.sh's stdout, and
                            gate_wait.sh's terminal-marker regex. An
                            observability change is a change (#3010).

The bash-driving tests never run a gate: every case exits inside the first
~120 lines of gate_full_cycle.sh, before the vacuous phase and long before any
build. Each is bounded by an explicit subprocess timeout.

Stdlib only (subprocess/tempfile/unittest) — the house style for this directory.
"""

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import verdict as gv   # noqa: E402

# #3463: the two tests below drive the REAL gate_and_push.sh in its real gating
# shape (stub cycle, no arguments), so the script correctly publishes its pid and
# exit status — to GLOBAL paths that, inside the #3415 gate-selftest phase, are
# the live gate's own. Measured overwriting them with "1", a FAILED verdict for a
# gate that went on to pass. Redirect them into a temp dir.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from marker_isolation import isolated_markers  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPTS = REPO / "scripts"
FULL_CYCLE = SCRIPTS / "gate_full_cycle.sh"
GATE_SH = SCRIPTS / "gate.sh"
GATE_AND_PUSH = SCRIPTS / "gate_and_push.sh"

# A git environment that works on a machine with no user identity configured and
# cannot be perturbed by the operator's own git config.
GIT_ENV = dict(
    os.environ,
    GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
    GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t",
    GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null",
)


def _git(cwd, *args):
    return subprocess.run(("git",) + args, cwd=str(cwd), env=GIT_ENV,
                          capture_output=True, text=True, timeout=30)


def _make_repo(path, marker):
    """Create a real git repo at `path` carrying a copy of scripts/.

    A copy, not a symlink: the whole point of the fix is that the script's own
    location decides the root, so the test must give each repo its own physical
    copy of the file under test. `marker` distinguishes the two repos' commits
    so their HEAD shas differ.
    """
    path = pathlib.Path(path)
    path.mkdir(parents=True, exist_ok=True)
    _git(path, "init", "-q", "-b", "main")
    shutil.copytree(str(SCRIPTS), str(path / "scripts"))
    (path / "marker.txt").write_text(marker)
    _git(path, "add", "-A")
    _git(path, "commit", "-q", "-m", "seed " + marker)
    # Resolve symlinks the same way the script does (`pwd -P`), so a comparison
    # against the script's own output is not defeated by /var/tmp being a link
    # on some machines.
    return pathlib.Path(os.path.realpath(str(path)))


def _run_cycle(script_repo, cwd, env=None, args=("-h",), timeout=60):
    """Invoke `script_repo/scripts/gate_full_cycle.sh` with cwd=`cwd`.

    `-h` is the probe: the argument parse runs AFTER the root resolution and the
    mismatch refusal, so this exercises exactly the code under test and then
    exits — no configure, no build, no shard. That ordering is itself part of
    the contract (a gate must decide which tree it is looking at before it does
    anything), and `RootResolutionTest.test_refusal_precedes_any_gate_work`
    pins it.
    """
    e = dict(GIT_ENV)
    if env:
        e.update(env)
    return subprocess.run(
        [str(pathlib.Path(script_repo) / "scripts" / "gate_full_cycle.sh")] + list(args),
        cwd=str(cwd), env=e, capture_output=True, text=True, timeout=timeout)


class _TwoRepos(unittest.TestCase):
    """Fixture: two real, DIFFERENT git repos, each with its own scripts/ copy."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.mkdtemp(prefix="gate-repo-identity-")
        cls.repo_a = _make_repo(os.path.join(cls._tmp, "a"), "alpha")
        cls.repo_b = _make_repo(os.path.join(cls._tmp, "b"), "beta")
        cls.head_a = _git(cls.repo_a, "rev-parse", "HEAD").stdout.strip()
        cls.head_b = _git(cls.repo_b, "rev-parse", "HEAD").stdout.strip()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls._tmp, ignore_errors=True)

    def test_fixture_repos_are_distinct(self):
        """Guard the guard: if the two fixture repos shared a HEAD, every
        mismatch assertion below would be vacuous — a green from a matcher that
        matched nothing is worth nothing (the check_vacuous_tests lesson)."""
        self.assertNotEqual(self.repo_a, self.repo_b)
        self.assertNotEqual(self.head_a, self.head_b)
        self.assertTrue(self.head_a and self.head_b)


class RootResolutionTest(_TwoRepos):
    """The repo root comes from the script's own location, and is announced."""

    def test_no_absolute_repo_literal_survives_in_scripts(self):
        """THE RATCHET. No script may hardcode an absolute path to a checkout.

        This is the defect in one line, so it is scanned for in one line. It
        covers every file in scripts/, not just gate_full_cycle.sh, because the
        same shape had already spread (scripts/lane_status.sh carried it as a
        fallback). Matching is on the literal user-checkout prefix a hardcode
        takes; a comment quoting the historical line is excluded, since the
        record of what went wrong must not read as the thing going wrong (the
        false-positive class check_imported_symbol_reads.py hit on its own
        documentation).
        """
        offenders = []
        for path in sorted(SCRIPTS.rglob("*")):
            if not path.is_file() or path.suffix not in (".sh", ".py"):
                continue
            for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                stripped = line.lstrip()
                if stripped.startswith("#"):
                    continue
                if re.search(r"/home/[A-Za-z0-9._-]+/w/code/bezel-lang", line):
                    offenders.append("%s:%d: %s" % (
                        path.relative_to(REPO), n, line.strip()))
        self.assertEqual(
            offenders, [],
            "a script hardcodes an absolute checkout path (#3413). Derive the "
            "root from the script's own location instead:\n  " +
            "\n  ".join(offenders))

    def test_root_follows_the_script_not_the_cwd(self):
        """Run repo A's script from inside repo A: the banner must name A."""
        r = _run_cycle(self.repo_a, self.repo_a)
        self.assertEqual(r.returncode, 0,
                         "gating the tree you stand in must not refuse; "
                         "rc=%d stderr=%r" % (r.returncode, r.stderr))
        self.assertIn("TREE UNDER GATE", r.stdout)
        self.assertIn(str(self.repo_a), r.stdout,
                      "the banner must print the resolved repo path (#3413); "
                      "stdout=%r" % r.stdout[:800])
        self.assertIn(self.head_a[:7], r.stdout,
                      "the banner must print the gated HEAD (#3413); "
                      "stdout=%r" % r.stdout[:800])
        self.assertNotIn(str(self.repo_b), r.stdout)

    def test_root_ignores_the_launching_trees_path_entirely(self):
        """Launched from a NON-repo directory, the root is still the script's.

        This is the detached-launch shape CLAUDE.md documents
        (`setsid nohup … &` from a scratch dir). It must keep working, and it
        must resolve to the script's tree rather than to nothing.
        """
        r = _run_cycle(self.repo_a, self._tmp)
        self.assertEqual(r.returncode, 0,
                         "a launch from outside any worktree must not refuse "
                         "(there is no second tree to confuse it with); "
                         "rc=%d stderr=%r" % (r.returncode, r.stderr))
        self.assertIn(str(self.repo_a), r.stdout)

    def test_refusal_precedes_any_gate_work(self):
        """No phase may start before the tree is settled.

        Ordering is load-bearing: a mismatch caught after the configure/build
        would have already spent ~25 minutes on the wrong tree, and (worse) the
        vacuous and seam phases would have reported on it. Assert the source
        order directly — the resolution block sits ahead of the first phase.
        """
        text = FULL_CYCLE.read_text()
        i_resolve = text.index("WHICH TREE DOES THIS GATE VALIDATE?")
        i_refuse = text.index("REFUSE A TREE THE CALLER IS NOT STANDING IN")
        i_vacuous = text.index("VACUOUS-ASSERTION PHASE")
        i_run = text.index("run_until_trustworthy \"$LOGDIR")
        self.assertLess(i_resolve, i_refuse)
        self.assertLess(i_refuse, i_vacuous,
                        "the tree must be settled before the first phase runs")
        self.assertLess(i_vacuous, i_run)

    def test_unresolvable_root_refuses_instead_of_guessing(self):
        """A script that cannot find its repo must exit, not fall back.

        Constructed by copying the script OUT of any git repo. Fail closed: the
        original code's answer to "which tree?" was a literal, and the whole
        lesson is that a wrong answer is worse than no answer.
        """
        outside = pathlib.Path(self._tmp) / "loose" / "scripts"
        outside.mkdir(parents=True, exist_ok=True)
        shutil.copy(str(FULL_CYCLE), str(outside / "gate_full_cycle.sh"))
        r = subprocess.run(
            [str(outside / "gate_full_cycle.sh"), "-h"],
            cwd=self._tmp, env=GIT_ENV, capture_output=True, text=True, timeout=60)
        self.assertNotEqual(r.returncode, 0,
                            "an unresolvable root must refuse, not proceed")
        self.assertIn("REFUSING", r.stderr)
        self.assertIn("#3413", r.stderr)


class MismatchRefusalTest(_TwoRepos):
    """THE PROOF: construct the mismatch and observe the refusal."""

    def test_gating_another_tree_from_a_worktree_is_refused(self):
        """cwd = repo B, script = repo A's copy. This is the Run 6 shape:
        the coordinator launched the shared checkout's gate from an integration
        worktree, and it validated the shared checkout while the launcher
        printed the worktree's HEAD."""
        r = _run_cycle(self.repo_a, self.repo_b)
        self.assertNotEqual(
            r.returncode, 0,
            "gating a tree the caller is not standing in MUST fail loudly "
            "(#3413); it exited 0 with stdout=%r" % r.stdout[:800])
        self.assertEqual(r.returncode, 66,
                         "the documented refusal code is 66; got %d, stderr=%r"
                         % (r.returncode, r.stderr[:800]))

    def test_the_refusal_names_both_trees_and_both_heads(self):
        """A refusal that does not say WHICH two trees just moves the confusion.

        The whole defect was a verdict that did not name its subject, so the
        message must carry: the tree that would have been gated, the tree the
        caller is in, and both HEADs.
        """
        r = _run_cycle(self.repo_a, self.repo_b)
        err = r.stderr
        self.assertIn(str(self.repo_a), err, "must name the tree it would gate")
        self.assertIn(str(self.repo_b), err, "must name the caller's tree")
        self.assertIn(self.head_a[:7], err, "must name the gated HEAD")
        self.assertIn(self.head_b[:7], err, "must name the caller's HEAD")
        self.assertIn("BEZEL_GATE_REPO", err,
                      "must tell the operator how to proceed deliberately")

    def test_refusal_starts_no_gate_work(self):
        """Nothing beyond the identity check may have run.

        A refusal that had already configured, built, or written a cycle log
        would be a refusal in name only. The phase banners are the observable:
        none of them may appear.
        """
        r = _run_cycle(self.repo_a, self.repo_b)
        self.assertEqual(r.returncode, 66,
                         "guard: without the refusal this case exits 0 and the "
                         "phase assertions below would pass vacuously")
        combined = r.stdout + r.stderr
        for phase in ("vacuous-assertion phase", "imported-symbol phase",
                      "cycle start:", "conformance phase"):
            self.assertNotIn(phase, combined,
                             "a refused gate must not reach the %r phase" % phase)

    def test_no_verdict_is_written_by_a_refused_gate(self):
        """A refusal must leave no verdict a later reader could mistake for one.

        The EXIT trap that writes the cycle verdict is registered AFTER the
        identity block, so a refusal writes nothing. Pinned because the opposite
        — a verdict file saying "refused" at the shared default path — would be
        read by gate_wait.sh as this gate's outcome.
        """
        vf = os.path.join(self._tmp, "verdict-refused.json")
        r = _run_cycle(self.repo_a, self.repo_b,
                       env={"GATE_VERDICT_FILE": vf})
        self.assertEqual(r.returncode, 66)
        self.assertFalse(os.path.exists(vf),
                         "a refused gate must not write a verdict file")

    def test_override_permits_the_deliberate_case(self):
        """BEZEL_GATE_REPO is the escape hatch, and it must actually work.

        A refusal with no documented way past it gets worked around by editing
        the script — which is how the literal got there. The override makes the
        operator NAME the tree, so the deliberate case is still explicit.
        """
        r = _run_cycle(self.repo_a, self.repo_b,
                       env={"BEZEL_GATE_REPO": str(self.repo_b)})
        self.assertEqual(r.returncode, 0,
                         "an explicit BEZEL_GATE_REPO must be honored; rc=%d "
                         "stderr=%r" % (r.returncode, r.stderr[:800]))
        self.assertIn(str(self.repo_b), r.stdout,
                      "the override's tree must be the one announced")
        self.assertIn("BEZEL_GATE_REPO override", r.stdout,
                      "the banner must say the root came from the override, so "
                      "a log reader can see the refusal was bypassed on purpose")

    def test_override_pointing_outside_a_repo_still_refuses(self):
        """The override selects a tree; it does not disable the requirement.

        Fail closed: `BEZEL_GATE_REPO=/tmp` must not turn into "gate whatever".
        """
        r = _run_cycle(self.repo_a, self.repo_b,
                       env={"BEZEL_GATE_REPO": self._tmp})
        self.assertEqual(r.returncode, 66,
                         "an override that names no worktree must refuse; "
                         "rc=%d" % r.returncode)
        self.assertIn("not inside a git worktree", r.stderr)

    def test_a_subdirectory_of_the_same_tree_is_not_a_mismatch(self):
        """NO FALSE REFUSAL. Standing in scripts/ is standing in the tree.

        The check compares worktree ROOTS, not cwds. If it compared cwds, every
        gate launched from a subdirectory would refuse — and a refusal on the
        correct tree blocks every push, which is worse than the bug being fixed.
        """
        r = _run_cycle(self.repo_a, self.repo_a / "scripts")
        self.assertEqual(r.returncode, 0,
                         "a launch from a subdirectory of the gated tree must "
                         "not refuse; rc=%d stderr=%r"
                         % (r.returncode, r.stderr[:800]))

    def test_a_symlinked_spelling_of_the_same_tree_is_not_a_mismatch(self):
        """NO FALSE REFUSAL. scripts/lane_setup.sh symlinks lane paths around,
        so one tree routinely has two spellings. Both roots are normalized with
        `pwd -P`; a symlinked cwd must therefore compare equal."""
        link = pathlib.Path(self._tmp) / "link-to-a"
        if link.exists() or link.is_symlink():
            link.unlink()
        os.symlink(str(self.repo_a), str(link))
        r = _run_cycle(self.repo_a, link)
        self.assertEqual(r.returncode, 0,
                         "a symlinked spelling of the gated tree must not read "
                         "as a different tree; rc=%d stderr=%r"
                         % (r.returncode, r.stderr[:800]))


class VerdictNamesItsTreeTest(unittest.TestCase):
    """The verdict must name the tree it describes, not only the commit."""

    def test_verdict_object_carries_repo(self):
        obj = gv.build_verdict_obj(rc=0, head="deadbeef", repo="/some/tree")
        self.assertEqual(obj["repo"], "/some/tree")
        self.assertEqual(obj["head"], "deadbeef")

    def test_repo_defaults_to_empty_not_missing(self):
        """A verdict written by a caller that does not pass --repo still HAS the
        key, so a reader can distinguish "gate did not record a tree" from "this
        verdict predates the field"."""
        self.assertEqual(gv.build_verdict_obj(rc=0)["repo"], "")

    def test_cli_write_and_read_round_trip_the_repo(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "v.json")
            rc = gv.main(["verdict.py", "write", "--path", path, "--rc", "0",
                          "--head", "abc123", "--repo", "/tree/one"])
            self.assertEqual(rc, 0)
            with open(path) as f:
                self.assertEqual(json.load(f)["repo"], "/tree/one")

    def test_write_from_state_records_the_repo(self):
        """gate.sh's writer path — the one a single-oracle gate uses."""
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "v.json")
            rc = gv.main(["verdict.py", "write_from_state", "--path", path,
                          "--rc", "0", "--head", "abc123", "--repo", "/tree/one"])
            self.assertEqual(rc, 0)
            with open(path) as f:
                self.assertEqual(json.load(f)["repo"], "/tree/one")

    def test_expect_repo_refuses_a_verdict_about_another_tree(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "v.json")
            gv.main(["verdict.py", "write", "--path", path, "--rc", "0",
                     "--head", "abc123", "--repo", "/tree/one"])
            self.assertEqual(
                gv.main(["verdict.py", "read", "--path", path,
                         "--expect-repo", "/tree/one"]), 0)
            self.assertEqual(
                gv.main(["verdict.py", "read", "--path", path,
                         "--expect-repo", "/tree/two"]), 2,
                "a verdict describing another tree must be refused, not read")

    def test_expect_repo_refuses_a_verdict_that_predates_the_field(self):
        """An absent field is not evidence of agreement.

        The "not X => assume Z" category defect: a verdict with no repo key
        cannot answer the question, so the answer is "refuse and fall back",
        never "assume it matches".
        """
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "v.json")
            gv.main(["verdict.py", "write", "--path", path, "--rc", "0",
                     "--head", "abc123", "--repo", "/tree/one"])
            with open(path) as f:
                obj = json.load(f)
            del obj["repo"]
            with open(path, "w") as f:
                json.dump(obj, f)
            self.assertEqual(
                gv.main(["verdict.py", "read", "--path", path,
                         "--expect-repo", "/tree/one"]), 2)

    def test_cycle_verdict_records_the_captured_head_not_a_fresh_read(self):
        """The cycle's verdict writer must use the sha captured on entry.

        Re-reading HEAD in the EXIT trap would let a HEAD that moved under a
        running gate silently relabel the verdict as being about the new commit
        — the same "the report names the wrong subject" defect in a second
        place.
        """
        body = FULL_CYCLE.read_text().split(
            "write_cycle_verdict() {", 1)[1].split("\n}", 1)[0]
        self.assertIn('--head="${GATE_REPO_HEAD:-}"', body)
        self.assertIn('--repo="${REPO:-}"', body)
        self.assertNotIn("rev-parse", body,
                         "the verdict writer must not re-read HEAD at exit")

    def test_gate_sh_passes_its_repo_to_the_verdict(self):
        text = GATE_SH.read_text()
        self.assertIn('GATE_REPO="$(pwd -P)"', text)
        self.assertIn('--repo "${GATE_REPO:-}"', text)


class PushBoundaryRefusalTest(unittest.TestCase):
    """gate_and_push.sh must refuse a verdict that is not about this push.

    This is the fail-open the Run 4 forensic actually walked through, and it is
    independent of the root-derivation fix: even with a correct root, a stale
    verdict at the shared default path was reported as "no usable cycle verdict
    … relying on $?" and the push proceeded.
    """

    def setUp(self):
        self.text = GATE_AND_PUSH.read_text()

    def test_head_field_is_read_before_the_expect_head_call(self):
        """Order is the whole fix: `--expect-head` cannot distinguish stale from
        missing (both exit 2), so the direct field read must come first."""
        i_field = self.text.index('--field head')
        i_expect = self.text.index('--expect-head "$_head"')
        self.assertLess(i_field, i_expect,
                        "the direct head read must precede the --expect-head "
                        "call, whose exit 2 is indistinguishable from 'absent'")

    def test_a_disagreeing_head_exits_rather_than_noting(self):
        """The refusal must EXIT. The bug was that it printed a note."""
        block = self.text.split('--field head', 1)[1].split('--expect-head', 1)[0]
        self.assertIn('REFUSING TO PUSH', block)
        self.assertIn('exit 1', block)
        self.assertIn('#3413', block)

    def test_a_disagreeing_repo_exits_too(self):
        block = self.text.split('--field repo', 1)[1].split('--expect-head', 1)[0]
        self.assertIn('DIFFERENT WORKING TREE', block)
        self.assertIn('exit 1', block)

    def test_absent_field_keeps_the_documented_fallback(self):
        """Exit 2 from the direct read means genuinely absent — old verdicts
        must still fall back to $?, or this change would block every push made
        while a tree carries a pre-#3413 verdict."""
        for field in ("head", "repo"):
            block = self.text.split('--field %s' % field, 1)[1]
            block = block.split('exit 1', 1)[0]
            self.assertRegex(
                block, r'-ne 2 \]',
                "the %s check must stand down when the read reports the field "
                "absent (exit 2)" % field)

    def test_gate_and_push_announces_the_tree(self):
        self.assertIn('GATE_REPO_ROOT="$(pwd -P)"', self.text)
        self.assertIn('gate_and_push: tree =', self.text)


class HandoffTest(_TwoRepos):
    """gate_and_push.sh must hand the cycle the tree it announced.

    The two scripts together are the surface that broke: gate_and_push.sh cds to
    the launching worktree, so its banner is honest — and then the cycle cd'd
    away. A stub cycle script that reports its own cwd proves the handoff
    end-to-end without running a gate.

    GATE_SKIP_LOCK=1 keeps the probe off the production gate lockfile
    (/var/tmp/bezel-scratch/gate.lock): a real gate holding it would otherwise
    make this test report "another gate is running" and pass vacuously, and
    grabbing it would interfere with a real gate.
    """

    def _stub_cycle(self, repo):
        """Replace repo's gate_full_cycle.sh with a stub that reports its cwd
        and exits 64, so gate_and_push takes its GATE FAILED branch (no push,
        no trend check, no network)."""
        stub = pathlib.Path(repo) / "scripts" / "gate_full_cycle.sh"
        stub.write_text(
            "#!/usr/bin/env bash\n"
            'echo "STUBCYCLE cwd=$PWD arg0=$0"\n'
            "exit 64\n")
        stub.chmod(0o755)
        _git(repo, "add", "-A")
        _git(repo, "commit", "-q", "-m", "stub cycle")

    def test_gate_and_push_hands_the_cycle_its_own_tree(self):
        with tempfile.TemporaryDirectory() as d:
            repo = _make_repo(os.path.join(d, "handoff"), "handoff")
            self._stub_cycle(repo)
            with isolated_markers(dict(GIT_ENV, GATE_SKIP_LOCK="1")) as env:
                r = subprocess.run(
                    [str(repo / "scripts" / "gate_and_push.sh")],
                    cwd=str(repo), env=env, capture_output=True, text=True,
                    timeout=120)
            out = r.stdout + r.stderr
            self.assertIn("STUBCYCLE", out,
                          "gate_and_push must invoke the cycle script; got %r"
                          % out[:800])
            self.assertIn("cwd=%s" % repo, out,
                          "the cycle must be handed the announced tree's root, "
                          "not another checkout (#3413); got %r" % out[:800])
            self.assertIn("arg0=scripts/gate_full_cycle.sh", out,
                          "the cycle must be invoked as the tree-local copy, so "
                          "its BASH_SOURCE derivation names this tree")
            self.assertIn("gate_and_push: tree = %s" % repo, out,
                          "the announced tree must be the one handed over")
            self.assertNotEqual(r.returncode, 0,
                                "a failing cycle must not push")

    def test_gate_and_push_announcement_matches_what_the_cycle_resolves(self):
        """The banner and the cycle's own resolution must agree.

        Two independent derivations — `pwd -P` after gate_and_push's cd, and the
        cycle's BASH_SOURCE walk — that disagree would put two different tree
        names in one log, which is how the original defect stayed invisible.
        """
        with tempfile.TemporaryDirectory() as d:
            repo = _make_repo(os.path.join(d, "agree"), "agree")
            with isolated_markers(dict(GIT_ENV, GATE_SKIP_LOCK="1")) as env:
                push_out = subprocess.run(
                    [str(repo / "scripts" / "gate_and_push.sh"), "--nonsense"],
                    cwd=str(repo), env=env, capture_output=True, text=True,
                    timeout=120)
            cycle_out = _run_cycle(repo, repo)
            m = re.search(r"gate_and_push: tree = (\S+)", push_out.stdout)
            self.assertIsNotNone(m, "no tree banner: %r" % push_out.stdout[:800])
            m2 = re.search(r"repo: (\S+)", cycle_out.stdout)
            self.assertIsNotNone(m2, "no cycle banner: %r" % cycle_out.stdout[:800])
            self.assertEqual(m.group(1), m2.group(1))


class NoGrepPoisoningTest(unittest.TestCase):
    """An observability change is a change (#3010): the new banner must not
    feed any stream a gate decision is made from."""

    # The two log-derived decisions in the gate, and what would break them.
    WAITER_MARKERS = ("GATE PASSED", "PUSHED origin/main now", "NOT pushing")

    def _added_echo_lines(self, path, needle):
        """Every echo/say line in `path` that mentions `needle`."""
        return [ln.strip() for ln in path.read_text().splitlines()
                if needle in ln and not ln.lstrip().startswith("#")
                and re.match(r'\s*(echo|say|printf)\b', ln)]

    def test_gate_sh_never_echoes_the_repo_path(self):
        """THE LOAD-BEARING ONE. gate.sh's stdout is captured to the per-oracle
        log, and the ASan-ran evidence check is `grep -qi 'asan\\|sanitize'` over
        the --full log. A repo path containing "asan" — routine under
        /var/tmp/bezel-scratch/build-asan-* — would satisfy that grep from the
        PATH alone, turning the silent-degradation detector (#3011) into a
        tautology. The identity therefore goes into gate.sh's VERDICT (JSON,
        never grepped) and its BANNER lives in gate_full_cycle.sh, whose stdout
        that grep does not read.
        """
        text = GATE_SH.read_text()
        self.assertIn('GATE_REPO=', text,
                      "guard: with no GATE_REPO in gate.sh at all this check "
                      "would pass vacuously")
        offenders = self._added_echo_lines(GATE_SH, "GATE_REPO")
        self.assertEqual(
            offenders, [],
            "gate.sh must not print the repo path to a stream the ASan "
            "evidence grep reads (#3413/#3011):\n  " + "\n  ".join(offenders))

    def test_the_asan_evidence_grep_still_reads_only_gate_sh_output(self):
        """The banner's safety depends on WHICH file that grep reads. Pin it,
        so moving the banner later cannot silently break the reasoning."""
        text = FULL_CYCLE.read_text()
        self.assertIn("""grep -qi 'asan\\|sanitize' "$LOGDIR/${TAG}_full.log\"""", text)
        self.assertIn('run_until_trustworthy "$LOGDIR/${TAG}_full.log"', text)

    def test_new_banners_do_not_collide_with_the_waiter_terminal_regex(self):
        """gate_wait.sh stops on the first terminal marker it sees. A banner
        printed BEFORE the gate that matched one would make every waiter report
        a verdict instantly — the mirror image of the 59-minute spin the regex
        was widened to fix."""
        for path in (FULL_CYCLE, GATE_AND_PUSH):
            text = path.read_text()
            for marker in self.WAITER_MARKERS:
                for ln in text.splitlines():
                    if not re.match(r'\s*(echo|say|printf)\b', ln.strip()):
                        continue
                    if "TREE UNDER GATE" in ln or "tree = " in ln or "head = " in ln:
                        self.assertNotIn(
                            marker, ln,
                            "%s: identity banner line collides with "
                            "gate_wait.sh's terminal marker %r" % (path.name, marker))

    def test_shard_summary_stream_is_untouched(self):
        """The shard accounting comes from run_parallel.sh, and a previous gate
        telemetry change split a summary line so that a shard reporting 102
        tests contributed zero. Nothing in this change may write into that
        stream: gate.sh's shard phase and run_parallel.sh must carry no repo
        identity output at all."""
        rp = REPO / "tests" / "run_parallel.sh"
        if rp.exists():
            self.assertNotIn("GATE_REPO", rp.read_text(),
                             "the shard runner must stay out of this change")
        gate_text = GATE_SH.read_text()
        self.assertEqual(gate_text.count("GATE_REPO"), 2,
                         "gate.sh should mention GATE_REPO exactly twice — the "
                         "assignment and the verdict argument; anything more is "
                         "output into a stream a decision is made from")


if __name__ == "__main__":
    unittest.main()
