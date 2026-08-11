"""Integration tests for the migrated gate — composition end-to-end (#3010).

The unit tests in this directory (230 of them) pin each migrated module ALONE:
test_coverage.py pins coverage.check_coverage_floor on synthetic manifests;
test_arbitration.py pins arbitrate on crafted logs; test_verdict.py pins the
verdict enum and the atomic file; test_identity.py / test_cold.py pin the k2
identity and the cold/warm truth table; test_summary.py pins parse/merge/account.

This file pins the COMPOSITION — the way the gate threads those modules
TOGETHER on realistic fixtures. A regression in one module's INTERFACE (e.g.
coverage expects a field summary.merge stopped emitting, or cold.evaluate's
marker_changed no longer lines up with identity.compute_identity's output) is
caught HERE, not only at a full gate run. Each case composes at least two
modules across the boundary a real gate threads:

  - coverage-short / missing-binary / full:  summary.merge  ->  coverage floor
    (the ran total flows from merged shard summaries into the floor decision).
  - summary -> coverage (2c->4): the FIELD-NAME / KEY contract across the
    boundary (merge keys == manifest groups; ran == passed+failed).
  - poisoned gate:  verdict_from_rc + is_trustworthy + build_verdict_obj +
    write/read round-trip (the rc==0+undercount distrust, #3001/1.19).
  - arbitration over a realistic mixed shard log: parse_crashed + parse_failed
    + the arbitrate loop + an injected run_suite (0 flake / 1 real / 2
    untrustworthy).
  - identity + cold:  compute_identity + read_marker  ->  cold.evaluate  ->
    ceiling (the marker_changed signal is DERIVED from the identity module,
    not asserted by hand — the load-bearing #2100 composition).
  - decision mapping: the verdict consumer logic (pass=>accept,
    refused/retry/poisoned=>retry, fail=>bail) over all 5 verdicts x
    trustworthy {T,F}, plus the end-to-end 0-disagreement proof against the
    legacy $?/undercount decision over every reachable gate.sh exit.

Stdlib only (unittest + tempfile). NO real compiler/build: fixtures are
in-test temp files and in-memory dicts; runners are injected stubs. per #3002,
#3011, #1861, #2100, #3001, #2918, #3000.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import arbitration as arb   # noqa: E402
from gate import cold                 # noqa: E402
from gate import coverage             # noqa: E402
from gate import identity             # noqa: E402
from gate import summary              # noqa: E402
from gate import verdict              # noqa: E402


# ---------------------------------------------------------------------------
# Fixture helpers (mirror the styles in the per-module unit tests so an
# integration fixture reads the same as its unit-test counterpart).
# ---------------------------------------------------------------------------

def _row(group, path, count):
    """A manifest line: <group>\\t<path>\\t<#sources>. The #sources field is
    incidental to the floor (the count comes from BEZEL_TEST_ENUMERATE); kept so
    the parser exercises the real 3-field shape."""
    return "{}\t{}\t{}".format(group, path, count)


def _write_manifest(dirpath, rows):
    p = os.path.join(dirpath, "test_binaries.txt")
    with open(p, "w") as f:
        for r in rows:
            f.write(r + "\n")
    return p


def _enumerate_runner(counts):
    """An injected coverage runner: {binary path -> enumerate line-count}.

    Returns (rc, stdout_text) where stdout is `n` `suite.test\\n` lines, exactly
    as BEZEL_TEST_ENUMERATE=1 emits (the line count IS the test count)."""
    def run(binary):
        n = counts.get(binary, 0)
        return 0, "".join("Suite.Test{}\n".format(i) for i in range(n))
    return run


def _fail_line(test):
    # The real shard-log [FAIL] shape (run_parallel.sh).
    return "[ FAIL ] {} (0 ms)".format(test)


def _died_line(test):
    # The real shard-log crash-victim shape (run_parallel.sh:320):
    # "##   shard <s> exited nonzero — died on: <lasttest or <none>>".
    return "##   shard codegen.0 exited nonzero — died on: {}".format(test)


class _ArbRecorder:
    """A stub run_suite(suite)->bool that records every call. `result_for` maps
    suite -> bool; a suite absent from the map defaults to True (clean flake)."""

    def __init__(self, result_for=None):
        self.result_for = dict(result_for or {})
        self.calls = []

    def __call__(self, suite):
        self.calls.append(suite)
        return self.result_for.get(suite, True)


def _seed_cache(root, n):
    """Populate `root` with `n` empty files (the bincache entry stand-ins)."""
    os.makedirs(root, exist_ok=True)
    for i in range(n):
        open(os.path.join(root, "entry%d.bin" % i), "wb").close()


def _ran_from_merge(merged):
    """The ran total coverage consumes: grand (passed + failed). Exposed so the
    coverage tests derive `ran` from summary.merge's output shape, not a
    hand-picked literal — this is the 2c->4 contract under test."""
    t = merged["total"]
    return t["passed"] + t["failed"]


def _per_binary_ran_from_merge(merged):
    """The per-binary ran map coverage consumes: {name: passed+failed}. In this
    gate the binary NAME == the manifest GROUP (run_parallel launches by group
    label), so merge's keys must line up with coverage's per_binary_expected."""
    return {name: b["passed"] + b["failed"]
            for name, b in merged["binaries"].items()}


# ---------------------------------------------------------------------------
# 1. Coverage floor: summary.merge  ->  coverage.check_coverage_floor.
# ---------------------------------------------------------------------------

class CoverageFloorCompositionTest(unittest.TestCase):
    """Composition: manifest -> enumerate_expected -> floor_ok, with `ran`
    sourced from summary.merge of fixture shard summaries (the 2c->4 pipeline
    that threads a merged run into the coverage decision). per #3002 / #3011."""

    def test_short_run_is_coverage_short_exit3(self):
        # Manifest of 2 binaries: codegen enumerates 10, sema enumerates 20 ->
        # expected 30. Shard summaries total ran=25 -> coverage-short (exit 3).
        # per #3002: a pass over a truncated run is not a pass, however the
        # missing shards were arbitrated.
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen")
            b2 = os.path.join(d, "sema")
            mp = _write_manifest(d, [_row("codegen", b1, 9),
                                     _row("sema", b2, 7)])
            merged = summary.merge([
                {"binary": "codegen", "passed": 10, "failed": 0},
                {"binary": "sema", "passed": 12, "failed": 3},
            ])
            ran = _ran_from_merge(merged)
            per_binary_ran = _per_binary_ran_from_merge(merged)
            self.assertEqual(ran, 25)             # the fixture's intent
            self.assertEqual(per_binary_ran,
                             {"codegen": 10, "sema": 15})
            v = coverage.check_coverage_floor(
                ran, per_binary_ran, mp,
                runner=_enumerate_runner({b1: 10, b2: 20}),
                is_executable=lambda p: True)
        self.assertFalse(v.ok)
        self.assertEqual(v.verdict, "coverage-short")
        self.assertEqual(v.exit_code, coverage.EXIT_RETRY)   # 3, per #3002
        self.assertEqual(v.expected, 30)
        self.assertIn("25 of 30", v.message)

    def test_missing_binary_is_registry_unusable_not_lowered_bar(self):
        # per #3011: a binary the manifest lists but that is NOT executable is a
        # HARD failure — skipping it would silently LOWER the bar by however
        # many tests it holds (the exact defect #3002 exists to prevent). This
        # holds even if `ran` would otherwise meet the secretly-lowered expected.
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen")
            b2 = os.path.join(d, "missing")
            mp = _write_manifest(d, [_row("codegen", b1, 9),
                                     _row("sema", b2, 7)])
            v = coverage.check_coverage_floor(
                9999, {"codegen": 9999}, mp,
                runner=_enumerate_runner({b1: 10, b2: 20}),
                is_executable=lambda p: p == b1)   # b2 NOT executable
        self.assertFalse(v.ok)
        self.assertEqual(v.verdict, "registry-unusable")
        self.assertEqual(v.exit_code, coverage.EXIT_RETRY)

    def test_full_run_is_ok(self):
        # ran == expected (30 == 30) -> ok. The composition produces a green
        # floor when the merged shard summaries cover the registry.
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen")
            b2 = os.path.join(d, "sema")
            mp = _write_manifest(d, [_row("codegen", b1, 9),
                                     _row("sema", b2, 7)])
            merged = summary.merge([
                {"binary": "codegen", "passed": 10, "failed": 0},
                {"binary": "sema", "passed": 18, "failed": 2},
            ])
            ran = _ran_from_merge(merged)
            per_binary_ran = _per_binary_ran_from_merge(merged)
            self.assertEqual(ran, 30)             # == expected
            v = coverage.check_coverage_floor(
                ran, per_binary_ran, mp,
                runner=_enumerate_runner({b1: 10, b2: 20}),
                is_executable=lambda p: True)
        self.assertTrue(v.ok)
        self.assertEqual(v.verdict, "ok")
        self.assertEqual(v.exit_code, coverage.EXIT_OK)


