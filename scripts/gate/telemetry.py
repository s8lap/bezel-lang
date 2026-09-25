"""telemetry.py — the schema:2 gate-timings.jsonl record builder (#3010).

Python port of gate.sh's emit_json (gate.sh:308-360). emit_json was the densest
remaining logic in gate.sh: two documented footguns lived in it —

  1. the `awk -v exp=... ran=...` merge at :339-340 used `exp`/`ran` as `-v`
     names, which are GAWK BUILTINS — a fatal awk error that would have made the
     per-binary merge emit nothing at all (silently dropping the R2 signal).
  2. the 25-field positional `printf` at :354-359 — one misaligned `%s` and a
     field silently lands in another field's slot, or a `null`/`0`/`true`
     literal is emitted inside a string context.

Both are gone here: the awk merge is a Python dict union, and the printf is a
dict fed to json.dumps. The #3010 null-vs-0 distinction — an UNMEASURED number
emits `null`, never `0`, so a harvest failure and a genuine zero stay
distinguishable — becomes `None` vs `0` in Python, which is trivially correct
and is pinned by test_telemetry.py.

BYTE-IDENTITY (the dual-run proof). The record this module emits is diffed
byte-for-byte against the old bash emit_json over the landing gate (Wave-3
migration methodology: emit-alongside -> prove-equivalence -> switch). Two
details make that possible against json.dumps, which would otherwise diverge
from the bash printf:

  * COMPACT SEPARATORS — `separators=(",", ":")`. The bash printf emits no
    whitespace after `,` or `:`; json.dumps defaults to `, ` / `: `.
  * FIXED-DECIMAL DURATIONS — bash formats every duration with awk `%.2f`
    (5.00, 0.00), but json.dumps renders a Python float with repr (5.0). The
    RawNumber wrapper (below) carries the pre-formatted text through dumps as a
    bare JSON number, so `5.00` survives. shard_times are NOT fixed-decimal
    (they are the raw float strings sed pulled from the shard log), so they are
    likewise wrapped verbatim.

SCHEMA:2 IS A PRESERVATION CONTRACT. gate.trend R1-R5 parse these records, and
the blocking push check (gate_and_push.sh -> gate_trend.sh --last) depends on
them. A field rename, a dropped field, or a null-vs-0 flip reintroduces the
#3010 defect class (55 `pass` verdicts contradicted by their own coverage,
legible for 24 days, read by nothing). The field set, field order, and the
null-vs-0 rule per field are each pinned by test_telemetry.py.

Stdlib only (unittest, no pytest). See test_telemetry.py for the pins.
"""

import argparse
import collections
import json
import os
import re
import sys

# ---------------------------------------------------------------------------
# RawNumber — a JSON number emitted verbatim (unquoted).
#
# json.dumps renders a Python float with float.__repr__ (5.0), but the bash
# emit_json formats durations with awk %.2f (5.00). This wrapper carries a
# pre-formatted decimal string through dumps and emits it as a BARE JSON
# number, keeping the Python record byte-identical to the bash one (the
# dual-run proof). The C-accelerated json encoder hardcodes float formatting,
# so a float subclass cannot override it; the sentinel technique below (a
# control-byte token that cannot appear in any real field value) is the
# stdlib-only way to inject a raw token. See test_telemetry.py's
# SerializeByteIdentityTest for the round-trip + byte pins.
# ---------------------------------------------------------------------------

# A control byte that cannot appear in any field value the gate produces
# (hostnames, git hashes, ISO timestamps, short human notes, decimal numbers).
# json.dumps escapes it as the 6-char literal , which the regex in
# serialize() matches and unquotes.
_TOKEN = "\x01"

# A bare JSON number (the only text RawNumber may carry verbatim, since it is
# unquoted into the record). Used to validate a TSV `dur` BEFORE wrapping it: a
# corrupt/non-numeric duration would otherwise be emitted as a bare token where
# a number is expected, writing a malformed line into gate-timings.jsonl that
# the next `gate_trend --last` then refuses every push over until hand-edited
# (the #3010 availability trap). No match => "0.00" fallback.
_JSON_NUM_RE = re.compile(r"^-?\d+(\.\d+)?([eE][+-]?\d+)?$")


