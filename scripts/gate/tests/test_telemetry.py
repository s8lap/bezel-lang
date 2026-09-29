"""Tests for gate.telemetry (Wave 3, #3010 — the schema:2 record builder).

The load-bearing pins the migration requires:
  1. SCHEMA:2 FIELD SET + ORDER — gate.trend R1-R5 parse these records; a
     renamed/dropped/reordered field reintroduces the #3010 defect class. The
     exact 29-key top-level order + the phase/aot/binary sub-object orders are
     pinned against the bash printf (gate.sh:354).
  2. THE NULL-VS-0 DISTINCTION (#3010) — an UNMEASURED number emits `null`,
     never `0`; a MEASURED zero emits `0`. Each field's rule is pinned
     individually (gtests null on 0; cache_n 0 on 0; cold/floor null on unset).
  3. BYTE-IDENTITY with the bash emit_json — compact separators (no spaces),
     fixed-decimal durations (5.00 not 5.0 via RawNumber), bare numbers.
  4. The Telemetry accumulator (replaces gate.sh's PH_* parallel arrays).
  5. The CLI emit subcommand (the gate.sh bridge).

The dual-run equivalence proof (verbatim bash emit_json vs this module, 8 cases
covering every null-vs-0 branch, zero raw-byte diff) lives in the commit
message; these unit tests pin the invariants individually so a future edit
cannot silently drift.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import telemetry as T  # noqa: E402


# The EXACT top-level field order from the bash printf (gate.sh:354). Load-
# bearing: the dual-run diffs raw bytes, so order must match. Do not reorder.
TOP_LEVEL_FIELDS = [
    "schema", "ts", "epoch", "host", "mode", "head", "head_short", "dirty",
    "result", "failed_phase", "total_s", "gtests", "littests",
    "expected_gtests", "coverage_floor_enforced", "coverage_floor_satisfied",
    "cold", "cache_n",
    "shard_timeout_s", "aot", "binaries", "shard_count", "shard_reported_s",
    "arbitration_suspects", "serial_attempts", "serial_total",
    "asan_shards_reported", "asan_errors", "shard_times", "mem", "phases",
]

PHASE_FIELDS = ["name", "s", "rc", "status", "note"]
AOT_FIELDS = ["hits", "misses", "store_failures"]
MEM_FIELDS = ["anon_bytes", "current_bytes", "samples"]
BINARY_FIELDS = ["ran", "expected"]


def _basic_record(**overrides):
    """A record with every field measured (no nulls), for schema pinning."""
    base = dict(
        ts="2026-08-09T01:02:03Z", epoch="1723170000", host="box",
        mode="--full", head="abc123", head_short="abc1", dirty="true",
        result="pass", failed_phase="", total_s="123.45",
        phases=[T.Phase("configure", T.RawNumber("5.00"), 0, "ran", "")],
        shard_times="12.3,45.67",
        cov_gtests="16482", cov_littests="2103", expected_gtests="16482",
        coverage_floor_ok="1", cold="0", cache_n="102299", shard_timeout="900",
        aot_hits="5000", aot_misses="200", aot_store_fails="3",
        bin_expected="codegen:3000", bin_ran="codegen:3000",
        shard_count="16", shard_reported_s="358",
        mem_anon_max="6442450944", mem_current_max="12884901888",
        mem_samples="700",
    )
    base.update(overrides)
    return T.build_record(**base)


class SchemaFieldOrderTest(unittest.TestCase):
    """Pin 1: the schema:2 field set + order (gate.sh:354 / gate.trend R1-R5)."""

    def test_top_level_field_set_matches_bash_printf(self):
        rec = _basic_record()
        self.assertEqual(list(rec.keys()), TOP_LEVEL_FIELDS)

    def test_top_level_field_count_is_31(self):
        # 31 keys — adding or dropping one is a schema break the dual-run
        # would catch, but this count is a cheap first-line defense.
        # 29 -> 30 on 2026-08-21: `mem` (gate audit F1 Tier-0, the shard
        # phase's anon high-water mark) joined the record.
        # 30 -> 31, fix #3737: `coverage_floor_enforced` used to conflate two
        # concepts — "the floor check ran" and "the floor's ran>=expected
        # comparison passed" — in one bool, which made gate.trend's R5 fire on
        # a record whose floor genuinely ran and correctly reported
        # result=fail (misread as "ran WITHOUT the floor"). Split into
        # `coverage_floor_enforced` (now presence-based: was the check
        # invoked at all) + the new `coverage_floor_satisfied` (the old
        # value-based semantics: did the comparison pass).
        rec = _basic_record()
        self.assertEqual(len(rec), 31)

    def test_mem_sub_object_field_order(self):
        rec = _basic_record()
        self.assertEqual(list(rec["mem"].keys()), MEM_FIELDS)

    def test_mem_is_null_when_unmeasured(self):
        # per #3010's rule: AN UNMEASURED NUMBER EMITS null, NEVER 0. The
        # sampler is best-effort, so "no measurement" must be distinguishable
        # from "the slice really used 0 bytes".
        rec = _basic_record(mem_anon_max=None, mem_current_max=None,
                            mem_samples=None)
        self.assertIsNone(rec["mem"])

    def test_mem_anon_survives_when_only_anon_measured(self):
        rec = _basic_record(mem_anon_max="123", mem_current_max=None,
                            mem_samples=None)
        self.assertEqual(rec["mem"]["anon_bytes"], 123)
        self.assertIsNone(rec["mem"]["current_bytes"])

    def test_schema_is_always_2(self):
        self.assertEqual(_basic_record()["schema"], 2)
        self.assertEqual(T.build_record(
            ts="x", epoch="0", host="h", mode="default", head="", head_short="",
            dirty="false", result="fail", failed_phase="", total_s="0.00",
            phases=[], shard_times="")["schema"], 2)

    def test_phase_sub_object_field_order(self):
        rec = _basic_record()
        self.assertEqual(list(rec["phases"][0].keys()), PHASE_FIELDS)

    def test_aot_sub_object_field_order(self):
        rec = _basic_record()
        self.assertEqual(list(rec["aot"].keys()), AOT_FIELDS)

    def test_binary_sub_object_field_order(self):
        rec = _basic_record()
        name = list(rec["binaries"].keys())[0]
        self.assertEqual(list(rec["binaries"][name].keys()), BINARY_FIELDS)


class NullVsZeroTest(unittest.TestCase):
    """Pin 2: the #3010 null-vs-0 distinction, per field (gate.sh:317-329).

    The whole point: an UNMEASURED number emits `null`, never `0`, so a failed
    harvest and a genuine zero are distinguishable. Each field's rule below is
    lifted verbatim from the bash conditionals.
    """

    def _bare(self, **kw):
        defaults = dict(ts="x", epoch="0", host="h", mode="default",
                        head="", head_short="", dirty="false", result="pass",
                        failed_phase="", total_s="0.00", phases=[],
                        shard_times="")
        defaults.update(kw)
        return T.build_record(**defaults)

    def test_gtests_null_when_unmeasured(self):
        # per gate.sh:323: COV_GTESTS unset => null (NOT 0).
        self.assertIsNone(self._bare()["gtests"])
        self.assertIsNone(self._bare(cov_gtests=None)["gtests"])
        self.assertIsNone(self._bare(cov_gtests="")["gtests"])

    def test_gtests_null_when_zero(self):
        # per gate.sh:323: `[ "${COV_GTESTS:-0}" -gt 0 ]` — 0 is NOT > 0, so a
        # measured-but-zero harvest emits null. This is the EXACT rule that
        # 26 false-pass records violated.
        self.assertIsNone(self._bare(cov_gtests="0")["gtests"])

    def test_gtests_emits_positive_int(self):
        self.assertEqual(self._bare(cov_gtests="16482")["gtests"], 16482)

    def test_littests_same_rule_as_gtests(self):
        self.assertIsNone(self._bare()["littests"])
        self.assertIsNone(self._bare(cov_littests="0")["littests"])
        self.assertEqual(self._bare(cov_littests="2103")["littests"], 2103)

    def test_expected_gtests_null_when_zero_or_unset(self):
        # per gate.sh:325: same >0 rule.
        self.assertIsNone(self._bare()["expected_gtests"])
        self.assertIsNone(self._bare(expected_gtests="0")["expected_gtests"])
        self.assertEqual(
            self._bare(expected_gtests="16482")["expected_gtests"], 16482)

    def test_cache_n_ZERO_IS_NOT_NULL(self):
        # per gate.sh:326: `[ -n "${cache_n:-}" ] && j_cachen="$cache_n"` — a
        # measured 0 (an empty bincache, the #3000 shape) emits 0, distinct
        # from null (never measured). This is the critical distinction.
        self.assertEqual(self._bare(cache_n="0")["cache_n"], 0)
        self.assertIsNone(self._bare()["cache_n"])
        self.assertIsNone(self._bare(cache_n=None)["cache_n"])
        self.assertEqual(self._bare(cache_n="102299")["cache_n"], 102299)

    def test_shard_timeout_zero_is_not_null(self):
        # per gate.sh:327: same -n rule as cache_n.
        self.assertEqual(self._bare(shard_timeout="0")["shard_timeout_s"], 0)
        self.assertIsNone(self._bare()["shard_timeout_s"])

    def test_cold_null_when_unset(self):
        # per gate.sh:328: unset => null; set => bool(int==1).
        self.assertIsNone(self._bare()["cold"])
        self.assertIsNone(self._bare(cold=None)["cold"])

    def test_cold_true_when_one(self):
        self.assertIs(self._bare(cold="1")["cold"], True)

    def test_cold_false_when_zero(self):
        self.assertIs(self._bare(cold="0")["cold"], False)

    def test_coverage_floor_enforced_null_when_unset(self):
        # per gate.sh:329: unset => null (the check code path never ran).
        self.assertIsNone(self._bare()["coverage_floor_enforced"])

    def test_coverage_floor_enforced_true_whenever_the_flag_is_passed(self):
        # fix #3737: coverage_floor_enforced is now PRESENCE-based — True iff
        # gate.sh actually invoked the floor check, regardless of whether the
        # comparison itself passed. This is the field gate.trend R5 now reads
        # (a `result=pass` record with this NOT True means the floor was
        # skipped, the real "coverage-floor-disabled" signature).
        self.assertIs(
            self._bare(coverage_floor_ok="1")["coverage_floor_enforced"], True)
        self.assertIs(
            self._bare(coverage_floor_ok="0")["coverage_floor_enforced"], True)

    def test_coverage_floor_satisfied_null_when_unset(self):
        # fix #3737: the new field carries the OLD value-based semantics
        # (did the ran>=expected comparison pass) that coverage_floor_enforced
        # used to conflate with "did the check run at all".
        self.assertIsNone(self._bare()["coverage_floor_satisfied"])

    def test_coverage_floor_satisfied_true_when_one(self):
        self.assertIs(
            self._bare(coverage_floor_ok="1")["coverage_floor_satisfied"], True)

    def test_coverage_floor_satisfied_false_when_zero(self):
        # fix #3737: this is the 466a28e2 record-1185 shape — the floor
        # genuinely ran and found a real shortfall. Before the fix this value
        # (then named coverage_floor_enforced) made gate.trend's R5 fire with
        # the misleading "the gate ran without the #3002 floor" message on a
        # record whose `result` was already correctly "fail". R5 is now scoped
        # to result=pass and fires on enforced-not-True OR satisfied-False.
        self.assertIs(
            self._bare(coverage_floor_ok="0")["coverage_floor_satisfied"], False)

    def test_failed_phase_null_when_empty(self):
        self.assertIsNone(self._bare()["failed_phase"])
        self.assertEqual(
            self._bare(failed_phase="build")["failed_phase"], "build")

    def test_aot_null_when_both_hits_and_misses_absent(self):
        # per gate.sh:330-331: emitted only when at least one is non-empty.
        self.assertIsNone(self._bare()["aot"])
        self.assertIsNone(
            self._bare(aot_hits="", aot_misses="")["aot"])

    def test_aot_emits_when_only_misses_present(self):
        aot = self._bare(aot_hits=None, aot_misses="5")["aot"]
        self.assertEqual(aot["misses"], 5)
        self.assertIsNone(aot["hits"])  # hits absent => null (not 0)

    def test_aot_store_failures_null_when_absent(self):
        aot = self._bare(aot_hits="10", aot_misses="5")["aot"]
        self.assertIsNone(aot["store_failures"])

    def test_aot_store_failures_present(self):
        aot = self._bare(aot_hits="10", aot_misses="5",
                         aot_store_fails="2")["aot"]
        self.assertEqual(aot["store_failures"], 2)

    def test_binaries_null_when_both_maps_empty(self):
        self.assertIsNone(self._bare()["binaries"])

    def test_binaries_expected_only_ran_defaults_zero(self):
        # per gate.sh:347: a binary EXPECTED but never REPORTED appears with
        # ran=0 — the R2 signature ("a whole area contributed nothing").
        b = self._bare(bin_expected="codegen:3000")["binaries"]
        self.assertEqual(b["codegen"]["expected"], 3000)
        self.assertEqual(b["codegen"]["ran"], 0)

    def test_binaries_ran_only_expected_defaults_null(self):
        # per gate.sh:347: expected absent => null (not 0).
        b = self._bare(bin_ran="codegen:3000")["binaries"]
        self.assertEqual(b["codegen"]["ran"], 3000)
        self.assertIsNone(b["codegen"]["expected"])

    def test_shard_times_empty_list_when_unset(self):
        # gate.sh emits `"shard_times":[]` when SHARD_TIMES is empty.
        self.assertEqual(self._bare()["shard_times"], [])

    def test_always_numeric_fields_default_zero_never_null(self):
        # shard_count / shard_reported_s / asan_errors etc. are ${VAR:-0} in
        # bash — always numbers, never null, even when unset.
        rec = self._bare()
        for f in ["shard_count", "shard_reported_s", "arbitration_suspects",
                  "serial_attempts", "serial_total", "asan_shards_reported",
                  "asan_errors"]:
            self.assertEqual(rec[f], 0, "%s should be 0 not None" % f)


class SerializeByteIdentityTest(unittest.TestCase):
    """Pin 3: serialize() produces compact, byte-identical-to-bash JSON.

    The bash printf (gate.sh:354) emits compact JSON (no spaces) with bare
    numbers. json.dumps defaults would add spaces (`, `/`: `) and render floats
    with repr (5.0). This pins that serialize() matches the bash exactly.
    """

    def test_no_spaces_after_colon_or_comma(self):
        line = T.serialize(_basic_record())
        self.assertNotIn(", ", line)
        self.assertNotIn(": ", line)

    def test_duration_emits_two_decimals(self):
        # The RawNumber technique: 5.00 survives (not 5.0).
        rec = T.build_record(
            ts="x", epoch="0", host="h", mode="default", head="", head_short="",
            dirty="false", result="pass", failed_phase="", total_s="123.45",
            phases=[T.Phase("p", T.RawNumber("5.00"), 0, "ran", "")],
            shard_times="")
        line = T.serialize(rec)
        self.assertIn('"total_s":123.45', line)
        self.assertIn('"s":5.00', line)
        self.assertNotIn('"s":5.0,', line)

    def test_shard_times_not_reformatted(self):
        # shard_times are raw float strings (not %.2f); 12.3 stays 12.3.
        rec = T.build_record(
            ts="x", epoch="0", host="h", mode="default", head="", head_short="",
            dirty="false", result="pass", failed_phase="", total_s="0.00",
            phases=[], shard_times="12.3,8,45.67")
        line = T.serialize(rec)
        self.assertIn('"shard_times":[12.3,8,45.67]', line)

    def test_null_emits_as_json_null(self):
        line = T.serialize(self._bare())
        self.assertIn('"gtests":null', line)
        self.assertIn('"aot":null', line)
        self.assertIn('"binaries":null', line)

    def test_bool_emits_as_json_bool(self):
        line = T.serialize(_basic_record(dirty="true", cold="0",
                                          coverage_floor_ok="1"))
        self.assertIn('"dirty":true', line)
        self.assertIn('"cold":false', line)
        self.assertIn('"coverage_floor_enforced":true', line)
        self.assertIn('"coverage_floor_satisfied":true', line)

    def test_round_trips_through_json_loads(self):
        # The emitted line is valid JSON that parses back to the same values.
        rec = _basic_record()
        line = T.serialize(rec)
        parsed = json.loads(line)
        self.assertEqual(parsed["schema"], 2)
        self.assertEqual(parsed["gtests"], 16482)
        self.assertEqual(parsed["cache_n"], 102299)
        self.assertEqual(parsed["total_s"], 123.45)
        self.assertEqual(parsed["phases"][0]["s"], 5.0)  # parsed as float
        self.assertEqual(parsed["aot"]["store_failures"], 3)

    def test_empty_phases_and_shard_times(self):
        rec = T.build_record(
            ts="x", epoch="0", host="h", mode="default", head="", head_short="",
            dirty="false", result="fail", failed_phase="build", total_s="0.00",
            phases=[], shard_times="")
        line = T.serialize(rec)
        self.assertIn('"shard_times":[]', line)
        self.assertIn('"phases":[]', line)

    def test_rawnumber_repr_for_debugging(self):
        # Not load-bearing for serialize, but useful for debugging.
        self.assertIn("5.00", repr(T.RawNumber("5.00")))

    def _bare(self, **kw):
        d = dict(ts="x", epoch="0", host="h", mode="default", head="",
                 head_short="", dirty="false", result="pass", failed_phase="",
                 total_s="0.00", phases=[], shard_times="")
        d.update(kw)
        return T.build_record(**d)


class TelemetryAccumulatorTest(unittest.TestCase):
    """Pin 4: the Telemetry accumulator replaces gate.sh's PH_* arrays."""

    def test_ran_records_a_ran_phase(self):
        tel = T.Telemetry()
        tel.ran("configure", 5.0, 0)
        p = tel.phases[0]
        self.assertEqual(p.name, "configure")
        self.assertEqual(p.s.text, "5.00")  # %.2f formatted
        self.assertEqual(p.rc, 0)
        self.assertEqual(p.status, "ran")
        self.assertEqual(p.note, "")

    def test_ran_formats_to_two_decimals(self):
        tel = T.Telemetry()
        tel.ran("build", 180.567, 0)  # would be 180.57
        self.assertEqual(tel.phases[0].s.text, "180.57")

    def test_skip_records_a_skipped_phase(self):
        tel = T.Telemetry()
        tel.skip("lit", "not configured")
        p = tel.phases[0]
        self.assertEqual(p.s.text, "0.00")
        self.assertEqual(p.status, "skipped")
        self.assertEqual(p.rc, 0)
        self.assertEqual(p.note, "not configured")

    def test_accumulates_multiple_phases_in_order(self):
        tel = T.Telemetry()
        tel.ran("configure", 5.0, 0)
        tel.ran("build", 180.0, 0)
        tel.skip("lit", "absent")
        names = [p.name for p in tel.phases]
        self.assertEqual(names, ["configure", "build", "lit"])

    def test_phases_property_returns_copy(self):
        tel = T.Telemetry()
        tel.ran("p", 1.0, 0)
        phases = tel.phases
        phases.append("x")  # mutating the copy must not affect the accumulator
        self.assertEqual(len(tel.phases), 1)