# ---------------------------------------------------------------------------
# 2. Summary -> coverage: the 2c->4 INTERFACE contract.
# ---------------------------------------------------------------------------

class SummaryToCoverageCompositionTest(unittest.TestCase):
    """The 2c->4 composition's load-bearing contract: summary.merge's output
    SHAPE is exactly what coverage's per_binary_ran/ran inputs consume. Pins
    the field names AND key alignment (merge keys == manifest groups) so a
    rename in summary.merge is caught here, not only at a full gate run."""

    def test_merge_keys_align_with_manifest_groups(self):
        # run_parallel launches by GROUP label, so the shard-summary `binary`
        # field IS the manifest group. coverage keys per_binary_expected by
        # group; merge keys by binary name. The two MUST coincide or the floor
        # silently drops a binary's contribution.
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen")
            b2 = os.path.join(d, "sema")
            mp = _write_manifest(d, [_row("codegen", b1, 9),
                                     _row("sema", b2, 7)])
            merged = summary.merge([
                {"binary": "codegen", "passed": 6, "failed": 0},
                {"binary": "codegen", "passed": 4, "failed": 0},  # 2 shards
                {"binary": "sema", "passed": 20, "failed": 0},
            ])
            # The keys merge emits are exactly the manifest's groups:
            self.assertEqual(set(merged["binaries"]), {"codegen", "sema"})
            ran = _ran_from_merge(merged)
            per_binary_ran = _per_binary_ran_from_merge(merged)
            self.assertEqual(per_binary_ran, {"codegen": 10, "sema": 20})
            v = coverage.check_coverage_floor(
                ran, per_binary_ran, mp,
                runner=_enumerate_runner({b1: 10, b2: 20}),
                is_executable=lambda p: True)
            self.assertTrue(v.ok)
            # The boundary contract: both sides carry the same keys/values.
            self.assertEqual(v.per_binary_ran,
                             {"codegen": 10, "sema": 20})
            self.assertEqual(v.per_binary_expected,
                             {"codegen": 10, "sema": 20})

    def test_superset_run_still_ok(self):
        # per #3002: ran >= expected is ok (== would be too strict — a re-run
        # that picks up a few extra tests is not a defect). The merge->floor
        # pipeline must not reject a superset.
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen")
            mp = _write_manifest(d, [_row("codegen", b1, 9)])
            merged = summary.merge([
                {"binary": "codegen", "passed": 12, "failed": 0}])
            ran = _ran_from_merge(merged)
            v = coverage.check_coverage_floor(
                ran, {"codegen": ran}, mp,
                runner=_enumerate_runner({b1: 10}),
                is_executable=lambda p: True)
        self.assertTrue(v.ok)
        self.assertEqual(v.verdict, "ok")
        self.assertGreater(v.ran, v.expected)   # 12 > 10, still ok


