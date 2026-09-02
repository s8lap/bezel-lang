"""summary.py — structured shard-summary parse/merge + legacy agreement (#3012).

Phase 2a (tests/test_main.cpp) writes one JSON object per shard when
BEZEL_SHARD_SUMMARY is set. This module reads those back, merges them, and —
the load-bearing Phase 2b job — compares them against the LEGACY stderr summary
line ("N passed, M failed ... T total") that tests/run_parallel.sh still greps
(run_parallel.sh:202). The comparison is OBSERVATION-ONLY: a mismatch is logged
loudly but does NOT fail the gate (the legacy path remains authoritative until
Phase 2c switches the reader, and only after N gates prove agreement). This is
the equivalence data-gathering step.

Both sources describe the SAME shard process, so per-shard passed/failed/total
MUST agree exactly; a divergence is a parsing bug in one path or a #3012-style
stream corruption (the atexit hook racing the summary printer). Pinning the
agreement here is what makes Phase 2c safe.

Stdlib only (no pytest) — see test_summary.py for the fixture-driven pins:
parse/merge on a corpus, the legacy-vs-JSON agreement (zero-mismatch and a
deliberate mismatch), and the malformed-JSON graceful-degradation invariant.
"""

import glob
import json
import os
import re
import sys

# Reuse the #3010 Phase 0 atomic writer: the run-summary that gate.sh reads
# (the BEZEL_RUN_SUMMARY bridge, see write_run_summary) must survive a crash
# mid-write — a half-written target is the #3000/#3003 shape.
from gate import io

# Schema for the MERGED run summary run_parallel writes for gate.sh to read
# (one per run, vs. one per shard for the v1 shard summary above). Distinct
# schema name so a reader that expects a shard summary refuses a run summary.
RUN_SCHEMA = "bezel.run-summary.v1"

# The schema every shard JSON carries. run_parallel's agreement check asserts
# this so a future field rename cannot silently make the two parses diverge.
SCHEMA = "bezel.shard-summary.v1"

# The legacy summary line run_parallel.sh:202 keys on (Plan-A invariant 1.26 —
# the shard-summary line is LOAD-BEARING; do not change it). This regex is a
# direct translation of `grep -E '^[0-9]+ passed, [0-9]+ failed'`.
_LEGACY_HEAD_RE = re.compile(r"^(?P<passed>\d+) passed, (?P<failed>\d+) failed")
# ", T total" — the trailing field; `.*` swallows the optional shard marker.
_LEGACY_TOTAL_RE = re.compile(r",\s*(?P<total>\d+) total\s*$")
# ", N selected" — only present in BEZEL_TEST_LIST subset mode (#2100).
_LEGACY_SELECTED_RE = re.compile(r",\s*(?P<selected>\d+) selected")


def parse_legacy_summary(text):
    """Parse the LAST legacy summary line in `text` into a dict.

    Mirrors run_parallel.sh:202-220: `grep '^[0-9]+ passed, [0-9]+ failed' |
    tail -1`, then harvest passed/failed/total, plus selected (subset mode
    only). Returns None when no summary line is present — a shard that printed
    no completion summary validated NONE of its slice (run_parallel's
    summary_ok check), which the agreement layer must surface, not paper over.
    """
    line = None
    for raw in text.splitlines():
        if _LEGACY_HEAD_RE.match(raw):
            line = raw  # tail -1: keep the last matching line.
    if line is None:
        return None
    m = _LEGACY_HEAD_RE.match(line)
    out = {"passed": int(m.group("passed")), "failed": int(m.group("failed")),
           "selected": None}
    mt = _LEGACY_TOTAL_RE.search(line)
    out["total"] = int(mt.group("total")) if mt else None
    ms = _LEGACY_SELECTED_RE.search(line)
    if ms:
        out["selected"] = int(ms.group("selected"))
    return out


def read_shard_summary(path):
    """Read one structured shard summary JSON. Returns None if the file is
    missing or unparseable — a reader must never crash on a half-written
    document (the atomic write makes that impossible in practice, but the
    graceful degradation is the #3003 shape: plausible-looking garbage must
    not take the whole agreement pass down)."""
    try:
        with open(path) as f:
            obj = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(obj, dict):
        return None
    return obj


def _split_shard_name(filename, prefix, suffix):
    """Strip `prefix.` and `suffix`, then rsplit the remainder on '.' once:
    [name, shard_index]. The shard index is always the LAST dot-segment (the
    name may itself contain dots), matching run_parallel's
    `<prefix>.${names[$b]}.$i<suffix>` layout. Returns (name, idx) or None."""
    if not filename.startswith(prefix + ".") or not filename.endswith(suffix):
        return None
    core = filename[len(prefix) + 1:-len(suffix)]
    if "." not in core:
        return None
    name, _, idx = core.rpartition(".")
    if not idx.isdigit():
        return None
    return name, int(idx)


def read_legacy_summaries(logdir):
    """Read every shard.<name>.<i>.log legacy summary in `logdir`.

    Yields dicts: {binary, shard_index, legacy} where `legacy` is the
    parse_legacy_summary result (None if the shard printed no summary)."""
    out = []
    for path in sorted(glob.glob(os.path.join(logdir, "shard.*.log"))):
        key = _split_shard_name(os.path.basename(path), "shard", ".log")
        if key is None:
            continue
        name, idx = key
        try:
            with open(path, errors="replace") as f:
                text = f.read()
        except OSError:
            text = ""
        out.append({"binary": name, "shard_index": idx,
                    "legacy": parse_legacy_summary(text)})
    return out