class BuildBinariesTest(unittest.TestCase):
    """Direct tests for the awk-merge replacement (gate.sh:336-353)."""

    def test_both_maps_populated(self):
        b = T._build_binaries("codegen:3000 sema:11482",
                              "codegen:3000 sema:11482")
        self.assertEqual(b["codegen"], {"ran": 3000, "expected": 3000})
        self.assertEqual(b["sema"], {"ran": 11482, "expected": 11482})

    def test_keys_are_sorted(self):
        # gawk hash order is non-deterministic; Python emits sorted (cleaner).
        b = T._build_binaries("sema:1 codegen:2 ownership:3", "")
        self.assertEqual(list(b.keys()), ["codegen", "ownership", "sema"])

    def test_expected_only_ran_zero(self):
        b = T._build_binaries("codegen:3000", "")
        self.assertEqual(b["codegen"]["expected"], 3000)
        self.assertEqual(b["codegen"]["ran"], 0)

    def test_ran_only_expected_null(self):
        b = T._build_binaries("", "codegen:3000")
        self.assertEqual(b["codegen"]["ran"], 3000)
        self.assertIsNone(b["codegen"]["expected"])

    def test_both_empty_returns_none(self):
        self.assertIsNone(T._build_binaries("", ""))
        self.assertIsNone(T._build_binaries(None, None))

    def test_trailing_space_handled(self):
        # gate.sh:806 adds a trailing space to GATE_BIN_RAN; must not create
        # a phantom empty-name entry.
        b = T._build_binaries("", "codegen:3000 ")
        self.assertEqual(list(b.keys()), ["codegen"])

    def test_empty_output_returns_none(self):
        # If parsing yields no real entries, return None (not {}).
        self.assertIsNone(T._build_binaries(" :", " :"))