# ---------------------------------------------------------------------------
# 3. Poisoned gate: verdict_from_rc + is_trustworthy + the authoritative file.
# ---------------------------------------------------------------------------

class VerdictPoisonCompositionTest(unittest.TestCase):
    """Composition: verdict_from_rc + is_trustworthy + build_verdict_obj +
    write_verdict + read_verdict — the authoritative-verdict pipeline a
    consumer reads. per #3002 / #3001 (Plan-A 1.19): rc==0 + truncation is
    POISONED and must NOT be accepted as a pass, however the exit code reads."""

    def test_rc0_truncated_is_poisoned_not_accepted(self):
        # rc==0 over a truncated run: the verdict is POISONED. The in-memory
        # composition (verdict_from_rc + is_trustworthy) AND the round-tripped
        # file both agree a consumer must not accept.
        v = verdict.verdict_from_rc(0, truncated=True)
        self.assertEqual(v, verdict.Verdict.POISONED)
        # per #3001/1.19: rc==0+undercount distrust — the verdict IS the signal.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            obj = verdict.build_verdict_obj(rc=0, truncated=True,
                                            undercount_banner=True)
            verdict.write_verdict(p, obj)
            got = verdict.read_verdict(p)
            self.assertEqual(got["verdict"], "poisoned")
            # The consumer accept predicate (pass && trustworthy) is False:
            self.assertFalse(got["verdict"] == "pass"
                             and got["trustworthy"])

    def test_rc0_clean_is_pass_trusted(self):
        # rc==0, no truncation: PASS and a consumer reading the file accepts.
        v = verdict.verdict_from_rc(0, truncated=False)
        self.assertEqual(v, verdict.Verdict.PASS)
        self.assertTrue(verdict.is_trustworthy(v, False))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            obj = verdict.build_verdict_obj(rc=0, truncated=False)
            verdict.write_verdict(p, obj)
            got = verdict.read_verdict(p)
            self.assertEqual(got["verdict"], "pass")
            self.assertTrue(got["trustworthy"])
            self.assertTrue(got["verdict"] == "pass" and got["trustworthy"])

    def test_truncation_moves_rc0_from_pass_to_poisoned(self):
        # The precise #3001 composition: the SAME rc (0) yields DIFFERENT
        # verdicts depending on the truncation signal. The distrust is carried
        # by the verdict enum (not by trustworthy-on-pass alone), so a consumer
        # checking the verdict field gets the right answer either way.
        clean = verdict.verdict_from_rc(0, truncated=False)
        poisoned = verdict.verdict_from_rc(0, truncated=True)
        self.assertNotEqual(clean, poisoned)
        self.assertEqual(clean, verdict.Verdict.PASS)
        self.assertEqual(poisoned, verdict.Verdict.POISONED)