def read_shard_summaries(logdir):
    """Read every summary.<name>.<i>.json structured summary in `logdir`.

    Yields dicts: {binary, shard_index, json} where `json` is the
    read_shard_summary result (None if missing/unparseable)."""
    out = []
    for path in sorted(glob.glob(os.path.join(logdir, "summary.*.json"))):
        key = _split_shard_name(os.path.basename(path), "summary", ".json")
        if key is None:
            continue
        name, idx = key
        out.append({"binary": name, "shard_index": idx,
                    "json": read_shard_summary(path)})
    return out


def merge(summaries):
    """Aggregate a list of per-shard summary dicts into per-binary + grand
    totals. `summaries` items carry a `passed`/`failed`/`total` (the JSON
    schema fields). Returns {binaries: {name: {passed,failed,total,shards}},
    total: {passed,failed,shards}}."""
    bins = {}
    for s in summaries:
        name = s["binary"]
        b = bins.setdefault(name, {"passed": 0, "failed": 0, "shards": 0})
        b["passed"] += int(s.get("passed", 0))
        b["failed"] += int(s.get("failed", 0))
        b["shards"] += 1
    grand = {"passed": 0, "failed": 0, "shards": 0}
    for b in bins.values():
        grand["passed"] += b["passed"]
        grand["failed"] += b["failed"]
        grand["shards"] += b["shards"]
    return {"binaries": bins, "total": grand}


def check_agreement(logdir):
    """Pair JSON summaries with legacy summaries by (binary, shard_index) and
    compare passed/failed/total (and selected when the legacy line carries
    it). Returns a list of mismatch-description strings — EMPTY when the two
    parses agree on every shard. Also flags shards present in one source but
    not the other, and any JSON whose schema is not the expected one.

    Observation-only: this never raises on a malformed input; it records the
    defect as a mismatch string so the caller (run_parallel) can log it
    without affecting its verdict."""
    legacy = {(s["binary"], s["shard_index"]): s["legacy"]
              for s in read_legacy_summaries(logdir)}
    structured = {(s["binary"], s["shard_index"]): s["json"]
                  for s in read_shard_summaries(logdir)}
    mismatches = []
    keys = sorted(set(legacy) | set(structured))
    for key in keys:
        name, idx = key
        leg = legacy.get(key)
        js = structured.get(key)
        where = "{} shard {}".format(name, idx)
        if js is None:
            mismatches.append("{}: NO JSON summary (file missing/unparseable)"
                              .format(where))
            continue
        if leg is None:
            mismatches.append("{}: NO legacy summary line (shard printed none)"
                              .format(where))
            continue
        if js.get("schema") != SCHEMA:
            mismatches.append("{}: schema mismatch: JSON={} expected={}"
                              .format(where, js.get("schema"), SCHEMA))
        for field in ("passed", "failed", "total"):
            jv = js.get(field)
            lv = leg.get(field)
            if jv != lv:
                mismatches.append("{}: {} mismatch: JSON={} legacy={}"
                                  .format(where, field, jv, lv))
        # `selected` is only in the legacy line under BEZEL_TEST_LIST; compare
        # it only when the legacy line actually carried it.
        if leg.get("selected") is not None and js.get("selected") != leg["selected"]:
            mismatches.append("{}: selected mismatch: JSON={} legacy={}"
                              .format(where, js.get("selected"),
                                      leg["selected"]))
    return mismatches


# ---------------------------------------------------------------------------
# Phase 2c: run-level accounting from the structured JSON shard summaries.
#
# `account` replaces run_parallel.sh's per-shard grep of the legacy
# "N passed, M failed" line (run_parallel.sh:202-254) AND gate.sh's harvest of
# the printed TOTAL / per-binary lines (gate.sh:831, :855-860). It reads the
# per-shard JSON summaries + the per-shard rc.<name>.<i> files and produces the
# SAME accounting the legacy greps did: grand totals, per-binary ran/expected,
# and the integrity flags + defect lists that drive run_parallel's exit code
# and the "## GATE FAILURE: shard crash / undercount" banner.
#
# CRASH DETECTION reads rc.<name>.<i> (the subshell's recorded process exit),
# NOT the JSON `rc` field: test_main.cpp writes the JSON just before `return`
# (test_main.cpp:281-318), so a shard killed mid-run (segfault/timeout) never
# reaches the JSON write, and the JSON `rc` field only ever records a normal
# test-failure exit. The rc FILE is the only signal that catches a kill — this
# mirrors run_parallel.sh:202-207 (`cat rc...; ${rc:-1} -ne 0`) exactly.
# per #3012, #1861, #2100.


def _read_rc_files(logdir):
    """Read every rc.<name>.<i> exit-code file in `logdir`.

    Returns {(name, idx): rc_int}. A missing file maps to nothing here; the
    account() loop treats an absent key as rc=1 (crash), matching
    run_parallel's `${rc:-1}` default. rc files have NO extension (unlike the
    .log/.json shard files), so they are parsed directly rather than via
    _split_shard_name (whose suffix stripping assumes a non-empty suffix)."""
    out = {}
    for path in sorted(glob.glob(os.path.join(logdir, "rc.*"))):
        base = os.path.basename(path)
        core = base[len("rc."):]  # <name>.<idx>
        name, _, idx = core.rpartition(".")
        if not name or not idx.isdigit():
            continue
        try:
            with open(path) as f:
                raw = f.read().strip()
        except OSError:
            raw = ""
        out[(name, int(idx))] = int(raw) if raw.lstrip("-").isdigit() else 1
    return out