class CLITest(unittest.TestCase):
    """Pin 5: `python3 -m gate.telemetry emit ...` — the gate.sh bridge."""

    _SCRIPTS_DIR = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))

    def _run_cli(self, args, env=None):
        e = dict(os.environ, PYTHONPATH=self._SCRIPTS_DIR)
        if env:
            e.update(env)
        return subprocess.run(
            [sys.executable, "-m", "gate.telemetry"] + args,
            capture_output=True, text=True, env=e)

    def _write_tsv(self, path, phases):
        with open(path, "w") as f:
            for name, dur, rc, status, note in phases:
                f.write("%s\t%s\t%s\t%s\t%s\n" % (name, dur, rc, status, note))

    def test_emit_appends_to_tlog(self):
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "phases.tsv")
            tlog = os.path.join(d, "t.jsonl")
            self._write_tsv(tsv, [("configure", "5.00", "0", "ran", "")])
            r = self._run_cli([
                "emit", "--phases-file", tsv, "--tlog", tlog,
                "--result", "pass", "--total-s", "5.00",
                "--ts", "T", "--host", "H", "--mode", "default",
                "--cov-gtests", "100",
            ])
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(tlog) as f:
                line = f.read().strip()
            rec = json.loads(line)
            self.assertEqual(rec["result"], "pass")
            self.assertEqual(rec["gtests"], 100)
            self.assertEqual(rec["phases"][0]["name"], "configure")

    def test_emit_echoes_line_on_stdout(self):
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "p.tsv")
            open(tsv, "w").close()
            tlog = os.path.join(d, "t.jsonl")
            r = self._run_cli([
                "emit", "--phases-file", tsv, "--tlog", tlog,
                "--result", "fail", "--total-s", "0.00"])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(r.stdout.strip().startswith('{"schema":2,'))
            json.loads(r.stdout.strip())  # valid JSON

    def test_emit_unmeasured_fields_are_null(self):
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "p.tsv")
            open(tsv, "w").close()
            tlog = os.path.join(d, "t.jsonl")
            r = self._run_cli([
                "emit", "--phases-file", tsv, "--tlog", tlog,
                "--result", "fail", "--total-s", "0.00",
                "--failed-phase", "build"])
            rec = json.loads(r.stdout.strip())
            self.assertIsNone(rec["gtests"])
            self.assertIsNone(rec["cache_n"])
            self.assertIsNone(rec["cold"])
            self.assertEqual(rec["failed_phase"], "build")

    def test_emit_measured_zero_cache_n_is_zero_not_null(self):
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "p.tsv")
            open(tsv, "w").close()
            tlog = os.path.join(d, "t.jsonl")
            r = self._run_cli([
                "emit", "--phases-file", tsv, "--tlog", tlog,
                "--result", "pass", "--total-s", "1.00",
                "--cache-n", "0"])
            rec = json.loads(r.stdout.strip())
            self.assertEqual(rec["cache_n"], 0)  # measured zero, NOT null

    def test_emit_writes_to_out_when_given(self):
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "p.tsv")
            open(tsv, "w").close()
            tlog = os.path.join(d, "tlog.jsonl")
            out = os.path.join(d, "out.jsonl")
            self._run_cli([
                "emit", "--phases-file", tsv, "--tlog", tlog,
                "--out", out, "--result", "pass", "--total-s", "1.00"])
            self.assertTrue(os.path.exists(out))
            self.assertFalse(os.path.exists(tlog))  # --out overrides --tlog

    def test_no_subcommand_exits_2(self):
        r = self._run_cli([])
        self.assertEqual(r.returncode, 2)
        self.assertIn("usage", r.stderr)


if __name__ == "__main__":
    unittest.main()