class RawNumber:
    """A number whose text is emitted verbatim and unquoted in the JSON line.

    `text` is already a valid bare JSON number (e.g. "5.00", "12.3", "0.00") —
    the bash awk/sed formatting is preserved as-is so the record is
    byte-identical. Not a float subclass: the C json encoder ignores a
    subclass __repr__, so the sentinel-unquoting in serialize() is the path.
    """
    __slots__ = ("text",)

    def __init__(self, text):
        self.text = text

    def __repr__(self):
        # For debugging only; serialize() does not rely on this.
        return "RawNumber(%r)" % self.text


# ---------------------------------------------------------------------------
# Phase + Telemetry — the accumulator that replaces gate.sh's PH_* parallel
# arrays (PH_NAME / PH_DUR / PH_RC / PH_STATUS / PH_NOTE at gate.sh:303-306).
# gate.sh still keeps those arrays for its human timing summary
# (gate.sh:369-375, NOT in this migration's scope); the Python emit reads the
# same phase data via a TSV file (see the CLI's --phases-file).
# ---------------------------------------------------------------------------

Phase = collections.namedtuple("Phase", ["name", "s", "rc", "status", "note"])
# `s` is a RawNumber: the phase duration as a "%.2f" string (gate.sh:305),
# preserved verbatim for byte-identity with the bash printf. `rc` is an int;
# `status` is "ran" or "skipped"; `note` is a short human string (may be "").


class Telemetry:
    """Accumulator for per-phase timing records (replaces gate.sh's PH_* arrays).

    Mirrors the bash rec_ran / rec_skip contract (gate.sh:305-306):
      * ran(name, dur, rc, note)  — a phase that executed; dur is seconds.
      * skip(name, note)          — a phase that was skipped (s=0.00, rc=0).

    `dur` is formatted to "%.2f" here, matching the bash rec_ran's awk
    `printf \"%.2f\"` (gate.sh:305). The CLI does NOT use this method — it reads
    the bash-pre-formatted "%.2f" strings from the TSV verbatim (no float
    round-trip) — but tests use it to build phases with natural float inputs.
    """

    def __init__(self):
        self._phases = []

    def ran(self, name, dur, rc, note=""):
        self._phases.append(
            Phase(name, RawNumber("%.2f" % dur), int(rc), "ran", note))

    def skip(self, name, note=""):
        self._phases.append(
            Phase(name, RawNumber("0.00"), 0, "skipped", note))

    @property
    def phases(self):
        return list(self._phases)


# ---------------------------------------------------------------------------
# The null-vs-0 rules — per #3010 (gate.sh:317-329). AN UNMEASURED NUMBER EMITS
# null, NEVER 0. Each helper takes a raw bash value (a string, or None when the
# bash variable was unset / the flag omitted) and returns the JSON-native value
# the record carries. The rules are lifted verbatim from the bash conditionals.
# ---------------------------------------------------------------------------

def _posint(v):
    """null unless set AND > 0. per gate.sh:323-324 (j_gtests/j_lit) and :325.

    `"gtests":${COV_GTESTS:-0}` is how 26 records came to say result=pass with
    gtests=0: a failed harvest regex and a suite that genuinely ran nothing
    were byte-identical. Only a STRICTLY POSITIVE int is a measurement.
    """
    if v is None or v == "":
        return None
    try:
        n = int(v)
    except (ValueError, TypeError):
        return None
    return n if n > 0 else None


def _optint(v):
    """null if unset/empty, else int (0 ALLOWED). per gate.sh:326-327.

    cache_n and shard_timeout_s: a measured zero is a real value (an empty
    bincache; a 0s ceiling), distinct from "never measured". Unset => null.
    """
    if v is None or v == "":
        return None
    try:
        return int(v)
    except (ValueError, TypeError):
        return None


def _optbool(v):
    """null if unset/empty, else (int==1). per gate.sh:328-329 (j_cold/j_floor).

    GATE_COLD / GATE_FLOOR_OK are 0/1 ints. Unset => null (not measured); set
    => True only on exactly 1 (any other int is False, matching bash -eq 1).
    """
    if v is None or v == "":
        return None
    try:
        return int(v) == 1
    except (ValueError, TypeError):
        return None