def account(logdir, conc, names, subset=False, shard_timeout=None):
    """Compute run_parallel's full accounting from the structured JSON shard
    summaries + the rc files.

    `conc` is the per-binary shard count (run_parallel's resource cap);
    `names` is the ordered list of binary labels run_parallel launched;
    `subset` is True iff BEZEL_TEST_LIST was set (the selected-count integrity
    mode, #2100) — this is the JSON equivalent of the legacy "did the summary
    line carry 'selected'" test, read from the run config that determined it
    (the JSON always carries a `selected` field, so it cannot itself distinguish
    list mode from a filter; the env config can, and does, cleanly).
    `shard_timeout` (optional, narrow #3222) is the configured per-shard
    ceiling in seconds (BEZEL_SHARD_TIMEOUT). When set, crashed shards with
    rc in {124, 137} AND elapsed >= CEILING_KILL_THRESHOLD * shard_timeout
    are reclassified into `timed_out_shards` instead of `crashed_shards`,
    so the banner distinguishes a ceiling-kill (infra defect, fix the
    ceiling) from a real crash (suite defect, fix the code).

    Faithful to run_parallel.sh:185-270: per-binary ran==expected (#1861),
    every-shard-printed-a-summary (#1861), empty-subset-never-a-pass (#2100).
    Returns the run-summary dict (see RUN_SCHEMA)."""
    rc_map = _read_rc_files(logdir)
    summaries = {(s["binary"], s["shard_index"]): s["json"]
                 for s in read_shard_summaries(logdir)}

    total_pass = 0
    total_fail = 0
    shard_rc_ok = 1
    count_ok = 1
    summary_ok = 1
    crashed_shards = []
    timed_out_shards = []
    missing_summary_shards = []
    undercount_bins = []
    global_selected = 0
    bin_rows = []

    for name in names:
        b_pass = 0
        b_fail = 0
        b_registry = 0
        b_selected = 0
        b_summaries = 0
        for i in range(conc):
            key = (name, i)
            shard_id = "%s/%d" % (name, i)
            rc = rc_map.get(key, 1)
            if rc != 0:
                shard_rc_ok = 0
                # narrow #3222: classify ceiling-killed shards separately. A
                # shard with rc in {124, 137} AND elapsed near the configured
                # ceiling was killed by the timeout budget (an INFRA defect),
                # NOT a generic crash (a SUITE defect). See
                # extract_shard_elapsed / CEILING_KILL_THRESHOLD docstrings for
                # why each side of that AND is load-bearing: 124 alone catches
                # deadlocks that happened to outlive the ceiling; near-ceiling
                # alone catches a real crash whose `timeout` rc got mangled by
                # an unrelated layer. Both required.
                elapsed = extract_shard_elapsed(logdir, shard_id) \
                    if shard_timeout else None
                if (shard_timeout and rc in _TIMEOUT_RCS
                        and elapsed is not None
                        and elapsed
                            >= CEILING_KILL_THRESHOLD * shard_timeout):
                    timed_out_shards.append(shard_id)
                else:
                    crashed_shards.append(shard_id)
            js = summaries.get(key)
            if js is None:
                # No structured summary: the shard validated NONE of its slice
                # (crashed before the write, or a path that exit()d the harness
                # mid-loop). summary_ok below catches it unconditionally.
                missing_summary_shards.append(shard_id)
                continue
            b_summaries += 1
            b_pass += int(js.get("passed", 0))
            b_fail += int(js.get("failed", 0))
            t = int(js.get("total", 0))
            if t > b_registry:
                b_registry = t
            if subset:
                s = int(js.get("selected", 0))
                if s > b_selected:
                    b_selected = s
        if subset:
            b_expected = b_selected
            what = "selected"
        else:
            b_expected = b_registry
            what = "registered"
        b_ran = b_pass + b_fail
        if b_expected > 0 and b_ran != b_expected:
            count_ok = 0
            undercount_bins.append(
                "%s: ran %d of %d %s" % (name, b_ran, b_expected, what))
        # EVERY shard must have written a structured summary. A shard missing
        # one validated NONE of its slice, so a "0 failed" aggregate is not
        # trustworthy regardless of exit code — stricter than the undercount
        # check and never lets a dropped slice pass green (#1861).
        if b_summaries != conc:
            summary_ok = 0
        if subset:
            global_selected += b_selected
        total_pass += b_pass
        total_fail += b_fail
        bin_rows.append({
            "name": name, "passed": b_pass, "failed": b_fail,
            "ran": b_ran, "expected": b_expected, "registry": b_registry,
            "selected": b_selected, "summaries": b_summaries,
        })

    ran = total_pass + total_fail
    expected_all = sum(r["expected"] for r in bin_rows)
    registry_all = sum(r["registry"] for r in bin_rows)
    subset_empty = 1 if (subset and global_selected == 0) else 0
    integrity_ok = (shard_rc_ok and count_ok and summary_ok
                    and not subset_empty)
    return {
        "schema": RUN_SCHEMA,
        "total_pass": total_pass,
        "total_fail": total_fail,
        "ran": ran,
        "expected_all": expected_all,
        "registry_all": registry_all,
        "nbin": len(names),
        "njobs": len(names) * conc,
        "conc": conc,
        "subset_mode": bool(subset),
        "global_selected": global_selected,
        "subset_empty": bool(subset_empty),
        "shard_rc_ok": bool(shard_rc_ok),
        "count_ok": bool(count_ok),
        "summary_ok": bool(summary_ok),
        "integrity_ok": bool(integrity_ok),
        "crashed_shards": crashed_shards,
        "timed_out_shards": timed_out_shards,
        "missing_summary_shards": missing_summary_shards,
        "undercount_bins": undercount_bins,
        "binaries": bin_rows,
    }


def _shell_quote(s):
    """Quote a string for a bash single-quoted context (handles embedded ')."""
    return "'" + s.replace("'", "'\"'\"'") + "'"