# ---------------------------------------------------------------------------
# 4. Arbitration over a realistic mixed shard log.
# ---------------------------------------------------------------------------

class ArbitrationCompositionTest(unittest.TestCase):
    """Composition over a realistic mixed shard log: parse_crashed +
    parse_failed + the arbitrate loop + an injected run_suite. per #1861 /
    #2100.

    The unit tests pin each parser and each invariant alone; this pins them
    working TOGETHER on a log that mixes a [FAIL] (real evidence), a real crash
    victim, and a <none> (a shard killed at startup) — the three-way shape the
    gate actually arbitrates, returning 0 flake / 1 real / 2 untrustworthy."""

    def _mixed_log(self):
        # [FAIL] aaaa (real evidence) + died-on bbbb (timeout victim) +
        # died-on <none> (a shard killed before running ANY test).
        return (_fail_line("aaaa.Something") + "\n" +
                _died_line("bbbb.Other") + "\n" +
                _died_line("<none>") + "\n")

    def test_fail_arbitrated_in_full_crash_sampled_none_stripped_flaky(self):
        # The mixed log, all suspects flaky on re-run -> verdict 0 (FLAKY).
        # per #2100 invariant 2: the [FAIL] is arbitrated IN FULL; only the
        # crash victim is subject to the sample cap; <none> is STRIPPED.
        res = arb.arbitrate(self._mixed_log(), _ArbRecorder(),
                            sample_cap=8)
        self.assertEqual(res.verdict, arb.ARB_FLAKY)        # 0
        self.assertEqual(res.fail_suites, ["aaaa"])         # arbitrated IN FULL
        self.assertEqual(res.crash_suites, ["bbbb"])        # sampled (cap not hit)
        self.assertNotIn("<none>", res.crash_suites)        # stripped, per #2100
        self.assertEqual(res.sampled, ["aaaa", "bbbb"])
        self.assertEqual(res.last_suspects, 2)              # <none> excluded
        self.assertTrue(any("FLAKY: aaaa" in m for m in res.messages))
        self.assertTrue(any("FLAKY: bbbb" in m for m in res.messages))

    def test_fail_that_never_passes_alone_is_real(self):
        # Same mixed log, but aaaa fails all 5 re-tries -> verdict 1 (REAL).
        # per #1861: a suspect that fails alone all 5 tries is a real regression.
        res = arb.arbitrate(self._mixed_log(),
                            _ArbRecorder({"aaaa": False}), sample_cap=8)
        self.assertEqual(res.verdict, arb.ARB_REAL)         # 1
        self.assertTrue(any("REAL: aaaa" in m for m in res.messages))

    def test_only_none_victims_is_untrustworthy_not_laundered(self):
        # A whole slice dropped at startup -> only `died on: <none>`. After
        # stripping there are NO arbitrable suspects -> verdict 2
        # (UNTRUSTWORTHY), NEVER 0 (a pass). per #2100 invariant 3/4: re-running
        # "<none>" matches nothing and would launder a dropped slice into a
        # flake-ACCEPT; stripping degrades to return 2 the caller distrusts.
        res = arb.arbitrate(_died_line("<none>") + "\n",
                            _ArbRecorder(), sample_cap=8)
        self.assertEqual(res.verdict, arb.ARB_UNTRUSTWORTHY)   # 2
        self.assertEqual(res.sampled, [])