def _build_aot(hits, misses, store_fails):
    """The aot sub-object, or None. per gate.sh:330-331.

    Emitted only when at least one of hits/misses was measured (non-empty).
    Each sub-field is independently null if its own value was absent —
    store_failures is often null even when hits/misses are present.
    """
    if (hits is None or hits == "") and (misses is None or misses == ""):
        return None
    return {
        "hits": _optint(hits),
        "misses": _optint(misses),
        "store_failures": _optint(store_fails),
    }


def _build_mem(anon_max, current_max, samples):
    """The mem sub-object, or None. per gate audit F1 (Tier 0).

    anon_bytes is THE figure: anonymous+shmem+slab in the dev_heavy slice,
    sampled while the shard phase ran. It is what OOM-kills a shard and so what
    bounds shard concurrency. current_bytes (memory.current) is carried only as
    a foil — it is dominated by RECLAIMABLE page cache (measured: anon 0 /
    file 14.3G on an idle slice), so sizing concurrency against it overstates
    the load by more than 10x. Null when the sampler recorded nothing (it is
    best-effort and never fails a gate).
    """
    if (anon_max is None or anon_max == "") and (current_max is None or current_max == ""):
        return None
    return {
        "anon_bytes": _optint(anon_max),
        "current_bytes": _optint(current_max),
        "samples": _optint(samples),
    }


def _build_binaries(bin_expected, bin_ran):
    """The per-binary ran/expected map, or None. per gate.sh:336-353 (the awk merge).

    Replaces the awk `-v emap=... -v rmap=...` union (and its `exp`/`ran`
    gawk-builtin-names footgun at :339-340). The two flat maps are written at
    different points in the run (ran at the harvest, expected at the coverage
    floor — see the ORDERING note at gate.sh:796-800); the union of names is
    the point: a binary that was EXPECTED but never REPORTED appears with
    ran=0 — exactly the R2 signature ("a whole area contributed nothing").

    Keys are emitted SORTED. The bash awk iterated `for(k in seen)` in gawk
    hash order (non-deterministic across versions/key sets); sorted is strictly
    better (reproducible) and semantically identical — gate.trend R2 sorts the
    keys before judging anyway (`for name, v in sorted(per_bin.items())`).
    """
    if (bin_expected is None or bin_expected == "") and (
            bin_ran is None or bin_ran == ""):
        return None

    def _parse_map(s):
        # gate.sh:342-343: split on spaces, each token "name:value".
        m = {}
        for tok in (s or "").split():
            if ":" not in tok:
                continue
            name, _, val = tok.partition(":")
            if name != "":
                m[name] = val
        return m

    e = _parse_map(bin_expected)
    r = _parse_map(bin_ran)
    out = {}
    for name in sorted(set(e) | set(r)):
        ev = e.get(name)
        rv = r.get(name)
        out[name] = {
            # per gate.sh:347: expected is null if absent; ran is 0 if absent.
            # int() is guarded via _to_int: a non-numeric map value used to
            # raise here, crashing emit (silently dropping the whole telemetry
            # record via the caller's `|| true`). Now it degrades to ran=0 /
            # expected=null like _posint/_optint, #3010.
            "ran": _to_int(rv, 0),
            "expected": _to_int(ev, None),
        }
    return out if out else None


def _shard_times_list(s):
    """Parse the comma-separated shard-time string into a list of RawNumber.

    per gate.sh:716: SHARD_TIMES is `paste -sd,` over sed-extracted raw float
    strings (e.g. "12.3,45.67,8"). These are NOT fixed-decimal (unlike phase
    durations), so each token is wrapped verbatim — no %.2f reformatting. An
    empty string => [] (gate.sh emits `"shard_times":[]`).
    """
    if s is None or s == "":
        return []
    return [RawNumber(tok) for tok in s.split(",") if tok != ""]


# ---------------------------------------------------------------------------
# build_record — assemble the schema:2 dict. FIELD ORDER IS LOAD-BEARING: the
# bash printf (gate.sh:354) emits fields in a fixed order, and although JSON
# objects are unordered by spec, the dual-run byte-identity proof diffs the raw
# lines, so the dict below is built in the EXACT printf order. Python dicts
# preserve insertion order (3.7+) and json.dumps (sort_keys=False, the default)
# honours it. Do not reorder.
# ---------------------------------------------------------------------------