def emit_shell(acc):
    """Emit the accounting dict as shell-sourceable assignments (KEY=VALUE and
    bash array literals) for run_parallel.sh to `eval`. One python call
    replaces the legacy per-shard grep+sed harvest. Keys are rp_-prefixed so the
    eval cannot clobber run_parallel's loop temporaries."""
    lines = []
    flag = lambda b: "1" if b else "0"
    lines.append("rp_total_pass=%d" % acc["total_pass"])
    lines.append("rp_total_fail=%d" % acc["total_fail"])
    lines.append("rp_ran=%d" % acc["ran"])
    lines.append("rp_expected_all=%d" % acc["expected_all"])
    lines.append("rp_registry_all=%d" % acc["registry_all"])
    lines.append("rp_nbin=%d" % acc["nbin"])
    lines.append("rp_njobs=%d" % acc["njobs"])
    lines.append("rp_subset_mode=%s" % flag(acc["subset_mode"]))
    lines.append("rp_global_selected=%d" % acc["global_selected"])
    lines.append("rp_subset_empty=%s" % flag(acc["subset_empty"]))
    lines.append("rp_shard_rc_ok=%s" % flag(acc["shard_rc_ok"]))
    lines.append("rp_count_ok=%s" % flag(acc["count_ok"]))
    lines.append("rp_summary_ok=%s" % flag(acc["summary_ok"]))
    lines.append("rp_integrity_ok=%s" % flag(acc["integrity_ok"]))
    for key in ("crashed_shards", "timed_out_shards",
                "missing_summary_shards", "undercount_bins"):
        vals = " ".join(_shell_quote(x) for x in acc[key])
        lines.append("rp_%s=(%s)" % (key, vals))
    rows = acc["binaries"]
    lines.append("rp_bin_names=(%s)" % " ".join(_shell_quote(r["name"]) for r in rows))
    lines.append("rp_bin_pass=(%s)" % " ".join("%d" % r["passed"] for r in rows))
    lines.append("rp_bin_fail=(%s)" % " ".join("%d" % r["failed"] for r in rows))
    lines.append("rp_bin_expected=(%s)" % " ".join("%d" % r["expected"] for r in rows))
    lines.append("rp_bin_registry=(%s)" % " ".join("%d" % r["registry"] for r in rows))
    lines.append("rp_bin_selected=(%s)" % " ".join("%d" % r["selected"] for r in rows))
    return "\n".join(lines)


def write_run_summary(path, acc):
    """Write the accounting dict to `path` atomically — the BEZEL_RUN_SUMMARY
    bridge from run_parallel to gate.sh. gate.sh reads this for the gtest grand
    total (gate.sh:831) and the per-binary ran map (gate.sh:855-860) instead of
    grepping run_parallel's printed log: the per-shard JSONs themselves live in
    run_parallel's temp logdir and are reaped on a clean exit (run_parallel's
    _rp_keep trap), so a persistent merged artifact is the only handle gate.sh
    can hold. Atomic via io.atomic_json_write (#3010)."""
    io.atomic_json_write(path, acc)


def harvest_run_summary(path):
    """Read a run-summary written by write_run_summary. Returns
    (grand_passed, ran_map) where ran_map is an ordered {binary: passed} dict,
    or (None, None) if the file is missing/unparseable/wrong-schema — gate.sh
    then falls back to its legacy grep (the switch keeps the grep as a fallback
    until N gates prove the JSON path, per the Phase 2c methodology)."""
    obj = read_shard_summary(path)
    if obj is None or obj.get("schema") != RUN_SCHEMA:
        return None, None
    ran = {}
    for row in obj.get("binaries", []):
        name = row.get("name")
        if name is not None:
            ran[name] = int(row.get("passed", 0))
    return int(obj.get("total_pass", 0)), ran


# ---------------------------------------------------------------------------
# Phase 6: the GATE FAILURE banner + phantom-name warning formatters.
#
# run_parallel.sh:275-317 (the integrity-failure banner + the phantom-name
# warning) becomes `python3 -m gate.summary format-failure` / `format-phantom`
# calls. Both return the EXACT text the bash emits — the literals are contracts:
# 'shard crash / undercount' is grepped by gate_full_cycle.sh:79 (retry/accept)
# and gate.sh:106 (verdict-truncation detection); 'WARNING: only' is grepped by
# gate.sh:637 and agent_check.sh:266. per #2100 (the phantom-name distinct-names
# rule has a real bug history — clearer + testable in Python) and #1608 (the
# last-[ RUN ]-line culprit is a MISATTRIBUTION, but it is the documented
# behavior consumers expect — preserved verbatim).

# `grep -E '^\[ RUN'` — the crashed-shard log line that names the last test the
# shard started (test_main.cpp:238 prints "[ RUN  ] suite.name").
_RUN_PREFIX_RE = re.compile(r'^\[ RUN')
# `sed -E 's/^\[ RUN  \] //'` — strip the gtest "[ RUN  ] " marker (two spaces
# before the bracket, matching test_main.cpp's exact emission).
_RUN_CULPRIT_RE = re.compile(r'^\[ RUN  \] ')