# ---------------------------------------------------------------------------
# 5. Identity + cold: compute_identity -> marker comparison -> cold -> ceiling.
# ---------------------------------------------------------------------------

class IdentityColdCompositionTest(unittest.TestCase):
    """Composition: identity.compute_identity + identity.read_marker determine
    the marker_changed signal, which (with the cache count) drives
    cold.evaluate's cold/warm verdict and the ceiling. per #2100 / #2918 / #3000.

    The unit tests pin compute_identity and is_cold separately; this pins the
    PIPELINE: the marker_changed boolean is DERIVED from the identity module
    (compute_identity vs read_marker), not asserted by hand — exactly how
    gate.sh:622-625 threads them."""

    def test_unchanged_identity_and_full_cache_predicts_warm(self):
        # The only WARM case: archives unchanged, marker matches the computed
        # identity, and the cache is above the near-empty threshold. per #2100.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "libbezel_core.a")
            rt = os.path.join(d, "libbezel_runtime.a")
            with open(core, "wb") as f:
                f.write(b"core-v1")
            with open(rt, "wb") as f:
                f.write(b"runtime-v1")
            marker = os.path.join(d, identity.MARKER_PLAIN)
            cache = os.path.join(d, "bincache")
            _seed_cache(cache, 5000)
            # A completed previous pass recorded the identity (gate.sh:986-990).
            id_then = identity.compute_identity(core, rt)
            identity.write_marker(marker, id_then)
            # Next gate: recompute and derive marker_changed from the marker.
            id_now = identity.compute_identity(core, rt)
            marker_recorded = identity.read_marker(marker)
            marker_changed = (marker_recorded != id_now)
            identity_changed = False   # core.a not rebuilt this gate
            self.assertFalse(marker_changed)
            r = cold.evaluate(identity_changed, cache, marker_changed)
            self.assertIs(r.cold, False)
            self.assertEqual(r.cache_n, 5000)
            self.assertEqual(r.ceiling, cold.CEILING_WARM)    # 900, per #2918

    def test_identity_changed_and_near_empty_cache_predicts_cold(self):
        # core.a rebuilt this gate (identity_changed) + a near-empty store ->
        # cold, ceiling 1800. Either signal alone forces cold; both present
        # here. per #2100 signal 1 + signal 2.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "libbezel_core.a")
            rt = os.path.join(d, "libbezel_runtime.a")
            with open(core, "wb") as f:
                f.write(b"core-v1")
            with open(rt, "wb") as f:
                f.write(b"runtime-v1")
            cache = os.path.join(d, "bincache")
            _seed_cache(cache, 100)
            r = cold.evaluate(True, cache, False)   # identity_changed=True
            self.assertIs(r.cold, True)
            self.assertEqual(r.cache_n, 100)
            self.assertEqual(r.ceiling, cold.CEILING_COLD)    # 1800

    def test_stale_marker_forces_cold_even_with_full_cache(self):
        # per #2100 — THE load-bearing case. A timed-out previous gate leaves a
        # full-looking store under a STALE identity. The count says warm and
        # the mtime (identity_changed) signal may be quiet; only the marker
        # (compute_identity != recorded) forces cold. This is the 2026-07-29
        # miss gate.sh:602-614 fixed — a runtime-only change that moved the
        # identity but not core.a's mtime.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "libbezel_core.a")
            rt = os.path.join(d, "libbezel_runtime.a")
            with open(core, "wb") as f:
                f.write(b"core-v1")
            with open(rt, "wb") as f:
                f.write(b"runtime-v1")
            marker = os.path.join(d, identity.MARKER_PLAIN)
            cache = os.path.join(d, "bincache")
            _seed_cache(cache, 5000)
            # Previous pass recorded id_then.
            id_then = identity.compute_identity(core, rt)
            identity.write_marker(marker, id_then)
            # Runtime-only change (core.a untouched on disk); marker NOT
            # refreshed (the previous gate timed out before writing it).
            with open(rt, "wb") as f:
                f.write(b"runtime-v2-different")
            id_now = identity.compute_identity(core, rt)
            self.assertNotEqual(id_now, id_then)
            marker_recorded = identity.read_marker(marker)
            marker_changed = (marker_recorded != id_now)
            self.assertTrue(marker_changed)     # the derived signal fires
            # Count says warm (5000 > 2000) and identity_changed is quiet, but
            # marker_changed forces cold:
            r = cold.evaluate(False, cache, marker_changed)
            self.assertIs(r.cold, True)
            self.assertEqual(r.ceiling, cold.CEILING_COLD)