def build_record(*, ts, epoch, host, mode, head, head_short, dirty, result,
                 failed_phase, total_s, phases, shard_times,
                 cov_gtests=None, cov_littests=None, expected_gtests=None,
                 coverage_floor_ok=None, cold=None, cache_n=None,
                 shard_timeout=None, aot_hits=None, aot_misses=None,
                 aot_store_fails=None, bin_expected="", bin_ran="",
                 shard_count="0", shard_reported_s="0",
                 arbitration_suspects="0", serial_attempts="0",
                 serial_total="0", asan_shards_reported="0",
                 asan_errors="0", mem_anon_max=None, mem_current_max=None,
                 mem_samples=None):
    """Build the schema:2 record dict (one gate-timings.jsonl entry).

    The null-vs-0 fields (#3010) take the RAW bash string values (or None for
    an unset/omitted variable) and apply the rules internally — so the faithful
    port lives in one testable place. `total_s` is the bash-pre-formatted
    "%.2f" string (wrapped in RawNumber for byte-identity). `phases` is a list
    of Phase (s already a RawNumber). `shard_times` is the raw comma-string.

    NB: emit_json's `rc` parameter is NOT in the record (gate.sh:354-359 emits
    `result`, not `rc`); it was vestigial in the bash. Not carried here.
    """
    failed = failed_phase if failed_phase else None
    return {
        "schema": 2,
        "ts": ts,
        "epoch": _to_int(epoch, 0),
        "host": host,
        "mode": mode,
        "head": head,
        "head_short": head_short,
        "dirty": _to_bool(dirty),
        "result": result,
        "failed_phase": failed,
        "total_s": RawNumber(total_s),
        "gtests": _posint(cov_gtests),
        "littests": _posint(cov_littests),
        "expected_gtests": _posint(expected_gtests),
        # fix #3737: split what was one conflated field. coverage_floor_ok
        # (gate.sh's GATE_FLOOR_OK / --coverage-floor-ok) is 0/1: 1 when
        # `ran >= expected`, 0 when the floor genuinely ran and found a
        # shortfall (coverage-short) OR the registry was unusable — either
        # way the check DID run, it just did not pass. The old single field
        # collapsed "ran" and "passed" into the same bit, so gate.trend's R5
        # read a legitimate coverage-short FAIL (floor enforced, not
        # satisfied) as "the gate ran WITHOUT the #3002 floor" — a
        # misleading message on a record whose `result` was already
        # correctly "fail". `coverage_floor_enforced` is now presence-based
        # (was the flag passed at all — null only when gate.sh never called
        # the floor); `coverage_floor_satisfied` keeps the old value-based
        # semantics (did the comparison pass) that R5 actually needs.
        "coverage_floor_enforced": (
            None if coverage_floor_ok is None or coverage_floor_ok == ""
            else True),
        "coverage_floor_satisfied": _optbool(coverage_floor_ok),
        "cold": _optbool(cold),
        "cache_n": _optint(cache_n),
        "shard_timeout_s": _optint(shard_timeout),
        "aot": _build_aot(aot_hits, aot_misses, aot_store_fails),
        "binaries": _build_binaries(bin_expected, bin_ran),
        "shard_count": _to_int(shard_count, 0),
        "shard_reported_s": _to_int(shard_reported_s, 0),
        "arbitration_suspects": _to_int(arbitration_suspects, 0),
        "serial_attempts": _to_int(serial_attempts, 0),
        "serial_total": _to_int(serial_total, 0),
        "asan_shards_reported": _to_int(asan_shards_reported, 0),
        "asan_errors": _to_int(asan_errors, 0),
        "shard_times": _shard_times_list(shard_times),
        "mem": _build_mem(mem_anon_max, mem_current_max, mem_samples),
        "phases": [
            {"name": p.name, "s": p.s, "rc": p.rc, "status": p.status,
             "note": p.note}
            for p in phases
        ],
    }