# narrow #3222: ceiling-killed shards were misattributed as coverage defects.
#
# A shard killed at the per-shard timeout ceiling (BEZEL_SHARD_TIMEOUT,
# tests/run_parallel.sh:159 `timeout -k 10 "${BEZEL_SHARD_TIMEOUT:-300}" ...`)
# writes neither JSON nor a legacy summary line — `timeout` kills the harness
# before the atexit hook (test_main.cpp:281-318) prints them — so the gate
# headline read "shard X printed NO completion summary (slice not validated)"
# and pointed an operator at the coverage/accounting reader, which is the one
# place the problem is not. cf84884 raised the ASan ceiling from 1800s to
# 3600s (x2) so the workload clears it; this module classifies any
# remaining timeout-kill as an INFRA defect (ceiling too low) distinct from
# a SUITE defect (a shard crashed mid-test).
#
# Classification signal: `timeout` returns 124 (SIGTERM at the deadline) or
# 137 (SIGKILL after the -k grace period — tests/run_parallel.sh:159 sets
# `-k 10`) — both are ceiling-kills, neither is a real crash. The shard's
# SHARD_ELAPSED marker (tests/run_parallel.sh:163-164) records the process
# wall time; near the configured ceiling confirms the kill was a budget hit
# rather than a deadlock (which would either hang indefinitely or be caught
# by an earlier timeout exit and a much smaller elapsed). Threshold: 95%
# of BEZEL_SHARD_TIMEOUT — gives 5% slack for the SHARD_ELAPSED marker's
# 1s /proc/uptime resolution and the post-kill cleanup between `timeout`
# SIGTERM and the subshell recording rc.
#
# `extract_shard_elapsed` mirrors the SHARD_ELAPSED grep at
# tests/run_parallel.sh:163-164: the LAST matching line in the log wins
# (matching the awk `tail -1` semantics in the original). Returns None on
# a missing log or absent marker — the caller treats None as "could not
# classify, leave in crashed_shards" (conservative: a missing elapsed
# marker on an rc=124 shard might be an even rarer harness bug we do not
# want to silently re-classify).
_SHARD_ELAPSED_RE = re.compile(r"^SHARD_ELAPSED\s+([0-9]+(?:\.[0-9]+)?)\s*$")
# Fraction of BEZEL_SHARD_TIMEOUT at (or above) which an rc=124/137 shard is
# classified as a ceiling-kill rather than a generic crash. See comment
# above for the rationale.
CEILING_KILL_THRESHOLD = 0.95
# exit codes `timeout` returns when it killed the process. 124 = SIGTERM at
# the deadline; 137 = SIGKILL after the -k grace period
# (tests/run_parallel.sh:159). Distinct from test-failure exit codes
# (typically 1) and segfault/abort (139/134).
_TIMEOUT_RCS = frozenset((124, 137))


def extract_shard_elapsed(logdir, shard_id):
    """The shard's recorded SHARD_ELAPSED wall time in seconds, or None.

    Mirrors tests/run_parallel.sh:163-164: `grep -a -oE 'SHARD_ELAPSED [0-9.]+'
    <log> | awk '{print $2}' | tail -1`. Returns None on a missing log,
    an unreadable log, or no marker — a missing marker on a ceiling-killed
    shard is rare and the caller conservatively leaves the shard in
    crashed_shards rather than re-classify on incomplete evidence."""
    name, sep, idx_s = shard_id.partition('/')
    if not sep:
        return None
    path = os.path.join(logdir, 'shard.{}.{}.log'.format(name, idx_s))
    last = None
    try:
        with open(path, errors='replace') as f:
            for line in f:
                m = _SHARD_ELAPSED_RE.match(line)
                if m:
                    last = m.group(1)
    except OSError:
        return None  # mirrors `2>/dev/null` — a missing log reads as no marker
    if last is None:
        return None
    try:
        return float(last)
    except ValueError:
        return None


def extract_crash_culprit(logdir, shard_id):
    """The last [ RUN ] test name from a crashed shard's log, or '' if none.

    Mirrors run_parallel.sh:313 — `grep -aE '^\\[ RUN' <log> | tail -1 |
    sed -E 's/^\\[ RUN  \\] //'` — byte-for-byte. per #1608: the last-run test
    need not be the crash culprit (a crash may surface one test late), but this
    MISATTRIBUTION is the documented behavior consumers expect, so it is
    preserved. Returns '' (which the banner renders as '<none>') when the shard
    log is missing or holds no [ RUN ] line — the shard died at startup."""
    name, sep, _idx = shard_id.partition('/')
    if not sep:
        return ''
    path = os.path.join(logdir, 'shard.{}.{}.log'.format(name, _idx))
    last = None
    try:
        with open(path, errors='replace') as f:  # -a: treat as text
            for line in f:
                if _RUN_PREFIX_RE.match(line):  # grep -E '^\[ RUN'
                    last = line
    except OSError:
        return ''  # mirrors `2>/dev/null` — a missing log reads as <none>
    if last is None:  # grep found nothing => tail -1 empty
        return ''
    # sed -E 's/^\[ RUN  \] //' — strip the marker if present (count=1: anchored
    # at start, so this only fires when the prefix matches, else leaves the line
    # unchanged, exactly like sed substitution semantics).
    return _RUN_CULPRIT_RE.sub('', last.rstrip('\n'), count=1)


# `[ FAIL ] <test>` — the line a test that RAN and failed prints, mirrored from
# arbitration.py's `_FAIL_LINE_RE` and the gtest output convention. Per #1608
# Appendix and #3018: the "died on: <last RUN>" attribution in the banner is
# documented as a MISATTRIBUTION (the last [ RUN ] line may be an innocent
# bystander at a timeout; the actual failing test is signaled by `[ FAIL ]`).
# The arbitration layer already arbitrates [FAIL] suites first (un-capped),
# but the *banner* showed only the last [ RUN ] — leaving the actual failing
# test invisible to whoever reads the GATE FAILURE block. Cross-check the log
# for [FAIL] lines so the banner names the real culprit alongside the
# (preserved) last-[RUN] attribution.
_FAIL_LINE_RE = re.compile(r"^\[ FAIL \]\s*(\S+)")


def extract_log_failures(logdir, shard_id):
    """The list of `[ FAIL ] <test>` test names from a crashed shard's log.

    The test name is the first whitespace-delimited token after the `] `
    marker (faithful to gtest's emission and arbitration.py's `_FAIL_LINE_RE`).
    Returns [] when the log is missing or carries no [ FAIL ] line — distinct
    from the "no crash" case (which never calls this).

    Per #1608/#3018: when a shard crashes, the last [ RUN ] line is the
    `died on:` attribution the banner reports, but the *actual* failing test
    is the one whose `[ FAIL ]` line appeared before the crash. The banner
    now shows both side-by-side so a reader sees the real culprit without
    having to re-run the shard.
    """
    name, sep, _idx = shard_id.partition('/')
    if not sep:
        return []
    path = os.path.join(logdir, 'shard.{}.{}.log'.format(name, _idx))
    out = []
    try:
        with open(path, errors='replace') as f:
            for line in f:
                m = _FAIL_LINE_RE.match(line)
                if m:
                    out.append(m.group(1))
    except OSError:
        return []
    return out


