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
        # Still exit 0: observation-only — never change the gate's verdict.
        return 0
    print("shard-summary agreement: 0 mismatches over {} JSON summaries "
          "(observation-only)".format(n_json))
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "check":
        if len(argv) < 3:
            print("usage: summary.py check <logdir>", file=sys.stderr)
            return 2
        return _main_check(argv[2])
    print("usage: summary.py check <logdir>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