def _to_int(v, default=0):
    """Coerce a bash numeric string to int; fall back to `default` (gate.sh's
    `${VAR:-0}`). These fields are ALWAYS emitted as numbers, never null."""
    try:
        return int(v)
    except (ValueError, TypeError):
        return default


def _to_bool(v):
    """GATE_DIRTY is the bash string "true"/"false" (gate.sh:295)."""
    if isinstance(v, bool):
        return v
    return v == "true"


# ---------------------------------------------------------------------------
# serialize — dict -> one compact JSONL line (no trailing newline). Byte-
# identical to the bash printf (gate.sh:354): compact separators, insertion-
# order fields, and RawNumber tokens unquoted. Best-effort like the bash (the
# caller appends with `>> ... 2>/dev/null || return 0`); this function itself
# is pure I/O-free and never raises on valid input.
# ---------------------------------------------------------------------------

def serialize(record):
    """Serialize a record dict to one compact JSON line (no trailing newline).

    Compact separators (no spaces) match the bash printf. RawNumber values are
    emitted as bare JSON numbers: json.dumps escapes the control-byte sentinel
    to the literal `\\u0001`, which the regex then unquotes. The number text is
    a controlled decimal string (no quotes/backslashes), so `[^"]*` matches it
    exactly. See SerializeByteIdentityTest for the round-trip pin.
    """
    encoder = json.JSONEncoder(default=_raw_default, separators=(",", ":"))
    s = encoder.encode(record)
    # Unquote the sentinel-wrapped raw numbers: `"\\u0001TEXT\\u0001"` -> `TEXT`.
    return re.sub(r'"\\u0001([^"]*)\\u0001"', r"\1", s)


def _raw_default(o):
    """json.dumps default hook: render RawNumber as a sentinel string.

    The sentinel (control byte \\x01, escaped by json as \\u0001) wraps the
    number text so serialize()'s regex can strip the surrounding quotes,
    leaving a bare JSON number. Any other unknown type is a real error."""
    if isinstance(o, RawNumber):
        return _TOKEN + o.text + _TOKEN
    raise TypeError("Object of type %s is not JSON serializable"
                    % type(o).__name__)


# ---------------------------------------------------------------------------
# CLI — gate.sh calls `python3 -m gate.telemetry emit ...` (Wave 3). The emit
# subcommand reads the phase data from a TSV file (--phases-file) that gate.sh
# writes from its PH_* arrays at exit, plus the scalar fields as --flags. The
# null-vs-0 fields use the convention: flag OMITTED (or empty) => unmeasured
# => null; flag present with a value => measured => the rule applies. This
# mirrors the bash `[ -n "${VAR:-}" ] && args+=(...)` conditional exactly.
# ---------------------------------------------------------------------------