def format_phantom_warning(listed, selected):
    """The phantom-name warning line (#2100), or '' if no warning.

    Mirrors run_parallel.sh:283-289: a BEZEL_TEST_LIST name matching NO
    registered test is a silently-dropped test (typo, rename, or a selector
    parse artifact). The warning text ('WARNING: only') is LOAD-BEARING —
    gate.sh:637 and agent_check.sh:266 grep it, so the literal is preserved
    verbatim. `listed` is the count of DISTINCT names in BEZEL_TEST_LIST (the
    caller computes it: sed/sort -u/grep -c — a repeated list line must not read
    as a phantom, matching the binary's own set semantics). Returns '' when every
    listed name matched (selected >= listed)."""
    if selected < listed:
        return ("bezel_tests: WARNING: only %d of %d listed tests matched the "
                "registry (BEZEL_TEST_LIST has phantom/renamed names)"
                % (selected, listed))
    return ""


def format_failure_banner(ran, expected, nbin, undercount_bins, subset_empty,
                          missing_summary_shards, njobs, crashed_culprits,
                          crashed_log_failures=None, timed_out_shards=None):
    """The GATE FAILURE banner, byte-identical to run_parallel.sh:298-317, or
    '' when integrity is ok.

    `crashed_culprits` is an ordered list of (shard_id, culprit) pairs (culprit
    from extract_crash_culprit; '' if none). The 'shard crash / undercount'
    literal is LOAD-BEARING (gate_full_cycle.sh:79 + gate.sh:106 grep it). per
    #1608: the culprit is the last [ RUN ] line.

    `crashed_log_failures` (optional, #3018) is a parallel ordered list of
    (shard_id, [fail_name, ...]) pairs from `extract_log_failures`. When a
    shard_id appears in BOTH lists with a non-empty failure list, the banner
    appends an "actual FAIL: <list>" line after the "died on" line, so the
    reader sees the real culprit (the test that ran and failed) alongside the
    preserved last-[RUN] attribution. When no failure markers were observed
    (the shard died at a timeout before any [FAIL] could print, or the
    failure was a clean signal-killed process), only the "died on" line is
    emitted — the new line is conditional, so the existing banner is
    byte-identical for the common no-FAIL case.

    fix #3668: a shard that COMPLETED its slice (it wrote a summary, so it is
    NOT in `missing_summary_shards`) and exited nonzero WITH a `[ FAIL ]` did
    not die on anything — it ran every test and one of them failed. For that
    shard the banner emits ONE line naming the real failure and NO `died on:`
    line at all, because `died on:` is exactly what arbitration.py turns into
    a re-run suspect (`parse_crashed`), and the last [ RUN ] of a completed
    shard is a PASSING test. Every other shape is unchanged: a shard with no
    completion summary keeps both lines (for a real crash the last-[ RUN ]
    victim CAN be the culprit), and a completed shard with NO `[ FAIL ]` keeps
    its `died on:` line — silencing that one would leave arbitration with no
    marker at all, i.e. the return-2/distrust path, which is not an
    improvement on a false accusation.

    `timed_out_shards` (optional, narrow #3222) is the list of shard_ids that
    account() reclassified as ceiling-killed (rc in {124,137} AND elapsed
    near BEZEL_SHARD_TIMEOUT). They are surfaced as a SEPARATE block BEFORE
    the generic "exited nonzero" block, with wording that names the defect
    class — "shard TIMED OUT at the configured ceiling" — so an operator
    reading the banner knows to fix the ceiling (BEZEL_SHARD_TIMEOUT / the
    ASan multiplier in scripts/gate.sh:650) rather than chase the reader.
    The "died on:" attribution is intentionally suppressed for these
    shards: a timed-out shard's "last [ RUN ]" is the test in flight when
    the clock ran out — a misattribution (#1608) that reads as a real
    culprit and would point the operator at the wrong place. When
    timed_out_shards is None or empty (the pre-#3222 shape), the banner is
    byte-identical for the no-real-defect case.

    Returns '' when no integrity defect is present — the bash `if` guard maps
    exactly (any crashed shard, any undercount bin, any missing-summary shard,
    or subset-empty; count_ok==0 and summary_ok==0 are equivalent to those
    lists being non-empty, see account()).
    """
    has_defect = bool(crashed_culprits or undercount_bins
                      or missing_summary_shards or subset_empty
                      or timed_out_shards)
    if not has_defect:
        return ""
    lines = [
        "############################################################",
        "## GATE FAILURE: shard crash / undercount — result is NOT trustworthy",
        ("##   ran %d of %d expected tests (%d unaccounted) across %d binaries"
         % (ran, expected, expected - ran, nbin)),
    ]
    # narrow #3222: surface ceiling-killed shards FIRST with their own
    # distinct wording. The reader needs to see "TIMED OUT at ceiling" before
    # the generic "exited nonzero" block, because the action is different:
    # raise the ceiling (or shorten the suite), not chase the reader.
    if timed_out_shards:
        lines.append("##   %d shard(s) hit the configured per-shard ceiling "
                     "(BEZEL_SHARD_TIMEOUT) — ceiling-killed, NOT a test crash."
                     % len(timed_out_shards))
        lines.append("##   shard(s) TIMED OUT at ceiling: %s"
                     "  (raise BEZEL_SHARD_TIMEOUT or the ASan multiplier in"
                     " scripts/gate.sh:650; the test runner itself was healthy)"
                     % " ".join(timed_out_shards))
    for u in undercount_bins:
        lines.append("##   UNDERCOUNT %s" % u)
    if subset_empty:
        lines.append("##   SUBSET run selected 0 tests in EVERY binary "
                     "(empty/unmatched BEZEL_TEST_LIST) — nothing was validated")
    if missing_summary_shards:
        lines.append("##   %d of %d shards printed NO completion summary "
                     "(slice not validated): shard(s) %s"
                     % (len(missing_summary_shards), njobs,
                        " ".join(missing_summary_shards)))
    # Index failures by shard_id for the cross-check below. Default to {} when
    # the caller didn't pass the new list — the banner stays byte-identical.
    failures_by_id = {}
    if crashed_log_failures:
        for sid, fs in crashed_log_failures:
            if fs:
                failures_by_id[sid] = fs
    # fix #3668: a shard that COMPLETED its slice did not "die on" anything.
    # missing_summary_shards is account()'s own record of which shards printed
    # no completion summary, so `shard_id not in no_summary` IS the completed
    # signal — no second, drifting spelling of a decision account() makes.
    no_summary = set(missing_summary_shards or ())
    for shard_id, culprit in crashed_culprits:
        completed_fails = failures_by_id.get(shard_id)
        if completed_fails and shard_id not in no_summary:
            # The measured #3668 shape: the shard ran every test (RUN==OK+FAIL)
            # and printed its summary, then exited nonzero because ONE test
            # FAILED. Its last [ RUN ] is simply the last test in the slice —
            # a PASSING one — and printing that as `died on:` made
            # arbitration.py:parse_crashed adopt an innocent suite as a
            # suspect, re-run it 5x under ASan and stamp it FLAKY. Name the
            # test that actually failed and emit NO `died on:` line: the
            # `[ FAIL ]` marker arbitration needs is still in the same log
            # (run_parallel.sh:317-322 prints it under `--- failing tests ---`
            # from the very lines this list was harvested from), so invariant 4
            # (no marker => return 2 => distrust) is untouched.
            lines.append("##   shard %s exited nonzero — completed its slice; "
                         "actual FAIL: %s"
                         % (shard_id, " ".join(completed_fails)))
            continue
        lines.append("##   shard %s exited nonzero — died on: %s"
                     % (shard_id, culprit or "<none>"))
        # Per #3018: surface the actual [FAIL] lines (the test that ran and
        # FAILED) alongside the (preserved) last-[RUN] attribution. The
        # "last [RUN]" attribution is documented as a misattribution (#1608),
        # so the FAIL list is the real culprit. Distinct line so existing
        # grep/scraper consumers of the "died on:" line are unaffected.
        if shard_id in failures_by_id:
            fails = failures_by_id[shard_id]
            lines.append("##   shard %s actual FAIL: %s"
                         % (shard_id, " ".join(fails)))
    lines.append("############################################################")
    return "\n".join(lines)