# ---------------------------------------------------------------------------
# 6. Decision mapping: the verdict consumer logic over 5 verdicts x trustworthy.
# ---------------------------------------------------------------------------

class ConsumerDecisionMappingTest(unittest.TestCase):
    """The verdict consumer logic (gate_full_cycle run_until_trustworthy, Phase
    3c): pass+trustworthy => accept; refused/retry/poisoned => retry; fail =>
    bail. per #3002. Exhaustive over all 5 verdicts x trustworthy {T,F}.

    Plus the end-to-end proof: every REACHABLE gate.sh exit (verdict_from_rc
    over the exit taxonomy x truncation) round-trips through write_verdict ->
    read_verdict, and the consumer's decision agrees with the legacy
    $?/undercount decision (the Phase 3c 0-disagreement proof)."""

    @staticmethod
    def _consumer(verdict_value, trustworthy):
        """The run_until_trustworthy decision, mirrored from verdict.py's
        Phase 3c contract. per #3002: accept ONLY on pass+trustworthy; a
        non-trustworthy pass (the #3002 shape) is retried, never accepted."""
        if verdict_value == "pass" and trustworthy:
            return "accept"
        if verdict_value == "fail":
            return "bail"
        return "retry"

    def test_decision_table_all_five_verdicts_by_trustworthy(self):
        # The contract: 5 verdicts x trustworthy {T,F}. The load-bearing cell
        # is (pass, False) -> retry: a pass the writer somehow let through over
        # a truncated run is NEVER accepted (defense-in-depth on top of the
        # POISONED verdict, which is how rc0+truncated is normally represented).
        expected = {
            ("pass", True): "accept",
            ("pass", False): "retry",   # per #3002 — never accept an untrusted pass
            ("fail", True): "bail",
            ("fail", False): "bail",
            ("refused", True): "retry",
            ("refused", False): "retry",
            ("retry", True): "retry",
            ("retry", False): "retry",
            ("poisoned", True): "retry",
            ("poisoned", False): "retry",
        }
        for v in verdict.Verdict:
            for tw in (True, False):
                with self.subTest(verdict=v.value, trustworthy=tw):
                    self.assertEqual(
                        self._consumer(v.value, tw), expected[(v.value, tw)])

    def test_reachable_states_round_trip_and_match_legacy_decision(self):
        # Every reachable gate.sh exit (rc in {0,1,2,3}) x truncation round-trips
        # through the authoritative file, and the consumer agrees with the
        # legacy $?/undercount decision. per #3002 Phase 3c (the
        # 0-disagreement proof over the reachable states).
        def legacy(rc, undercount):
            # Faithful copy of gate_full_cycle's original run_until_trustworthy.
            if rc == 2:
                return "retry"
            if rc == 3:
                return "retry"
            if rc != 0:
                return "bail"
            return "retry" if undercount else "accept"

        reachable = [(rc, trunc)
                     for rc in (0, 1, 2, 3)
                     for trunc in (False, True)]
        for rc, trunc in reachable:
            with self.subTest(rc=rc, truncated=trunc):
                with tempfile.TemporaryDirectory() as d:
                    p = os.path.join(d, "gv.json")
                    obj = verdict.build_verdict_obj(
                        rc=rc, truncated=trunc, undercount_banner=trunc)
                    verdict.write_verdict(p, obj)
                    got = verdict.read_verdict(p)
                    action = self._consumer(got["verdict"], got["trustworthy"])
                self.assertEqual(action, legacy(rc, trunc),
                    "rc=%d trunc=%s: consumer=%s legacy=%s"
                    % (rc, trunc, action, legacy(rc, trunc)))
                # The reachable states NEVER produce pass+untrustworthy: the
                # writer maps rc0+truncated -> poisoned. Pin that invariant —
                # it is why the (pass, False) cell is defense-in-depth, not a
                # state the file ever carries.
                if got["verdict"] == verdict.Verdict.PASS.value:
                    self.assertTrue(got["trustworthy"])


if __name__ == "__main__":
    unittest.main()