def _read_phases_tsv(path):
    """Read the phase TSV gate.sh writes (name\\tdur\\trc\\tstatus\\tnote).

    `dur` is the bash-pre-formatted "%.2f" string (gate.sh:305); wrapped in
    RawNumber VERBATIM (no float round-trip) so the record is byte-identical.
    Blank lines are skipped. Returns a list of Phase.
    """
    phases = []
    try:
        with open(path, errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                parts = line.split("\t")
                # Pad to 5 fields in case a malformed line is short.
                while len(parts) < 5:
                    parts.append("")
                name, dur, rc, status, note = parts[:5]
                try:
                    rc_i = int(rc)
                except ValueError:
                    rc_i = 0
                # Validate dur is a bare JSON number before wrapping: a corrupt
                # TSV row (non-numeric dur) would be emitted verbatim as a bare
                # token, writing a malformed line into gate-timings.jsonl that
                # the next gate_trend --last refuses every push over until
                # hand-edited (#3010 availability trap). Fall back to "0.00".
                if not _JSON_NUM_RE.match(dur):
                    dur = "0.00"
                phases.append(
                    Phase(name, RawNumber(dur), rc_i, status, note))
    except OSError:
        pass  # best-effort — matches the bash `2>/dev/null` swallow
    return phases


def _emit(path, line):
    """Append `line` + newline to the gate-timings log. Best-effort.

    Faithful to gate.sh:310 (`mkdir -p ... || return 0`) and :359
    (`>> "$TLOG" 2>/dev/null || return 0`): a logging hiccup never changes the
    gate's exit code. Returns 0 unconditionally.
    """
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass
    return 0


def _main_emit(argv):
    """`emit --phases-file F --tlog P --result R --total-s S ...` -> append one
    JSONL record. The scalar --flags mirror gate.sh's emit_json globals; the
    null-vs-0 fields are optional (absent => null)."""
    p = argparse.ArgumentParser(prog="gate.telemetry emit")
    p.add_argument("--phases-file", required=True)
    p.add_argument("--tlog", required=True)
    p.add_argument("--result", required=True)
    p.add_argument("--total-s", required=True,
                   help="total wall seconds, 2-decimal-formatted (matches bash awk)")
    # identity / context (always present in gate.sh)
    p.add_argument("--ts", default="")
    p.add_argument("--epoch", default="0")
    p.add_argument("--host", default="?")
    p.add_argument("--mode", default="default")
    p.add_argument("--head", default="")
    p.add_argument("--head-short", default="")
    p.add_argument("--dirty", default="false")
    p.add_argument("--failed-phase", default="")
    p.add_argument("--shard-times", default="")
    # null-vs-0 fields: absent/empty => null. per #3010.
    p.add_argument("--cov-gtests", default="")
    p.add_argument("--cov-littests", default="")
    p.add_argument("--expected-gtests", default="")
    p.add_argument("--coverage-floor-ok", default="")
    p.add_argument("--cold", default="")
    p.add_argument("--cache-n", default="")
    p.add_argument("--shard-timeout", default="")
    p.add_argument("--aot-hits", default="")
    p.add_argument("--aot-misses", default="")
    p.add_argument("--aot-store-fails", default="")
    p.add_argument("--mem-anon-max", default="")
    p.add_argument("--mem-current-max", default="")
    p.add_argument("--mem-samples", default="")
    p.add_argument("--bin-expected", default="")
    p.add_argument("--bin-ran", default="")
    # always-numeric fields (default "0", never null)
    p.add_argument("--shard-count", default="0")
    p.add_argument("--shard-reported-s", default="0")
    p.add_argument("--arbitration-suspects", default="0")
    p.add_argument("--serial-attempts", default="0")
    p.add_argument("--serial-total", default="0")
    p.add_argument("--asan-shards-reported", default="0")
    p.add_argument("--asan-errors", default="0")
    p.add_argument("--out", default=None,
                   help="write the line to this path instead of --tlog (dual-run)")
    args = p.parse_args(argv)

    record = build_record(
        ts=args.ts, epoch=args.epoch, host=args.host, mode=args.mode,
        head=args.head, head_short=args.head_short, dirty=args.dirty,
        result=args.result, failed_phase=args.failed_phase,
        total_s=args.total_s, phases=_read_phases_tsv(args.phases_file),
        shard_times=args.shard_times,
        cov_gtests=args.cov_gtests or None,
        cov_littests=args.cov_littests or None,
        expected_gtests=args.expected_gtests or None,
        coverage_floor_ok=args.coverage_floor_ok or None,
        cold=args.cold or None,
        cache_n=args.cache_n or None,
        shard_timeout=args.shard_timeout or None,
        aot_hits=args.aot_hits or None,
        aot_misses=args.aot_misses or None,
        aot_store_fails=args.aot_store_fails or None,
        mem_anon_max=args.mem_anon_max or None,
        mem_current_max=args.mem_current_max or None,
        mem_samples=args.mem_samples or None,
        bin_expected=args.bin_expected, bin_ran=args.bin_ran,
        shard_count=args.shard_count, shard_reported_s=args.shard_reported_s,
        arbitration_suspects=args.arbitration_suspects,
        serial_attempts=args.serial_attempts, serial_total=args.serial_total,
        asan_shards_reported=args.asan_shards_reported,
        asan_errors=args.asan_errors,
    )
    line = serialize(record)
    out_path = args.out if args.out else args.tlog
    _emit(out_path, line)
    # Echo the line on stdout so the dual-run harness can capture it.
    sys.stdout.write(line + "\n")
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "emit":
        return _main_emit(argv[2:])
    sys.stderr.write("usage: gate.telemetry {emit} ...\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