def _main_check(logdir):
    mismatches = check_agreement(logdir)
    n_json = len(read_shard_summaries(logdir))
    if mismatches:
        # LOUD: a divergence is a parsing bug or a #3012-style stream race.
        for m in mismatches:
            print("shard-summary MISMATCH: {}".format(m), file=sys.stderr)
        print("shard-summary agreement: {} MISMATCH(es) over {} JSON summaries "
              "(observation-only; legacy path authoritative)".format(
                  len(mismatches), n_json), file=sys.stderr)
        # Exit 1: a mismatch is a real divergence. The OBSERVATION-ONLY nature
        # of the legacy call site (run_parallel.sh `... || true`) is enforced by
        # the caller, not here — returning nonzero lets the Phase 2c dual-read
        # assertion treat a mismatch as a hard failure while the observation
        # call still swallows it with `|| true`.
        return 1
    print("shard-summary agreement: 0 mismatches over {} JSON summaries "
          "(observation-only)".format(n_json))
    return 0


def _main_account(argv):
    """`summary.py account <logdir> --conc N [--subset] [--run-summary PATH]
    [names ...]` — compute the run accounting from JSON shard summaries and
    print shell-sourceable rp_* assignments. run_parallel evals this; the
    rp_integrity_ok flag drives its exit. [--run-summary PATH] additionally
    writes the merged JSON for gate.sh to read (the BEZEL_RUN_SUMMARY bridge).
    """
    import argparse
    p = argparse.ArgumentParser(prog="summary.py account", add_help=True)
    p.add_argument("logdir", help="run_parallel shard logdir (summary.*.json)")
    p.add_argument("--conc", type=int, required=True,
                   help="per-binary shard count (run_parallel's resource cap)")
    p.add_argument("--subset", action="store_true",
                   help="BEZEL_TEST_LIST mode: expected = selected, not registry")
    p.add_argument("--shard-timeout", type=int, default=None,
                   help="per-shard ceiling in seconds (BEZEL_SHARD_TIMEOUT). "
                        "When set, rc=124/137 shards with elapsed >= "
                        "95%% of this are reclassified as timed_out_shards "
                        "(ceiling-killed, #3222) rather than crashed_shards.")
    p.add_argument("--run-summary", default=None,
                   help="also write the merged run-summary JSON to PATH")
    p.add_argument("names", nargs="*", help="binary labels run_parallel launched")
    args = p.parse_args(argv)
    acc = account(args.logdir, args.conc, args.names, subset=args.subset,
                  shard_timeout=args.shard_timeout)
    if args.run_summary:
        write_run_summary(args.run_summary, acc)
    sys.stdout.write(emit_shell(acc) + "\n")
    # Exit 0 = accounting computed (the integrity verdict is in rp_integrity_ok,
    # which run_parallel reads — a failed run is not a failure to compute).
    return 0


def _main_format_phantom(argv):
    """`summary.py format-phantom --listed N --selected M` — print the
    phantom-name warning line (or nothing). The gating (subset mode, no filter,
    readable list) stays in run_parallel; this is the pure formatter (#2100)."""
    import argparse
    p = argparse.ArgumentParser(prog="summary.py format-phantom", add_help=True)
    p.add_argument("--listed", type=int, required=True,
                   help="distinct name count in BEZEL_TEST_LIST (caller-computed)")
    p.add_argument("--selected", type=int, required=True,
                   help="global selected count (run_parallel's global_selected)")
    args = p.parse_args(argv)
    text = format_phantom_warning(args.listed, args.selected)
    if text:
        sys.stdout.write(text + "\n")
    return 0


def _main_format_failure(argv):
    """`summary.py format-failure --ran N --expected M --nbin N [--subset-empty 1]
    --njobs N [--undercount a b ...] [--missing-summary x y ...]
    [--crashed a/0 b/1 ...] --logdir PATH` — print the GATE FAILURE banner (or
    nothing). The culprit extraction (grep RUN | tail -1 | sed) is done HERE
    from the shard logs, so run_parallel no longer carries that fiddly shell.

    Per #3018: for each crashed shard, also cross-check the log for [FAIL]
    lines via `extract_log_failures` and include them in the banner as
    "actual FAIL:" so the reader sees the real failing test alongside the
    (preserved) last-[RUN] attribution."""
    import argparse
    p = argparse.ArgumentParser(prog="summary.py format-failure", add_help=True)
    p.add_argument("--ran", type=int, required=True)
    p.add_argument("--expected", type=int, required=True,
                   help="expected_all (run_parallel's expected_all)")
    p.add_argument("--nbin", type=int, required=True, help="binary count")
    p.add_argument("--subset-empty", type=int, default=0,
                   help="1 iff a SUBSET run selected 0 in every binary")
    p.add_argument("--njobs", type=int, required=True, help="total shard count")
    p.add_argument("--undercount", nargs="*", default=[],
                   help="per-binary undercount bin strings")
    p.add_argument("--missing-summary", nargs="*", default=[],
                   help="shard IDs (name/idx) that printed no summary")
    p.add_argument("--crashed", nargs="*", default=[],
                   help="shard IDs (name/idx) that exited nonzero")
    # narrow #3222: ceiling-killed shards are passed separately so the banner
    # surfaces them with their own wording ("TIMED OUT at ceiling") BEFORE the
    # generic "exited nonzero" block. Empty list is the pre-#3222 shape —
    # banner stays byte-identical.
    p.add_argument("--timed-out", nargs="*", default=[],
                   help="shard IDs (name/idx) reclassified as ceiling-killed "
                        "(rc in {124,137} AND elapsed near BEZEL_SHARD_TIMEOUT)")
    p.add_argument("--logdir", required=True,
                   help="run_parallel shard logdir (shard.<name>.<i>.log)")
    args = p.parse_args(argv)
    crashed_culprits = [(s, extract_crash_culprit(args.logdir, s))
                        for s in args.crashed]
    # Per #3018: cross-check the log for [FAIL] lines so the banner names the
    # real culprit alongside the preserved last-[RUN] attribution. Parallel list
    # to crashed_culprits (same shard_ids, in the same order).
    crashed_log_failures = [(s, extract_log_failures(args.logdir, s))
                            for s in args.crashed]
    text = format_failure_banner(
        args.ran, args.expected, args.nbin, args.undercount,
        bool(args.subset_empty), args.missing_summary, args.njobs,
        crashed_culprits, crashed_log_failures,
        timed_out_shards=args.timed_out)
    if text:
        sys.stdout.write(text + "\n")
    return 0


def _main_run_summary(argv):
    """`summary.py run-summary harvest <path>` — read a BEZEL_RUN_SUMMARY and
    print shell-sourceable rp_run_* assignments for gate.sh. Emits
    rp_run_present=0 when the file is absent/wrong-schema so gate.sh falls back
    to its legacy grep (the grep stays a fallback until N gates prove the JSON
    path, per the Phase 2c methodology)."""
    if len(argv) < 2 or argv[0] != "harvest":
        sys.stderr.write("usage: summary.py run-summary harvest <path>\n")
        return 2
    gtests, ran = harvest_run_summary(argv[1])
    if gtests is None:
        sys.stdout.write("rp_run_present=0\n")
        return 0
    # GATE_BIN_RAN format (gate.sh:855-860): space-separated "name:passed"
    # pairs, matching the legacy grep+awk harvest so the awk merge at gate.sh
    # :330-347 consumes it unchanged.
    pairs = " ".join("%s:%d" % (k, v) for k, v in ran.items())
    sys.stdout.write("rp_run_present=1\n")
    sys.stdout.write("rp_run_gtests=%d\n" % gtests)
    sys.stdout.write('rp_run_bin_ran="%s"\n' % pairs)
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "check":
        if len(argv) < 3:
            print("usage: summary.py check <logdir>", file=sys.stderr)
            return 2
        return _main_check(argv[2])
    if len(argv) >= 2 and argv[1] == "account":
        return _main_account(argv[2:])
    if len(argv) >= 2 and argv[1] == "format-phantom":
        return _main_format_phantom(argv[2:])
    if len(argv) >= 2 and argv[1] == "format-failure":
        return _main_format_failure(argv[2:])
    if len(argv) >= 2 and argv[1] == "run-summary":
        if len(argv) < 3:
            print("usage: summary.py run-summary harvest <path>", file=sys.stderr)
            return 2
        return _main_run_summary(argv[2:])
    print("usage: summary.py {check|account|format-phantom|format-failure|"
          "run-summary} ...", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
